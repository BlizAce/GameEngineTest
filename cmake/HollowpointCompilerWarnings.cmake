# Compiler and linker configuration for Hollowpoint.
# Included from the root CMakeLists.txt; provides hp_set_engine_target() which applies the
# project-wide warning policy to engine (non-third-party) targets.
#
# Optimization levels are left at CMake's defaults (Debug -O0 -g, RelWithDebInfo -O2 -g,
# Release -O2). Anything more exotic belongs in a CI preset, not in the project file.

include_guard(GLOBAL)

option(HP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors for engine code" ON)

# Development-build switches. All three are OFF for shipping builds and ON for the Debug preset, so
# that a normal development run reports everything worth reporting.
option(HP_TRACK_ALLOCATIONS "Record allocation statistics (memory HUD, leak checks)" OFF)
option(HP_ENABLE_TRACE_LOGS "Compile in HP_LOG_TRACE call sites" OFF)

# ---------------------------------------------------------------------------
# Hollowpoint::CompilerSetup — interface target carrying the project-wide compile features and
# feature macros that every engine module needs (C++20, strict conformance, symbol visibility).
# Link it PUBLIC.
# ---------------------------------------------------------------------------
add_library(HollowpointCompilerSetup INTERFACE)
target_compile_features(HollowpointCompilerSetup INTERFACE cxx_std_20)
set_target_properties(HollowpointCompilerSetup PROPERTIES INTERFACE_POSITION_INDEPENDENT_CODE ON)

# Engine modules are static, so no export decoration is needed anywhere. Shared-module builds set
# HP_<MODULE>_EXPORTS per target instead (see engine/*/CMakeLists.txt).
target_compile_definitions(HollowpointCompilerSetup INTERFACE HP_STATIC_ENGINE=1)

if(HP_TRACK_ALLOCATIONS)
    target_compile_definitions(HollowpointCompilerSetup INTERFACE HP_TRACK_ALLOCATIONS=1)
endif()

if(HP_ENABLE_TRACE_LOGS)
    target_compile_definitions(HollowpointCompilerSetup INTERFACE HP_ENABLE_TRACE_LOGS=1)
endif()

if(NOT MSVC)
    # NVRHI's Vulkan backend uses the dynamic C++ dispatch loader, which requires RTTI and exceptions.
    target_compile_options(HollowpointCompilerSetup INTERFACE -frtti -fexceptions)
endif()

add_library(Hollowpoint::CompilerSetup ALIAS HollowpointCompilerSetup)

# ---------------------------------------------------------------------------
# hp_set_engine_target(<target>)
# Applies Hollowpoint's warning policy. Third-party targets are deliberately NOT run through
# this function: they build with relaxed warnings so upstream code cannot break our build.
# ---------------------------------------------------------------------------
function(hp_set_engine_target TARGET)
    if(MSVC)
        target_compile_options(${TARGET} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
        if(HP_WARNINGS_AS_ERRORS)
            target_compile_options(${TARGET} PRIVATE /WX)
        endif()
    else()
        # Flags every supported compiler (GCC and Clang, including AppleClang) understands.
        target_compile_options(${TARGET} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wshadow                      # shadowing is a classic source of stale-state bugs
            -Wnon-virtual-dtor
            -Wcast-align
            -Wimplicit-fallthrough
            -Wold-style-cast)

        # GCC-specific diagnostics. Clang accepts -Wno-... for unknown names but errors on unknown
        # positive warnings, so these are gated on the compiler id — otherwise a macOS or sanitizer
        # build fails at the first file.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${TARGET} PRIVATE
                -Walloc-zero
                -Wduplicated-branches
                -Wduplicated-cond
                -Wmaybe-uninitialized)
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${TARGET} PRIVATE
                -Wdelete-non-virtual-dtor
                -Wrange-loop-analysis
                -Wdocumentation)
        endif()

        if(HP_WARNINGS_AS_ERRORS)
            target_compile_options(${TARGET} PRIVATE -Werror)
        endif()
    endif()

    set_target_properties(${TARGET} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
endfunction()

# ---------------------------------------------------------------------------
# hp_set_third_party_target(<target>)
# Silences warnings for vendored code without changing its optimization level.
# ---------------------------------------------------------------------------
function(hp_set_third_party_target TARGET)
    if(MSVC)
        target_compile_options(${TARGET} PRIVATE /W0)
    else()
        target_compile_options(${TARGET} PRIVATE -w)
    endif()
endfunction()
