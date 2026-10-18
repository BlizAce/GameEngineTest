#include "TestSupport.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Core/Log.h"

using namespace Hollowpoint::Core;
using Hollowpoint::Test::ScopedAssertCapture;

namespace
{

// Sink that records everything written to it, so tests can assert on formatted output.
class RecordingSink final : public ILogSink
{
public:
    explicit RecordingSink(LogLevel maxLevel = LogLevel::Trace) : m_MaxLevel(maxLevel) {}

    bool Accepts(LogLevel level) const override
    {
        return level <= m_MaxLevel;
    }

    void Write(std::chrono::system_clock::time_point timestamp, LogLevel level, std::string_view channel,
               std::string_view message) override
    {
        static_cast<void>(timestamp);
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Records.push_back(Record{level, std::string(channel), std::string(message)});
    }

    void Flush() override
    {
        ++m_FlushCount;
    }

    [[nodiscard]] std::size_t GetRecordCount() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Records.size();
    }

    [[nodiscard]] std::vector<std::string> GetMessages() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        std::vector<std::string> messages;
        messages.reserve(m_Records.size());
        for (const Record& record : m_Records)
        {
            messages.push_back(record.Message);
        }
        return messages;
    }

    [[nodiscard]] bool Contains(std::string_view needle) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (const Record& record : m_Records)
        {
            if (record.Message.find(needle) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] int GetFlushCount() const
    {
        return m_FlushCount;
    }
    void ClearRecords()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Records.clear();
    }

private:
    struct Record
    {
        LogLevel Level;
        std::string Channel;
        std::string Message;
    };

    mutable std::mutex m_Mutex;
    std::vector<Record> m_Records;
    int m_FlushCount = 0;
    LogLevel m_MaxLevel = LogLevel::Trace;
};

// Restores the global logger configuration so tests cannot leak state into each other.
class LoggerScope
{
public:
    explicit LoggerScope(ILogSink* sink, LogLevel level = LogLevel::Trace)
        : m_Sink(sink), m_PreviousLevel(GetMaxLogLevel())
    {
        AttachLogSink(sink);
        SetMaxLogLevel(level);
    }

    ~LoggerScope()
    {
        DetachLogSink(m_Sink);
        SetMaxLogLevel(m_PreviousLevel);
    }

    LoggerScope(const LoggerScope&) = delete;
    LoggerScope& operator=(const LoggerScope&) = delete;

private:
    ILogSink* m_Sink = nullptr;
    LogLevel m_PreviousLevel = LogLevel::Info;
};

}  // namespace

TEST_SUITE("Core/Log")
{
    TEST_CASE("level tags and parsing round-trip")
    {
        CHECK(LogLevelToTag(LogLevel::Fatal) == "FATAL");
        CHECK(LogLevelToTag(LogLevel::Warning) == "WARN ");
        CHECK(LogLevelToTag(static_cast<LogLevel>(99)).empty());

        LogLevel parsed = LogLevel::Info;
        CHECK(ParseLogLevel("error", parsed));
        CHECK(parsed == LogLevel::Error);
        CHECK(ParseLogLevel("WARNING", parsed));
        CHECK(parsed == LogLevel::Warning);
        CHECK(ParseLogLevel("warn", parsed));
        CHECK(parsed == LogLevel::Warning);
        CHECK_FALSE(ParseLogLevel("verbose", parsed));
    }

    TEST_CASE("messages reach an attached sink with channel and formatting intact")
    {
        RecordingSink sink;
        LoggerScope scope(&sink);

        HP_LOG_INFO("TESTS", "loaded {} assets in {:.2f}s", 12, 0.5);

        REQUIRE_EQ(sink.GetRecordCount(), 1u);
        const std::vector<std::string> messages = sink.GetMessages();
        CHECK(messages[0] == "loaded 12 assets in 0.50s");
    }

    TEST_CASE("messages above the maximum level are dropped before formatting")
    {
        RecordingSink sink;
        LoggerScope scope(&sink, LogLevel::Warning);

        HP_LOG_INFO("TESTS", "not visible");
        HP_LOG_DEBUG("TESTS", "definitely not visible");
        CHECK_EQ(sink.GetRecordCount(), 0u);

        HP_LOG_WARNING("TESTS", "visible");
        HP_LOG_ERROR("TESTS", "also visible");
        CHECK_EQ(sink.GetRecordCount(), 2u);
    }

    TEST_CASE("a sink with a lower ceiling filters independently")
    {
        RecordingSink errorsOnly(LogLevel::Error);
        LoggerScope scope(&errorsOnly, LogLevel::Trace);

        HP_LOG_INFO("TESTS", "ignored by the sink");
        HP_LOG_ERROR("TESTS", "kept");

        CHECK_EQ(errorsOnly.GetRecordCount(), 1u);
        CHECK(errorsOnly.Contains("kept"));
    }

    TEST_CASE("detaching a sink stops delivery")
    {
        RecordingSink sink;
        AttachLogSink(&sink);
        SetMaxLogLevel(LogLevel::Trace);

        HP_LOG_INFO("TESTS", "first");
        DetachLogSink(&sink);
        HP_LOG_INFO("TESTS", "second");

        CHECK_EQ(sink.GetRecordCount(), 1u);
    }

    TEST_CASE("attaching the same sink twice is rejected")
    {
        RecordingSink sink;
        ScopedAssertCapture capture;

        CHECK(AttachLogSink(&sink));
        CHECK_FALSE(AttachLogSink(&sink));
        CHECK(capture.GetCaptureCount() == 1);

        DetachLogSink(&sink);
    }

    TEST_CASE("detaching an unattached or null sink is harmless")
    {
        DetachLogSink(nullptr);
        RecordingSink sink;
        DetachLogSink(&sink);  // never attached
        CHECK(true);
    }

    TEST_CASE("fatal messages flush every sink")
    {
        RecordingSink sink;
        LoggerScope scope(&sink);

        const int flushesBefore = sink.GetFlushCount();
        HP_LOG_FATAL("TESTS", "the renderer is gone");

        CHECK(sink.Contains("the renderer is gone"));
        CHECK(sink.GetFlushCount() > flushesBefore);
    }

    TEST_CASE("debug messages carry the source location")
    {
        RecordingSink sink;
        LoggerScope scope(&sink);

        HP_LOG_DEBUG("TESTS", "with location");  // NOLINT(readability-simplify-boolean-expr)

        REQUIRE_EQ(sink.GetRecordCount(), 1u);
        const std::string message = sink.GetMessages()[0];
        CHECK(message.find("TestLog.cpp:") != std::string::npos);
    }

    TEST_CASE("logging is safe from multiple threads")
    {
        RecordingSink sink;
        LoggerScope scope(&sink);

        constexpr int ThreadCount = 6;
        constexpr int MessagesPerThread = 200;

        std::vector<std::thread> threads;
        threads.reserve(ThreadCount);
        for (int threadIndex = 0; threadIndex < ThreadCount; ++threadIndex)
        {
            threads.emplace_back([threadIndex] {
                for (int messageIndex = 0; messageIndex < MessagesPerThread; ++messageIndex)
                {
                    HP_LOG_INFO("THREAD", "thread {} message {}", threadIndex, messageIndex);
                }
            });
        }
        for (std::thread& thread : threads)
        {
            thread.join();
        }

        CHECK_EQ(sink.GetRecordCount(), static_cast<std::size_t>(ThreadCount * MessagesPerThread));
    }

    TEST_CASE("a file sink writes to disk")
    {
        const std::string path = (std::filesystem::temp_directory_path() / "hollowpoint_log_sink_test.log").string();
        std::filesystem::remove(path);

        ILogSink* sink = AttachFileLogSink(path, LogLevel::Info);
        REQUIRE(sink != nullptr);

        const LogLevel previousLevel = GetMaxLogLevel();
        SetMaxLogLevel(LogLevel::Info);
        HP_LOG_INFO("FILES", "written to disk");
        FlushLogs();
        SetMaxLogLevel(previousLevel);

        std::ifstream file(path);
        REQUIRE(file.is_open());
        std::stringstream contents;
        contents << file.rdbuf();
        CHECK(contents.str().find("written to disk") != std::string::npos);
        CHECK(contents.str().find("[FILES]") != std::string::npos);

        DetachLogSink(sink);
        std::filesystem::remove(path);
    }

    TEST_CASE("attaching a file sink with an unusable path fails cleanly")
    {
        CHECK_FALSE(AttachFileLogSink("", LogLevel::Info));
        CHECK_FALSE(AttachFileLogSink("/nonexistent-directory-for-hollowpoint-test/file.log", LogLevel::Info));
    }
}
