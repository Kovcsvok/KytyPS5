#ifndef KYTY_COMMON_ASSERT_H_
#define KYTY_COMMON_ASSERT_H_

#include "common/common.h"
#include "common/logging/log.h"

#include <cstdlib>
#include <string>
#include <string_view>

namespace Common {

#ifdef __clang__
int DbgExitHandler(char const* file, int line, std::string_view text)
    __attribute__((analyzer_noreturn));
int DbgExitHandler(char const* file, int line, fmt::text_style style, std::string_view text)
    __attribute__((analyzer_noreturn));
int DbgExitIfHandler(char const* expr, char const* file, int line)
    __attribute__((analyzer_noreturn));
int DbgNotImplementedHandler(char const* expr, char const* file, int line)
    __attribute__((analyzer_noreturn));
void DbgExit(int status) __attribute__((analyzer_noreturn));
#else
int  DbgExitHandler(char const* file, int line, std::string_view text);
int  DbgExitHandler(char const* file, int line, fmt::text_style style, std::string_view text);
int  DbgExitIfHandler(char const* expr, char const* file, int line);
int  DbgNotImplementedHandler(char const* expr, char const* file, int line);
void DbgExit(int status);
#endif

std::string HostBacktrace();
// "0x<address> Symbol+0xoff [file:line]" for a host code address (Windows; plain address elsewhere).
std::string HostSymbolize(uint64_t address);
// A thread has waited over a second for a tracking spin lock (regionManager.h): prints the waiter
// and the owner (thread ids, names, host stacks; frames inside the emulator also as exe+offset,
// which the build's lld map resolves). At most one report every 2 s; returns whether it printed.
// done: the wait has ended after waited_ns (printed only after a printed start).
// KYTY_LOCK_WAIT_REPORT=0 (environment) turns it off.
bool ReportLongLockWait(const void* lock, uint32_t owner_thread, bool done, uint64_t waited_ns);

} // namespace Common

#define EXIT_HALT() (Common::DbgExit(321), 1)

#ifndef KYTY_FINAL
#define EXIT_IF(x)                                                                                 \
	((void)((x) && Common::DbgExitIfHandler(#x, __FILE__, __LINE__) != 0 && (EXIT_HALT(), 1) != 0))
#else
#define EXIT_IF(x)                                                                                 \
	do {                                                                                           \
		constexpr bool kyty_exit_if_disabled = false && (x);                                       \
		(void)kyty_exit_if_disabled;                                                               \
	} while (0)
#endif

#define EXIT(...)                                                                                  \
	do {                                                                                           \
		((void)(Common::DbgExitHandler(__FILE__, __LINE__, ::fmt::sprintf(__VA_ARGS__)) &&         \
		        (EXIT_HALT(), 1)));                                                                \
	} while (0)

#define EXIT_COLOR(style, ...)                                                                     \
	do {                                                                                           \
		((void)(Common::DbgExitHandler(__FILE__, __LINE__, (style),                                \
		                               ::fmt::sprintf(__VA_ARGS__)) &&                             \
		        (EXIT_HALT(), 1)));                                                                \
	} while (0)

#define EXIT_NOT_IMPLEMENTED(x)                                                                    \
	((void)((x) && Common::DbgNotImplementedHandler(#x, __FILE__, __LINE__) != 0 &&                \
	        (EXIT_HALT(), 1) != 0))
#define KYTY_NOT_IMPLEMENTED EXIT_NOT_IMPLEMENTED(true)

#endif /* KYTY_COMMON_ASSERT_H_ */
