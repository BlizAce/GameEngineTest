#include "WindowImpl.h"

#include <thread>

#include "Hollowpoint/Core/Format.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Platform/PlatformInit.h"

namespace Hollowpoint::Platform
{

using Core::Format;

// ============================================================================
// Shared state
// ============================================================================

namespace Detail
{

WindowingState& GetWindowingState()
{
    // Function-local static rather than a namespace-scope object: the scope count is touched during
    // PlatformScope destruction, which can happen after main returns, and a namespace-scope object could be
    // destroyed before that runs. A local static is initialised on first use and destroyed last.
    static WindowingState state;
    return state;
}

}  // namespace Detail

namespace
{

//! GLFW error callback. GLFW reports failures through a callback defaulting to fprintf(stderr), which would
//! put windowing errors outside our log — and therefore outside crash reports and the editor console.
void GlfwErrorCallback(int error, const char* description)
{
    const char* text = description != nullptr ? description : "(no description)";
    HP_LOG_ERROR("PLATFORM", "GLFW error {}: {}", error, text);

    // Also stashed while no scope is live, because GLFW clears its error state on the next call: by the time
    // glfwInit returns to us the reason would be gone.
    Detail::WindowingState& state = Detail::GetWindowingState();
    if (state.ScopeCount == 0)
    {
        state.FailureReason = Format("GLFW error {}: {}", error, text);
    }
}

[[nodiscard]] WindowingBackend QueryBackend() noexcept
{
    // glfwGetPlatform returns the *selected* platform as a single enumeration value, not a set of bits (GLFW's
    // documentation: "will be one of GLFW_PLATFORM_*", zero on error). The values are consecutive from
    // GLFW_ANY_PLATFORM — 0x60001 Win32, 0x60002 Cocoa, 0x60003 Wayland, 0x60004 X11, 0x60005 Null — so masking
    // them against each other produces false positives: testing `platform & GLFW_PLATFORM_WAYLAND` matches Win32
    // (0x60001) and Cocoa (0x60002) too, which would report a Windows build as running Wayland. Equality is the
    // only correct comparison here.
    switch (glfwGetPlatform())
    {
        case GLFW_PLATFORM_WIN32:
            return WindowingBackend::Win32;
        case GLFW_PLATFORM_COCOA:
            return WindowingBackend::Cocoa;
        case GLFW_PLATFORM_WAYLAND:
            return WindowingBackend::Wayland;
        case GLFW_PLATFORM_X11:
            return WindowingBackend::X11;
        case GLFW_PLATFORM_NULL:
            return WindowingBackend::Null;
        default:
            // Zero (error) or a value from a newer GLFW we do not know. Reporting None is honest; guessing Null would
            // make the renderer skip presentation on a machine that has one.
            return WindowingBackend::None;
    }
}

[[nodiscard]] bool TryInitialize(WindowingBackend preferred, bool allowNullFallback)
{
    Detail::WindowingState& state = Detail::GetWindowingState();

    if (!state.ErrorCallbackInstalled)
    {
        glfwSetErrorCallback(GlfwErrorCallback);
        state.ErrorCallbackInstalled = true;
    }

    // An explicit backend is requested: hint it before the first attempt, and do *not* fall back — asking for
    // Wayland and silently getting Null would give a caller that checks "can I present?" a wrong answer.
    if (preferred != WindowingBackend::None)
    {
        int platform = 0;
        switch (preferred)
        {
            case WindowingBackend::Wayland:
                platform = GLFW_PLATFORM_WAYLAND;
                break;
            case WindowingBackend::X11:
                platform = GLFW_PLATFORM_X11;
                break;
            case WindowingBackend::Win32:
                platform = GLFW_PLATFORM_WIN32;
                break;
            case WindowingBackend::Cocoa:
                platform = GLFW_PLATFORM_COCOA;
                break;
            case WindowingBackend::Null:
                platform = GLFW_PLATFORM_NULL;
                break;
            default:
                // None was handled by the caller; an unhandled value must not guess a backend.
                state.FailureReason = "Requested windowing backend is not selectable";
                return false;
        }

        glfwInitHint(GLFW_PLATFORM, platform);

        if (glfwInit())
        {
            state.Backend = QueryBackend();
            state.OwnerThread = std::this_thread::get_id();
            return true;
        }

        if (state.FailureReason.empty())
        {
            state.FailureReason =
                Format("Requested windowing backend '{}' is unavailable", WindowingBackendName(preferred));
        }
        return false;
    }

    if (glfwInit())
    {
        state.Backend = QueryBackend();
        state.OwnerThread = std::this_thread::get_id();
        return true;
    }

    if (!allowNullFallback)
    {
        if (state.FailureReason.empty())
        {
            state.FailureReason = "glfwInit failed (no display server, or GLFW built without a backend)";
        }
        return false;
    }

    // No display: retry with the Null backend so headless tools and tests still exercise windowing. The hint
    // is sticky for the process, which is fine — a process that had no display at startup will not gain one.
    state.FailureReason.clear();
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_NULL);

    if (glfwInit())
    {
        state.Backend = WindowingBackend::Null;
        state.OwnerThread = std::this_thread::get_id();
        HP_LOG_INFO("PLATFORM", "No display available — using the headless (null) windowing backend");
        return true;
    }

    if (state.FailureReason.empty())
    {
        state.FailureReason = "glfwInit failed even with the null backend";
    }
    return false;
}

}  // namespace

PlatformScope::PlatformScope(const PlatformInitOptions& options)
{
    Detail::WindowingState& state = Detail::GetWindowingState();

    if (state.ScopeCount > 0)
    {
        // Already initialised: join the existing initialisation, since GLFW cannot switch backends mid-process.
        //
        // But a caller that asked for a *specific* backend must be told whether it got one: joining silently would
        // leave code that checked "is the Wayland backend active?" believing whatever the first scope happened to
        // choose. So an explicit, mismatched request fails this scope (IsInitialized() false) rather than lying.
        m_OwnsInitialization = false;

        if (options.PreferredBackend != WindowingBackend::None && options.PreferredBackend != state.Backend)
        {
            HP_LOG_ERROR("PLATFORM",
                         "Windowing already initialized with backend '{}'; cannot honor a request for '{}' — this "
                         "scope is not usable",
                         WindowingBackendName(state.Backend), WindowingBackendName(options.PreferredBackend));
            return;
        }

        m_Usable = true;
        ++state.ScopeCount;
        return;
    }

    if (!TryInitialize(options.PreferredBackend, options.AllowHeadlessFallback))
    {
        HP_LOG_ERROR("PLATFORM", "Windowing initialization failed: {}", state.FailureReason);
        return;
    }

    m_OwnsInitialization = true;
    m_Usable = true;
    state.ScopeCount = 1;
    HP_LOG_INFO("PLATFORM", "Windowing ready (backend {}, library {})", WindowingBackendName(state.Backend),
                glfwGetVersionString());
}

PlatformScope::~PlatformScope()
{
    Detail::WindowingState& state = Detail::GetWindowingState();

    if (m_OwnsInitialization)
    {
        glfwTerminate();
        state.ScopeCount = 0;
        state.Backend = WindowingBackend::None;
        return;
    }

    // Only a scope that actually joined decrements. A scope whose backend request was refused never incremented,
    // so releasing it here would drive the count to zero while GLFW is still initialised — after which every later
    // window creation is rejected as "windowing not initialized". Increments and decrements must pair exactly.
    if (m_Usable && state.ScopeCount > 0)
    {
        --state.ScopeCount;
    }
}

bool PlatformScope::IsInitialized() const noexcept
{
    // Per-instance: see the declaration. The global count is still what keeps GLFW alive, but "can *this* scope be
    // used" depends on whether its backend request was honoured.
    return m_Usable;
}

WindowingBackend PlatformScope::GetBackend() const noexcept
{
    // A scope whose backend request was refused reports None rather than the process's actual backend: reporting
    // "wayland" to a caller that asked for Win32 and cannot run would be the exact lie IsInitialized() exists to
    // prevent.
    if (!m_Usable)
    {
        return WindowingBackend::None;
    }

    return Detail::GetWindowingState().Backend;
}

const char* PlatformScope::GetFailureReason() noexcept
{
    return Detail::GetWindowingState().FailureReason.c_str();
}

int PlatformScope::GetActiveScopeCount() noexcept
{
    return Detail::GetWindowingState().ScopeCount;
}

const char* WindowingBackendName(WindowingBackend backend) noexcept
{
    switch (backend)
    {
        case WindowingBackend::None:
            return "none";
        case WindowingBackend::Wayland:
            return "wayland";
        case WindowingBackend::X11:
            return "x11";
        case WindowingBackend::Win32:
            return "win32";
        case WindowingBackend::Cocoa:
            return "cocoa";
        case WindowingBackend::Null:
            return "null";
    }
    return "unknown";
}

const char* GetWindowingLibraryVersionString() noexcept
{
    return glfwGetVersionString();
}

}  // namespace Hollowpoint::Platform
