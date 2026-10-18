# Platform detection and per-platform link/setup helpers for Hollowpoint.

include_guard(GLOBAL)

if(WIN32)
    set(HP_PLATFORM_NAME "Windows")
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(APPLE)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(HP_PLATFORM_NAME "macOS")
    else()
        set(HP_PLATFORM_NAME "${CMAKE_SYSTEM_NAME}")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(HP_PLATFORM_NAME "Linux")
else()
    set(HP_PLATFORM_NAME "${CMAKE_SYSTEM_NAME}")
endif()

# HP_RUNTIME_OUTPUT_DIR keeps every executable and its side-by-side assets (shaders, configs)
# in one place so that a built engine can be launched and tested from CI without installing.
set(HP_OUTPUT_ROOT "${CMAKE_BINARY_DIR}/output")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${HP_OUTPUT_ROOT}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${HP_OUTPUT_ROOT}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ---------------------------------------------------------------------------
# hp_link_platform_libs(<target>)
# Links the system libraries required by the windowing/graphics layer on each platform.
# ---------------------------------------------------------------------------
function(hp_link_platform_libs TARGET)
    if(WIN32)
        target_link_libraries(${TARGET} PUBLIC ws2_32 user32 gdi32 shell32 ole32 winipsec)
    elseif(APPLE)
        find_library(COCOA_LIBRARY Cocoa REQUIRED)
        find_library(IOKIT_LIBRARY IOKit REQUIRED)
        target_link_libraries(${TARGET} PUBLIC "${COCOA_LIBRARY}" "${IOKIT_LIBRARY}")
    else()
        # The Vulkan loader is dlopen'd by NVRHI, so we only need libdl here.
        target_link_libraries(${TARGET} PRIVATE ${CMAKE_DL_LIBS})
    endif()
endfunction()
