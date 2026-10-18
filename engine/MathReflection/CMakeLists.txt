# Hollowpoint MathReflection: reflection descriptors for the engine's math types.
#
# An explicitly opt-in layer, mirroring SerializationReflection. Math must not depend on Reflection (it is the lowest
# engine module and gets linked into tools, tests and benchmarks that want no metadata), and Reflection must not know
# about Math (it is a generic type-description facility). So the two meet here: link this to serialise or inspect a
# Vec3/Quat/Color, and everything else pays nothing for it.

add_library(HollowpointMathReflection STATIC)
hp_set_engine_target(HollowpointMathReflection)

target_include_directories(HollowpointMathReflection PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include")

target_sources(HollowpointMathReflection PRIVATE
    Include/Hollowpoint/MathReflection/MathReflection.h
    Src/MathReflection.cpp)

target_link_libraries(HollowpointMathReflection PUBLIC
    Hollowpoint::Core Hollowpoint::Math Hollowpoint::Reflection Hollowpoint::CompilerSetup)

add_library(Hollowpoint::MathReflection ALIAS HollowpointMathReflection)
