# Hollowpoint third-party dependencies.
#
# Policy (see AGENTS.md):
#   * Every dependency is pinned to an exact tag or commit — no branches, no floating refs.
#   * Dependencies are fetched with FetchContent so a fresh clone builds with no manual setup.
#     Point HP_DEPS_ROOT at a shared directory (or set FETCHCONTENT_BASE_DIR) to reuse checkouts
#     between build trees and to build offline.
#   * Vendored code is never passed through hp_set_engine_target(): upstream warnings must not break
#     our build. Their compile ABI settings still match ours, so mixing remains safe.

include(FetchContent)

if(DEFINED HP_DEPS_ROOT)
    set(FETCHCONTENT_BASE_DIR "${HP_DEPS_ROOT}")
endif()

set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------------------
# glm — header-only math (vectors, matrices, quaternions).
# ---------------------------------------------------------------------------
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.3
    GIT_SHALLOW TRUE)

# ---------------------------------------------------------------------------
# doctest — lightweight header-only unit test framework (see tests/).
# ---------------------------------------------------------------------------
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.5.3
    GIT_SHALLOW TRUE)

# ---------------------------------------------------------------------------
# nlohmann_json — JSON for scene files, project settings and asset metadata.
# ---------------------------------------------------------------------------
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE)

# ---------------------------------------------------------------------------
# cgltf — single-header glTF 2.0 loader (meshes, materials, textures, animation).
# Upstream ships no CMakeLists in this revision, so an interface target is created below.
# ---------------------------------------------------------------------------
FetchContent_Declare(
    cgltf
    GIT_REPOSITORY https://github.com/zeux/cgltf.git
    GIT_TAG 85cd62382dfea638278962690cf515023f33ed00)

# ---------------------------------------------------------------------------
# stb — single-header image codecs (PNG/JPEG/HDR) used by the texture importer.
# ---------------------------------------------------------------------------
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20)

# ---------------------------------------------------------------------------
# GLFW — window creation and input on Windows, macOS and Linux.
#
# Building GLFW from source needs the platform's windowing *development* packages (X11/Wayland/OpenGL
# headers on Linux), which a plain server or container does not carry. HP_USE_SYSTEM_GLFW (default ON)
# therefore prefers an installed glfw3 CMake package and only falls back to fetching and building when
# none is found — the same "prefer the platform, else vendor" order we use for the Vulkan loader.
# ---------------------------------------------------------------------------
option(HP_USE_SYSTEM_GLFW "Use an installed GLFW package instead of building the fetched one" ON)

if(HP_ENABLE_VULKAN)
    set(hp_glfw_target "")

    if(HP_USE_SYSTEM_GLFW)
        # No package prefix is searched by default, so an explicit hint (or an environment variable)
        # is what makes an installed GLFW discoverable. Prefer that over a source build on machines
        # without the X11/Wayland development headers.
        set(HP_Glfw_DIR "" CACHE PATH "Directory containing glfw3Config.cmake (empty = search default paths)")
        if(HP_Glfw_DIR)
            find_package(glfw3 CONFIG QUIET HINTS "${HP_Glfw_DIR}" NO_CMAKE_FIND_ROOT_PATH)
        else()
            find_package(glfw3 CONFIG QUIET)
        endif()

        if(TARGET glfw)
            set(hp_glfw_target glfw)
            get_target_property(_glfwLocation glfw LOCATION)
            message(STATUS "Hollowpoint: using installed GLFW (${_glfwLocation})")
        else()
            message(STATUS "Hollowpoint: no installed GLFW found, building the fetched copy "
                           "(needs X11/Wayland development headers on Linux)")
        endif()
    endif()

    if(NOT hp_glfw_target)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

        FetchContent_Declare(
            glfw
            GIT_REPOSITORY https://github.com/glfw/glfw.git
            GIT_TAG 3.5.1
            GIT_SHALLOW TRUE)

    endif()
endif()

# ---------------------------------------------------------------------------
# NVRHI — graphics API abstraction. Vulkan backend only; the D3D backends are Windows-only and
# disabled here to keep build times down (enabling them is a cache switch, not a code change).
# ---------------------------------------------------------------------------
if(HP_ENABLE_VULKAN)
    set(NVRHI_WITH_VALIDATION ON CACHE BOOL "" FORCE)
    set(NVRHI_WITH_VULKAN ON CACHE BOOL "" FORCE)
    set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_DX12 OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_RTXMU OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_AFTERMATH OFF CACHE BOOL "" FORCE)
    set(NVRHI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(NVRHI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NVRHI_INSTALL OFF CACHE BOOL "" FORCE)
    # Vulkan-Headers is declared once by us and shared with NVRHI, so it must not fetch its own copy.
    set(NVRHI_FETCH_VULKAN_HEADERS OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        nvrhi
        GIT_REPOSITORY https://github.com/NVIDIA-RTX/NVRHI.git
        GIT_TAG 6b96fb03e07539f08327aea76c56d55f1de9d906)

    # Pinned to exactly the tag NVRHI's own build expects, so both see identical types.
    FetchContent_Declare(
        vulkan_headers
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG v1.4.352
        GIT_SHALLOW TRUE)
endif()

# ---------------------------------------------------------------------------
# glslang — offline shader compilation (GLSL/HLSL -> SPIR-V) used by HollowpointShaderC.
#
# glslang expects its dependencies to be submodules inside its own source tree, which FetchContent
# does not populate recursively. Instead we declare spirv-headers and spirv-tools ourselves, in the
# order their CMake scripts require, and hand the resulting targets to glslang. The pinned SPIRV
# commits are the ones listed in glslang's known_good.json — verified with
# scripts/check_dependency_pins.py.
# ---------------------------------------------------------------------------
if(HP_ENABLE_VULKAN)
    set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
    set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
    set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        spirv_headers
        GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
        GIT_TAG 496543121ce6419f23d6fa5d7194ba66c36212d2)

    FetchContent_Declare(
        spirv_tools
        GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Tools.git
        GIT_TAG ef96ed763b43b59b33b31b362f09a02b729fa1c9)

    set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)  # we link the library, and use its CLI in a pinch
    set(ENABLE_HLSL ON CACHE BOOL "" FORCE)              # HLSL front-end for HP_SHADER_PROFILE=HLSL
    set(ENABLE_OPT ON CACHE BOOL "" FORCE)               # SPIR-V optimizer
    set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)          # provided above
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_WERROR OFF CACHE BOOL "" FORCE)
    set(ENABLE_PCH OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        glslang
        GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
        GIT_TAG vulkan-sdk-1.4.363.0
        GIT_SHALLOW TRUE)
endif()

# ---------------------------------------------------------------------------
# Jolt Physics — 3D rigid body, character and vehicle physics.
# Built from its Build/ subdirectory; the Vulkan/DX12 debug-renderer integrations stay off because
# Hollowpoint draws its own debug geometry through the graphics module.
# ---------------------------------------------------------------------------
if(HP_ENABLE_PHYSICS)
    set(TARGET_FRAMEWORK OFF CACHE BOOL "" FORCE)
    set(JPH_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(JPH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
    set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
    set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
    set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
    set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(OVERRIDE_CXX_FLAGS ON CACHE BOOL "" FORCE)
    set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
    set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    # Hollowpoint relies on dynamic_cast for component handles and script bindings.
    set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
    # Deterministic simulation across platforms is required for networked play and replays.
    set(CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)

    FetchContent_Declare(
        joltphysics
        GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
        GIT_TAG v5.6.0
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR Build)
endif()

# ---------------------------------------------------------------------------
# miniaudio — cross-platform audio output (WASAPI/XAudio2, CoreAudio, ALSA/PulseAudio).
# A single .c file plus headers; compiled into a static library.
# ---------------------------------------------------------------------------
if(HP_ENABLE_AUDIO)
    FetchContent_Declare(
        miniaudio
        GIT_REPOSITORY https://github.com/mackron/miniaudio.git
        GIT_TAG 0.11.25
        GIT_SHALLOW TRUE)
endif()

# ---------------------------------------------------------------------------
# Lua + sol2 — gameplay scripting.
#
# Lua's upstream repository ships a makefile only, so the sources are compiled into a static library
# by us (which also guarantees C linkage and our ABI). sol2 is header-only; its own CMake insists on
# locating a *system* Lua installation, which is precisely what we do not want when vendoring Lua, so
# it is exposed as an interface target instead of being added as a subdirectory.
# ---------------------------------------------------------------------------
if(HP_ENABLE_SCRIPTING)
    FetchContent_Declare(
        lua
        GIT_REPOSITORY https://github.com/lua/lua.git
        GIT_TAG v5.4.9)

    FetchContent_Declare(
        sol2
        GIT_REPOSITORY https://github.com/ThePhD/sol2.git
        GIT_TAG v3.5.0
        GIT_SHALLOW TRUE)
endif()

# ===========================================================================
# Populate
# ===========================================================================

# FetchContent_Populate is still required here (we must control the order in which these three are
# configured, which MakeAvailable does not allow), but CMake 3.30+ deprecates the direct call. Scope
# the policy override to just those calls so the rest of the project keeps the new behaviour.
macro(hp_populate_quietly name)
    if(POLICY CMP0169)
        cmake_policy(PUSH)
        cmake_policy(SET CMP0169 OLD)
        FetchContent_Populate(${name})
        cmake_policy(POP)
    else()
        FetchContent_Populate(${name})
    endif()
endmacro()

if(HP_ENABLE_VULKAN)
    # SPIRV-Tools looks for headers either at external/spirv-headers or wherever this cache variable
    # points; populate the headers first and point it there.
    hp_populate_quietly(spirv_headers)
    set(SPIRV-Headers_SOURCE_DIR "${spirv_headers_SOURCE_DIR}" CACHE PATH "" FORCE)

    hp_populate_quietly(spirv_tools)

    # SPIRV-Tools: build only the static library and the optimiser pass we call from the shader
    # compiler; its own executables and tests are switched off above.
    add_subdirectory("${spirv_tools_SOURCE_DIR}" "${spirv_tools_BINARY_DIR}" EXCLUDE_FROM_ALL)

    hp_populate_quietly(glslang)
    add_subdirectory("${glslang_SOURCE_DIR}" "${glslang_BINARY_DIR}" EXCLUDE_FROM_ALL)
endif()

set(hp_dependencies glm nlohmann_json cgltf stb)
if(HP_BUILD_TESTS)
    list(APPEND hp_dependencies doctest)
endif()
if(HP_ENABLE_VULKAN)
    list(APPEND hp_dependencies vulkan_headers nvrhi)
    if(NOT hp_glfw_target)
        # Only fetch/build GLFW when no installed package was used (see the option above).
        list(APPEND hp_dependencies glfw)
        set(hp_glfw_target glfw)
    endif()
endif()
if(HP_ENABLE_PHYSICS)
    list(APPEND hp_dependencies joltphysics)
endif()

FetchContent_MakeAvailable(${hp_dependencies})

if(HP_ENABLE_SCRIPTING)
    hp_populate_quietly(lua)
    file(GLOB HP_LUA_SOURCES "${lua_SOURCE_DIR}/*.c")
    # lua.c is the standalone interpreter and onelua.c the single-TU amalgamation; neither belongs
    # in an engine build.
    list(REMOVE_ITEM HP_LUA_SOURCES "${lua_SOURCE_DIR}/lua.c" "${lua_SOURCE_DIR}/onelua.c")
    add_library(lua_static STATIC ${HP_LUA_SOURCES})
    target_include_directories(lua_static PUBLIC "${lua_SOURCE_DIR}")
    hp_set_third_party_target(lua_static)
    if(NOT MSVC)
        target_link_libraries(lua_static PUBLIC m)
    endif()
    add_library(Hollowpoint::Lua ALIAS lua_static)

    hp_populate_quietly(sol2)
    add_library(hp_sol2 INTERFACE)
    target_include_directories(hp_sol2 SYSTEM INTERFACE "${sol2_SOURCE_DIR}/include")
    target_link_libraries(hp_sol2 INTERFACE lua_static)
endif()

# ---------------------------------------------------------------------------
# Interface targets for the header-only / non-CMake dependencies.
# ---------------------------------------------------------------------------
if(NOT TARGET cgltf)
    add_library(hp_cgltf INTERFACE)
    target_include_directories(hp_cgltf SYSTEM INTERFACE "${cgltf_SOURCE_DIR}")
elseif(TARGET cgltf AND NOT TARGET hp_cgltf)
    add_library(hp_cgltf ALIAS cgltf)
endif()

if(NOT TARGET hp_stb)
    add_library(hp_stb INTERFACE)
    target_include_directories(hp_stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")
endif()

if(HP_ENABLE_AUDIO AND NOT TARGET hp_miniaudio)
    hp_populate_quietly(miniaudio)
    add_library(hp_miniaudio STATIC "${miniaudio_SOURCE_DIR}/miniaudio.c")
    target_include_directories(hp_miniaudio SYSTEM PUBLIC "${miniaudio_SOURCE_DIR}")
    hp_set_third_party_target(hp_miniaudio)
    if(NOT MSVC)
        target_link_libraries(hp_miniaudio PUBLIC m)
    endif()
endif()

# Jolt builds a plain `Jolt` target as a subproject; alias the namespaced form used by engine code.
if(HP_ENABLE_PHYSICS AND TARGET Jolt AND NOT TARGET Jolt::Jolt)
    add_library(Jolt::Jolt ALIAS Jolt)
endif()

# ---------------------------------------------------------------------------
# Aggregate interface target consumed by every engine module.
# ---------------------------------------------------------------------------
add_library(HollowpointThirdParty INTERFACE)
target_link_libraries(HollowpointThirdParty INTERFACE glm nlohmann_json::nlohmann_json hp_cgltf hp_stb)

if(HP_ENABLE_VULKAN)
    # `glfw` is an IMPORTED target, and imported targets are visible only in the directory that created them
    # and its subdirectories — so engine modules (siblings of third_party) cannot link it by name. Wrapping it
    # in an INTERFACE library turns it into a normal target whose usage requirements propagate everywhere,
    # which is how hp_sol2 already works for scripting.
    add_library(hp_glfw INTERFACE)
    target_link_libraries(hp_glfw INTERFACE ${hp_glfw_target})

    target_link_libraries(HollowpointThirdParty INTERFACE hp_glfw Vulkan::Headers nvrhi nvrhi_vk)
endif()
if(HP_ENABLE_PHYSICS AND TARGET Jolt::Jolt)
    target_link_libraries(HollowpointThirdParty INTERFACE Jolt::Jolt)
endif()
if(HP_ENABLE_SCRIPTING)
    target_link_libraries(HollowpointThirdParty INTERFACE hp_sol2)
endif()
if(HP_ENABLE_AUDIO)
    target_link_libraries(HollowpointThirdParty INTERFACE hp_miniaudio)
endif()

add_library(Hollowpoint::ThirdParty ALIAS HollowpointThirdParty)

# ---------------------------------------------------------------------------
# glslang targets, exposed for the shader compiler tool (tools/HollowpointShaderC).
# Kept out of the aggregate interface: only one tool needs them.
# ---------------------------------------------------------------------------
if(HP_ENABLE_VULKAN)
    set(HP_GLSLANG_TARGETS glslang MachineIndependent GenericCodeGen SPIRV SPIRV-Tools-opt SPIRV-Tools
        glslang-default-resource-limits CACHE STRING "glslang link targets for the shader compiler")
endif()
