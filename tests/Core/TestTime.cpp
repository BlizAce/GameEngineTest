#include "TestSupport.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <thread>

#include <doctest/doctest.h>

#include "Hollowpoint/Core/Time.h"

using namespace Hollowpoint::Core;
using Hollowpoint::Test::ScopedAssertCapture;

TEST_SUITE("Core/Time")
{
    TEST_CASE("the first tick produces a zero delta and frame 1")
    {
        TimeSystem time;
        time.Tick(1000.0);

        CHECK_EQ(time.GetFrameNumber(), 1u);
        CHECK(time.GetUnscaledDeltaSeconds() == doctest::Approx(0.0));
        CHECK(time.GetDeltaSeconds() == doctest::Approx(0.0));
    }

    TEST_CASE("elapsed time is scaled by the time scale")
    {
        TimeSystem time;
        time.Tick(0.0);
        time.SetTimeScale(2.0);
        time.Tick(0.016);  // one 16 ms frame at double speed

        CHECK(time.GetUnscaledDeltaSeconds() == doctest::Approx(0.016));
        CHECK(time.GetDeltaSeconds() == doctest::Approx(0.032));
    }

    TEST_CASE("pause produces zero game delta but keeps real time flowing")
    {
        TimeSystem time;
        time.Tick(0.0);
        time.Pause();
        CHECK(time.IsPaused());

        time.Tick(0.25);
        CHECK(time.GetDeltaSeconds() == doctest::Approx(0.0));
        CHECK(time.GetUnscaledDeltaSeconds() == doctest::Approx(0.25));
        CHECK(time.GetTotalTime().count() == doctest::Approx(0.0));
        CHECK(time.GetRealTime().count() == doctest::Approx(0.25));

        time.Resume();
        time.Tick(0.5);
        CHECK(time.GetDeltaSeconds() == doctest::Approx(0.25));
    }

    TEST_CASE("hitches are clamped to the maximum delta")
    {
        TimeSystem time;
        time.SetMaxDeltaSeconds(0.1);
        time.Tick(0.0);
        time.Tick(10.0);  // a ten second stall (debugger, loading screen)

        CHECK(time.GetUnscaledDeltaSeconds() == doctest::Approx(0.1));
    }

    TEST_CASE("a clock that goes backwards never yields a negative delta")
    {
        TimeSystem time;
        time.Tick(100.0);

        ScopedAssertCapture capture;
        time.Tick(99.5);  // non-monotonic reading from a misbehaving caller
        CHECK(time.GetUnscaledDeltaSeconds() == doctest::Approx(0.0));
    }

    TEST_CASE("Reset clears accumulated time and the frame counter")
    {
        TimeSystem time;
        time.Tick(0.0);
        time.Tick(1.0);
        CHECK(time.GetFrameNumber() == 2u);

        time.Reset();
        CHECK_EQ(time.GetFrameNumber(), 0u);
        CHECK(time.GetTotalTime().count() == doctest::Approx(0.0));

        // The first tick after a reset is again delta-free.
        time.Tick(500.0);
        CHECK(time.GetUnscaledDeltaSeconds() == doctest::Approx(0.0));
    }

    TEST_CASE("time scale rejects NaN and negative values")
    {
        TimeSystem time;

        ScopedAssertCapture capture;

        time.SetTimeScale(std::numeric_limits<double>::quiet_NaN());
        CHECK(time.GetTimeScale() == doctest::Approx(1.0));  // unchanged

        time.SetTimeScale(-3.0);
        CHECK(time.GetTimeScale() == doctest::Approx(0.0));  // clamped, not negative

        CHECK(capture.GetCaptureCount() == 2);
    }

    TEST_CASE("SetMaxDeltaSeconds validates its argument")
    {
        TimeSystem time;
        ScopedAssertCapture capture;

        time.SetMaxDeltaSeconds(0.0);
        time.SetMaxDeltaSeconds(-1.0);
        CHECK(time.GetMaxDeltaSeconds() == doctest::Approx(0.25));  // default kept
        CHECK(capture.GetCaptureCount() == 2);

        time.SetMaxDeltaSeconds(0.05);
        CHECK(time.GetMaxDeltaSeconds() == doctest::Approx(0.05));
    }

    TEST_CASE("the monotonic clock never goes backwards")
    {
        const double first = GetMonotonicSeconds();
        const UInt64 firstNanos = GetMonotonicNanoseconds();

        std::this_thread::sleep_for(std::chrono::milliseconds(2));

        const double second = GetMonotonicSeconds();
        const UInt64 secondNanos = GetMonotonicNanoseconds();

        CHECK(second >= first);
        CHECK(secondNanos >= firstNanos);
        CHECK(second - first >= 0.001);
    }

    TEST_CASE("the wall clock is close to the monotonic clock's scale")
    {
        const double wallStart = GetWallClockSeconds();
        const double monoStart = GetMonotonicSeconds();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const double wallEnd = GetWallClockSeconds();
        const double monoEnd = GetMonotonicSeconds();

        // Both measure the same sleep within a generous tolerance; this catches an accidental change
        // of time unit (e.g. milliseconds reported as seconds).
        CHECK(std::abs((wallEnd - wallStart) - (monoEnd - monoStart)) < 0.02);
    }

    TEST_CASE("ScopedTimer measures elapsed time")
    {
        ScopedTimer timer;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));

        const double milliseconds = timer.GetElapsedMilliseconds();
        CHECK(milliseconds >= 10.0);
        CHECK(milliseconds < 2000.0);
        CHECK(timer.GetElapsedSeconds() * 1000.0 == doctest::Approx(milliseconds).epsilon(0.01));

        timer.Restart();
        CHECK(timer.GetElapsedMilliseconds() < 50.0);
    }
}
