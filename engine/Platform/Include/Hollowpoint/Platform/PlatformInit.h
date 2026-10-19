#pragma once

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"

namespace Hollowpoint::Platform
{

using Core::UInt8;

// ---------------------------------------------------------------------------
// Windowing-library lifetime.
//
// GLFW is initialised per *process*, not per window, and its termination must happen after every window
// is gone. Wrapping that in a reference-counted scope object makes the ordering automatic: the first
// PlatformScope initialises, the last releases, and anything created in between can rely on the library
// being up without checking. It also gives us one place to install GLFW's error callback (GLFW reports
// failures through a callback that defaults to writing to stderr, which would bypass our log entirely)
// and one place to decide the backend.
//
// Backend choice: on a machine with no display server (CI, a build container, an asset baker run over
// ssh) glfwInit fails for Wayland/X11. Rather than make every windowing test unrunnable there, a scope
// may fall back to the **Null** backend: windows are created in memory and events flow through exactly
// the same code paths — only presentation is missing. That is what lets this module be tested on a
// headless machine instead of skipping, and IsHeadless() reports which backend won so the renderer can
// skip rather than fail.
// ---------------------------------------------------------------------------

//! Which windowing backend is actually running.
enum class WindowingBackend : UInt8
{
    None = 0,  //!< Not initialised.
    Wayland,
    X11,
    Win32,
    Cocoa,
    Null,  //!< Headless: windows exist but present nothing.
};

//! Human-readable backend name for logs ("wayland", "null"). Never null.
[[nodiscard]] HP_PLATFORM_API const char* WindowingBackendName(WindowingBackend backend) noexcept;

//! Options for PlatformScope.
struct PlatformInitOptions
{
    //! Force a specific backend instead of letting GLFW pick the platform default. None means "auto".
    //!
    //! Two legitimate uses. Product-wise, this is what implements a `-headless` switch: asset bakers, batch
    //! importers and dedicated servers create windows for their offscreen work and must not depend on a display
    //! server, nor pop one up on someone's desktop. Test-wise, it makes windowing tests deterministic: forcing
    //! Null keeps GTK/GLib/D-Bus (which libdecor loads to draw Wayland decorations) out of the process entirely,
    //! so leak checking stays strict instead of needing suppressions for libraries we do not control.
    WindowingBackend PreferredBackend = WindowingBackend::None;

    //! Fall back to the Null backend when no display server is available instead of failing. True is right
    //! for tools and tests; an application that must show a window should pass false so it can report "no
    //! display" rather than running invisibly.
    bool AllowHeadlessFallback = true;
};

//! RAII scope initialising the windowing library. Neither copyable nor movable: it models a process-wide
//! resource whose release order matters, and a movable scope makes "who terminated GLFW?" unanswerable.
class HP_PLATFORM_API PlatformScope
{
public:
    //! Initialises, or joins an existing initialisation. On failure IsInitialized() is false and
    //! GetFailureReason() explains why. Construction never throws: a game must be able to report "no
    //! display available" and exit cleanly rather than abort during startup.
    explicit PlatformScope(const PlatformInitOptions& options = {});
    ~PlatformScope();

    PlatformScope(const PlatformScope&) = delete;
    PlatformScope& operator=(const PlatformScope&) = delete;
    PlatformScope(PlatformScope&&) = delete;
    PlatformScope& operator=(PlatformScope&&) = delete;

    //! True when this scope may be used — i.e. windowing is up *and* the backend it provides is the one this
    //! scope asked for. Per-instance rather than "is GLFW up globally", because a caller that explicitly requested
    //! Wayland and got a process already running the Null backend must not proceed believing it can present.
    [[nodiscard]] bool IsInitialized() const noexcept;

    //! Backend in effect, or None when initialisation failed.
    [[nodiscard]] WindowingBackend GetBackend() const noexcept;

    //! True when the Null backend is in use: windows work, presentation does not. GPU-dependent code checks
    //! this to skip rather than fail where no display exists.
    [[nodiscard]] bool IsHeadless() const noexcept
    {
        return GetBackend() == WindowingBackend::Null;
    }

    //! Why initialisation failed (empty when it succeeded), naming the missing piece where GLFW tells us —
    //! intended for a dialog or log line a user can act on.
    [[nodiscard]] static const char* GetFailureReason() noexcept;

    //! Number of live scopes, for shutdown-order assertions and tests.
    [[nodiscard]] static int GetActiveScopeCount() noexcept;

private:
    bool m_OwnsInitialization = false;
    bool m_Usable = false;
};

//! The windowing library version actually loaded, e.g. "3.5.1 Wayland X11 GLX Null EGL OSMesa monotonic".
//! Worth logging at startup and in crash reports: on Linux the loader can pick up a different libglfw than
//! the headers were taken from, which produces failures that look like engine bugs.
[[nodiscard]] HP_PLATFORM_API const char* GetWindowingLibraryVersionString() noexcept;

}  // namespace Hollowpoint::Platform
