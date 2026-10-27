# Hollowpoint Runtime: the application shell and frame loop.
#
# This is the module that makes the engine run. Everything it drives already existed and had been tested in isolation —
# device, swapchain, frame ring, scene renderer, loader, scripting — but none of it had an owner: HP_BUILD_RUNTIME built
# nothing, and the only code that ever submitted a frame lived in a test file. The consequence was visible in FrameRing's own
# header ("tests and no production caller"): correct machinery nobody used. This module is where those pieces have to agree.
#
# Deliberately NOT here: an asset database (meshes come from BuiltInMeshes behind Game's GeometryResolver seam, so a glTF
# loader replaces one function pointer), physics and audio (neither module exists yet; where they slot into the frame order is
# documented on RuntimeApplication), and any editor UI — the loop is exposed as RunFrame() so an editor hosts it rather than
# duplicating it.

add_library(HollowpointRuntime STATIC)
hp_set_engine_target(HollowpointRuntime)

target_include_directories(HollowpointRuntime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include")

target_sources(HollowpointRuntime PRIVATE
    Include/Hollowpoint/Runtime/ApplicationHost.h
    Include/Hollowpoint/Runtime/BuiltInMeshes.h
    Include/Hollowpoint/Runtime/RuntimeApplication.h
    Include/Hollowpoint/Runtime/RuntimeConfig.h
    Include/Hollowpoint/Runtime/RuntimeConfigLoader.h
    Include/Hollowpoint/Runtime/Screenshot.h
    Src/BuiltInMeshes.cpp
    Src/RuntimeApplication.cpp
    Src/RuntimeConfigLoader.cpp
    Src/Screenshot.cpp)

target_link_libraries(HollowpointRuntime PUBLIC
    Hollowpoint::Core
    Hollowpoint::Math
    Hollowpoint::ECS
    Hollowpoint::Scene
    Hollowpoint::Platform
    Hollowpoint::Graphics
    Hollowpoint::Serialization
    Hollowpoint::Renderer
    Hollowpoint::Game
    Hollowpoint::CompilerSetup
    hp_stb)

# Scripting is optional at build level (HP_ENABLE_SCRIPTING). The runtime works without it — the frame loop's script step is
# skipped when the VM was never created — so the dependency is conditional rather than forcing every runtime build to link Lua.
if(TARGET HollowpointScripting)
    target_link_libraries(HollowpointRuntime PUBLIC Hollowpoint::Scripting)
    target_compile_definitions(HollowpointRuntime PUBLIC HP_RUNTIME_HAS_SCRIPTING=1)
endif()

add_library(Hollowpoint::Runtime ALIAS HollowpointRuntime)

# ---------------------------------------------------------------------------
# The standalone executable.
#
# Built here rather than in a top-level apps/ directory because it is part of this module's contract: RuntimeApplication is
# designed to be embedded (the editor hosts the same loop), and having exactly one caller in-tree keeps that design honest —
# an API nobody outside the module can call is not yet known to work.
#
# Thin by intent. Argument parsing and exit codes only; every behaviour worth testing lives in the library and is tested there.
# This target's existence is also the smoke test for the module graph: it links the whole engine top-down, so a cycle or a
# missing link dependency fails here rather than surfacing as an undefined symbol at some later point.
if(HP_BUILD_RUNTIME)
    add_executable(HollowpointRuntimeApp Src/Main.cpp)
    hp_set_engine_target(HollowpointRuntimeApp)

    target_link_libraries(HollowpointRuntimeApp PRIVATE Hollowpoint::Runtime)

    set_target_properties(HollowpointRuntimeApp PROPERTIES OUTPUT_NAME "hollowpoint-runtime" FOLDER "apps")
endif()
