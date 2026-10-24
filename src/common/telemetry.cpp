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
#endif

namespace Common::Telemetry {

namespace {

constexpr const char* CounterNames[] = {
    "gpu_process_ms",  "gpu_idle_ms",     "gpu_readmem_ms",  "gpu_copyqueue_ms",
    "findbuffers_ms",  "createbuffer_ms", "buildkey_ms",     "materialize_ms",
    "guest_fault_ms",  "guest_faults",    "readback_wait_ms", "readback_waits",
    "buffer_lock_ms",  "buffer_locks",    "memory_lock_ms",  "memory_locks",
    "spin_lock_ms",    "spin_locks",      "gpu_busy_ms",     "cmdbufs",
};
static_assert(std::size(CounterNames) == static_cast<size_t>(Counter::Count));

bool IsCount(size_t index) {
	const auto counter = static_cast<Counter>(index);
	return counter == Counter::GuestFaults || counter == Counter::ReadbackWaits ||
	       counter == Counter::BufferLockWaits || counter == Counter::MemoryLockWaits ||
	       counter == Counter::SpinLockWaits || counter == Counter::CommandBuffers;
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
#endif
};

// Never destroyed: the writer thread may still run while the process exits.
State& GetState() {
	static auto* state = new State;
	return *state;
}

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

// Process CPU time (all threads) and the GPU thread's cycles, for the share of the CPU the
// emulator takes and whether the GPU thread runs or waits.
void SampleCpu(int64_t* process_ns, int64_t* gpu_cycles) {
	*process_ns = 0;
	*gpu_cycles = 0;
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
		*process_ns = (to_100ns(kernel) + to_100ns(user)) * 100;
	}
	ULONG64 cycles = 0;
	if (auto* thread = GetState().gpu_thread.load(std::memory_order_acquire);
	    thread != nullptr && QueryThreadCycleTime(thread, &cycles) != 0) {
		*gpu_cycles = static_cast<int64_t>(cycles);
	}
#endif
}

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
	auto&   state       = GetState();
	FILE*   rows        = nullptr;
	FILE*   frames      = nullptr;
	int64_t last_ns     = 0;
	int64_t last_cpu_ns = 0;
	int64_t last_cycles = 0;
	for (;;) {
		const auto interval = Interval().load(std::memory_order_relaxed);
		if (interval <= 0) {
			last_ns = 0;
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
			                   "gpu_thread_mcycles");
			for (const auto* name: CounterNames) {
				std::fprintf(rows, ",%s", name);
			}
			std::fprintf(rows, "\n");
			std::fprintf(frames, "unix_ms,tag,frame_ms\n");
		}
		if (last_ns == 0) {
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
			last_ns = NowNs();
			SampleCpu(&last_cpu_ns, &last_cycles);
			std::this_thread::sleep_for(std::chrono::milliseconds(interval));
			continue;
		}
		const auto now_ns = NowNs();
		int64_t    cpu_ns = 0;
		int64_t    cycles = 0;
		SampleCpu(&cpu_ns, &cycles);
		std::fprintf(rows, "%lld,%lld,%.1f,%lld,%.2f,%.1f,%.2f",
		             static_cast<long long>(UnixMs()),
		             static_cast<long long>(Tag().load(std::memory_order_relaxed)),
		             static_cast<double>(now_ns - last_ns) / 1e6,
		             static_cast<long long>(state.frames.exchange(0, std::memory_order_relaxed)),
		             static_cast<double>(state.frame_max_ns.exchange(0, std::memory_order_relaxed)) / 1e6,
		             static_cast<double>(cpu_ns - last_cpu_ns) / 1e6,
		             static_cast<double>(cycles - last_cycles) / 1e6);
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
		last_ns     = now_ns;
		last_cpu_ns = cpu_ns;
		last_cycles = cycles;

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

void Add(Counter counter, int64_t value) {
	GetState().counters[static_cast<size_t>(counter)].value.fetch_add(value,
	                                                                  std::memory_order_relaxed);
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
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	HANDLE thread = nullptr;
	if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread,
	                    THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0) != 0) {
		GetState().gpu_thread.store(thread, std::memory_order_release);
	}
#endif
}

} // namespace Common::Telemetry
