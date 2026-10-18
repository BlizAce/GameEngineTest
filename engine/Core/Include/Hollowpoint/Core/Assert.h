#pragma once

#include <source_location>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"

namespace Hollowpoint::Core
{

// Severity/kind of an assertion failure. Kept separate from LogLevel so that assertions can be
// routed differently (break into a debugger, show a dialog) than ordinary log messages.
enum class AssertFailure : UInt8
{
    ConditionFailed,  //!< An expression evaluated to false.
    Unreachable,      //!< Control flow reached a branch that must never execute.
    InvalidArgument,  //!< A public API precondition was violated.
    NotSupported      //!< A requested operation is unavailable in this build/platform.
};

// Callback signature for assertion handlers. Return true to let execution continue after the
// failure has been reported (useful in tests and in the editor), false to abort the process.
// Handlers must be reentrant: an assertion can fire while a handler is running.
using AssertHandler = bool (*)(AssertFailure failure, const char* expression, const char* message,
                               const std::source_location& location, void* userData);

// Installs a handler for assertion failures. Passing nullptr restores the default behaviour
// (write to stderr and abort). Returns the previously installed handler. Thread-safe.
HP_CORE_API AssertHandler SetAssertHandler(AssertHandler handler, void* userData = nullptr);

namespace Detail
{
// Reports an assertion failure through the installed handler and aborts unless a handler explicitly
// requests continuation (which tests and the editor do). Not [[noreturn]] on purpose: continuing is
// part of the contract. Never call this directly — use the HP_ASSERT family of macros below.
HP_CORE_API void ReportAssertFailure(AssertFailure failure, const char* expression, const char* message,
                                     const std::source_location& location);
}  // namespace Detail

}  // namespace Hollowpoint::Core

// ---------------------------------------------------------------------------
// Public macros
//
// HP_ASSERT             - internal invariant; reports and (by default) aborts when false.
// HP_ASSERT_MSG         - same, with an extra human-readable message.
// HP_ASSERT_ARGUMENT    - public API precondition; use on parameters of exported entry points.
// HP_UNREACHABLE        - reports that control reached an impossible branch. Like every assertion it
//                         can continue (tests, editor), so it does NOT terminate by itself: put a
//                         return/HP_BREAKPOINT after it if the surrounding code must not fall through.
// HP_VERIFY             - always evaluates the expression (so side effects happen in every build)
//                         but only checks the result when assertions are enabled.
//
// Design note: engine invariants are checked in *all* build configurations, not just debug. A
// corrupted scene or a bad asset must fail loudly at the point of violation rather than silently
// producing garbage three frames later — that is what makes field bugs reproducible. Use
// HP_ASSERT for genuinely free checks and HP_VERIFY where the check itself costs something and the
// expression has side effects. The failure path can be redirected (see SetAssertHandler), so the
// editor can turn a failed invariant into a non-fatal, reported error instead of a crash.
// ---------------------------------------------------------------------------

#define HP_ASSERT(condition)                                                                                        \
    do                                                                                                              \
    {                                                                                                               \
        if (HP_UNLIKELY(!(condition)))                                                                              \
        {                                                                                                           \
            ::Hollowpoint::Core::Detail::ReportAssertFailure(::Hollowpoint::Core::AssertFailure::ConditionFailed,   \
                                                             #condition, nullptr, std::source_location::current()); \
        }                                                                                                           \
    }                                                                                                               \
    while (false)

#define HP_ASSERT_MSG(condition, message)                                                                             \
    do                                                                                                                \
    {                                                                                                                 \
        if (HP_UNLIKELY(!(condition)))                                                                                \
        {                                                                                                             \
            ::Hollowpoint::Core::Detail::ReportAssertFailure(::Hollowpoint::Core::AssertFailure::ConditionFailed,     \
                                                             #condition, (message), std::source_location::current()); \
        }                                                                                                             \
    }                                                                                                                 \
    while (false)

#define HP_ASSERT_ARGUMENT(condition, message)                                                                        \
    do                                                                                                                \
    {                                                                                                                 \
        if (HP_UNLIKELY(!(condition)))                                                                                \
        {                                                                                                             \
            ::Hollowpoint::Core::Detail::ReportAssertFailure(::Hollowpoint::Core::AssertFailure::InvalidArgument,     \
                                                             #condition, (message), std::source_location::current()); \
        }                                                                                                             \
    }                                                                                                                 \
    while (false)

#define HP_UNREACHABLE(message)                                                                                    \
    do                                                                                                             \
    {                                                                                                              \
        ::Hollowpoint::Core::Detail::ReportAssertFailure(::Hollowpoint::Core::AssertFailure::Unreachable, nullptr, \
                                                         (message), std::source_location::current());              \
    }                                                                                                              \
    while (false)

#if defined(HP_DEBUG) && HP_DEBUG
    #define HP_VERIFY(condition) HP_ASSERT(condition)
#else
    #define HP_VERIFY(condition) \
        do                       \
        {                        \
            (void)(condition);   \
        }                        \
        while (false)
#endif
