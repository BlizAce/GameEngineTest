# Hollowpoint Renderer: the forward renderer — passes, materials, lighting and post-processing.
#
# Sits above Graphics (which owns devices/resources) and consumes ECS/Scene for what to draw. The CPU shading
# reference lives here deliberately: it defines what the GPU shaders must compute, so a mismatch is a bug with an
# expected value rather than a disagreement about appearance.

add_library(HollowpointRenderer STATIC)
hp_set_engine_target(HollowpointRenderer)

target_include_directories(HollowpointRenderer PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include")

target_sources(HollowpointRenderer PRIVATE
    Include/Hollowpoint/Renderer/PbrShading.h
    Src/PbrShading.cpp)

# Graphics is not linked yet: the CPU shading reference is pure math, and keeping it free of the RHI means the
# shading model can be tested (and used by tooling) without a GPU or a Vulkan loader.
target_link_libraries(HollowpointRenderer PUBLIC
    Hollowpoint::Core
    Hollowpoint::Math
    Hollowpoint::CompilerSetup)

add_library(Hollowpoint::Renderer ALIAS HollowpointRenderer)
