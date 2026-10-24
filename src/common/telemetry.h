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
// Times are summed over all threads that pass the site, in nanoseconds (written as ms); the Gpu*
// waits count only on the GPU thread (RegisterGpuThread).
enum class Counter : uint32_t {
	GpuProcess,     // GPU thread: CommandProcessor::Process
	GpuIdle,        // GPU thread: waiting for guest work
	GpuReadMemory,  // GPU thread: its own readbacks
	GpuCopyQueue,   // GPU thread: synchronous readback-queue copies
	FindBuffers,    // GPU thread: RenderExecutor::FindBuffers
	CreateBuffer,   // GPU thread: BufferCache::CreateBuffer (inside FindBuffers)
	BuildKey,       // GPU thread: ProgramCache::BuildKey
	Materialize,    // GPU thread: ProgramCache::MaterializeResources
	GuestFault,     // any thread: RenderContext::HandleFault
	GuestFaults,    // count
	ReadbackWait,   // guest threads: waiting for a readback's GPU work
	ReadbackWaits,  // count
	BufferLockWait, // any thread: the contended buffer-cache range lock
	BufferLockWaits, // count
	MemoryLockWait, // any thread: a contended guest-memory lock (backing store, address space)
	MemoryLockWaits, // count
	RangesLockWait, // any thread: the contended virtual-range table lock (VirtualRanges)
	RangesLockWaits, // count
	SpinLockWait,   // any thread: a contended texture-cache or region-tracking spin lock
	SpinLockWaits,  // count
	MutexWait,      // any thread: a contended Common::Mutex (render context, GPU queues, ...)
	MutexWaits,     // count
	GpuBusy,        // GPU timestamps of the submitted command buffers
	CommandBuffers, // count
	GpuLockWait,    // GPU thread: all of the lock waits above
	GpuLockWaits,   // count
	GpuFenceWait,   // GPU thread: waiting for host GPU work (semaphores, fences)
	GpuCondWait,    // GPU thread: Common::CondVar waits (includes GpuIdle and GpuBlocked)
	GpuOperationWait, // GPU thread: waiting for the scheduler's priority operations
	GpuRecordWait,  // GPU thread: waiting for the recording thread (DrainRecording)
	GpuSubmit,      // GPU thread: vkQueueSubmit
	GpuProtect,     // GPU thread: guest page protection changes (VirtualProtect)
	GpuFault,       // GPU thread: its own page faults
	GpuBlocked,     // GPU thread: waiting with every guest queue blocked (outside Process)
	GpuOnPCore,     // count: Process calls that started on a performance core
	GpuOnECore,     // count: ... on an efficiency core (hybrid CPUs)
	GpuMemoryLockWait, // GPU thread: the MemoryLockWait part of GpuLockWait
	GpuRangesLockWait, // GPU thread: the RangesLockWait part of GpuLockWait
	RangesHold,     // any thread: time a change holds the virtual-range table lock
	RangesChanges,  // count: changes of the virtual-range table
	ClampLocked,    // count: TryClampRangeSize answers computed under the range lock
	ClampRingSaved, // count: cached answers KYTY_CLAMP_RING kept across unrelated changes
	RangesEdit,     // any thread: range-table changes that cut ranges (inside RangesHold)
	RangesEdits,    // count
	RangesSize,     // count: the range table's size at each change (/ RangesChanges = average)
	RangesEvent,    // any thread: RecordRangeEvent, the range history with a stack trace
	Count
};

bool Enabled();
bool OnGpuThread();
void Add(Counter counter, int64_t value);
// A lock wait: the counter, its count (the next counter) and, on the GPU thread, GpuLockWait.
void AddWait(Counter counter, int64_t ns);
// Presenter, once per flip, with the time since the previous flip.
void Frame(int64_t frame_ns);
// The GPU thread names itself, so each row has its CPU cycles (its busy time includes waits)
// and the Gpu* waits count it alone.
void RegisterGpuThread();
// GPU thread, at each Process: whether it runs on a performance or an efficiency core.
void SampleGpuCore();

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

// Adds the scope's duration to a counter when it runs on the GPU thread.
class ScopedGpuTime {
public:
	explicit ScopedGpuTime(Counter counter)
	    : m_counter(counter), m_start(Enabled() && OnGpuThread() ? NowNs() : 0) {}
	~ScopedGpuTime() {
		if (m_start != 0) {
			Add(m_counter, NowNs() - m_start);
		}
	}
	ScopedGpuTime(const ScopedGpuTime&)            = delete;
	ScopedGpuTime& operator=(const ScopedGpuTime&) = delete;

private:
	Counter m_counter;
	int64_t m_start;
};

// Takes a lock; when another thread holds it, the wait shows as a "LockWait" zone under the
// caller in Tracy and in AddWait's counters.
template <typename Lock>
void LockCounted(Lock& lock, Counter wait_counter) {
	if (lock.try_lock()) {
		return;
	}
	KYTY_PROFILER_BLOCK("LockWait", profiler::colors::RedA100);
	const auto start = Enabled() ? NowNs() : 0;
	lock.lock();
	if (start != 0) {
		AddWait(wait_counter, NowNs() - start);
	}
}

} // namespace Common::Telemetry

#endif /* KYTY_COMMON_TELEMETRY_H_ */
