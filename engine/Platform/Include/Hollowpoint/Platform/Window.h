#pragma once

#include <string>
#include <vector>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/Platform/InputEvent.h"

namespace Hollowpoint::Platform
{

using Core::Size;
using Core::UInt32;

namespace Detail
{
struct WindowImpl;
}

// ---------------------------------------------------------------------------
// Window — one OS window, and the engine's source of input events.
//
// Lifetime: requires an initialised PlatformScope that outlives it. That is enforced by construction
// order rather than reference counting because GLFW itself ties windows to library initialisation —
// terminating the library first would make every glfwDestroyWindow a use-after-free — so the engine
// creates the scope before any window and destroys it last.
//
// Threading: GLFW is single-threaded, so all window creation, polling and destruction must happen on the
// thread that initialised it. That is checked (not merely documented) in Create; see Window.cpp.
//
// Events are *queued*, not delivered synchronously: callbacks fire inside PollEvents and append to a
// per-window queue which the caller drains once per frame. Draining into a caller-supplied vector means a
// frame with no input allocates nothing (the vector keeps its capacity) — the difference between "input is
// free" and "input hits the allocator twenty times a frame".
// ---------------------------------------------------------------------------

//! How the cursor behaves over the window.
enum class CursorMode : UInt8
{
    Normal,    //!< Visible and movable.
    Hidden,    //!< Invisible; position still reported (editor overlays).
    Disabled,  //!< Captured: invisible and position-locked, so an FPS camera can read unbounded relative
               //!< motion without the pointer leaving the window.
};

//! Hints applied when creating a window. All defaulted, so `WindowDesc desc;` is valid and only what the
//! caller cares about is set explicitly.
struct WindowDesc
{
    std::string Title = "Hollowpoint";

    //! Size in **content** coordinates, not framebuffer pixels: on a 2× display this is the layout size the
    //! user expects from other applications, and the framebuffer comes out twice that.
    UInt32 Width = 1280;
    UInt32 Height = 720;

    bool Resizable = true;
    bool Visible = true;
    bool Focused = true;
    bool Decorated = true;

    //! Requires a compositor that supports it; failure to get transparency is reported, not fatal.
    bool TransparentFramebuffer = false;

    //! Start iconified — useful for a tool an agent launches that should not steal the screen.
    bool Iconified = false;
};

class HP_PLATFORM_API Window
{
public:
    //! Creates a window, or returns nullptr on failure with the reason logged. Creating a window is the kind
    //! of thing that legitimately fails on a user's machine (no display, unsupported transparency), so it
    //! must be reportable rather than fatal.
    [[nodiscard]] static Window* Create(const WindowDesc& desc);

    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    //! Drains queued input into `outEvents` (appended; callers normally clear once per frame) and returns
    //! how many were appended. This processes OS messages, so callbacks run during the call and may re-enter
    //! engine code that reads window state.
    Size PollEvents(std::vector<InputEvent>& outEvents);

    //! Blocks until an event arrives or `timeoutSeconds` elapses (0 = wait indefinitely). Used to idle a
    //! backgrounded window instead of spinning the CPU rendering nothing; returns false on timeout.
    bool WaitEvents(double timeoutSeconds);

    [[nodiscard]] UInt32 GetFramebufferWidth() const noexcept;
    [[nodiscard]] UInt32 GetFramebufferHeight() const noexcept;

    [[nodiscard]] UInt32 GetContentWidth() const noexcept;
    [[nodiscard]] UInt32 GetContentHeight() const noexcept;

    //! DPI scale factor (1.0 on a plain display, 2.0 on HiDPI; compositors can report fractions such as
    //! 1.25). Framebuffer size equals content size times this, rounded.
    [[nodiscard]] float GetContentScale() const noexcept;

    void SetTitle(std::string_view title);
    [[nodiscard]] const std::string& GetTitle() const noexcept;

    //! Resizes in content coordinates. A resize is asynchronous — the compositor may answer with a different
    //! size — so the authoritative value arrives as a WindowResize event rather than from this call.
    void SetContentSize(UInt32 width, UInt32 height);

    void SetVisible(bool visible);
    [[nodiscard]] bool IsVisible() const noexcept;

    void SetIconified(bool iconified);
    [[nodiscard]] bool IsIconified() const noexcept;

    void Focus();
    [[nodiscard]] bool IsFocused() const noexcept;

    void SetCursorMode(CursorMode mode) noexcept;
    [[nodiscard]] CursorMode GetCursorMode() const noexcept;

    //! Sets the cursor position in content coordinates. Refused (and logged) in Disabled mode, where the
    //! pointer is locked by definition and a silent no-op would be confusing.
    void SetCursorPosition(float x, float y);

    //! True once the user asked to close the window (titlebar button) or RequestClose() was called. The main
    //! loop uses this as its exit condition; ResetCloseRequested() clears it (an editor may veto a close).
    [[nodiscard]] bool ShouldClose() const noexcept;
    void RequestClose() noexcept;
    void ResetCloseRequested() noexcept;

    //! Clipboard access, UTF-8. Returns an empty string when unavailable or holding no text — a Wayland
    //! compositor may refuse clipboard reads to unfocused windows, which is normal rather than an error.
    void SetClipboardText(std::string_view text);
    [[nodiscard]] std::string GetClipboardText() const;

    // -----------------------------------------------------------------------
    // Input injection.
    //
    // Queues an event as though the platform had delivered it, bypassing the windowing library. This is not
    // a test-only backdoor: driving input programmatically is a product requirement, because the editor is
    // designed to be operated by agents (see AGENTS.md) and automated scene work needs to send keystrokes
    // and clicks without a human or a real device. It also makes input-handling behaviour testable on a
    // machine with no display at all — GLFW's Null backend creates windows but cannot synthesise device
    // events, so without this seam the queue would be untestable headless.
    //
    // Events go through exactly the same queue (and therefore the same overflow policy) as real ones, so a
    // test exercising injected input is exercising the production path.
    // -----------------------------------------------------------------------

    void InjectEvent(const InputEvent& event);

    //! Number of events queued but not yet drained. Used by the frame loop to decide whether to wake, and by
    //! tests to assert consumption without draining.
    [[nodiscard]] Size GetPendingEventCount() const noexcept;

    //! Vulkan surface for this window.
    //!
    //! VkInstance/VkSurfaceKHR are passed as `void*` deliberately: Platform must not require the Vulkan
    //! headers, so that tools needing only a window (and the headless test binary) link nothing
    //! GPU-related. The casts live in exactly one place here.
    //!
    //! \param vkInstance A VkInstance owned by the caller (not referenced).
    //! \param outVkSurface Receives a VkSurfaceKHR that the **caller** must destroy with
    //!        vkDestroySurfaceKHR; GLFW does not own surfaces it creates.
    //! \return false when the backend cannot present, or creation failed; `outVkSurface` is then set to null.
    bool CreateVulkanSurface(void* vkInstance, void** outVkSurface) const noexcept;

    //! Vulkan instance extensions this window's surface requires (e.g. VK_KHR_wayland_surface). Empty on a
    //! headless backend, where no surface extension is needed because there is no surface to create.
    [[nodiscard]] std::vector<std::string> GetRequiredVulkanInstanceExtensions() const;

    //! The native window handle (HWND / wl_surface* / xcb_window_t), or nullptr when this build has no native
    //! header support for the running backend. For graphics-layer integration only.
    [[nodiscard]] void* GetNativeHandle() const noexcept;

private:
    Window() = default;

    // Opaque so GLFW types never appear in a public header. Defined in Platform/Src/WindowImpl.h.
    Detail::WindowImpl* m_Impl = nullptr;
};

}  // namespace Hollowpoint::Platform
