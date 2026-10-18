# Hollowpoint ECS: entities, components and the world container that systems run against.
#
# Depends on Core only. Component *types* are registered with Reflection by whoever owns them (the game
# module, or the renderer for its own components), which keeps the ECS independent of both reflection's
# policies and any concrete component — and lets this header set stay includable from hot code.

add_library(HollowpointECS STATIC)
hp_set_engine_target(HollowpointECS)

target_include_directories(HollowpointECS PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include")

target_sources(HollowpointECS PRIVATE
    Include/Hollowpoint/ECS/Detail/SparseSet.h
    Include/Hollowpoint/ECS/Entity.h
    Src/Entity.cpp)

target_link_libraries(HollowpointECS PUBLIC
    Hollowpoint::Core Hollowpoint::CompilerSetup)

add_library(Hollowpoint::ECS ALIAS HollowpointECS)
