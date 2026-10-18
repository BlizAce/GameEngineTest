#pragma once

#include <chrono>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"

namespace Hollowpoint::Core
{

// ---------------------------------------------------------------------------
// Time.
//
// Two clocks are exposed and the difference matters:
//   * RealTime  — wall clock, can jump when the OS adjusts the date. Use for timestamps in logs
//                 and saved data only.
//   * Monotonic — never goes backwards. Every frame-timing, timeout and interpolation calculation
//                 in the engine uses this clock.
//
// EngineTime additionally implements the classic game-clock controls (scale, pause, fixed step)
// that both the editor's "pause while editing" behaviour and network replays depend on.
// ---------------------------------------------------------------------------

using MonotonicClock = std::chrono::steady_clock;
using WallClock = std::chrono::system_clock;

using Seconds = std::chrono::duration<double>;
using Milliseconds = std::chrono::duration<double, std::milli>;
using Nanoseconds = std::chrono::duration<std::int64_t, std::nano>;

// Seconds since an arbitrary but fixed point in time, from the monotonic clock. Never decreases.
[[nodiscard]] HP_CORE_API double GetMonotonicSeconds();

// Nanoseconds since the same origin, for high-resolution profiling.
[[nodiscard]] HP_CORE_API UInt64 GetMonotonicNanoseconds();

// Wall-clock time as seconds since the Unix epoch (may jump if the user changes the system clock).
[[nodiscard]] HP_CORE_API double GetWallClockSeconds();

// Snapshot of one frame's timing information, produced by TimeSystem::Tick().
struct FrameTime
{
    double DeltaSeconds = 0.0;          //!< Scaled time since the previous tick (game logic uses this).
    double UnscaledDeltaSeconds = 0.0;  //!< Real time since the previous tick (never zero after the first tick).
    double TimeScale = 1.0;             //!< Scale that produced DeltaSeconds.
    bool Paused = false;                //!< True when the game clock is paused (DeltaSeconds == 0).
    UInt64 FrameNumber = 0;             //!< Monotonically increasing frame counter, starting at 1.
    Seconds TotalTime{0};               //!< Scaled time since the time system started.
    Seconds RealTime{0};                //!< Unscaled time since the time system started.
};

// Owns the game clock. One instance lives in Engine; systems read it through Engine::GetTime().
class HP_CORE_API TimeSystem
{
public:
    TimeSystem();

    // Advances the clock by one frame using the real elapsed time. Call exactly once per frame
    // from the main loop, before any system reads the frame time.
    void Tick()
    {
        Tick(GetMonotonicSeconds());
    }

    // Overload taking an explicit wall-clock reading (seconds), used by tests and by deterministic
    // replays that feed recorded timestamps.
    void Tick(double realTimeSeconds);

    // Resets elapsed time to zero and the frame counter to 0. The next Tick() produces a zero delta.
    void Reset();

    [[nodiscard]] const FrameTime& GetFrameTime() const noexcept
    {
        return m_FrameTime;
    }
    [[nodiscard]] double GetDeltaSeconds() const noexcept
    {
        return m_FrameTime.DeltaSeconds;
    }
    [[nodiscard]] double GetUnscaledDeltaSeconds() const noexcept
    {
        return m_FrameTime.UnscaledDeltaSeconds;
    }
    [[nodiscard]] UInt64 GetFrameNumber() const noexcept
    {
        return m_FrameTime.FrameNumber;
    }
    [[nodiscard]] Seconds GetTotalTime() const noexcept
    {
        return m_FrameTime.TotalTime;
    }
    [[nodiscard]] Seconds GetRealTime() const noexcept
    {
        return m_FrameTime.RealTime;
    }

    // Time scale: 1.0 is normal speed, 0.5 is half speed, 2.0 is double speed. Negative values are
    // clamped to 0 (use Pause for a stopped clock); NaN is rejected and ignored.
    void SetTimeScale(double scale);
    [[nodiscard]] double GetTimeScale() const noexcept
    {
        return m_FrameTime.TimeScale;
    }

    void Pause()
    {
        m_Paused = true;
    }
    void Resume()
    {
        m_Paused = false;
    }
    [[nodiscard]] bool IsPaused() const noexcept
    {
        return m_Paused;
    }

    // Clamps the delta produced by Tick(), protecting simulation stability against hitches such as
    // a debugger breakpoint or a slow asset load. Default 0.25 s (an editor-friendly value).
    void SetMaxDeltaSeconds(double maxDelta);
    [[nodiscard]] double GetMaxDeltaSeconds() const noexcept
    {
        return m_MaxDeltaSeconds;
    }

private:
    FrameTime m_FrameTime;
    bool m_Paused = false;
    bool m_HasPreviousReading = false;
    double m_PreviousRealSeconds = 0.0;
    double m_MaxDeltaSeconds = 0.25;
};

// Measures a block of code and returns its duration in milliseconds:
//   HP_SCOPED_TIMER(timer); ... ; LOG_INFO("PERF", "{} ms", timer.GetElapsedMilliseconds());
class HP_CORE_API ScopedTimer
{
public:
    ScopedTimer() : m_Start(GetMonotonicNanoseconds()) {}

    void Restart()
    {
        m_Start = GetMonotonicNanoseconds();
    }

    [[nodiscard]] double GetElapsedMilliseconds() const
    {
        return static_cast<double>(GetMonotonicNanoseconds() - m_Start) * 1e-6;
    }

    [[nodiscard]] double GetElapsedSeconds() const
    {
        return static_cast<double>(GetMonotonicNanoseconds() - m_Start) * 1e-9;
    }

private:
    UInt64 m_Start = 0;
};

}  // namespace Hollowpoint::Core
