#include "TestSupport.h"

#include <doctest/doctest.h>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Memory.h"
#include "Hollowpoint/Core/MemoryStats.h"

using namespace Hollowpoint::Core;
using Hollowpoint::Test::ScopedAssertCapture;

TEST_SUITE("Core/Assert")
{
    TEST_CASE("HP_ASSERT captures a failed condition and lets execution continue")
    {
        ScopedAssertCapture capture;

        bool reachedAfterAssert = false;
        HP_ASSERT(false);
        reachedAfterAssert = true;

        CHECK(reachedAfterAssert);
        CHECK(capture.GetCaptureCount() == 1);
        REQUIRE_FALSE(capture.GetRecords().empty());
        CHECK(capture.GetRecords()[0].find("false") != std::string::npos);
    }

    TEST_CASE("HP_ASSERT does not fire when the condition holds")
    {
        ScopedAssertCapture capture;
        HP_ASSERT(1 + 1 == 2);
        CHECK(capture.GetCaptureCount() == 0);
    }

    TEST_CASE("HP_ASSERT_MSG records the custom message")
    {
        ScopedAssertCapture capture;
        HP_ASSERT_MSG(false, "entity has no transform");
        REQUIRE_EQ(capture.GetCaptureCount(), 1);
        CHECK(capture.GetRecords()[0].find("entity has no transform") != std::string::npos);
    }

    TEST_CASE("HP_ASSERT_ARGUMENT records an invalid-argument failure")
    {
        ScopedAssertCapture capture;
        HP_ASSERT_ARGUMENT(false, "index out of range");
        REQUIRE_EQ(capture.GetCaptureCount(), 1);
        CHECK(capture.GetRecords()[0].find("index out of range") != std::string::npos);
    }

    TEST_CASE("HP_UNREACHABLE always reports")
    {
        ScopedAssertCapture capture;
        HP_UNREACHABLE("switch fell through");
        CHECK(capture.GetCaptureCount() == 1);
    }

    TEST_CASE("nested capture scopes attribute assertions to the innermost scope")
    {
        ScopedAssertCapture outer;
        HP_ASSERT(false);
        CHECK(outer.GetCaptureCount() == 1);

        {
            ScopedAssertCapture inner;
            HP_ASSERT(false);
            HP_ASSERT(false);
            CHECK(inner.GetCaptureCount() == 2);
            CHECK(outer.GetCaptureCount() == 1);
        }

        // After the inner scope closes, assertions go to the outer one again.
        HP_ASSERT(false);
        CHECK(outer.GetCaptureCount() == 2);
    }

    TEST_CASE("the handler can be replaced and restored")
    {
        static int s_CallCount = 0;
        AssertHandler previous =
            SetAssertHandler([](AssertFailure, const char*, const char*, const std::source_location&, void*) -> bool {
                ++s_CallCount;
                return true;
            });

        HP_ASSERT(false);
        CHECK(s_CallCount == 1);

        SetAssertHandler(previous);
    }
}
