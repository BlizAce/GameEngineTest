#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Log.h"

namespace Hollowpoint::Core
{

namespace
{

constexpr std::size_t MaxSinkCount = 16;

// Logger state lives in one translation unit and is guarded by a recursive mutex so that logging
// from inside a sink (a common debugging habit) does not deadlock.
struct LoggerState
{
    std::recursive_mutex Mutex;
    std::array<ILogSink*, MaxSinkCount> Sinks{};
    std::size_t SinkCount = 0;
    std::atomic<UInt8> MaxLevel{static_cast<UInt8>(LogLevel::Info)};
};

LoggerState& GetState()
{
    static LoggerState state;
    return state;
}

//! Strips the directory from a __FILE__ path, returning a view into the caller's buffer.
std::string_view BaseFileName(std::string_view path)
{
    const std::size_t separator = path.find_last_of("/\\");
    return separator == std::string_view::npos ? path : path.substr(separator + 1);
}

class StandardStreamSink final : public ILogSink
{
public:
    explicit StandardStreamSink(LogLevel level) : m_Level(level) {}

    bool Accepts(LogLevel level) const override
    {
        return level <= m_Level;
    }

    void Write(std::chrono::system_clock::time_point timestamp, LogLevel level, std::string_view channel,
               std::string_view message) override
    {
        // Errors go to stderr so they stay visible even when stdout is redirected to a log file.
        std::FILE* stream = (level <= LogLevel::Error) ? stderr : stdout;

        const std::time_t time = std::chrono::system_clock::to_time_t(timestamp);
        std::tm localTime{};
#if defined(_WIN32)
        localtime_s(&localTime, &time);
#else
        localtime_r(&time, &localTime);
#endif
        char timestampText[16];
        std::strftime(timestampText, sizeof(timestampText), "%H:%M:%S", &localTime);

        std::fprintf(stream, "%s %.*s %.*s %.*s\n", timestampText, static_cast<int>(LogLevelToTag(level).size()),
                     LogLevelToTag(level).data(), static_cast<int>(channel.size()), channel.data(),
                     static_cast<int>(message.size()), message.data());
    }

    void Flush() override
    {
        std::fflush(stdout);
        std::fflush(stderr);
    }

private:
    LogLevel m_Level;
};

class FileSink final : public ILogSink
{
public:
    FileSink(std::FILE* file, LogLevel level) : m_File(file), m_Level(level) {}

    ~FileSink() override
    {
        if (m_File != nullptr)
        {
            std::fclose(m_File);
        }
    }

    bool Accepts(LogLevel level) const override
    {
        return level <= m_Level;
    }

    void Write(std::chrono::system_clock::time_point timestamp, LogLevel level, std::string_view channel,
               std::string_view message) override
    {
        if (m_File == nullptr)
        {
            return;
        }
        const std::time_t time = std::chrono::system_clock::to_time_t(timestamp);
        std::tm localTime{};
#if defined(_WIN32)
        localtime_s(&localTime, &time);
#else
        localtime_r(&time, &localTime);
#endif
        char timestampText[24];
        std::strftime(timestampText, sizeof(timestampText), "%Y-%m-%d %H:%M:%S", &localTime);

        std::fprintf(m_File, "%s [%.*s] [%.*s] %.*s\n", timestampText, static_cast<int>(LogLevelToTag(level).size()),
                     LogLevelToTag(level).data(), static_cast<int>(channel.size()), channel.data(),
                     static_cast<int>(message.size()), message.data());
    }

    void Flush() override
    {
        if (m_File != nullptr)
        {
            std::fflush(m_File);
        }
    }

private:
    std::FILE* m_File = nullptr;
    LogLevel m_Level = LogLevel::Info;
};

}  // namespace

std::string_view LogLevelToTag(LogLevel level)
{
    switch (level)
    {
        case LogLevel::Fatal:
            return "FATAL";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Warning:
            return "WARN ";
        case LogLevel::Info:
            return "INFO ";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Trace:
            return "TRACE";
        default:
            return {};
    }
}

bool ParseLogLevel(std::string_view name, LogLevel& outLevel)
{
    auto equalsIgnoreCase = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size())
        {
            return false;
        }
        for (std::size_t index = 0; index < a.size(); ++index)
        {
            const char left = (a[index] >= 'A' && a[index] <= 'Z') ? static_cast<char>(a[index] + 32) : a[index];
            const char right = (b[index] >= 'A' && b[index] <= 'Z') ? static_cast<char>(b[index] + 32) : b[index];
            if (left != right)
            {
                return false;
            }
        }
        return true;
    };

    if (equalsIgnoreCase(name, "fatal"))
    {
        outLevel = LogLevel::Fatal;
        return true;
    }
    if (equalsIgnoreCase(name, "error"))
    {
        outLevel = LogLevel::Error;
        return true;
    }
    if (equalsIgnoreCase(name, "warning") || equalsIgnoreCase(name, "warn"))
    {
        outLevel = LogLevel::Warning;
        return true;
    }
    if (equalsIgnoreCase(name, "info"))
    {
        outLevel = LogLevel::Info;
        return true;
    }
    if (equalsIgnoreCase(name, "debug"))
    {
        outLevel = LogLevel::Debug;
        return true;
    }
    if (equalsIgnoreCase(name, "trace"))
    {
        outLevel = LogLevel::Trace;
        return true;
    }
    return false;
}

bool AttachLogSink(ILogSink* sink)
{
    if (sink == nullptr)
    {
        return false;
    }

    LoggerState& state = GetState();
    std::lock_guard<std::recursive_mutex> lock(state.Mutex);

    for (std::size_t index = 0; index < state.SinkCount; ++index)
    {
        if (state.Sinks[index] == sink)
        {
            HP_ASSERT_MSG(false, "AttachLogSink: sink is already attached");
            return false;
        }
    }

    if (state.SinkCount >= MaxSinkCount)
    {
        HP_ASSERT_MSG(false, "AttachLogSink: too many sinks attached");
        return false;
    }

    state.Sinks[state.SinkCount++] = sink;
    return true;
}

void DetachLogSink(ILogSink* sink)
{
    if (sink == nullptr)
    {
        return;
    }

    LoggerState& state = GetState();
    std::lock_guard<std::recursive_mutex> lock(state.Mutex);

    for (std::size_t index = 0; index < state.SinkCount; ++index)
    {
        if (state.Sinks[index] == sink)
        {
            state.Sinks[index] = state.Sinks[state.SinkCount - 1];
            state.Sinks[state.SinkCount - 1] = nullptr;
            --state.SinkCount;
            return;
        }
    }
}

void SetMaxLogLevel(LogLevel level)
{
    GetState().MaxLevel.store(static_cast<UInt8>(level), std::memory_order_relaxed);
}

LogLevel GetMaxLogLevel()
{
    return static_cast<LogLevel>(GetState().MaxLevel.load(std::memory_order_relaxed));
}

ILogSink* AttachStandardLogSink(LogLevel level)
{
    // Intentionally leaked: the sink must outlive every static destructor that might log.
    static StandardStreamSink* s_Sink = new StandardStreamSink(level);
    AttachLogSink(s_Sink);
    return s_Sink;
}

ILogSink* AttachFileLogSink(std::string_view path, LogLevel level)
{
    if (path.empty())
    {
        return nullptr;
    }

    const std::string pathString(path);
    std::FILE* file = nullptr;
#if defined(_WIN32)
    fopen_s(&file, pathString.c_str(), "a");
#else
    file = std::fopen(pathString.c_str(), "ae");
#endif
    if (file == nullptr)
    {
        return nullptr;
    }

    // The sink is intentionally kept for the process lifetime so that logging can never write to a
    // closed FILE*; callers stop delivery with DetachLogSink().
    static std::vector<FileSink*>& s_Owned = *(new std::vector<FileSink*>());
    FileSink* sink = new FileSink(file, level);
    s_Owned.push_back(sink);
    if (!AttachLogSink(sink))
    {
        return nullptr;
    }
    return sink;
}

void FlushLogs()
{
    LoggerState& state = GetState();
    std::lock_guard<std::recursive_mutex> lock(state.Mutex);
    for (std::size_t index = 0; index < state.SinkCount; ++index)
    {
        if (state.Sinks[index] != nullptr)
        {
            state.Sinks[index]->Flush();
        }
    }
}

void ShutdownLogger()
{
    LoggerState& state = GetState();
    std::lock_guard<std::recursive_mutex> lock(state.Mutex);
    for (std::size_t index = 0; index < state.SinkCount; ++index)
    {
        if (state.Sinks[index] != nullptr)
        {
            state.Sinks[index]->Flush();
        }
    }
    state.SinkCount = 0;
    state.Sinks.fill(nullptr);
}

namespace Detail
{

bool LogDispatcher::ShouldLog(LogLevel level)
{
    return static_cast<UInt8>(level) <= GetState().MaxLevel.load(std::memory_order_relaxed);
}

void LogDispatcher::Write(LogLevel level, std::string_view channel, std::string_view file, Int32 line,
                          std::string_view message)
{
    LoggerState& state = GetState();
    std::lock_guard<std::recursive_mutex> lock(state.Mutex);

    // Sinks receive the message text; the source location is appended for verbose levels only, so
    // Info-and-above lines stay readable in a console and cheap to emit.
    std::string fullMessage(message);
    if (level >= LogLevel::Debug)
    {
        fullMessage += Format(" ({}:{})", BaseFileName(file), line);
    }

    const auto now = std::chrono::system_clock::now();
    for (std::size_t index = 0; index < state.SinkCount; ++index)
    {
        ILogSink* sink = state.Sinks[index];
        if (sink != nullptr && sink->Accepts(level))
        {
            sink->Write(now, level, channel, fullMessage);
        }
    }

    // A fatal message must survive an abnormal exit, so flush unconditionally.
    if (level <= LogLevel::Fatal)
    {
        for (std::size_t index = 0; index < state.SinkCount; ++index)
        {
            if (state.Sinks[index] != nullptr)
            {
                state.Sinks[index]->Flush();
            }
        }
    }
}

}  // namespace Detail

}  // namespace Hollowpoint::Core
