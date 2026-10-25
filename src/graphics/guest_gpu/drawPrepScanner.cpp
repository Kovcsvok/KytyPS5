#include "graphics/guest_gpu/drawPrepScanner.h"

#include "common/profiler.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/command_processor/pm4Dispatch.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/renderer/drawPrep.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderDraw.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace Libs::Graphics {

namespace {

// GuestGpu::ComputeQueueBase: the interrupt event id of compute queue 1.
constexpr uint32_t ComputeQueueBase = 0x20;

bool IsGraphicsDraw(uint32_t opcode) {
	return DrawPrep::CountsAsDrawOrDispatch(opcode) && opcode != Pm4::IT_DISPATCH_DIRECT &&
	       opcode != Pm4::IT_DISPATCH_INDIRECT;
}

// A command stream the scanner follows: guest memory the guest has committed.
bool Readable(const uint32_t* commands, uint32_t count) {
	return commands != nullptr && count != 0 &&
	       LibKernel::Memory::IsCommittedRange(reinterpret_cast<uint64_t>(commands),
	                                           static_cast<uint64_t>(count) * sizeof(uint32_t));
}

} // namespace

DrawPrepScanner::DrawPrepScanner(RenderContext& renderer): m_renderer(renderer) {
	m_thread = std::thread([this] { Run(); });
}

DrawPrepScanner::~DrawPrepScanner() {
	{
		std::lock_guard lock(m_mutex);
		m_stop = true;
	}
	m_work.notify_one();
	if (m_thread.joinable()) {
		m_thread.join();
	}
}

DrawPrepScanner::QueueState& DrawPrepScanner::State(uint32_t queue) {
	auto& state = m_queues[queue % m_queues.size()];
	if (state == nullptr) {
		state     = std::make_unique<QueueState>();
		state->cp = std::make_unique<CommandProcessor>(
		    m_renderer, queue == 0 ? 0 : static_cast<int>(ComputeQueueBase + queue - 1u));
		state->saved_ctx = std::make_unique<HW::Context>();
	}
	return *state;
}

void DrawPrepScanner::EnqueueSubmission(uint32_t queue, uint64_t seq,
                                        std::span<const uint32_t> commands, bool reset) {
	{
		std::lock_guard lock(m_mutex);
		m_items.push_back({queue, seq, commands, reset});
	}
	m_work.notify_one();
}

void DrawPrepScanner::Run() {
	KYTY_PROFILER_THREAD("Thread_DrawPrep");
	// Only the GPU thread compiles resource plans to native code; the scanner uses what exists.
	ShaderRecompiler::IR::SetSrtNativeCompileBlocked(true);
	for (;;) {
		Item item;
		{
			std::unique_lock lock(m_mutex);
			m_work.wait(lock, [this] { return m_stop || !m_items.empty(); });
			if (m_stop) {
				return;
			}
			item = m_items.front();
			m_items.pop_front();
		}
		m_queue = item.queue;
		m_state = &State(item.queue);
		if (item.reset) {
			// As GuestGpu::Process before the submission's first slice.
			ResetState();
		}
		if (!item.commands.empty()) {
			Scan(item);
		}
		DrawPrep::LogCountersIfDue();
	}
}

void DrawPrepScanner::ResetState() {
	m_state->cp->Reset();
	m_state->saved_ctx->Reset();
	m_state->pushed  = false;
	m_state->tainted = false;
}

// As CommandProcessor::ApplyContextStateOperation, without its exits: a sequence the scanner
// cannot follow only makes its register copy unreliable.
void DrawPrepScanner::ApplyContextState(uint32_t operation) {
	auto& ctx = m_state->cp->GetCtx();
	switch (static_cast<ContextStateOperation>(operation)) {
		case ContextStateOperation::Clear: ctx.Reset(); break;
		case ContextStateOperation::Push:
			m_state->tainted |= m_state->pushed;
			*m_state->saved_ctx = ctx;
			m_state->pushed     = true;
			break;
		case ContextStateOperation::Pop:
			m_state->tainted |= !m_state->pushed;
			ctx             = *m_state->saved_ctx;
			m_state->pushed = false;
			break;
		case ContextStateOperation::PushClear:
			m_state->tainted |= m_state->pushed;
			*m_state->saved_ctx = ctx;
			m_state->pushed     = true;
			ctx.Reset();
			break;
		default: m_state->tainted = true; break;
	}
}

// SET_*_REG_INDIRECT: the register pairs are read from guest memory no tracked GPU write covers
// (no fault, no wait) and handed to the command processor's own handler from a local copy. False
// when they cannot be read that way: the register copy is then unreliable.
bool DrawPrepScanner::ApplyRegisterIndirect(uint32_t header, const uint32_t* body) {
	if (KYTY_PM4_LEN(header) != 5u) {
		return false;
	}
	const uint64_t address =
	    (static_cast<uint64_t>(body[0]) & 0xfffffffcu) | (static_cast<uint64_t>(body[1]) << 32u);
	const uint32_t count = body[3] & 0x3fffu;
	if (count == 0) {
		return true;
	}
	if (address == 0) {
		return false;
	}
	thread_local std::vector<uint32_t> pairs;
	pairs.resize(static_cast<size_t>(count) * 2u);
	const auto  bytes   = pairs.size() * sizeof(uint32_t);
	const auto* backing = LibKernel::Memory::FindConcurrentCleanBacking(address, bytes);
	if (backing == nullptr) {
		return false;
	}
	std::memcpy(pairs.data(), backing, bytes);
	const auto local = reinterpret_cast<uint64_t>(pairs.data());
	const std::array<uint32_t, 4> copy {static_cast<uint32_t>(local),
	                                    static_cast<uint32_t>(local >> 32u), body[2], body[3]};
	const auto opcode = (header >> 8u) & 0xffu;
	(void)g_cp_op_func[opcode](*m_state->cp, header, copy.data(), 5u, 5u);
	return true;
}

void DrawPrepScanner::ScanDraw(uint64_t seq, uint32_t ordinal, const uint32_t* packet) {
	if (m_state->tainted) {
		DrawPrep::Add(DrawPrep::Counter::SkipUnsupported);
		return;
	}
	const DrawPrep::Key key {.queue = m_queue, .seq = seq, .ordinal = ordinal, .packet = packet};
	if (!DrawPrep::GpuProgress(m_queue).Before(key)) {
		DrawPrep::Add(DrawPrep::Counter::SkipBehind);
		return;
	}
	auto&       cp   = *m_state->cp;
	const auto& ctx  = cp.GetCtx();
	const auto& sh   = cp.GetShCtx();
	const auto& ucfg = cp.GetUcfg();
	if (!DrawPrepHasVertexShader(sh)) {
		DrawPrep::Add(DrawPrep::Counter::SkipUnsupported);
		return;
	}
	std::array<Prospero::ColorComponentMapping, 8> mapping {};
	const bool                                     ps_active = DrawPrepPixelState(ctx, sh, mapping);
	DrawPrep::PacketResult                         result;
	DrawPrep::Recycle(result);
	result.key = key;
	if (m_renderer.GetPipelineCache().DrawPrepGraphics(sh.GetVs(), sh.GetPs(),
	                                                   ctx.GetShaderRegisters(), ctx, ucfg, mapping,
	                                                   ps_active, result)) {
		DrawPrep::Publish(std::move(result));
	}
}

// A dispatch: the compute inputs RenderExecutor::DispatchDirect / DispatchIndirect build.
void DrawPrepScanner::ScanDispatch(uint64_t seq, uint32_t ordinal, const uint32_t* packet,
                                   uint32_t opcode) {
	if (m_state->tainted) {
		DrawPrep::Add(DrawPrep::Counter::SkipUnsupported);
		return;
	}
	const DrawPrep::Key key {.queue = m_queue, .seq = seq, .ordinal = ordinal, .packet = packet};
	if (!DrawPrep::GpuProgress(m_queue).Before(key)) {
		DrawPrep::Add(DrawPrep::Counter::SkipBehind);
		return;
	}
	auto&       cp = *m_state->cp;
	const auto& cs = cp.GetShCtx().GetCs();
	if (cs.cs_regs.data_addr == 0) {
		DrawPrep::Add(DrawPrep::Counter::SkipUnsupported);
		return;
	}
	ShaderComputeInputInfo input_info {};
	if (opcode == Pm4::IT_DISPATCH_DIRECT) {
		if (KYTY_PM4_LEN(packet[0]) != 5u) {
			DrawPrep::Add(DrawPrep::Counter::SkipUnsupported);
			return;
		}
		const std::array<uint32_t, 3> dispatched {packet[1], packet[2], packet[3]};
		const uint32_t                mode = packet[4];
		const bool                    use_thread_dimensions =
		    (mode & Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0;
		input_info.dispatch_thread_dimensions = use_thread_dimensions;
		const std::array<uint32_t, 3> group_size {cs.cs_regs.num_thread_x, cs.cs_regs.num_thread_y,
		                                          cs.cs_regs.num_thread_z};
		for (uint32_t axis = 0; axis < 3u; axis++) {
			const auto size = std::max(group_size[axis], 1u);
			input_info.dispatch_groups[axis] =
			    use_thread_dimensions ? (dispatched[axis] + size - 1u) / size : dispatched[axis];
		}
	} else {
		// As CpOpDispatchIndirect, which gets the header without its predicate bit.
		const uint32_t header = packet[0] & ~1u;
		if (header != 0xc0011600u && header != 0xc0021600u) {
			DrawPrep::Add(DrawPrep::Counter::SkipUnsupported);
			return;
		}
		const uint32_t mode = header == 0xc0021600u ? packet[3] : packet[2];
		const bool     use_thread_dimensions =
		    (mode & Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0;
		input_info.dispatch_thread_dimensions   = use_thread_dimensions;
		input_info.dispatch_dimensions_indirect = use_thread_dimensions;
	}
	DrawPrep::PacketResult result;
	DrawPrep::Recycle(result);
	result.key = key;
	if (m_renderer.GetPipelineCache().DrawPrepCompute(cs, cp.GetCtx().GetShaderRegisters(),
	                                                  input_info, result)) {
		DrawPrep::Publish(std::move(result));
	}
}

// The packet walk of CommandProcessor::ProcessPm4, for registers only: register writes go to the
// scanner's command processor, draws and dispatches are prepared, everything else is stepped over.
// The scan stops where it cannot know what the GPU thread runs (a branch, COND_EXEC, a stream
// outside committed memory) and once the GPU thread has moved past the submission (the guest may
// reuse its memory then).
void DrawPrepScanner::Scan(const Item& item) {
	KYTY_PROFILER_FUNCTION();
	struct Cursor {
		std::span<const uint32_t> commands;
		uint32_t                  offset = 0;
	};
	if (!Readable(item.commands.data(), static_cast<uint32_t>(item.commands.size()))) {
		DrawPrep::Add(DrawPrep::Counter::StopScan);
		return;
	}
	thread_local std::vector<Cursor> stack;
	stack.clear();
	stack.push_back({item.commands});
	uint32_t   ordinal = 0;
	const auto stop    = [] { DrawPrep::Add(DrawPrep::Counter::StopScan); };
	while (!stack.empty()) {
		if (DrawPrep::GpuProgress(m_queue).seq > item.seq) {
			DrawPrep::Add(DrawPrep::Counter::SkipBehind);
			return;
		}
		auto& cursor = stack.back();
		if (cursor.offset >= cursor.commands.size()) {
			stack.pop_back();
			continue;
		}
		const auto* const packet    = cursor.commands.data() + cursor.offset;
		const auto        total     = static_cast<uint32_t>(cursor.commands.size());
		const auto        remaining = total - cursor.offset;
		const auto        header    = packet[0];
		if (header == 0x80000000u) {
			cursor.offset++;
			continue;
		}
		if (remaining < 2) {
			return stop();
		}
		if ((header >> 30u) == 0u) {
			const auto length = ((header >> 16u) & 0x3fffu) + 2u;
			if (length > remaining) {
				return stop();
			}
			cursor.offset += length;
			continue;
		}
		const auto opcode = (header >> 8u) & 0xffu;
		const auto length = KYTY_PM4_LEN(header);
		if (length > remaining) {
			return stop();
		}
		if (DrawPrep::CountsAsDrawOrDispatch(opcode)) {
			// Numbered as the GPU thread numbers them, predicated ones too.
			const auto this_ordinal = ordinal++;
			DrawPrep::Add(DrawPrep::Counter::Scanned);
			if (IsGraphicsDraw(opcode)) {
				ScanDraw(item.seq, this_ordinal, packet);
			} else {
				ScanDispatch(item.seq, this_ordinal, packet, opcode);
			}
			cursor.offset += length;
			continue;
		}
		if ((header & 1u) != 0) {
			// Predicated: whether the GPU thread runs it depends on a query result.
			cursor.offset += length;
			continue;
		}
		const auto command = header & ~1u;
		switch (opcode) {
			case Pm4::IT_SET_CONTEXT_REG:
			case Pm4::IT_SET_SH_REG:
			case Pm4::IT_SET_UCONFIG_REG:
			case Pm4::IT_SET_UCONFIG_REG_INDEX: {
				const auto consumed =
				    g_cp_op_func[opcode](*m_state->cp, command, packet + 1, remaining, total) + 1u;
				if (consumed > remaining) {
					return stop();
				}
				cursor.offset += consumed;
				continue;
			}
			case Pm4::IT_SET_CONTEXT_REG_INDIRECT:
			case Pm4::IT_SET_SH_REG_INDIRECT:
			case Pm4::IT_SET_UCONFIG_REG_INDIRECT:
				if (!ApplyRegisterIndirect(command, packet + 1)) {
					m_state->tainted = true;
				}
				break;
			case Pm4::IT_CLEAR_STATE: m_state->cp->GetCtx().Reset(); break;
			case Pm4::IT_COND_EXEC: return stop();
			case Pm4::IT_INDIRECT_BUFFER: {
				if (length != 4u) {
					// A branch on guest memory: the scanner cannot follow it.
					return stop();
				}
				const auto* sub = reinterpret_cast<const uint32_t*>(
				    packet[1] | (static_cast<uint64_t>(packet[2]) << 32u));
				const uint32_t control = packet[3];
				const uint32_t count   = control & 0xfffffu;
				cursor.offset += length;
				if (count != 0) {
					if (!Readable(sub, count)) {
						return stop();
					}
					if ((control & (1u << 20u)) != 0) {
						cursor = {std::span(sub, count)};
					} else {
						stack.push_back({std::span(sub, count)});
					}
				}
				continue;
			}
			case Pm4::IT_NOP: {
				const auto r = KYTY_PM4_R(header);
				if (r == Pm4::R_ZERO) {
					if ((packet[1] & 0xffff0000u) == 0x68750000u) {
						// CpOpMarker: the user-data markers only (the rest flip).
						const auto id = packet[1] & 0xfffu;
						if (id == 0x4u) {
							m_state->cp->SetUserDataMarker(HW::UserSgprType::Vsharp);
						} else if (id == 0xdu) {
							m_state->cp->SetUserDataMarker(HW::UserSgprType::Region);
						}
					}
				} else if (r == Pm4::R_CONTEXT_STATE) {
					if (length == 3u || length == 5u) {
						ApplyContextState(packet[1]);
					} else {
						m_state->tainted = true;
					}
				} else if (r == Pm4::R_DISPATCH_RESET) {
					ResetState();
				}
				break;
			}
			default: break;
		}
		cursor.offset += length;
	}
}

} // namespace Libs::Graphics
