#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_INDIRECTDISPATCH_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_INDIRECTDISPATCH_H_

#include "common/common.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <array>
#include <vulkan/vulkan.hpp>

namespace Libs::Graphics {

struct GraphicContext;
class CommandScheduler;
class CommandRecorder;

// DISPATCH_INDIRECT with the thread-dimension initiator carries thread counts, not workgroup
// counts. The command processor used to read them on the CPU, but the previous dispatch writes
// them on the GPU, so every read drained the GPU; the title issues about 200 of these a frame.
// This records a one-invocation pass that converts the counts on the GPU instead.
class IndirectDispatchGroups {
public:
	IndirectDispatchGroups(GraphicContext& graphics, CommandScheduler& scheduler);
	~IndirectDispatchGroups();
	KYTY_CLASS_NO_COPY(IndirectDispatchGroups);

	struct Result {
		vk::Buffer        groups_buffer;
		vk::DeviceSize    groups_offset = 0;
		// A copy of the thread counts that stays valid until the dispatch has run, unlike the
		// cache buffer the guest's arguments live in, which a later binding may merge away.
		vk::DeviceAddress threads = 0;
	};
	// Records the conversion of the three thread counts at `threads` and returns the workgroup
	// counts' location, ready for dispatchIndirect. Binds a compute pipeline and push state, so
	// call it before committing the guest dispatch's bindings.
	[[nodiscard]] Result Convert(const CommandRecorder& command, vk::DeviceAddress threads,
	                             const std::array<uint32_t, 3>& local_size);

private:
	// Ring of converted counts. An entry is rewritten only after 4096 later conversions, each
	// ordered behind the indirect reads before it.
	static constexpr uint32_t Entries      = 4096;
	static constexpr uint32_t EntryDwords  = 8; // groups at 0, thread counts at 4

	GraphicContext&         m_graphics;
	Buffer                  m_groups;
	vk::DescriptorSetLayout m_set_layout      = nullptr;
	vk::PipelineLayout      m_pipeline_layout = nullptr;
	vk::Pipeline            m_pipeline        = nullptr;
	uint32_t                m_next            = 0;
};

// DRAW_INDIRECT and DRAW_INDEX_INDIRECT read their counts on the GPU, from a record an earlier
// pass wrote. When that pass misbehaves the record holds garbage, and a draw of billions of
// vertices or instances keeps the geometry engine busy until the driver resets the device
// (Wolverine on AMD, combat: Radeon GPU Detective shows DrawIndirectMulti in progress with no
// page fault). This copies the record on the GPU and zeroes counts no real draw has.
// KYTY_INDIRECT_DRAW_GUARD=0 draws from the guest's record directly.
class IndirectDrawGuard {
public:
	IndirectDrawGuard(GraphicContext& graphics, CommandScheduler& scheduler);
	~IndirectDrawGuard();
	KYTY_CLASS_NO_COPY(IndirectDrawGuard);

	[[nodiscard]] static bool Enabled();

	struct Result {
		vk::Buffer     buffer;
		vk::DeviceSize offset = 0;
	};
	// Records the checked copy of the draw record at `record` (5 dwords indexed, 4 otherwise)
	// and returns where the draw reads it. Must be recorded outside a render pass.
	[[nodiscard]] Result Guard(const CommandRecorder& command, vk::DeviceAddress record,
	                           bool indexed);

private:
	void Report();

	static constexpr uint32_t Entries     = 4096;
	static constexpr uint32_t EntryDwords = 8;

	GraphicContext&         m_graphics;
	Buffer                  m_records;
	// The dropped-draw count and the last dropped record, read back for the log every 5 s.
	Buffer                  m_report;
	vk::DescriptorSetLayout m_set_layout      = nullptr;
	vk::PipelineLayout      m_pipeline_layout = nullptr;
	vk::Pipeline            m_pipeline        = nullptr;
	uint32_t                m_next            = 0;
	uint32_t                m_reported        = 0;
	int64_t                 m_last_report_us  = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_INDIRECTDISPATCH_H_
