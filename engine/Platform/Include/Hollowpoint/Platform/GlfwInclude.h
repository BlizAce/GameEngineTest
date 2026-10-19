#pragma once

// ---------------------------------------------------------------------------
// Include GLFW's C headers from C++ through this file, not directly.
//
// Same reasoning as Hollowpoint/Scripting/LuaInclude.h: GLFW's headers are C and contain constructs our
// engine warning policy rejects (old-style casts in macros), so including them directly breaks a clang
// -Werror build while passing the GCC one — i.e. it fails only the sanitiser preset, far from the commit
// that caused it. The suppression is scoped to the includes so engine code afterwards is still checked.
//
// glfw3native.h is deliberately NOT included here: it pulls in <wayland-client.h> / <X11/Xlib.h>, which
// are not present on every machine we build on (this project builds on ostree systems where the Wayland
// and X11 development headers are absent). Requiring them would make the whole engine unbuildable for one
// accessor. Native-handle support is therefore compiled in only when CMake finds those headers, gated by
// HP_PLATFORM_HAS_NATIVE_HEADERS — see Platform/CMakeLists.txt and Window.cpp.
// ---------------------------------------------------------------------------

#if defined(__clang__)
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wold-style-cast"
    #pragma clang diagnostic ignored "-Wreserved-identifier"
    #pragma clang diagnostic ignored "-Wpedantic"
#elif defined(__GNUC__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wold-style-cast"
    #pragma GCC diagnostic ignored "-Wpedantic"
#endif

// Both macros are required, and the combination is not redundant: glfw3.h includes <GL/gl.h> unless
// GLFW_INCLUDE_NONE is defined (verified against GLFW 3.5.1's header — the GL include is guarded by a
// chain of "already included some GL header" macros, none of which Vulkan defines), and it only declares
// glfwCreateWindowSurface / glfwVulkanSupported when GLFW_INCLUDE_VULKAN is set. Requesting no client API
// for the window itself is a separate runtime hint (GLFW_CLIENT_API = GLFW_NO_API).
#define GLFW_INCLUDE_NONE 1
#define GLFW_INCLUDE_VULKAN 1

extern "C"
{
#include <GLFW/glfw3.h>
}

#if defined(__clang__) || defined(__GNUC__)
    #pragma GCC diagnostic pop
#endif
