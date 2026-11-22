# Hollowpoint Platform: window creation, input, and OS integration.
#
# The lowest layer that touches the outside world. It depends on GLFW but must not depend on ECS,
# Scene or Renderer — everything above it consumes windows and events, never the reverse.
#
# Native-handle access (HWND / wl_surface* / X11 window id) is compiled in only when the platform's
# development headers are present: glfw3native.h includes <wayland-client.h> / <X11/Xlib.h>, which are
# absent on ostree-based systems such as Bazzite where the runtime libraries exist but the headers do
# not. Making them mandatory would make the whole engine unbuildable there to support one accessor, so
# GetNativeHandle() returns null instead and reports it.

add_library(HollowpointPlatform STATIC)
hp_set_engine_target(HollowpointPlatform)

target_include_directories(HollowpointPlatform PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include")

target_sources(HollowpointPlatform PRIVATE
    Include/Hollowpoint/Platform/GlfwInclude.h
    Include/Hollowpoint/Platform/InputEvent.h
    Include/Hollowpoint/Platform/PlatformInit.h
    Include/Hollowpoint/Platform/Window.h
    Src/GlfwInputMapping.cpp
    Src/GlfwInputMapping.h
    Src/InputEvent.cpp
    Src/PlatformInit.cpp
    Src/Window.cpp
    Src/WindowImpl.h)

# --- Optional native window-handle support -----------------------------------
# Probed rather than assumed: the headers may exist for one backend and not another.
set(HP_PLATFORM_NATIVE_SOURCES "")
set(_hp_native_defs "")

if(NOT WIN32 AND NOT APPLE)
    find_file(HP_WAYLAND_CLIENT_HEADER wayland-client.h)
    if(HP_WAYLAND_CLIENT_HEADER)
        list(APPEND _hp_native_defs GLFW_EXPOSE_NATIVE_WAYLAND=1)
        message(STATUS "Hollowpoint Platform: Wayland native handles enabled")
    endif()

    # X11 needs the whole header *set*, not just Xlib.h: glfw3native.h's X11 section includes <X11/Xlib.h>, which in
    # turn includes <X11/X.h>, and a machine can ship part of that set (a runtime package dropping headers, or another
    # package installing Xlib.h alone) — as this one does. Probing for Xlib.h alone enabled GLFW_EXPOSE_NATIVE_X11 on a
    # tree that cannot compile it, so the build died inside the system header instead of disabling the accessor.
    find_file(HP_X11_XLIB_HEADER X11/Xlib.h)
    find_file(HP_X11_X_HEADER X11/X.h)
    if(HP_X11_XLIB_HEADER AND HP_X11_X_HEADER)
        list(APPEND _hp_native_defs GLFW_EXPOSE_NATIVE_X11=1)
        message(STATUS "Hollowpoint Platform: X11 native handles enabled")
    else()
        message(STATUS "Hollowpoint Platform: X11 native handles disabled (X11/Xlib.h or X11/X.h not installed)")
    endif()
elseif(WIN32)
    list(APPEND _hp_native_defs GLFW_EXPOSE_NATIVE_WIN32=1)
elseif(APPLE)
    list(APPEND _hp_native_defs GLFW_EXPOSE_NATIVE_COCOA=1)
endif()

if(_hp_native_defs)
    list(APPEND _hp_native_defs HP_PLATFORM_HAS_NATIVE_HEADERS=1)
endif()

target_compile_definitions(HollowpointPlatform PRIVATE ${_hp_native_defs})

# Vulkan::Headers is needed because GlfwInclude.h sets GLFW_INCLUDE_VULKAN (that is what declares
# glfwCreateWindowSurface); the Platform module itself never calls into Vulkan.
target_link_libraries(HollowpointPlatform PUBLIC
    Hollowpoint::Core
    Hollowpoint::CompilerSetup
    hp_glfw
    Vulkan::Headers)

add_library(Hollowpoint::Platform ALIAS HollowpointPlatform)
