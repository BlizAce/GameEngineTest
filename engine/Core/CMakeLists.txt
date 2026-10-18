# Hollowpoint Core: logging, assertions, memory, hashing, identity, time, platform queries.
# This module must stay dependency-free (C++ standard library only) so that every other module can
# depend on it without pulling in GLFW, NVRHI or Lua.

add_library(HollowpointCore STATIC)
hp_set_engine_target(HollowpointCore)

# Feature macros (HP_STATIC_ENGINE, HP_TRACK_ALLOCATIONS, ...) come from Hollowpoint::CompilerSetup,
# which every engine module links PUBLIC.
target_compile_definitions(HollowpointCore PRIVATE HP_SOURCE_REVISION="${HP_GIT_REVISION}")

target_include_directories(HollowpointCore PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include")

target_sources(HollowpointCore PRIVATE
    Include/Hollowpoint/Core/API.h
    Include/Hollowpoint/Core/Assert.h
    Include/Hollowpoint/Core/EngineVersion.h
    Include/Hollowpoint/Core/Format.h
    Include/Hollowpoint/Core/Guid.h
    Include/Hollowpoint/Core/Hash.h
    Include/Hollowpoint/Core/Log.h
    Include/Hollowpoint/Core/Memory.h
    Include/Hollowpoint/Core/MemoryStats.h
    Include/Hollowpoint/Core/PlatformInfo.h
    Include/Hollowpoint/Core/RefCounted.h
    Include/Hollowpoint/Core/Time.h
    Include/Hollowpoint/Core/Types.h
    Src/Assert.cpp
    Src/Guid.cpp
    Src/Hash.cpp
    Src/Log.cpp
    Src/Memory.cpp
    Src/MemoryStats.cpp
    Src/PlatformInfo.cpp
    Src/Time.cpp)

target_link_libraries(HollowpointCore PUBLIC
    Hollowpoint::CompilerSetup)

if(WIN32)
    target_link_libraries(HollowpointCore PUBLIC shell32 ole32)
endif()

add_library(Hollowpoint::Core ALIAS HollowpointCore)
