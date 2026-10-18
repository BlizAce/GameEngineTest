#pragma once

#include <chrono>
#include <string_view>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Format.h"
#include "Hollowpoint/Core/Types.h"

namespace Hollowpoint::Core
{

// Verbosity levels, ordered from most to least severe. Trace is compiled out of shipping builds by
// default (see HP_ENABLE_TRACE_LOGS) because trace call sites are numerous and hot.
enum class LogLevel : UInt8
{
    Fatal = 0,
    Error = 1,
    Warning = 2,
    Info = 3,
    Debug = 4,
    Trace = 5
};

// Converts a level to its short upper-case tag ("[INFO ]"). Returns an empty string for invalid values.
HP_CORE_API std::string_view LogLevelToTag(LogLevel level);

// Parses a level name case-insensitively ("info", "WARNING", ...). Returns false when unknown.
HP_CORE_API bool ParseLogLevel(std::string_view name, LogLevel& outLevel);

// Sink interface: implement to route log output somewhere specific (file, editor console, test
// capture). Sinks are called from arbitrary threads and must be thread-safe.
class ILogSink
{
public:
    virtual ~ILogSink() = default;

    // Return false to let the logger skip formatting the message entirely (cheap filtering).
    virtual bool Accepts(LogLevel level) const = 0;

    // Writes one already-formatted log line.
    // Parameters are passed separately (rather than pre-formatted into one string) so that sinks
    // such as an editor console can colourise or filter on the individual fields.
    virtual void Write(std::chrono::system_clock::time_point timestamp, LogLevel level, std::string_view channel,
                       std::string_view message) = 0;

    // Flush any buffered output. Called on shutdown and after Fatal messages.
    virtual void Flush() {}
};

// Registers a sink. Ownership stays with the caller; the sink must outlive the logger or be removed
// first. Registering the same pointer twice is an error (asserted). Returns false in that case.
HP_CORE_API bool AttachLogSink(ILogSink* sink);

// Removes a previously registered sink. Safe to call with a null pointer or an unknown sink.
HP_CORE_API void DetachLogSink(ILogSink* sink);

// Sets the maximum level that will be dispatched to any sink. Messages above it are dropped before
// formatting, so raising this level at runtime is free. Default: LogLevel::Info.
HP_CORE_API void SetMaxLogLevel(LogLevel level);
HP_CORE_API LogLevel GetMaxLogLevel();

// Convenience: creates and registers a sink writing colourised lines to stdout (or stderr for
// Error/Fatal). Returns the sink pointer so it can be detached; mostly used by applications.
HP_CORE_API ILogSink* AttachStandardLogSink(LogLevel level = LogLevel::Info);

// Creates and registers a sink appending plain text lines to a file. Returns nullptr when the file
// cannot be opened. The logger keeps the sink alive for the process lifetime (so static destructors
// can still log); detach it with DetachLogSink() to stop writing.
HP_CORE_API ILogSink* AttachFileLogSink(std::string_view path, LogLevel level = LogLevel::Info);

// Flushes all sinks. Called automatically after Fatal messages and by ShutdownLogger().
HP_CORE_API void FlushLogs();

// Removes all sinks and flushes them. Called from Engine::Shutdown; applications rarely need it.
HP_CORE_API void ShutdownLogger();

namespace Detail
{

// Logger state used by the HP_LOG macros. Not part of the stable API — call the free functions above.
class HP_CORE_API LogDispatcher
{
public:
    // True when a message at this level would reach at least one sink. Used to skip formatting.
    static bool ShouldLog(LogLevel level);

    // Formats and dispatches one message. `file` is truncated to its file name for readability.
    static void Write(LogLevel level, std::string_view channel, std::string_view file, Int32 line,
                      std::string_view message);
};

}  // namespace Detail

}  // namespace Hollowpoint::Core

// ---------------------------------------------------------------------------
// Logging macros
//
// HP_LOG_<LEVEL>(channel, "format string")                    - no arguments
// HP_LOG_<LEVEL>(channel, FMT("loaded {} assets", count))     - formatted message
//
// Formatting uses the standard format library (see Format.h). The channel is a short subsystem
// tag ("RENDER", "PHYSICS") used for filtering.
// ---------------------------------------------------------------------------

#define HP_LOG_AT(level, channel, ...)                                                                   \
    do                                                                                                   \
    {                                                                                                    \
        if (::Hollowpoint::Core::Detail::LogDispatcher::ShouldLog(level))                                \
        {                                                                                                \
            ::Hollowpoint::Core::Detail::LogDispatcher::Write(level, channel, __FILE__, __LINE__,        \
                                                              ::Hollowpoint::Core::Format(__VA_ARGS__)); \
        }                                                                                                \
    }                                                                                                    \
    while (false)

#if defined(HP_ENABLE_TRACE_LOGS) && HP_ENABLE_TRACE_LOGS
    #define HP_LOG_TRACE(channel, ...) HP_LOG_AT(::Hollowpoint::Core::LogLevel::Trace, channel, __VA_ARGS__)
#else
    #define HP_LOG_TRACE(channel, ...) \
        do                             \
        {                              \
        }                              \
        while (false)
#endif

#define HP_LOG_DEBUG(channel, ...) HP_LOG_AT(::Hollowpoint::Core::LogLevel::Debug, channel, __VA_ARGS__)
#define HP_LOG_INFO(channel, ...) HP_LOG_AT(::Hollowpoint::Core::LogLevel::Info, channel, __VA_ARGS__)
#define HP_LOG_WARNING(channel, ...) HP_LOG_AT(::Hollowpoint::Core::LogLevel::Warning, channel, __VA_ARGS__)
#define HP_LOG_ERROR(channel, ...) HP_LOG_AT(::Hollowpoint::Core::LogLevel::Error, channel, __VA_ARGS__)
#define HP_LOG_FATAL(channel, ...) HP_LOG_AT(::Hollowpoint::Core::LogLevel::Fatal, channel, __VA_ARGS__)