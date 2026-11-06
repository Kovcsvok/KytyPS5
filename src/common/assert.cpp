#include "common/assert.h"

#include "common/logging/log.h"
#include "common/subsystems.h"
#include "kytyGitVersion.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#endif

namespace Common {

#ifdef _WIN32
static bool SymbolsInitialized() {
	const auto process = GetCurrentProcess();
	char       exe_path[MAX_PATH] {};
	GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
	std::string search = exe_path;
	if (const auto slash = search.find_last_of("\/"); slash != std::string::npos) {
		search.resize(slash);
		std::string parent = search;
		if (const auto up = parent.find_last_of("\/"); up != std::string::npos) {
			parent.resize(up);
			search += ";" + parent;
		}
	}
	SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
	return SymInitialize(process, search.c_str(), TRUE) != FALSE;
}

static std::mutex& SymbolMutex() {
	static std::mutex symbol_mutex;
	return symbol_mutex;
}

// Caller holds SymbolMutex().
static std::string SymbolizeUnlocked(uint64_t address) {
	static const bool initialized = SymbolsInitialized();
	std::string       out         = fmt::format("0x{:016x}", address);
	if (!initialized) {
		return out;
	}
	const auto process = GetCurrentProcess();
	alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] {};
	auto*                     symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
	symbol->SizeOfStruct             = sizeof(SYMBOL_INFO);
	symbol->MaxNameLen               = 512;
	DWORD64 displacement             = 0;
	if (SymFromAddr(process, address, &displacement, symbol) != FALSE) {
		out += fmt::format(" {}+0x{:x}", symbol->Name, displacement);
	}
	IMAGEHLP_LINE64 line {};
	line.SizeOfStruct = sizeof(line);
	DWORD line_displacement = 0;
	if (SymGetLineFromAddr64(process, address, &line_displacement, &line) != FALSE) {
		out += fmt::format(" [{}:{}]", line.FileName, line.LineNumber);
	}
	return out;
}
#endif

std::string HostSymbolize(uint64_t address) {
#ifdef _WIN32
	std::scoped_lock symbol_lock(SymbolMutex());
	return SymbolizeUnlocked(address);
#else
	return fmt::format("0x{:016x}", address);
#endif
}

std::string HostBacktrace() {
#ifdef _WIN32
	static constexpr ULONG MaxFrames = 48;
	void*                  frames[MaxFrames];
	const auto             count = CaptureStackBackTrace(1, MaxFrames, frames, nullptr);
	if (count == 0) {
		return {};
	}

	std::scoped_lock symbol_lock(SymbolMutex());
	std::string      out = "--- Host stack ---\n";
	for (ULONG i = 0; i < count; i++) {
		out += fmt::format("  #{:<2} {}\n", i,
		                   SymbolizeUnlocked(reinterpret_cast<uint64_t>(frames[i])));
	}
	return out;
#else
	return {};
#endif
}

#ifdef _WIN32
// The thread's name (SetThreadDescription), empty without one. Looked up at run time: older SDKs
// do not declare GetThreadDescription.
static std::string HostThreadName(HANDLE thread) {
	using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
	static const auto get_description = reinterpret_cast<GetThreadDescriptionFn>(
	    GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
	std::string out;
	PWSTR       name = nullptr;
	if (thread == nullptr || get_description == nullptr || FAILED(get_description(thread, &name)) ||
	    name == nullptr) {
		return out;
	}
	if (const int size = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
	    size > 1) {
		out.resize(static_cast<size_t>(size - 1));
		WideCharToMultiByte(CP_UTF8, 0, name, -1, out.data(), size, nullptr, nullptr);
	}
	LocalFree(name);
	return out;
}

// Another thread's return addresses, unwound while it is suspended (nothing allocates meanwhile).
// Stops at the first frame without unwind data after the top one (guest or generated code).
static size_t CaptureThreadFrames(HANDLE thread, uint64_t* frames, size_t max) {
#if defined(_M_X64) || defined(__x86_64__)
	if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
		return 0;
	}
	size_t  count = 0;
	CONTEXT context {};
	context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
	if (GetThreadContext(thread, &context) != FALSE) {
		while (count < max && context.Rip != 0) {
			frames[count++] = context.Rip;
			DWORD64 image_base = 0;
			auto*   entry      = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
			if (entry == nullptr) {
				if (count > 1) {
					break;
				}
				// A leaf function: its return address is on top of the stack.
				context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
				context.Rsp += 8;
				continue;
			}
			void*   handler_data = nullptr;
			DWORD64 establisher  = 0;
			RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, entry, &context,
			                 &handler_data, &establisher, nullptr);
		}
	}
	ResumeThread(thread);
	return count;
#else
	return 0;
#endif
}

static std::string FormatHostFrames(const uint64_t* frames, size_t count) {
	const auto  exe = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
	std::string out;
	for (size_t i = 0; i < count; i++) {
		MEMORY_BASIC_INFORMATION info {};
		const bool in_exe = VirtualQuery(reinterpret_cast<const void*>(frames[i]), &info, sizeof(info)) != 0 &&
		                    reinterpret_cast<uint64_t>(info.AllocationBase) == exe;
		out += fmt::format("  #{:<2} {}{}\n", i, HostSymbolize(frames[i]),
		                   in_exe ? fmt::format(" (exe+0x{:x})", frames[i] - exe) : std::string());
	}
	return out;
}
#endif

bool ReportLongLockWait(const void* lock, uint32_t owner_thread, bool done, uint64_t waited_ns) {
#ifdef _WIN32
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_LOCK_WAIT_REPORT");
		return value == nullptr || value[0] != '0';
	}();
	if (!enabled) {
		return false;
	}
	const auto waiter = GetCurrentThreadId();
	if (done) {
		::printf("Long lock wait: thread %lu got tracking lock %p after %.0f ms\n",
		         static_cast<unsigned long>(waiter), lock, static_cast<double>(waited_ns) / 1e6);
		std::fflush(stdout);
		return true;
	}
	static std::atomic<int64_t> last_report {0};
	const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
	                     std::chrono::steady_clock::now().time_since_epoch())
	                     .count();
	auto last = last_report.load(std::memory_order_relaxed);
	if (now - last < 2000 || !last_report.compare_exchange_strong(last, now)) {
		return false;
	}
	static constexpr size_t MaxFrames = 48;
	uint64_t                owner_frames[MaxFrames];
	size_t                  owner_count = 0;
	std::string             owner_name;
	if (owner_thread != 0 && owner_thread != waiter) {
		if (HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
		                                   THREAD_QUERY_LIMITED_INFORMATION,
		                               FALSE, owner_thread);
		    thread != nullptr) {
			owner_count = CaptureThreadFrames(thread, owner_frames, MaxFrames);
			owner_name  = HostThreadName(thread);
			CloseHandle(thread);
		}
	}
	void*      waiter_frames[MaxFrames];
	const auto waiter_count = CaptureStackBackTrace(1, MaxFrames, waiter_frames, nullptr);
	uint64_t   waiter_addresses[MaxFrames];
	for (USHORT i = 0; i < waiter_count; i++) {
		waiter_addresses[i] = reinterpret_cast<uint64_t>(waiter_frames[i]);
	}
	const auto text = fmt::format(
	    "Long lock wait: thread {} ({}) has waited {:.0f} ms for tracking lock {}, owned by thread {} "
	    "({})\n--- waiter stack ---\n{}--- owner stack ---\n{}",
	    waiter, HostThreadName(GetCurrentThread()), static_cast<double>(waited_ns) / 1e6, lock,
	    owner_thread, owner_name, FormatHostFrames(waiter_addresses, waiter_count),
	    owner_count != 0 ? FormatHostFrames(owner_frames, owner_count) : std::string("  (none)\n"));
	::printf("%s", text.c_str());
	std::fflush(stdout);
	return true;
#else
	(void)lock;
	(void)owner_thread;
	(void)done;
	(void)waited_ns;
	return false;
#endif
}

static std::string BuildFatalReport(const char* title, std::string_view text, const char* file,
                                    int line) {
	return fmt::format("--- Build ---\n{}\n{}\n{} in {}:{}\n{}", KYTY_BUILD_LABEL, title, text, file,
	                   line, HostBacktrace());
}

static int DbgReport(const char* title, std::string_view text, const char* file, int line) {
	Log::WriteFatal(BuildFatalReport(title, text, file, line));
	Subsystems::EmergencyShutdownActive();
	return 1;
}

int DbgExitIfHandler(const char* expr, const char* file, int line) {
	return DbgReport("--- Fatal Error ---", fmt::format("Error: condition ({}) is true", expr),
	                 file, line);
}

int DbgNotImplementedHandler(const char* expr, const char* file, int line) {
	return DbgReport("--- Fatal Error ---", fmt::format("Not implemented ({})", expr), file, line);
}

int DbgExitHandler(const char* file, int line, std::string_view text) {
	Log::WriteFatal(BuildFatalReport("--- Error ---", text, file, line));
	return 1;
}

int DbgExitHandler(const char* file, int line, fmt::text_style style, std::string_view text) {
	Log::WriteFatal(style, BuildFatalReport("--- Error ---", text, file, line));
	return 1;
}

void DbgExit(int status) {
	Subsystems::EmergencyShutdownActive();
	std::fflush(nullptr);
	std::_Exit(status);
}

} // namespace Common
