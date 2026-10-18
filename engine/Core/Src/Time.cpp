#include <cmath>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Time.h"

namespace Hollowpoint::Core
{

double GetMonotonicSeconds()
{
    return std::chrono::duration_cast<Seconds>(MonotonicClock::now().time_since_epoch()).count();
}

UInt64 GetMonotonicNanoseconds()
{
    return static_cast<UInt64>(
        std::chrono::duration_cast<Nanoseconds>(MonotonicClock::now().time_since_epoch()).count());
}

double GetWallClockSeconds()
{
    return std::chrono::duration_cast<Seconds>(WallClock::now().time_since_epoch()).count();
}

TimeSystem::TimeSystem()
{
    m_FrameTime.TimeScale = 1.0;
    m_FrameTime.DeltaSeconds = 0.0;
    m_FrameTime.UnscaledDeltaSeconds = 0.0;
}

void TimeSystem::Reset()
{
    m_FrameTime = FrameTime{};
    m_FrameTime.TimeScale = 1.0;
    m_HasPreviousReading = false;
    m_PreviousRealSeconds = 0.0;
}

void TimeSystem::Tick(double realTimeSeconds)
{
    double unscaledDelta = 0.0;
    if (m_HasPreviousReading)
    {
        // A clock that goes backwards (should be impossible with a monotonic source, but guard
        // anyway) must never produce a negative delta — systems below assume delta >= 0.
        unscaledDelta = realTimeSeconds - m_PreviousRealSeconds;
        if (!(unscaledDelta > 0.0))
        {
            unscaledDelta = 0.0;
        }
    }
    m_PreviousRealSeconds = realTimeSeconds;
    m_HasPreviousReading = true;

    if (unscaledDelta > m_MaxDeltaSeconds)
    {
        unscaledDelta = m_MaxDeltaSeconds;
    }

    m_FrameTime.UnscaledDeltaSeconds = unscaledDelta;
    m_FrameTime.Paused = m_Paused;
    m_FrameTime.DeltaSeconds = m_Paused ? 0.0 : unscaledDelta * m_FrameTime.TimeScale;
    m_FrameTime.RealTime += Seconds(unscaledDelta);
    m_FrameTime.TotalTime += Seconds(m_FrameTime.DeltaSeconds);
    ++m_FrameTime.FrameNumber;
}

void TimeSystem::SetTimeScale(double scale)
{
    if (std::isnan(scale))
    {
        HP_ASSERT_MSG(false, "TimeSystem::SetTimeScale: NaN is not a valid time scale");
        return;
    }
    if (scale < 0.0)
    {
        HP_ASSERT_MSG(false, "TimeSystem::SetTimeScale: negative time scales are not supported, use Pause()");
        scale = 0.0;
    }
    m_FrameTime.TimeScale = scale;
}

void TimeSystem::SetMaxDeltaSeconds(double maxDelta)
{
    HP_ASSERT_ARGUMENT(!std::isnan(maxDelta) && maxDelta > 0.0,
                       "TimeSystem::SetMaxDeltaSeconds: must be a positive number");
    if (!(maxDelta > 0.0))
    {
        return;
    }
    m_MaxDeltaSeconds = maxDelta;
}

}  // namespace Hollowpoint::Core
