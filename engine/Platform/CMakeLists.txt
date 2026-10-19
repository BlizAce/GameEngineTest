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

    find_file(HP_X11_XLIB_HEADER X11/Xlib.h)
    if(HP_X11_XLIB_HEADER)
        list(APPEND _hp_native_defs GLFW_EXPOSE_NATIVE_X11=1)
        message(STATUS "Hollowpoint Platform: X11 native handles enabled")
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
