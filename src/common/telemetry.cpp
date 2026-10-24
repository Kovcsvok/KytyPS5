#include "common/telemetry.h"

#include "common/common.h"
#include "common/liveSwitches.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h> // IWYU pragma: keep
#include <intrin.h>
#endif

namespace Common::Telemetry {

namespace {

constexpr const char* CounterNames[] = {
    "gpu_process_ms",  "gpu_idle_ms",       "gpu_readmem_ms",   "gpu_copyqueue_ms",
    "findbuffers_ms",  "createbuffer_ms",   "buildkey_ms",      "materialize_ms",
    "guest_fault_ms",  "guest_faults",      "readback_wait_ms", "readback_waits",
    "buffer_lock_ms",  "buffer_locks",      "memory_lock_ms",   "memory_locks",
    "ranges_lock_ms",  "ranges_locks",      "spin_lock_ms",    "spin_locks",        "mutex_ms",         "mutexes",
    "gpu_busy_ms",     "cmdbufs",           "gpu_lock_ms",      "gpu_locks",
    "gpu_fence_ms",    "gpu_cond_ms",       "gpu_operation_ms", "gpu_record_ms",
    "gpu_submit_ms",   "gpu_protect_ms",    "gpu_fault_ms",     "gpu_blocked_ms",
    "gpu_on_pcore",    "gpu_on_ecore",      "gpu_memory_lock_ms", "gpu_ranges_lock_ms",
    "ranges_hold_ms",  "ranges_changes",    "clamp_locked",     "clamp_ring_saved",
};
static_assert(std::size(CounterNames) == static_cast<size_t>(Counter::Count));

bool IsCount(size_t index) {
	switch (static_cast<Counter>(index)) {
		case Counter::GuestFaults:
		case Counter::ReadbackWaits:
		case Counter::BufferLockWaits:
		case Counter::MemoryLockWaits:
		case Counter::RangesLockWaits:
		case Counter::SpinLockWaits:
		case Counter::MutexWaits:
		case Counter::CommandBuffers:
		case Counter::GpuLockWaits:
		case Counter::GpuOnPCore:
		case Counter::GpuOnECore:
		case Counter::RangesChanges:
		case Counter::ClampLocked:
		case Counter::ClampRingSaved: return true;
		default: return false;
	}
}

struct alignas(64) Slot {
	std::atomic<int64_t> value {0};
};

struct FrameRow {
	int64_t unix_ms;
	int64_t tag;
	int64_t frame_ns;
};

struct State {
	std::array<Slot, static_cast<size_t>(Counter::Count)> counters;
	std::atomic<int64_t>                                  frames {0};
	std::atomic<int64_t>                                  frame_max_ns {0};
	std::mutex                                            frame_mutex;
	std::vector<FrameRow>                                 frame_rows;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	std::atomic<HANDLE> gpu_thread {nullptr};
	// Per logical processor (group * 64 + number): 1 = efficiency core, 2 = performance core.
	std::array<uint8_t, 256> core_kind {};
#endif
};

// Never destroyed: the writer thread may still run while the process exits.
State& GetState() {
	static auto* state = new State;
	return *state;
}

thread_local bool t_gpu_thread = false;

std::atomic<int64_t>& Interval() {
	static auto& interval = LiveSwitches::Get("KYTY_TELEMETRY_MS", 0);
	return interval;
}

std::atomic<int64_t>& Tag() {
	static auto& tag = LiveSwitches::Get("KYTY_EXPERIMENT", 0);
	return tag;
}

int64_t UnixMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	           std::chrono::system_clock::now().time_since_epoch())
	    .count();
}

struct CpuSample {
	int64_t process_ns = 0; // CPU time of all threads
	int64_t gpu_cycles = 0; // the GPU thread's cycles (TSC units)
	int64_t tsc        = 0;
	int64_t now_ns     = 0;
};

// Process CPU time (all threads), the GPU thread's cycles and the time-stamp counter, for the
// share of the CPU the emulator takes and how much of its busy time the GPU thread runs.
CpuSample SampleCpu() {
	CpuSample sample;
	sample.now_ns = NowNs();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	FILETIME creation {};
	FILETIME exited {};
	FILETIME kernel {};
	FILETIME user {};
	if (GetProcessTimes(GetCurrentProcess(), &creation, &exited, &kernel, &user) != 0) {
		const auto to_100ns = [](const FILETIME& time) {
			return (static_cast<int64_t>(time.dwHighDateTime) << 32) |
			       static_cast<int64_t>(time.dwLowDateTime);
		};
		sample.process_ns = (to_100ns(kernel) + to_100ns(user)) * 100;
	}
	ULONG64 cycles = 0;
	if (auto* thread = GetState().gpu_thread.load(std::memory_order_acquire);
	    thread != nullptr && QueryThreadCycleTime(thread, &cycles) != 0) {
		sample.gpu_cycles = static_cast<int64_t>(cycles);
	}
	sample.tsc = static_cast<int64_t>(__rdtsc());
#endif
	return sample;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
// Which logical processors are efficiency cores (the lowest EfficiencyClass on a hybrid CPU).
void DetectCoreKinds(std::array<uint8_t, 256>& kinds) {
	DWORD length = 0;
	GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
	std::vector<uint8_t> buffer(length);
	if (length == 0 ||
	    GetLogicalProcessorInformationEx(
	        RelationProcessorCore,
	        reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &length) == 0) {
		return;
	}
	int lowest = 255;
	int highest = 0;
	for (DWORD offset = 0; offset < length;) {
		const auto* info =
		    reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
		const int efficiency = info->Processor.EfficiencyClass;
		lowest               = efficiency < lowest ? efficiency : lowest;
		highest              = efficiency > highest ? efficiency : highest;
		offset += info->Size;
	}
	for (DWORD offset = 0; offset < length;) {
		const auto* info =
		    reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
		const uint8_t kind =
		    (highest != lowest && info->Processor.EfficiencyClass == lowest) ? 1 : 2;
		for (WORD g = 0; g < info->Processor.GroupCount; g++) {
			const auto& group = info->Processor.GroupMask[g];
			for (uint32_t bit = 0; bit < 64; bit++) {
				const auto index = static_cast<uint32_t>(group.Group) * 64 + bit;
				if ((group.Mask & (KAFFINITY {1} << bit)) != 0 && index < kinds.size()) {
					kinds[index] = kind;
				}
			}
		}
		offset += info->Size;
	}
}
#endif

FILE* OpenLog(const char* prefix) {
	std::error_code error;
	std::filesystem::create_directories("logs", error);
	const auto now = std::time(nullptr);
	std::tm    local {};
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif
	char name[96];
	std::snprintf(name, sizeof(name), "logs/%s_%04d-%02d-%02d_%02d%02d%02d.csv", prefix,
	              local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour,
	              local.tm_min, local.tm_sec);
	FILE* file = std::fopen(name, "w");
	if (file != nullptr) {
		std::printf("Telemetry: writing %s\n", name);
		std::fflush(stdout);
	}
	return file;
}

void WriterLoop() {
	auto&     state  = GetState();
	FILE*     rows   = nullptr;
	FILE*     frames = nullptr;
	CpuSample last;
	for (;;) {
		const auto interval = Interval().load(std::memory_order_relaxed);
		if (interval <= 0) {
			last.now_ns = 0;
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			continue;
		}
		if (rows == nullptr) {
			rows   = OpenLog("telemetry");
			frames = OpenLog("frames");
			if (rows == nullptr || frames == nullptr) {
				return;
			}
			std::fprintf(rows, "unix_ms,tag,interval_ms,frames,frame_max_ms,process_cpu_ms,"
			                   "gpu_thread_mcycles,gpu_thread_run_ms");
			for (const auto* name: CounterNames) {
				std::fprintf(rows, ",%s", name);
			}
			std::fprintf(rows, "\n");
			std::fprintf(frames, "unix_ms,tag,frame_ms\n");
		}
		if (last.now_ns == 0) {
			// Start a fresh interval: drop what the counters gathered while telemetry was off.
			for (auto& slot: state.counters) {
				slot.value.exchange(0, std::memory_order_relaxed);
			}
			state.frames.exchange(0, std::memory_order_relaxed);
			state.frame_max_ns.exchange(0, std::memory_order_relaxed);
			{
				std::lock_guard lock(state.frame_mutex);
				state.frame_rows.clear();
			}
			last = SampleCpu();
			std::this_thread::sleep_for(std::chrono::milliseconds(interval));
			continue;
		}
		const auto sample      = SampleCpu();
		const auto interval_ns = sample.now_ns - last.now_ns;
		const auto cycles      = sample.gpu_cycles - last.gpu_cycles;
		// The GPU thread's cycles count at the time-stamp counter's rate.
		const auto tsc_per_ns =
		    interval_ns > 0 ? static_cast<double>(sample.tsc - last.tsc) / static_cast<double>(interval_ns)
		                    : 0.0;
		const double run_ms = tsc_per_ns > 0.0 ? static_cast<double>(cycles) / tsc_per_ns / 1e6 : 0.0;
		std::fprintf(rows, "%lld,%lld,%.1f,%lld,%.2f,%.1f,%.2f,%.2f",
		             static_cast<long long>(UnixMs()),
		             static_cast<long long>(Tag().load(std::memory_order_relaxed)),
		             static_cast<double>(interval_ns) / 1e6,
		             static_cast<long long>(state.frames.exchange(0, std::memory_order_relaxed)),
		             static_cast<double>(state.frame_max_ns.exchange(0, std::memory_order_relaxed)) / 1e6,
		             static_cast<double>(sample.process_ns - last.process_ns) / 1e6,
		             static_cast<double>(cycles) / 1e6, run_ms);
		for (size_t i = 0; i < state.counters.size(); i++) {
			const auto value = state.counters[i].value.exchange(0, std::memory_order_relaxed);
			if (IsCount(i)) {
				std::fprintf(rows, ",%lld", static_cast<long long>(value));
			} else {
				std::fprintf(rows, ",%.3f", static_cast<double>(value) / 1e6);
			}
		}
		std::fprintf(rows, "\n");
		std::fflush(rows);
		last = sample;

		std::vector<FrameRow> frame_rows;
		{
			std::lock_guard lock(state.frame_mutex);
			frame_rows.swap(state.frame_rows);
		}
		for (const auto& row: frame_rows) {
			std::fprintf(frames, "%lld,%lld,%.2f\n", static_cast<long long>(row.unix_ms),
			             static_cast<long long>(row.tag), static_cast<double>(row.frame_ns) / 1e6);
		}
		std::fflush(frames);
		std::this_thread::sleep_for(std::chrono::milliseconds(interval));
	}
}

} // namespace

bool Enabled() {
	return Interval().load(std::memory_order_relaxed) > 0;
}

bool OnGpuThread() {
	return t_gpu_thread;
}

void Add(Counter counter, int64_t value) {
	GetState().counters[static_cast<size_t>(counter)].value.fetch_add(value,
	                                                                  std::memory_order_relaxed);
}

void AddWait(Counter counter, int64_t ns) {
	Add(counter, ns);
	Add(static_cast<Counter>(static_cast<uint32_t>(counter) + 1), 1);
	if (t_gpu_thread) {
		Add(Counter::GpuLockWait, ns);
		Add(Counter::GpuLockWaits, 1);
		if (counter == Counter::MemoryLockWait) {
			Add(Counter::GpuMemoryLockWait, ns);
		} else if (counter == Counter::RangesLockWait) {
			Add(Counter::GpuRangesLockWait, ns);
		}
	}
}

void Frame(int64_t frame_ns) {
	static std::once_flag writer;
	if (!Enabled()) {
		return;
	}
	std::call_once(writer, [] { std::thread(WriterLoop).detach(); });
	auto& state = GetState();
	state.frames.fetch_add(1, std::memory_order_relaxed);
	auto longest = state.frame_max_ns.load(std::memory_order_relaxed);
	while (frame_ns > longest &&
	       !state.frame_max_ns.compare_exchange_weak(longest, frame_ns, std::memory_order_relaxed)) {
	}
	std::lock_guard lock(state.frame_mutex);
	state.frame_rows.push_back({UnixMs(), Tag().load(std::memory_order_relaxed), frame_ns});
}

void RegisterGpuThread() {
	t_gpu_thread = true;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	DetectCoreKinds(GetState().core_kind);
	HANDLE thread = nullptr;
	if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread,
	                    THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0) != 0) {
		GetState().gpu_thread.store(thread, std::memory_order_release);
	}
#endif
}

void SampleGpuCore() {
	if (!Enabled()) {
		return;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	PROCESSOR_NUMBER number {};
	GetCurrentProcessorNumberEx(&number);
	const auto index = static_cast<uint32_t>(number.Group) * 64 + number.Number;
	if (index < 256 && GetState().core_kind[index] == 1) {
		Add(Counter::GpuOnECore, 1);
	} else {
		Add(Counter::GpuOnPCore, 1);
	}
#endif
}

} // namespace Common::Telemetry
