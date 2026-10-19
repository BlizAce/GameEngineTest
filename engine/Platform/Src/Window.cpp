#include "GlfwInputMapping.h"
#include "WindowImpl.h"

#include <cmath>
#include <vector>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Format.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Platform/Window.h"

namespace Hollowpoint::Platform
{

using Core::Format;
using Core::Size;

// ============================================================================
// WindowImpl
// ============================================================================

namespace Detail
{

void WindowImpl::Enqueue(InputEvent event)
{
    if (Queue.size() >= MaxQueuedEvents)
    {
        if (!OverflowReported)
        {
            OverflowReported = true;
            HP_LOG_WARNING("PLATFORM",
                           "Input queue full ({} events) — dropping the oldest. The application is not polling "
                           "input.",
                           MaxQueuedEvents);
        }

        // Drop a chunk rather than one event: at the cap, every subsequent event would otherwise re-enter this path.
        Queue.erase(Queue.begin(), Queue.begin() + static_cast<std::ptrdiff_t>(OverflowDropCount));
    }
    Queue.push_back(std::move(event));
}

WindowImpl* GetImpl(GLFWwindow* handle) noexcept
{
    return static_cast<WindowImpl*>(glfwGetWindowUserPointer(handle));
}

void SetImpl(GLFWwindow* handle, WindowImpl* impl) noexcept
{
    glfwSetWindowUserPointer(handle, impl);
}

namespace
{

//! Recovers the Impl for a callback's window. GLFW gives exactly one user pointer per window, so it stores the
//! Impl and every callback recovers it from there — the standard way into a C++ object without a global map
//! (which would need locking and would leak entries).
[[nodiscard]] WindowImpl* ImplFor(GLFWwindow* handle) noexcept
{
    return handle != nullptr ? GetImpl(handle) : nullptr;
}

InputEvent MakeEvent(InputEvent::Type type) noexcept
{
    InputEvent event;
    event.EventType = type;
    return event;
}

void OnKey(GLFWwindow* handle, int key, int scancode, int action, int mods)
{
    WindowImpl* impl = ImplFor(handle);
    if (impl == nullptr)
    {
        return;
    }

    InputEvent event = MakeEvent(InputEvent::Type::Key);
    event.Key = TranslateKey(key);
    event.Action = TranslateAction(action);
    event.Modifiers = TranslateModifiers(mods);

    // Scan codes are not carried: they exist only for layout-independent bindings, and those are served by
    // Character events (which deliver the code point the user's layout produced). Recorded as a known gap.
    (void)scancode;

    impl->Enqueue(std::move(event));
}

void OnChar(GLFWwindow* handle, unsigned int codepoint)
{
    WindowImpl* impl = ImplFor(handle);
    if (impl == nullptr)
    {
        return;
    }

    // GLFW's character callback delivers an already-decoded Unicode code point, so it must not be re-decoded
    // (doing so would corrupt every non-ASCII character). It is range-checked instead.
    InputEvent event = MakeEvent(InputEvent::Type::Character);
    event.Character = codepoint < 0x110000u ? static_cast<char32_t>(codepoint) : U'\uFFFD';

    impl->Enqueue(std::move(event));
}

void OnMouseButton(GLFWwindow* handle, int button, int action, int mods)
{
    WindowImpl* impl = ImplFor(handle);
    if (impl == nullptr)
    {
        return;
    }

    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(handle, &x, &y);

    InputEvent event = MakeEvent(InputEvent::Type::MouseButton);
    event.Button = TranslateMouseButton(button);
    event.Action = TranslateAction(action);
    event.Modifiers = TranslateModifiers(mods);
    event.Position.X = static_cast<float>(x);
    event.Position.Y = static_cast<float>(y);

    impl->Enqueue(std::move(event));
}

void OnCursorPos(GLFWwindow* handle, double x, double y)
{
    WindowImpl* impl = ImplFor(handle);
    if (impl == nullptr)
    {
        return;
    }

    InputEvent event = MakeEvent(InputEvent::Type::MouseMove);
    event.Position.X = static_cast<float>(x);
    event.Position.Y = static_cast<float>(y);

    impl->Enqueue(std::move(event));
}

void OnScroll(GLFWwindow* handle, double dx, double dy)
{
    WindowImpl* impl = ImplFor(handle);
    if (impl == nullptr)
    {
        return;
    }

    InputEvent event = MakeEvent(InputEvent::Type::Scroll);
    event.ScrollX = static_cast<float>(dx);
    event.ScrollY = static_cast<float>(dy);

    impl->Enqueue(std::move(event));
}

void OnFramebufferSize(GLFWwindow* handle, int width, int height)
{
    WindowImpl* impl = ImplFor(handle);
    if (impl == nullptr)
    {
        return;
    }

    InputEvent event = MakeEvent(InputEvent::Type::WindowResize);
    event.Width = static_cast<UInt32>(width < 0 ? 0 : width);
    event.Height = static_cast<UInt32>(height < 0 ? 0 : height);

    impl->Enqueue(std::move(event));
}

void OnFocus(GLFWwindow* handle, int focused)
{
    WindowImpl* impl = ImplFor(handle);
    if (impl == nullptr)
    {
        return;
    }

    InputEvent event = MakeEvent(InputEvent::Type::Focus);
    event.Focused = focused != 0;

    impl->Enqueue(std::move(event));
}

void OnIconify(GLFWwindow* handle, int iconified)
{
    WindowImpl* impl = ImplFor(handle);
    if (impl == nullptr)
    {
        return;
    }

    InputEvent event = MakeEvent(InputEvent::Type::Iconify);
    event.Iconified = iconified != 0;

    impl->Enqueue(std::move(event));
}

void OnClose(GLFWwindow* handle)
{
    WindowImpl* impl = ImplFor(handle);
    if (impl == nullptr)
    {
        return;
    }

    impl->CloseRequested = true;
    impl->Enqueue(MakeEvent(InputEvent::Type::CloseRequested));
}

void OnDrop(GLFWwindow* handle, int count, const char** paths)
{
    WindowImpl* impl = ImplFor(handle);
    if (impl == nullptr || paths == nullptr)
    {
        return;
    }

    // One event per file: dropping five assets is five editor actions, and a single batched event would make
    // every consumer special-case multi-file drops.
    for (int index = 0; index < count; ++index)
    {
        if (paths[index] == nullptr)
        {
            continue;
        }

        InputEvent event = MakeEvent(InputEvent::Type::DropFile);
        event.FilePath = paths[index];

        impl->Enqueue(std::move(event));
    }
}

void InstallCallbacks(GLFWwindow* handle)
{
    glfwSetKeyCallback(handle, OnKey);
    glfwSetCharCallback(handle, OnChar);
    glfwSetMouseButtonCallback(handle, OnMouseButton);
    glfwSetCursorPosCallback(handle, OnCursorPos);
    glfwSetScrollCallback(handle, OnScroll);
    glfwSetFramebufferSizeCallback(handle, OnFramebufferSize);
    glfwSetWindowFocusCallback(handle, OnFocus);
    glfwSetWindowIconifyCallback(handle, OnIconify);
    glfwSetWindowCloseCallback(handle, OnClose);
    glfwSetDropCallback(handle, OnDrop);
}

[[nodiscard]] int TranslateCursorMode(CursorMode mode) noexcept
{
    switch (mode)
    {
        case CursorMode::Normal:
            return GLFW_CURSOR_NORMAL;
        case CursorMode::Hidden:
            return GLFW_CURSOR_HIDDEN;
        case CursorMode::Disabled:
            return GLFW_CURSOR_DISABLED;
    }
    return GLFW_CURSOR_NORMAL;
}

[[nodiscard]] std::vector<std::string> CopyStringArray(const char* const* strings, int count)
{
    std::vector<std::string> result;
    if (strings == nullptr || count <= 0)
    {
        return result;
    }

    result.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index)
    {
        if (strings[index] != nullptr)
        {
            result.emplace_back(strings[index]);
        }
    }
    return result;
}

}  // namespace

}  // namespace Detail

// ============================================================================
// Window
// ============================================================================

Window* Window::Create(const WindowDesc& desc)
{
    Detail::WindowingState& state = Detail::GetWindowingState();

    if (state.ScopeCount <= 0)
    {
        HP_LOG_ERROR("PLATFORM", "Cannot create a window before PlatformScope is initialized");
        return nullptr;
    }

    if (!state.IsOnOwnerThread())
    {
        // Hard failure rather than assert-only: continuing corrupts GLFW's internal state on any backend, and
        // the corruption surfaces far from here.
        HP_LOG_ERROR("PLATFORM",
                     "Window::Create called from a thread other than the one that initialized windowing (GLFW is "
                     "single-threaded)");
        return nullptr;
    }

    if (desc.Width == 0u || desc.Height == 0u)
    {
        HP_LOG_ERROR("PLATFORM", "Cannot create a {}x{} window (size must be at least 1x1)", desc.Width, desc.Height);
        return nullptr;
    }

    // Clear GLFW's sticky error state first: an unrelated earlier failure would otherwise be reported as the
    // cause of this creation.
    glfwGetError(nullptr);

    glfwWindowHint(GLFW_VISIBLE, desc.Visible ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, desc.Resizable ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUSED, desc.Focused ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED, desc.Decorated ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_ICONIFIED, desc.Iconified ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, desc.TransparentFramebuffer ? GLFW_TRUE : GLFW_FALSE);

    // No client API: the renderer creates its own Vulkan context. Asking GLFW for an OpenGL context would both
    // conflict with Vulkan on some drivers and make a headless build depend on OpenGL being installed.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    auto* impl = new Detail::WindowImpl();
    impl->Title = desc.Title;

    GLFWwindow* handle = glfwCreateWindow(static_cast<int>(desc.Width), static_cast<int>(desc.Height),
                                          desc.Title.c_str(), nullptr, nullptr);

    if (handle == nullptr)
    {
        const int error = glfwGetError(nullptr);
        HP_LOG_ERROR("PLATFORM", "Failed to create a {}x{} window '{}' (GLFW error {})", desc.Width, desc.Height,
                     desc.Title, error);
        delete impl;
        return nullptr;
    }

    impl->Handle = handle;
    Detail::SetImpl(handle, impl);
    Detail::InstallCallbacks(handle);

    auto* window = new Window();
    window->m_Impl = impl;
    return window;
}

Window::~Window()
{
    if (m_Impl == nullptr)
    {
        return;
    }

    if (m_Impl->Handle != nullptr)
    {
        // Clearing the user pointer first prevents a late callback from dereferencing a dying Impl: GLFW does
        // not guarantee callbacks stop partway through a destroy on every backend.
        Detail::SetImpl(m_Impl->Handle, nullptr);
        glfwDestroyWindow(m_Impl->Handle);
    }

    delete m_Impl;
    m_Impl = nullptr;
}

Size Window::PollEvents(std::vector<InputEvent>& outEvents)
{
    if (m_Impl == nullptr || m_Impl->Handle == nullptr)
    {
        return 0;
    }

    // Callbacks append to the queue while this runs, which is why draining happens after polling rather than
    // swapping the queue out beforehand.
    glfwPollEvents();

    const Size appended = m_Impl->Queue.size();
    outEvents.insert(outEvents.end(), m_Impl->Queue.begin(), m_Impl->Queue.end());
    m_Impl->Queue.clear();

    // Re-arm overflow reporting for the next episode.
    m_Impl->OverflowReported = false;

    return appended;
}

bool Window::WaitEvents(double timeoutSeconds)
{
    if (m_Impl == nullptr || m_Impl->Handle == nullptr)
    {
        return false;
    }

    if (timeoutSeconds > 0.0)
    {
        glfwWaitEventsTimeout(timeoutSeconds);
    }
    else
    {
        glfwWaitEvents();
    }

    // The queue, not the wait's return value, is the authority on whether something arrived: some compositors
    // wake the wait spuriously.
    return !m_Impl->Queue.empty();
}

UInt32 Window::GetFramebufferWidth() const noexcept
{
    if (m_Impl == nullptr || m_Impl->Handle == nullptr)
    {
        return 0;
    }
    int width = 0;
    glfwGetFramebufferSize(m_Impl->Handle, &width, nullptr);
    return static_cast<UInt32>(width < 0 ? 0 : width);
}

UInt32 Window::GetFramebufferHeight() const noexcept
{
    if (m_Impl == nullptr || m_Impl->Handle == nullptr)
    {
        return 0;
    }
    int height = 0;
    glfwGetFramebufferSize(m_Impl->Handle, nullptr, &height);
    return static_cast<UInt32>(height < 0 ? 0 : height);
}

UInt32 Window::GetContentWidth() const noexcept
{
    if (m_Impl == nullptr || m_Impl->Handle == nullptr)
    {
        return 0;
    }
    int width = 0;
    glfwGetWindowSize(m_Impl->Handle, &width, nullptr);
    return static_cast<UInt32>(width < 0 ? 0 : width);
}

UInt32 Window::GetContentHeight() const noexcept
{
    if (m_Impl == nullptr || m_Impl->Handle == nullptr)
    {
        return 0;
    }
    int height = 0;
    glfwGetWindowSize(m_Impl->Handle, nullptr, &height);
    return static_cast<UInt32>(height < 0 ? 0 : height);
}

float Window::GetContentScale() const noexcept
{
    if (m_Impl == nullptr || m_Impl->Handle == nullptr)
    {
        return 1.0f;
    }

    float xs = 1.0f;
    float ys = 1.0f;
    glfwGetWindowContentScale(m_Impl->Handle, &xs, &ys);

    // The axes can differ on exotic displays. The engine assumes square pixels (every layout computation uses a
    // single scale), so the larger axis wins and the mismatch is reported: averaging would silently shrink
    // content by 25% on a 1x/2x split.
    if (xs > 0.0f && ys > 0.0f && std::fabs(xs - ys) > 0.01f)
    {
        HP_LOG_WARNING("PLATFORM", "Window has non-square content scale ({} x {}); using {}", xs, ys,
                       xs > ys ? xs : ys);
    }

    const float scale = xs > ys ? xs : ys;
    return scale > 0.0f ? scale : 1.0f;
}

void Window::SetTitle(std::string_view title)
{
    if (m_Impl == nullptr)
    {
        return;
    }

    m_Impl->Title.assign(title);
    if (m_Impl->Handle != nullptr)
    {
        glfwSetWindowTitle(m_Impl->Handle, m_Impl->Title.c_str());
    }
}

const std::string& Window::GetTitle() const noexcept
{
    static const std::string empty;
    return m_Impl != nullptr ? m_Impl->Title : empty;
}

void Window::SetContentSize(UInt32 width, UInt32 height)
{
    if (m_Impl == nullptr || m_Impl->Handle == nullptr)
    {
        return;
    }

    if (width == 0u || height == 0u)
    {
        // Rejected: most compositors refuse it, and a zero-size framebuffer makes every derived size in the
        // renderer divide by zero later. Reported rather than propagated.
        HP_LOG_WARNING("PLATFORM", "Ignoring a request to resize the window to {}x{}", width, height);
        return;
    }

    glfwSetWindowSize(m_Impl->Handle, static_cast<int>(width), static_cast<int>(height));
}

void Window::SetVisible(bool visible)
{
    if (m_Impl == nullptr || m_Impl->Handle == nullptr)
    {
        return;
    }

    if (visible)
    {
        glfwShowWindow(m_Impl->Handle);
    }
    else
    {
        glfwHideWindow(m_Impl->Handle);
    }
}

bool Window::IsVisible() const noexcept
{
    if (m_Impl == nullptr || m_Impl->Handle == nullptr)
    {
        return false;
    }
    return glfwGetWindowAttrib(m_Impl->Handle, GLFW_VISIBLE) != 0;
}

void Window::SetIconified(bool iconified)
{
    if (m_Impl == nullptr || m_Impl->Handle == nullptr)
    {
        return;
    }

    if (iconified)
    {
        glfwIconifyWindow(m_Impl->Handle);
    }
    else
    {
        glfwRestoreWindow(m_Impl->Handle);
    }
}

bool Window::IsIconified() const noexcept
{
    if (m_Impl == nullptr || m_Impl->Handle == nullptr)
    {
        return false;
    }
    return glfwGetWindowAttrib(m_Impl->Handle, GLFW_ICONIFIED) != 0;
}

void Window::Focus()
{
    if (m_Impl != nullptr && m_Impl->Handle != nullptr)
    {
        glfwFocusWindow(m_Impl->Handle);
    }
}

bool Window::IsFocused() const noexcept
{
    if (m_Impl == nullptr || m_Impl->Handle == nullptr)
    {
        return false;
    }
    return glfwGetWindowAttrib(m_Impl->Handle, GLFW_FOCUSED) != 0;
}

void Window::SetCursorMode(CursorMode mode) noexcept
{
    if (m_Impl == nullptr || m_Impl->Handle == nullptr)
    {
        return;
    }

    m_Impl->Cursor = mode;
    glfwSetInputMode(m_Impl->Handle, GLFW_CURSOR, Detail::TranslateCursorMode(mode));
}

CursorMode Window::GetCursorMode() const noexcept
{
    return m_Impl != nullptr ? m_Impl->Cursor : CursorMode::Normal;
}

void Window::SetCursorPosition(float x, float y)
{
    if (m_Impl == nullptr || m_Impl->Handle == nullptr)
    {
        return;
    }

    if (m_Impl->Cursor == CursorMode::Disabled)
    {
        // The pointer is locked by definition; GLFW accepts the call and the position snaps back on the next
        // motion event, which surprises callers, so it is refused explicitly.
        HP_LOG_WARNING("PLATFORM", "Ignoring SetCursorPosition while the cursor is captured (Disabled mode)");
        return;
    }

    glfwSetCursorPos(m_Impl->Handle, static_cast<double>(x), static_cast<double>(y));
}

bool Window::ShouldClose() const noexcept
{
    if (m_Impl == nullptr || m_Impl->Handle == nullptr)
    {
        return true;  // No window: a loop should not keep running.
    }
    return m_Impl->CloseRequested || glfwWindowShouldClose(m_Impl->Handle) != 0;
}

void Window::RequestClose() noexcept
{
    if (m_Impl == nullptr)
    {
        return;
    }
    m_Impl->CloseRequested = true;
    if (m_Impl->Handle != nullptr)
    {
        glfwSetWindowShouldClose(m_Impl->Handle, GLFW_TRUE);
    }
}

void Window::ResetCloseRequested() noexcept
{
    if (m_Impl == nullptr)
    {
        return;
    }
    m_Impl->CloseRequested = false;
    if (m_Impl->Handle != nullptr)
    {
        glfwSetWindowShouldClose(m_Impl->Handle, GLFW_FALSE);
    }
}

void Window::SetClipboardText(std::string_view text)
{
    if (m_Impl == nullptr || m_Impl->Handle == nullptr)
    {
        return;
    }

    // GLFW copies the string internally, so a temporary is safe here.
    const std::string owned(text);
    glfwSetClipboardString(m_Impl->Handle, owned.c_str());
}

std::string Window::GetClipboardText() const
{
    if (m_Impl == nullptr || m_Impl->Handle == nullptr)
    {
        return {};
    }

    // Null when the clipboard holds no text or the compositor refuses the read — normal on Wayland for an
    // unfocused window, so not logged as an error.
    const char* text = glfwGetClipboardString(m_Impl->Handle);
    return text != nullptr ? std::string(text) : std::string();
}

void Window::InjectEvent(const InputEvent& event)
{
    if (m_Impl == nullptr)
    {
        HP_LOG_WARNING("PLATFORM", "InjectEvent called on a window without an implementation");
        return;
    }

    m_Impl->Enqueue(event);
}

Size Window::GetPendingEventCount() const noexcept
{
    return m_Impl != nullptr ? m_Impl->Queue.size() : 0u;
}

bool Window::CreateVulkanSurface(void* vkInstance, void** outVkSurface) const noexcept
{
    if (outVkSurface == nullptr)
    {
        HP_LOG_ERROR("PLATFORM", "CreateVulkanSurface called with a null output pointer");
        return false;
    }

    *outVkSurface = nullptr;

    if (m_Impl == nullptr || m_Impl->Handle == nullptr || vkInstance == nullptr)
    {
        HP_LOG_ERROR("PLATFORM", "CreateVulkanSurface called without a window or a VkInstance");
        return false;
    }

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult result =
        glfwCreateWindowSurface(static_cast<VkInstance>(vkInstance), m_Impl->Handle, nullptr, &surface);

    if (result != VK_SUCCESS)
    {
        HP_LOG_ERROR("PLATFORM", "glfwCreateWindowSurface failed (VkResult {})", static_cast<int>(result));
        return false;
    }

    *outVkSurface = surface;
    return true;
}

std::vector<std::string> Window::GetRequiredVulkanInstanceExtensions() const
{
    uint32_t count = 0;
    const char** extensions = glfwGetRequiredInstanceExtensions(&count);
    return Detail::CopyStringArray(extensions, static_cast<int>(count));
}

void* Window::GetNativeHandle() const noexcept
{
#if defined(HP_PLATFORM_HAS_NATIVE_HEADERS)
    if (m_Impl == nullptr || m_Impl->Handle == nullptr)
    {
        return nullptr;
    }

    const Detail::WindowingState& state = Detail::GetWindowingState();

    #if defined(GLFW_EXPOSE_NATIVE_WAYLAND)
    if (state.Backend == WindowingBackend::Wayland)
    {
        return glfwGetWaylandWindow(m_Impl->Handle);
    }
    #endif
    #if defined(GLFW_EXPOSE_NATIVE_X11)
    if (state.Backend == WindowingBackend::X11)
    {
        // X11 window ids are 32-bit; widening through uintptr_t is the representation GLFW itself uses.
        return reinterpret_cast<void*>(static_cast<uintptr_t>(glfwGetX11Window(m_Impl->Handle)));
    }
    #endif
    #if defined(GLFW_EXPOSE_NATIVE_WIN32)
    if (state.Backend == WindowingBackend::Win32)
    {
        return glfwGetWin32Window(m_Impl->Handle);
    }
    #endif
    #if defined(GLFW_EXPOSE_NATIVE_COCOA)
    if (state.Backend == WindowingBackend::Cocoa)
    {
        return glfwGetCocoaWindow(m_Impl->Handle);
    }
    #endif
#endif

    // No native support compiled in (the usual case on a system without the Wayland/X11 development headers),
    // or the headless backend, which has no OS handle at all.
    return nullptr;
}

}  // namespace Hollowpoint::Platform
