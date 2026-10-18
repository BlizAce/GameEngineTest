#pragma once

#include <source_location>
#include <string>
#include <vector>

#include <doctest/doctest.h>

namespace Hollowpoint::Test
{

// ---------------------------------------------------------------------------
// Support shared by all test suites.
//
// The engine reports broken invariants through its assert handler (see Core/Assert.h). TestMain.cpp
// installs a handler for the whole test run: an unexpected engine assertion is recorded and reported
// as a failure of the currently running test case rather than aborting the process, so one run
// surfaces every problem and CI still exits non-zero.
//
// A test that deliberately exercises a guard rail wraps the call in ScopedAssertCapture; assertions
// raised inside its scope are diverted into the capture instead of being reported:
//
//     ScopedAssertCapture capture;
//     component->SetInvalidValue();
//     CHECK(capture.GetCaptureCount() == 1);
// ---------------------------------------------------------------------------

// The innermost active capture scope on the current thread, or nullptr. Internal plumbing shared
// between TestMain.cpp's assert handler and ScopedAssertCapture.
class ScopedAssertCapture;
ScopedAssertCapture* GetActiveAssertCapture();

// Formats an assertion as "file(line): assert 'expr' - message".
std::string MakeAssertRecord(const char* expression, const char* message, const std::source_location& location);

// Captures engine assertions raised in the current scope on the current thread.
class ScopedAssertCapture
{
public:
    ScopedAssertCapture();
    ~ScopedAssertCapture();

    ScopedAssertCapture(const ScopedAssertCapture&) = delete;
    ScopedAssertCapture& operator=(const ScopedAssertCapture&) = delete;

    // Number of assertions captured so far in this scope.
    [[nodiscard]] int GetCaptureCount() const
    {
        return m_CaptureCount;
    }

    // Textual records captured so far, oldest first.
    [[nodiscard]] std::vector<std::string> GetRecords() const;

    // Called by the global assert handler; not for engine or test code.
    void Record(const std::string& record);

private:
    ScopedAssertCapture* m_Outer = nullptr;
    int m_CaptureCount = 0;
    std::vector<std::string> m_Records;
};

// ---------------------------------------------------------------------------
// Misc helpers
// ---------------------------------------------------------------------------

// Path of the test data directory (compile-time HP_DATA_DIR).
std::string GetDataDirectory();

// Reads a whole file from the data directory; fails the current test case when it cannot be opened.
std::string ReadTestDataFile(const std::string& relativePath);

// Runs doctest with the engine's assertion handler installed (called from main()).
int RunDoctest(int argc, char** argv);

}  // namespace Hollowpoint::Test
