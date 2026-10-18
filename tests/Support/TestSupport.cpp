#include "TestSupport.h"

#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>

#include "Hollowpoint/Core/Log.h"

#ifndef HP_DATA_DIR
    #define HP_DATA_DIR "."
#endif

namespace Hollowpoint::Test
{

namespace
{

// The innermost capture scope on this thread. Assertions are attributed thread-locally so that a
// worker thread asserting while another test holds a capture scope cannot corrupt its count.
thread_local ScopedAssertCapture* t_CurrentCapture = nullptr;

std::string FormatAssertRecord(const char* expression, const char* message, const std::source_location& location)
{
    std::ostringstream stream;
    stream << location.file_name() << "(" << location.line() << "): assert";
    if (expression != nullptr && expression[0] != '\0')
    {
        stream << " '" << expression << "'";
    }
    if (message != nullptr && message[0] != '\0')
    {
        stream << " - " << message;
    }
    return stream.str();
}

}  // namespace

ScopedAssertCapture* GetActiveAssertCapture()
{
    return t_CurrentCapture;
}

std::string MakeAssertRecord(const char* expression, const char* message, const std::source_location& location)
{
    return FormatAssertRecord(expression, message, location);
}

ScopedAssertCapture::ScopedAssertCapture()
{
    m_Outer = t_CurrentCapture;
    t_CurrentCapture = this;
}

ScopedAssertCapture::~ScopedAssertCapture()
{
    t_CurrentCapture = m_Outer;
}

std::vector<std::string> ScopedAssertCapture::GetRecords() const
{
    return m_Records;
}

void ScopedAssertCapture::Record(const std::string& record)
{
    ++m_CaptureCount;
    m_Records.push_back(record);
}

// ---------------------------------------------------------------------------
// ScopedLogCapture
// ---------------------------------------------------------------------------

namespace
{

// The dispatcher drops everything above its global maximum before reaching any sink, so capturing Debug or
// Trace output requires raising that ceiling for the duration of the scope. Captures nest (and can be open on
// several threads at once), so the original level is saved by the first one in and restored by the last one
// out — restoring eagerly would silently truncate an outer capture still running.
std::mutex g_LogCeilingMutex;
int g_LogCeilingUsers = 0;
Hollowpoint::Core::LogLevel g_LogCeilingPrevious = Hollowpoint::Core::LogLevel::Info;

void RaiseLogCeiling(Hollowpoint::Core::LogLevel level)
{
    const std::lock_guard<std::mutex> guard(g_LogCeilingMutex);
    if (g_LogCeilingUsers == 0)
    {
        g_LogCeilingPrevious = Hollowpoint::Core::GetMaxLogLevel();
    }
    ++g_LogCeilingUsers;

    if (level > g_LogCeilingPrevious)
    {
        Hollowpoint::Core::SetMaxLogLevel(level);
    }
}

void LowerLogCeiling()
{
    const std::lock_guard<std::mutex> guard(g_LogCeilingMutex);
    if (--g_LogCeilingUsers <= 0)
    {
        g_LogCeilingUsers = 0;
        Hollowpoint::Core::SetMaxLogLevel(g_LogCeilingPrevious);
    }
}

}  // namespace

ScopedLogCapture::ScopedLogCapture(Hollowpoint::Core::LogLevel minimumLevel) : m_MinimumLevel(minimumLevel)
{
    // Attaching registers with the global logger; AttachLogSink asserts on a double-attach, so a stack
    // instance is always fresh and the scope guarantees the sink outlives the calls it observes.
    Hollowpoint::Core::AttachLogSink(this);
    RaiseLogCeiling(minimumLevel);
}

ScopedLogCapture::~ScopedLogCapture()
{
    Hollowpoint::Core::DetachLogSink(this);
    LowerLogCeiling();
}

bool ScopedLogCapture::Accepts(Hollowpoint::Core::LogLevel level) const
{
    // Levels are ordered most-severe-first, so "at least as severe as the minimum" is a <= comparison.
    return level <= m_MinimumLevel;
}

void ScopedLogCapture::Write(std::chrono::system_clock::time_point /*timestamp*/, Hollowpoint::Core::LogLevel level,
                             std::string_view channel, std::string_view message)
{
    const std::lock_guard<std::mutex> guard(m_Mutex);

    if (level <= Hollowpoint::Core::LogLevel::Error)
    {
        ++m_ErrorCount;
    }
    else if (level == Hollowpoint::Core::LogLevel::Warning)
    {
        ++m_WarningCount;
    }

    m_Lines.push_back(std::string(Hollowpoint::Core::LogLevelToTag(level)) + " " + std::string(channel) + ": " +
                      std::string(message));
}

int ScopedLogCapture::GetErrorCount() const
{
    const std::lock_guard<std::mutex> guard(m_Mutex);
    return m_ErrorCount;
}

int ScopedLogCapture::GetWarningCount() const
{
    const std::lock_guard<std::mutex> guard(m_Mutex);
    return m_WarningCount;
}

int ScopedLogCapture::GetTotalCount() const
{
    const std::lock_guard<std::mutex> guard(m_Mutex);
    return static_cast<int>(m_Lines.size());
}

bool ScopedLogCapture::AnyMessageContains(std::string_view needle) const
{
    const std::lock_guard<std::mutex> guard(m_Mutex);
    for (const std::string& line : m_Lines)
    {
        if (line.find(needle) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

std::vector<std::string> ScopedLogCapture::GetLines() const
{
    const std::lock_guard<std::mutex> guard(m_Mutex);
    return m_Lines;
}

void ScopedLogCapture::Clear()
{
    const std::lock_guard<std::mutex> guard(m_Mutex);
    m_Lines.clear();
    m_ErrorCount = 0;
    m_WarningCount = 0;
}

std::string GetDataDirectory()
{
    return HP_DATA_DIR;
}
std::string ReadTestDataFile(const std::string& relativePath)
{
    const std::string fullPath = GetDataDirectory() + "/" + relativePath;
    std::ifstream file(fullPath, std::ios::binary);
    if (!file.is_open())
    {
        FAIL("Cannot open test data file: " << fullPath);
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

}  // namespace Hollowpoint::Test
