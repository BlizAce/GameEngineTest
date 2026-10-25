# Hollowpoint Game: scene-facing components and the scene → renderer bridge.
#
# This module exists to hold the seam between two layers that must stay independent. Renderer links neither ECS nor
# Graphics on purpose — it is a CPU shading reference plus a GLSL generator, and that is what lets its numeric
# equivalence tests run with no device and full leak checking. Scene knows about entities but nothing about draw calls.
# The types that legitimately need both (a drawable component, a per-frame render plan) live here instead of leaking
# into either, and conversion runs one way only: scene data in, render plan out.
#
# Linking Graphics is the one dependency worth justifying: the plan carries Graphics::CullMode because that is what a
# pipeline is finally created with. It is a header-only enum here — no device, no NVRHI — so tests of this module still
# need no GPU and keep leak checking.

add_library(HollowpointGame STATIC)
hp_set_engine_target(HollowpointGame)

target_include_directories(HollowpointGame PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include")

target_sources(HollowpointGame PRIVATE
    Include/Hollowpoint/Game/MeshRenderer.h
    Include/Hollowpoint/Game/RegisterGameTypes.h
    Include/Hollowpoint/Game/RenderPrimitives.h
    Include/Hollowpoint/Game/ForwardShaderLayout.h
    Include/Hollowpoint/Game/SceneRenderer.h
    Src/ForwardShaderLayout.cpp
    Src/RegisterGameTypes.cpp
    Src/RenderPrimitives.cpp
    Src/SceneRenderer.cpp)

target_link_libraries(HollowpointGame PUBLIC
    Hollowpoint::Core
    Hollowpoint::Math
    Hollowpoint::ECS
    Hollowpoint::Scene
    Hollowpoint::Renderer
    Hollowpoint::Graphics
    Hollowpoint::CompilerSetup)

add_library(Hollowpoint::Game ALIAS HollowpointGame)
