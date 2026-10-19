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
            ECS/TestComponents.cpp
            ECS/TestEntity.cpp
            ECS/TestHierarchy.cpp
            ECS/TestWorldHierarchy.cpp
            ECS/TestSparseSet.cpp
            ECS/TestWorld.cpp
        LIBS Hollowpoint::ECS)
endif()

if(TARGET HollowpointScene)
    hp_add_module_test(Scene
        SOURCES
            Scene/TestPrefab.cpp
            Scene/TestSceneIO.cpp
            Scene/TestTransformHierarchy.cpp
        LIBS Hollowpoint::Scene)
endif()

if(TARGET HollowpointPlatform)
    hp_add_module_test(Platform
        SOURCES
            Platform/TestPlatform.cpp
        LIBS Hollowpoint::Platform)

    # Separate executable on purpose: it is the only test that initialises Vulkan through GLFW, and doing so loads
    # the GPU driver, which retains D-Bus state for the process lifetime. Keeping it isolated means the main
    # Platform suite runs with completely unsuppressed leak checking. See tests/Platform/TestPlatformVulkan.cpp.
    hp_add_module_test(PlatformVulkan
        SOURCES
            Platform/TestPlatformVulkan.cpp
        LIBS Hollowpoint::Platform
        LEAK_SUPPRESSIONS "${CMAKE_CURRENT_SOURCE_DIR}/Support/leak-suppressions.txt")
endif()

if(TARGET HollowpointScripting)
    hp_add_module_test(Scripting
        SOURCES
            Scripting/TestScripting.cpp
        LIBS Hollowpoint::Scripting)
endif()

if(TARGET HollowpointSerialization)
    hp_add_module_test(Serialization
        SOURCES
            Serialization/TestJson.cpp
            Serialization/TestReflectionBridge.cpp
        LIBS Hollowpoint::Serialization Hollowpoint::SerializationReflection)
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
