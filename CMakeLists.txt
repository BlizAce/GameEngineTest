# Hollowpoint — a production-grade 3D game engine.
# Root build file: configures third-party dependencies, engine modules, tools and tests.
cmake_minimum_required(VERSION 3.25)

project(Hollowpoint
    VERSION 0.1.0
    DESCRIPTION "Hollowpoint — production-grade 3D game engine (Windows, macOS, Linux)"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Hollowpoint: no build type specified, defaulting to Debug")
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(HOLLOWPOINT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(HOLLOWPOINT_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

include(HollowpointCompilerWarnings)
include(HollowpointPlatform)
include(HollowpointRevision)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(HP_BUILD_TESTS "Build the Hollowpoint unit test suites" ON)
option(HP_BUILD_EDITOR "Build the Hollowpoint editor executable" ON)
option(HP_BUILD_RUNTIME "Build the standalone runtime executable" ON)
option(HP_ENABLE_VULKAN "Build the Vulkan (NVRHI) graphics backend" ON)
option(HP_ENABLE_PHYSICS "Build the physics module (Jolt)" ON)
option(HP_ENABLE_AUDIO "Build the audio module (miniaudio)" ON)
option(HP_ENABLE_SCRIPTING "Build the Lua scripting module" ON)
option(HP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors for engine code" ON)

# Shader authoring format (orthogonal to platform — both compile to SPIR-V):
#   GLSL - authored in GLSL, compiled with glslang (default; available on all three platforms).
#   HLSL - authored in HLSL, compiled with DXC (-spirv). Requires a DXC install or HP_DXC_EXECUTABLE.
set(HP_SHADER_PROFILE "GLSL" CACHE STRING "Shader authoring language: GLSL or HLSL")
set_property(CACHE HP_SHADER_PROFILE PROPERTY STRINGS GLSL HLSL)
if(NOT HP_SHADER_PROFILE MATCHES "^(GLSL|HLSL)$")
    message(FATAL_ERROR "HP_SHADER_PROFILE must be GLSL or HLSL, got '${HP_SHADER_PROFILE}'")
endif()
message(STATUS "Hollowpoint: shader profile = ${HP_SHADER_PROFILE}")

if(NOT HP_ENABLE_VULKAN AND NOT HP_BUILD_TESTS)
    message(WARNING "Hollowpoint: Vulkan disabled and tests disabled — nothing meaningful will be built.")
endif()

# ---------------------------------------------------------------------------
# Third-party dependencies (all pinned; see third_party/CMakeLists.txt)
# ---------------------------------------------------------------------------
add_subdirectory(third_party)

# ---------------------------------------------------------------------------
# Engine modules (static libraries), added in dependency order as they land; see
# docs/ARCHITECTURE.md (Core -> Math -> ECS -> ... -> Renderer).
# ---------------------------------------------------------------------------
add_subdirectory(engine/Core)
add_subdirectory(engine/Math)
add_subdirectory(engine/Reflection)
add_subdirectory(engine/ECS)

if(HP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "-------------------------------------------------------------------")
message(STATUS "Hollowpoint ${PROJECT_VERSION}")
message(STATUS "  Platform       : ${CMAKE_SYSTEM_NAME} (${CMAKE_SYSTEM_PROCESSOR})")
message(STATUS "  Compiler       : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Build type     : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Vulkan/GPU     : ${HP_ENABLE_VULKAN}")
message(STATUS "  Physics        : ${HP_ENABLE_PHYSICS}")
message(STATUS "  Audio          : ${HP_ENABLE_AUDIO}")
message(STATUS "  Scripting      : ${HP_ENABLE_SCRIPTING}")
message(STATUS "  Tests          : ${HP_BUILD_TESTS}")
message(STATUS "  Editor         : ${HP_BUILD_EDITOR}")
message(STATUS "  Runtime        : ${HP_BUILD_RUNTIME}")
message(STATUS "-------------------------------------------------------------------")
