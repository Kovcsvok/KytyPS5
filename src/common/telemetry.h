#ifndef KYTY_COMMON_TELEMETRY_H_
#define KYTY_COMMON_TELEMETRY_H_

#include "common/profiler.h"

#include <chrono>
#include <cstdint>

namespace Common::Telemetry {

// KYTY_TELEMETRY_MS=N (live, default 0 = off): every N ms one row of the counters below goes to
// logs/telemetry_<time>.csv (relative to the working directory), and every frame one row to
// logs/frames_<time>.csv. Each row carries KYTY_EXPERIMENT (live), the tag a script switching
// other live switches sets for each setup, so one run compares setups in alternating short
// blocks and the scene's own changes average out (docs/wolverine-amd/experiment.py).
// Times are summed over all threads that pass the site, in nanoseconds (written as ms).
enum class Counter : uint32_t {
	GpuProcess,     // GPU thread: CommandProcessor::Process
	GpuIdle,        // GPU thread: waiting for guest work
	GpuReadMemory,  // GPU thread: its own readbacks
	GpuCopyQueue,   // GPU thread: synchronous readback-queue copies
	FindBuffers,    // GPU thread: RenderExecutor::FindBuffers
	CreateBuffer,   // GPU thread: BufferCache::CreateBuffer (inside FindBuffers)
	BuildKey,       // GPU thread: ProgramCache::BuildKey
	Materialize,    // GPU thread: ProgramCache::MaterializeResources
	GuestFault,     // guest threads: RenderContext::HandleFault
	GuestFaults,    // count
	ReadbackWait,   // guest threads: waiting for a readback's GPU work
	ReadbackWaits,  // count
	BufferLockWait, // any thread: the contended buffer-cache range lock
	BufferLockWaits, // count
	MemoryLockWait, // any thread: a contended guest-memory lock
	MemoryLockWaits, // count
	SpinLockWait,   // any thread: a contended texture-cache or region-tracking spin lock
	SpinLockWaits,  // count
	GpuBusy,        // GPU timestamps of the submitted command buffers
	CommandBuffers, // count
	Count
};

bool Enabled();
void Add(Counter counter, int64_t value);
// Presenter, once per flip, with the time since the previous flip.
void Frame(int64_t frame_ns);
// The GPU thread names itself, so each row has its CPU cycles (its busy time includes waits).
void RegisterGpuThread();

inline int64_t NowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

// Adds the scope's duration to a counter while telemetry is on.
class ScopedTime {
public:
	explicit ScopedTime(Counter counter): m_counter(counter), m_start(Enabled() ? NowNs() : 0) {}
	~ScopedTime() {
		if (m_start != 0) {
			Add(m_counter, NowNs() - m_start);
		}
	}
	ScopedTime(const ScopedTime&)            = delete;
	ScopedTime& operator=(const ScopedTime&) = delete;

private:
	Counter m_counter;
	int64_t m_start;
};

// Takes a lock; when another thread holds it, the wait shows as a "LockWait" zone under the
// caller in Tracy and in the wait counter and the one after it (the count).
template <typename Lock>
void LockCounted(Lock& lock, Counter wait_counter) {
	if (lock.try_lock()) {
		return;
	}
	KYTY_PROFILER_BLOCK("LockWait", profiler::colors::RedA100);
	ScopedTime wait(wait_counter);
	Add(static_cast<Counter>(static_cast<uint32_t>(wait_counter) + 1), 1);
	lock.lock();
}

} // namespace Common::Telemetry

#endif /* KYTY_COMMON_TELEMETRY_H_ */
