# Unit tests.
#
# Layout: one executable per engine module (CoreTests, MathTests, ...) so a failing module can be run
# on its own, plus a shared support library. Every executable registers with CTest.

add_subdirectory(Support)

include(HollowpointTestHelpers)

if(TARGET HollowpointMath)
    hp_add_module_test(Math
        SOURCES
            Math/TestVector.cpp
            Math/TestMatrix.cpp
            Math/TestQuaternion.cpp
            Math/TestTransform.cpp
            Math/TestAngles.cpp
            Math/TestBoundingBox.cpp
            Math/TestFrustum.cpp
            Math/TestColor.cpp
            Math/TestRandom.cpp
            Math/TestMathFunctions.cpp
        LIBS Hollowpoint::Math)
endif()

if(TARGET HollowpointECS)
    hp_add_module_test(ECS
        SOURCES
            ECS/TestEntity.cpp
            ECS/TestSparseSet.cpp
            ECS/TestWorld.cpp
        LIBS Hollowpoint::ECS)
endif()

if(TARGET HollowpointReflection)
    hp_add_module_test(Reflection
        SOURCES
            Reflection/TestRegistry.cpp
            Reflection/TestScalarAccess.cpp
        LIBS Hollowpoint::Reflection)
endif()

if(TARGET HollowpointCore)
    hp_add_module_test(Core
        SOURCES
            Core/TestAssert.cpp
            Core/TestGuid.cpp
            Core/TestHash.cpp
            Core/TestLog.cpp
            Core/TestMemory.cpp
            Core/TestRefCounted.cpp
            Core/TestTime.cpp
        LIBS Hollowpoint::Core)
endif()
