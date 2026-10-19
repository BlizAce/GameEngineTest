# Hollowpoint Scripting: the Lua VM, its safety policy, and the engine API exposed to gameplay code.
#
# Depends on ECS + Scene because its whole purpose is letting scripts create/destroy entities, spawn prefabs and read
# or write components. It must not gain a Renderer or Platform dependency: scripts run in tools (asset builders, the
# editor's preview world) that have no window and no GPU, and linking those would make headless tooling impossible.

add_library(HollowpointScripting STATIC)
hp_set_engine_target(HollowpointScripting)

target_include_directories(HollowpointScripting PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include")

target_sources(HollowpointScripting PRIVATE
    Include/Hollowpoint/Scripting/EngineAPI.h
    Include/Hollowpoint/Scripting/LuaInclude.h
    Include/Hollowpoint/Scripting/EntityHandle.h
    Include/Hollowpoint/Scripting/ScriptSystem.h
    Include/Hollowpoint/Scripting/ScriptVM.h
    Src/EngineAPI.cpp
    Src/EntityHandle.cpp
    Src/ScriptSystem.cpp
    Src/ScriptVM.cpp)

# sol2 is header-only and SYSTEM-installed so its warnings do not become ours; hp_sol2 carries Lua itself.
target_link_libraries(HollowpointScripting PUBLIC
    Hollowpoint::Core
    Hollowpoint::Math
    Hollowpoint::ECS
    Hollowpoint::Scene
    hp_sol2
    Hollowpoint::CompilerSetup)

add_library(Hollowpoint::Scripting ALIAS HollowpointScripting)
