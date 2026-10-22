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

if(TARGET HollowpointGraphics)
    # Separate executables on purpose. TestGraphicsDevice initialises Vulkan, and the driver retains D-Bus/loader
    # state for the process lifetime (see NO_LEAK_CHECK in cmake/HollowpointTestHelpers.cmake for why this is a
    # per-target switch rather than pattern suppression); keeping the pure CPU shader-compiler suite in its own
    # binary leaves that suite with fully unsuppressed leak checking.
    hp_add_module_test(ShaderCompiler
        SOURCES
            Graphics/TestShaderCompiler.cpp
        LIBS Hollowpoint::Graphics)

    hp_add_module_test(GraphicsDevice
        SOURCES
            Graphics/TestGraphicsDevice.cpp
        LIBS Hollowpoint::Graphics Hollowpoint::Platform
        NO_LEAK_CHECK)

    # The draw test is separate from the device tests so a rasterisation failure reads as "drawing broke" rather
    # than taking the device-creation cases down with it.
    hp_add_module_test(Draw
        SOURCES
            Graphics/TestDraw.cpp
            Graphics/TestShaderBindings.cpp
            Graphics/TestHdrRenderTarget.cpp
        LIBS Hollowpoint::Graphics Hollowpoint::Platform
        NO_LEAK_CHECK)

    # The engine-level resource wrappers (Buffer/Texture/RenderTarget/Pipeline), drawn through end to end. Own
    # binary for the same reason as above: it initialises Vulkan, so leak checking follows the GPU-target policy.
    # Probes for assumptions every other GPU test leans on but cannot see: that constant-buffer slots actually
    # reach the fragment stage through Pipeline's generated layout, and which way framebuffer Y runs on this driver.
    # Both were written to debug a real failure (zero-lit pixels from an unflipped texel mapping) and kept because
    # each pins behaviour that would otherwise fail somewhere far more confusing.
    hp_add_module_test(PipelineBindingProbe
        SOURCES
            Graphics/TestPipelineBindingProbe.cpp
        LIBS Hollowpoint::Graphics Hollowpoint::Renderer Hollowpoint::Platform
        NO_LEAK_CHECK)

    hp_add_module_test(Resources
        SOURCES
            Graphics/TestResources.cpp
        LIBS Hollowpoint::Graphics Hollowpoint::Platform
        NO_LEAK_CHECK)
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
        NO_LEAK_CHECK)
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

# ---------------------------------------------------------------------------
# Renderer tests.
#
# Pure CPU: the shading model is deliberately free of the RHI, so it is tested with no GPU, no Vulkan loader and full
# leak checking — unlike the Graphics targets above.
# ---------------------------------------------------------------------------

if(TARGET HollowpointRenderer)
    hp_add_module_test(PbrShading
        SOURCES
            Renderer/TestPbrShading.cpp
        LIBS Hollowpoint::Renderer)

    hp_add_module_test(ColorPipeline
        SOURCES
            Renderer/TestColorPipeline.cpp
        LIBS Hollowpoint::Renderer)

    hp_add_module_test(Camera
        SOURCES
            Renderer/TestCamera.cpp
        LIBS Hollowpoint::Renderer)

    # CPU-only like the other references: geometry correctness is winding/normals/indices, checked independently of
    # any device. The winding test in particular catches a face flipped in the static table, which renders as an
    # inside-out cube only once culling exists — earlier than that it hides.
    hp_add_module_test(Geometry
        SOURCES
            Renderer/TestGeometry.cpp
        LIBS Hollowpoint::Renderer)

    # The forward pass's CPU reference and shader generation. Links Graphics for glslang (the shaders must actually
    # compile) but creates no device, so unlike the GPU suites it keeps full leak checking.
    hp_add_module_test(ForwardRender
        SOURCES
            Renderer/TestForwardRender.cpp
        LIBS Hollowpoint::Renderer Hollowpoint::Graphics)

    # Shading equivalence needs the RHI (it renders), so unlike the other Renderer tests it links Graphics and skips
    # leak checking for the same reason the GPU targets above do.
    hp_add_module_test(ShadingEquivalence
        SOURCES
            Renderer/TestShadingEquivalence.cpp
        LIBS Hollowpoint::Renderer Hollowpoint::Graphics Hollowpoint::Platform
        NO_LEAK_CHECK)

    # The forward pass rendered and compared numerically against the CPU reference (M3's acceptance test). Renders,
    # so: Graphics + Platform for the device, leak checking off per the GPU-target policy.
    hp_add_module_test(ForwardRenderGpu
        SOURCES
            Renderer/TestForwardRenderGpu.cpp
        LIBS Hollowpoint::Renderer Hollowpoint::Graphics Hollowpoint::Platform
        NO_LEAK_CHECK)

    hp_add_module_test(Lights
        SOURCES
            Renderer/TestLights.cpp
        LIBS Hollowpoint::Renderer)

    # CPU-only like the other shading references: no RHI, no GPU, full leak checking.
    hp_add_module_test(ImageBasedLighting
        SOURCES
            Renderer/TestImageBasedLighting.cpp
        LIBS Hollowpoint::Renderer)
endif()

