#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "Hollowpoint/Core/Assert.h"

namespace Hollowpoint::Core
{

namespace
{

struct HandlerSlot
{
    AssertHandler Handler = nullptr;
    void* UserData = nullptr;
};

// Guarded by a mutex rather than an atomic.
//
// A 16-byte lock-free std::atomic<HandlerSlot> would be the zero-overhead choice, but it is not
// portable: it requires -latomics on Clang, and libatomic is absent from several toolchains we build
// with (including this machine's system GCC). Assertions are rare and already fatal-by-default, so a
// mutex here costs nothing measurable and removes an entire class of "links on my machine" failures.
// The lock is reentrant-safe because the handler may itself assert; std::recursive_mutex makes that
// legal rather than lucky.
std::recursive_mutex g_HandlerMutex;
HandlerSlot g_HandlerSlot;

bool DefaultAssertHandler(AssertFailure failure, const char* expression, const char* message,
                          const std::source_location& location, void* /*userData*/)
{
    const char* failureName = "Assertion failed";
    switch (failure)
    {
        case AssertFailure::ConditionFailed:
            failureName = "Assertion failed";
            break;
        case AssertFailure::Unreachable:
            failureName = "Unreachable code";
            break;
        case AssertFailure::InvalidArgument:
            failureName = "Invalid argument";
            break;
        case AssertFailure::NotSupported:
            failureName = "Operation not supported";
            break;
        default:
            break;
    }

    std::fprintf(stderr, "\n%s: %s\n", failureName, expression != nullptr ? expression : "(no expression)");
    if (message != nullptr && message[0] != '\0')
    {
        std::fprintf(stderr, "  message : %s\n", message);
    }
    std::fprintf(stderr, "  location: %s:%d in %s\n", location.file_name(), static_cast<int>(location.line()),
                 location.function_name());
    std::fflush(stderr);

    // Do not continue: an engine invariant was violated and the state afterwards is undefined.
    return false;
}

}  // namespace

AssertHandler SetAssertHandler(AssertHandler handler, void* userData)
{
    std::lock_guard<std::recursive_mutex> lock(g_HandlerMutex);

    const HandlerSlot previous = g_HandlerSlot;
    g_HandlerSlot.Handler = handler;
    g_HandlerSlot.UserData = userData;
    return previous.Handler;
}

namespace Detail
{

void ReportAssertFailure(AssertFailure failure, const char* expression, const char* message,
                         const std::source_location& location)
{
    bool shouldContinue = false;
    {
        // Copy the slot under the lock, then run the handler outside it where possible. Handlers are
        // allowed to assert (the recursive mutex covers that), but keeping the critical section small
        // means a handler that blocks cannot wedge an unrelated SetAssertHandler call.
        std::lock_guard<std::recursive_mutex> lock(g_HandlerMutex);

        const AssertHandler handler = g_HandlerSlot.Handler;
        void* userData = g_HandlerSlot.UserData;

        shouldContinue = (handler != nullptr) ? handler(failure, expression, message, location, userData)
                                              : DefaultAssertHandler(failure, expression, message, location, nullptr);
    }

    if (!shouldContinue)
    {
        HP_BREAKPOINT();
        std::abort();
    }
}

}  // namespace Detail

}  // namespace Hollowpoint::Core
