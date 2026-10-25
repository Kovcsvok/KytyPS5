#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWPREP_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWPREP_H_

#include "graphics/shader/recompiler/ir/ResourceSnapshot.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <array>
#include <cstdint>
#include <vector>

// Draw preparation ahead of the GPU thread (KYTY_DRAW_PREP, live, default 0), after Senaxx
// 6632a241 (Revi 73ffc2ee measured it on Wolverine: command processor 37.8 -> 33.8 ms a frame).
//
// A scanner thread (guest_gpu/drawPrepScanner.cpp) reads every graphics and compute submission as
// soon as it is queued, follows its register writes on command processors of its own, and for
// each draw or dispatch runs the descriptor walk (the SRT refresh of ProgramCache::Get) with a
// reader that takes only bytes no GPU work has written and records them. The GPU thread executes
// exactly as before; where it would walk, it takes the prepared result instead when the program,
// user data, shader base and dispatch size are the same and every recorded range still holds the
// same bytes and is still clean. Otherwise it walks, so correctness never depends on the scanner:
// a walk is a function of its program, those inputs and the bytes it read.
//
// KYTY_DRAW_PREP_VERIFY (live): 1 = every result taken is compared with a walk on the GPU thread
// and differences are logged, 2 = the emulator stops at the first difference.

namespace Libs::Graphics::DrawPrep {

[[nodiscard]] bool Enabled();
[[nodiscard]] int  VerifyMode(); // 0 off, 1 count and log, 2 exit on a difference

// GPU queues (GuestGpu: graphics 0, compute 1..56).
constexpr uint32_t MaxQueues = 64;

// A draw or dispatch packet: its queue, its submission (numbered at enqueue), its position among
// the draw and dispatch packets of that submission's command stream, and its address (a check that
// both sides counted the same packets).
struct Key {
	uint32_t        queue   = 0;
	uint64_t        seq     = 0;
	uint32_t        ordinal = 0;
	const uint32_t* packet  = nullptr;

	[[nodiscard]] bool Before(const Key& other) const {
		return seq < other.seq || (seq == other.seq && ordinal < other.ordinal);
	}
};

// Whether a packet opcode counts as a draw or dispatch for Key::ordinal (both sides use this).
[[nodiscard]] bool CountsAsDrawOrDispatch(uint32_t opcode);

// The guest bytes a preparation read, in read order, merged where contiguous.
class ReadSet {
public:
	void Clear() {
		m_ranges.clear();
		m_bytes.clear();
	}
	void Add(uint64_t address, const void* data, uint32_t size);
	// GPU thread: every range is still clean for a backing read and holds the recorded bytes.
	[[nodiscard]] bool Validate() const;

private:
	struct Range {
		uint64_t address;
		uint32_t offset; // into m_bytes
		uint32_t size;
	};
	std::vector<Range>   m_ranges;
	std::vector<uint8_t> m_bytes;
};

// One stage's prepared refresh.
struct StageResult {
	bool                                         ok          = false;
	bool                                         consumed    = false;
	uint64_t                                     serial      = 0; // unique per preparation
	uint32_t                                     stage       = 0; // ShaderType
	const void*                                  entry       = nullptr; // ProgramCache entry
	uint64_t                                     shader_base = 0;
	std::vector<uint32_t>                        user_data;
	// Compute: the dispatch size the walk saw (SrtRuntime::workgroup_count / workgroup_size).
	std::array<uint32_t, 3>                      workgroup_count {};
	std::array<uint32_t, 3>                      workgroup_size {};
	ShaderRecompiler::IR::ResourceSnapshot       resources;
	ShaderRecompiler::IR::ResourceSpecialization specialization;
	ReadSet                                      reads;
};

struct PacketResult {
	Key                        key;
	std::array<StageResult, 2> stages;
	uint32_t                   stage_count = 0;
};

// GPU thread: the draw or dispatch packet being executed (SetCurrent), and its progress per queue,
// which the scanner reads to skip packets and submissions the GPU thread has already passed.
void                     SetCurrent(const Key& key);
void                     ClearCurrent();
void                     NoteProgress(uint32_t queue, uint64_t seq, uint32_t ordinal);
[[nodiscard]] const Key* Current();
[[nodiscard]] Key        GpuProgress(uint32_t queue);

// Scanner: a prepared packet, in stream order. Waits a little while too many results wait.
void Publish(PacketResult&& result);
// Scanner: a result to fill, reusing the buffers of one the GPU thread is done with.
void Recycle(PacketResult& result);
// GPU thread: the prepared refresh of `stage` for the current packet, or null; consumed ones too
// (a later draw of the same packet may use it again). The result stays owned by the store.
[[nodiscard]] StageResult* Take(uint32_t stage);

// Counters (always counted; logged every 10 s while enabled).
enum class Counter : uint32_t {
	Scanned,         // draw/dispatch packets the scanner saw
	Prepared,        // stage refreshes prepared
	SkipBehind,      // packets the GPU thread had already passed
	SkipUnknownPair, // shader pair the GPU thread has not prepared yet
	SkipUnsupported, // tessellation, no vertex shader, ...
	SkipNoEntry,     // program not translated yet
	FailWalk,        // walk failed (unclean or unreadable bytes)
	StopScan,        // streams the scanner stopped reading (a branch, a conditional, bad memory)
	Takes,           // GPU thread asked
	Used,            // results used
	Reused,          // results used again by a later draw of the same packet (multi-draw, retry)
	MissNone,        // no result for the current packet
	MissEntry,       // different program
	MissInputs,      // user data, shader base or dispatch size differ
	MissReads,       // recorded bytes changed or are no longer clean
	VerifyChecks,
	VerifyMismatches,
	Count,
};
void Add(Counter counter, uint64_t value = 1);
void LogCountersIfDue();

// GPU thread: a vertex/pixel shader pair (or a compute shader) whose programs it prepared
// successfully; the scanner prepares only such pairs (it never meets a shader state the draw path
// has not handled).
void               NoteGoodPair(uint64_t vs_addr, uint64_t ps_addr);
[[nodiscard]] bool IsGoodPair(uint64_t vs_addr, uint64_t ps_addr);

} // namespace Libs::Graphics::DrawPrep

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWPREP_H_
