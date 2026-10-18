#include "TestSupport.h"

#include <fstream>
#include <sstream>

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
