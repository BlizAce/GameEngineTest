// Shared entry point for every doctest suite. Keeping it in one place standardises command line
// handling and — most importantly — the way engine assertions are reported across all modules.
#define DOCTEST_CONFIG_IMPLEMENT

#include "TestSupport.h"

#include <doctest/doctest.h>

#include "Hollowpoint/Core/Assert.h"

namespace Hollowpoint::Test
{

namespace
{

// Engine assertion handler for the test run. Records the assertion against the currently running
// test case (or into the active ScopedAssertCapture when the test expects it) and lets execution
// continue, so one run reports every failure instead of aborting on the first.
bool EngineAssertHandler(Hollowpoint::Core::AssertFailure /*failure*/, const char* expression, const char* message,
                         const std::source_location& location, void* /*userData*/)
{
    ScopedAssertCapture* capture = GetActiveAssertCapture();
    if (capture != nullptr)
    {
        capture->Record(MakeAssertRecord(expression, message, location));
        return true;
    }

    // Unexpected assertion: report it as a failure of the current test case. doctest handles the
    // case where no test is running by printing to stderr and failing the run.
    CAPTURE(location.file_name());
    CAPTURE(location.line());
    FAIL("Unexpected engine assertion: " << (expression != nullptr ? expression : "(none)")
                                         << (message != nullptr ? message : ""));
    return true;
}

}  // namespace

int RunDoctest(int argc, char** argv)
{
    doctest::Context context;
    context.applyCommandLine(argc, argv);

    const Hollowpoint::Core::AssertHandler previousHandler = Hollowpoint::Core::SetAssertHandler(&EngineAssertHandler);

    const int result = context.run();

    Hollowpoint::Core::SetAssertHandler(previousHandler);
    return result;
}

}  // namespace Hollowpoint::Test

int main(int argc, char** argv)
{
    return Hollowpoint::Test::RunDoctest(argc, argv);
}
