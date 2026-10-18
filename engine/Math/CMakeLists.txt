# Hollowpoint Math: value types over glm (vectors, matrices, transforms, colour, RNG, volumes).

add_library(HollowpointMath STATIC)
hp_set_engine_target(HollowpointMath)

target_include_directories(HollowpointMath PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include")

target_sources(HollowpointMath PRIVATE
    Include/Hollowpoint/Math/Angles.h
    Include/Hollowpoint/Math/BoundingBox.h
    Include/Hollowpoint/Math/Color.h
    Include/Hollowpoint/Math/Frustum.h
    Include/Hollowpoint/Math/MathCore.h
    Include/Hollowpoint/Math/MathFunctions.h
    Include/Hollowpoint/Math/Random.h
    Include/Hollowpoint/Math/Transform.h
    Src/Angles.cpp
    Src/BoundingBox.cpp
    Src/Color.cpp
    Src/Frustum.cpp
    Src/MathCore.cpp
    Src/MathFunctions.cpp
    Src/Random.cpp
    Src/Transform.cpp)

# The module implements its own vector/matrix algebra (see the rationale in MathCore.h), so it needs
# nothing but Core. That keeps the math layer dependency-free, deterministic and free of header cost.
target_link_libraries(HollowpointMath
    PUBLIC Hollowpoint::Core Hollowpoint::CompilerSetup)

add_library(Hollowpoint::Math ALIAS HollowpointMath)
