# Hollowpoint Scene: placeable-entity components and the systems that derive world state from them.
#
# Sits above ECS + Math because it composes transforms down the hierarchy the ECS owns. It must not gain a
# Graphics or Renderer dependency: the renderer reads world matrices, never the other way round.

add_library(HollowpointScene STATIC)
hp_set_engine_target(HollowpointScene)

target_include_directories(HollowpointScene PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include")

target_sources(HollowpointScene PRIVATE
    Include/Hollowpoint/Scene/LocalTransform.h
    Include/Hollowpoint/Scene/SceneIO.h
    Include/Hollowpoint/Scene/TransformHierarchy.h
    Src/SceneIO.cpp
    Src/TransformHierarchy.cpp)

# SceneIO needs the reflection-driven bridge to describe arbitrary components, and MathReflection so transforms (which
# are Vec3/Quat fields) can be described at all. Both are opt-in layers by design; taking them here is what makes a
# placeable entity saveable without the ECS or Math modules knowing about serialisation.
target_link_libraries(HollowpointScene PUBLIC
    Hollowpoint::Core
    Hollowpoint::Math
    Hollowpoint::ECS
    Hollowpoint::Reflection
    Hollowpoint::MathReflection
    Hollowpoint::Serialization
    Hollowpoint::SerializationReflection
    Hollowpoint::CompilerSetup)

add_library(Hollowpoint::Scene ALIAS HollowpointScene)
