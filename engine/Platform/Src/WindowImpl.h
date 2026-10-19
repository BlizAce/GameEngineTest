#pragma once

#include <deque>
#include <string>
#include <thread>

#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Platform/GlfwInclude.h"
#include "Hollowpoint/Platform/InputEvent.h"
#include "Hollowpoint/Platform/PlatformInit.h"
#include "Hollowpoint/Platform/Window.h"

namespace Hollowpoint::Platform::Detail
{

// ---------------------------------------------------------------------------
// Shared internal state for the Platform module: the windowing-library lifetime, and the concrete
// Window implementation. Both live here rather than in the public headers so GLFW types stay out of
// anything engine code includes.
// ---------------------------------------------------------------------------

//! Process-wide windowing state. One instance, accessed through GetWindowingState().
struct WindowingState
{
    int ScopeCount = 0;
    WindowingBackend Backend = WindowingBackend::None;
    std::string FailureReason;
    bool ErrorCallbackInstalled = false;

    //! Thread that initialised GLFW. Every entry point checks it: GLFW is documented single-threaded and
    //! violating that corrupts its internal state, producing crashes on the compositor thread with no engine
    //! frame on the stack — days to diagnose otherwise.
    std::thread::id OwnerThread = std::this_thread::get_id();

    [[nodiscard]] bool IsOnOwnerThread() const noexcept
    {
        return std::this_thread::get_id() == OwnerThread;
    }
};

[[nodiscard]] WindowingState& GetWindowingState();

//! The concrete window. Held by the public Window through an opaque pointer so GLFWwindow* never appears in
//! a public header.
struct WindowImpl
{
    GLFWwindow* Handle = nullptr;

    //! A deque rather than a vector: the overflow policy drops from the front, and erasing the front of a vector is
    //! O(n) per event — an application that stops pumping input while keys repeat would then spend its time
    //! memmoving events (with their file-path strings) instead of doing nothing. pop_front is O(1).
    std::deque<InputEvent> Queue;
    std::string Title;
    CursorMode Cursor = CursorMode::Normal;
    bool CloseRequested = false;

    //! Input queue cap. A paused or backgrounded application never pumps, and a held key generates repeats
    //! forever — unbounded growth here is an out-of-memory condition caused by doing nothing. When exceeded
    //! the oldest events are dropped (newest input is what gameplay needs) and it is reported once per
    //! overflow episode rather than on every event.
    static constexpr std::size_t MaxQueuedEvents = 16384;

    //! How many events to discard at once when the cap is hit. Dropping a chunk keeps the queue from sitting exactly
    //! at the limit and re-entering the overflow path for every subsequent event.
    static constexpr std::size_t OverflowDropCount = MaxQueuedEvents / 8;
    bool OverflowReported = false;

    //! Appends, applying the cap below by discarding the oldest events in one chunk (a per-event drop would do the
    //! bookkeeping once per overflow rather than once per episode).
    void Enqueue(InputEvent event);

    // GLFW callbacks are static members of Window (declared in the class, defined here) and recover the
    // Impl from the window's user pointer.
};

[[nodiscard]] WindowImpl* GetImpl(GLFWwindow* handle) noexcept;
void SetImpl(GLFWwindow* handle, WindowImpl* impl) noexcept;

}  // namespace Hollowpoint::Platform::Detail
