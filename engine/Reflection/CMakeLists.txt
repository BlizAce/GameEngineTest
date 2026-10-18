# Hollowpoint Reflection: runtime type descriptions consumed by the scene format, the editor inspector and
# scripting bindings. Depends only on Core (deliberately not Math), so it stays a leaf every other module can
# build on; math types get registered by whoever owns them rather than by reflection knowing about them.

add_library(HollowpointReflection STATIC)
hp_set_engine_target(HollowpointReflection)

target_include_directories(HollowpointReflection PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include")

target_sources(HollowpointReflection PRIVATE
    Include/Hollowpoint/Reflection/Detail/ScalarConvert.h
    Include/Hollowpoint/Reflection/Descriptors.h
    Include/Hollowpoint/Reflection/NameInterner.h
    Include/Hollowpoint/Reflection/ReflectionTypes.h
    Include/Hollowpoint/Reflection/Registry.h
    Include/Hollowpoint/Reflection/ScalarAccess.h
    Include/Hollowpoint/Reflection/TypeTraits.h
    Src/NameInterner.cpp
    Src/Registry.cpp
    Src/ScalarAccess.cpp)

target_link_libraries(HollowpointReflection PUBLIC
    Hollowpoint::Core Hollowpoint::CompilerSetup)

add_library(Hollowpoint::Reflection ALIAS HollowpointReflection)
