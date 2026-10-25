// Tests for SceneRenderer — the engine's production GPU scene pass.
//
// Why a separate suite when SceneToPixelTests already renders a scene to pixels: that file proves the *generated
// shaders* reproduce the CPU reference, and it does so with its own hand-rolled submission code. This suite tests the
// class an application actually calls — its caching, its clear policy, its frame-slot discipline, its failure paths.
// Before this class existed there was no production submission path to test at all; after it, leaving SceneToPixel's
// private copy as the only coverage would mean shipping the real renderer untested while a near-identical mock stays
// green.
//
// What each case pins down, in one line:
//   * Create / refusals            — a renderer needs a device and a non-zero light bound; both fail by returning null.
//   * Forward pixels               — production code, unmodified engine shaders, pixels matching the CPU reference.
//   * Clear policy                 — an empty plan still produces backgroundColour (a "nothing drew" frame is visible).
//   * Depth behaviour              — a target with depth occludes; without it, nothing does, and that logs once.
//   * Geometry cache               — one upload per mesh across frames; ReleaseGeometry forces a re-upload.
//   * Pipeline caches              — resize creates no new pipelines (format-keyed), while a real format change does.
//   * Frame slots                — separate pools per slot; an out-of-range slot asserts and refuses rather than
//   guessing.
//   * Tonemap                    — the two-pass frame lands display values matching ColorPipeline::LinearToDisplay.

#include "TestSupport.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Core/RefCounted.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/Game/RegisterGameTypes.h"
#include "Hollowpoint/Game/RenderPrimitives.h"
#include "Hollowpoint/Game/SceneRenderer.h"
#include "Hollowpoint/Graphics/Buffer.h"
#include "Hollowpoint/Graphics/GraphicsDevice.h"
#include "Hollowpoint/Graphics/GraphicsTypes.h"
#include "Hollowpoint/Graphics/Pipeline.h"
#include "Hollowpoint/Graphics/RenderTarget.h"
#include "Hollowpoint/Graphics/RhiAccess.h"
#include "Hollowpoint/Graphics/ShaderCompiler.h"
#include "Hollowpoint/Graphics/Texture.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Renderer/Camera.h"
#include "Hollowpoint/Renderer/ColorPipeline.h"
#include "Hollowpoint/Renderer/ForwardRender.h"
#include "Hollowpoint/Renderer/Geometry.h"
#include "Hollowpoint/Renderer/Lights.h"

using namespace Hollowpoint;
using namespace Hollowpoint::Graphics;

using Core::Ref;
using Core::Size;
using Core::UInt32;
using Core::UInt64;
using ECS::Entity;
using Game::RenderMeshEntry;
using Game::RenderPlan;
using Game::SceneRenderer;
using Game::SceneRendererDesc;
using Math::Float32;
using Math::Mat4;
using Math::Vec2;
using Math::Vec3;
using Math::Vec4;
using Renderer::CameraComponent;
using Renderer::CameraState;
using Renderer::ComputeCameraState;
using Renderer::ComputeForwardShadingCpu;
using Renderer::ForwardDrawItem;
using Renderer::ForwardLight;
using Renderer::ForwardPassParams;
using Renderer::GeometryData;
using Renderer::LightComponent;
using Renderer::LinearToDisplay;
using Renderer::MakeCube;

namespace
{

[[nodiscard]] GraphicsDevice* GetDevice()
{
    static GraphicsDevice* device = []() -> GraphicsDevice* {
        GraphicsDeviceDesc desc;
        desc.ApplicationName = "Hollowpoint scene renderer tests";
        return GraphicsDevice::Create(desc);
    }();

    return device;
}

//! Relative error that is safe when the reference is zero. A plain |a-b|/|b| returns inf for an unlit reference pixel —
//! and an unlit pixel is exactly what a broken light upload produces, so the common failure must not hide as infinity.
[[nodiscard]] Float32 RelativeError(Float32 gpu, Float32 cpu)
{
    const Float32 difference = std::fabs(gpu - cpu);
    const Float32 magnitude = std::max(std::fabs(cpu), 1.0e-6f);
    return difference / magnitude;
}

[[nodiscard]] std::string Describe(const Vec3& value)
{
    return "(" + std::to_string(value.X) + ", " + std::to_string(value.Y) + ", " + std::to_string(value.Z) + ")";
}

struct OffscreenTarget
{
    Ref<Texture> Colour;
    Ref<Texture> Depth;  //!< may be null when the test wants a depthless target
    Ref<RenderTarget> Target;
};

//! Reads one texel out of a float-format readback, decoding by the format actually stored.
struct Texel
{
    Float32 Red = std::numeric_limits<float>::quiet_NaN();
    Float32 Green = std::numeric_limits<float>::quiet_NaN();
    Float32 Blue = std::numeric_limits<float>::quiet_NaN();
    Float32 Alpha = std::numeric_limits<float>::quiet_NaN();
};

[[nodiscard]] Texel ReadTexel(GraphicsDevice& device, Texture& texture, UInt32 x, UInt32 y)
{
    UInt32 rowPitchBytes = 0;
    const std::vector<UInt8> bytes = texture.ReadBack(device, rowPitchBytes);
    REQUIRE_FALSE(bytes.empty());

    const auto* floats = reinterpret_cast<const float*>(bytes.data());
    const Size floatsPerRow = rowPitchBytes / sizeof(float);
    const Size offset = floatsPerRow * y + static_cast<Size>(x) * 4u;

    Texel texel;
    texel.Red = floats[offset + 0];
    texel.Green = floats[offset + 1];
    texel.Blue = floats[offset + 2];
    texel.Alpha = floats[offset + 3];
    return texel;
}

//! A one-mesh plan: a cube at the origin lit by one directional light aimed at the sampled face.
/**
 * The light direction is authored toward -Z so it strikes the +Z face the camera looks at. Pointing it anywhere else
 * makes the CPU reference black, and every comparison below then compares 0 against 0 — a vacuous pass that was
 * actually hit while writing SceneToPixelTests (notes/EXPERIMENTS.md), so the value is pinned here too rather than
 * eyeballed.
 */
[[nodiscard]] RenderPlan MakeTestPlan(const CameraState& cameraState, const Vec3& baseColour, Float32 intensity)
{
    static GeometryData cube = MakeCube(1.0f);

    RenderPlan plan;
    plan.Pass.ViewProjection = cameraState.ViewProjection;
    plan.Pass.CameraPosition = cameraState.EyePosition;

    ForwardLight light;
    light.Light.Type = LightComponent::LightType::Directional;
    // (0.3, 0.6, -0.9) normalised: travels toward -Z, so it lights the face the camera sees.
    light.WorldDirection = Vec3(0.2673f, 0.5345f, -0.8018f);
    light.Light.Intensity = intensity;
    light.Light.Color = Vec3(1.0f, 0.9f, 0.8f);
    plan.Pass.Lights.push_back(light);

    ForwardDrawItem draw;
    draw.ModelMatrix = Mat4::Identity();
    draw.Material.BaseColor = baseColour;
    draw.Material.Roughness = 0.34f;
    draw.Material.Metallic = 0.0f;
    draw.Geometry = &cube;
    draw.LightIndices.push_back(0u);

    RenderMeshEntry entry;
    entry.Draw = draw;
    plan.Meshes.push_back(entry);
    return plan;
}

//! The world position the centre pixel shows, solved rather than assumed.
/**
 * A cube centred at the origin puts its +Z face plane at z = +0.5; intersecting the camera's principal ray (NDC 0,0)
 * with that plane gives the exact point under the sampled texel. On a flat face every point shares one normal, so the
 * CPU reference is exact there rather than approximately interpolated — which is what makes a tight tolerance honest.
 */
[[nodiscard]] Vec3 SolveCentreSample(const CameraState& cameraState, Float32 planeZ)
{
    const Vec4 nearPoint = cameraState.InverseViewProjection * Vec4(0.0f, 0.0f, 0.0f, 1.0f);
    const Vec4 farPoint = cameraState.InverseViewProjection * Vec4(0.0f, 0.0f, 1.0f, 1.0f);

    const Vec3 rayOrigin(nearPoint.X / nearPoint.W, nearPoint.Y / nearPoint.W, nearPoint.Z / nearPoint.W);
    const Vec4 farDivided(farPoint.X / farPoint.W, farPoint.Y / farPoint.W, farPoint.Z / farPoint.W, 1.0f);
    const Vec3 rayDirection = (Vec3(farDivided.X, farDivided.Y, farDivided.Z) - rayOrigin).Normalized();

    REQUIRE(std::fabs(rayDirection.Z) > 1.0e-6f);
    const Float32 distance = (planeZ - rayOrigin.Z) / rayDirection.Z;
    REQUIRE(distance > 0.0f);

    return Vec3(rayOrigin.X + rayDirection.X * distance, rayOrigin.Y + rayDirection.Y * distance, planeZ);
}

[[nodiscard]] CameraState MakeCameraState()
{
    CameraComponent camera;
    camera.Position = Vec3(0.0f, 0.0f, 4.0f);
    camera.Target = Vec3(0.0f, 0.0f, 0.0f);
    camera.NearPlane = 0.1f;
    camera.FarPlane = 100.0f;
    return ComputeCameraState(camera, 1.0f);
}

[[nodiscard]] Ref<SceneRenderer> CreateRenderer(GraphicsDevice& device, UInt32 frameSlots = 1)
{
    SceneRendererDesc desc;
    desc.Name = "test-renderer";
    desc.FrameSlots = frameSlots;
    return SceneRenderer::Create(device, desc);
}

//! Texture used for the geometry-address cache tests: distinct objects with identical counts, to exercise the alias
//! rule.
struct TwoGeometries
{
    GeometryData First = MakeCube(1.0f);
    GeometryData Second = MakeCube(2.0f);  // same vertex/index counts, different positions
};

}  // namespace

TEST_SUITE("Game/SceneRenderer")
{
    TEST_CASE("creation requires a device and a non-zero light bound")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN(("no graphics device on this host: " + std::string(GraphicsDevice::GetFailureReason())).c_str());
            return;
        }

        // The default description works.
        Ref<SceneRenderer> renderer = SceneRenderer::Create(*device);
        REQUIRE(renderer != nullptr);
        CHECK(renderer->GetMaxLightsPerDraw() == 8u);
        CHECK(renderer->GetFrameSlotCount() == 1u);
        CHECK(renderer->GetDrawCount() == 0u);
        CHECK(renderer->GetSubmissionCount() == 0u);

        // A zero light bound would generate `vec4 LightArray[0]`, which glslang rejects with a message naming neither
        // the call nor the field — so it is refused before shaders are built, and asserted because passing it is a bug.
        {
            Test::ScopedAssertCapture capture;
            SceneRendererDesc bad;
            bad.MaxLightsPerDraw = 0u;
            CHECK(SceneRenderer::Create(*device, bad) == nullptr);
            CHECK(capture.GetCaptureCount() >= 1);
        }

        // Same for zero frame slots: every slot argument would be out of range.
        {
            Test::ScopedAssertCapture capture;
            SceneRendererDesc bad;
            bad.FrameSlots = 0u;
            CHECK(SceneRenderer::Create(*device, bad) == nullptr);
            CHECK(capture.GetCaptureCount() >= 1);
        }

        SceneRendererDesc slots;
        slots.FrameSlots = 3u;
        Ref<SceneRenderer> ringed = SceneRenderer::Create(*device, slots);
        REQUIRE(ringed != nullptr);
        CHECK(ringed->GetFrameSlotCount() == 3u);
    }

    TEST_CASE("RenderForward draws a cube whose centre pixel matches the CPU reference")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN("no graphics device");
            return;
        }

        const CameraState cameraState = MakeCameraState();
        const Vec3 baseColour(0.72f, 0.31f, 0.18f);
        const RenderPlan plan = MakeTestPlan(cameraState, baseColour, 3.5f);
        REQUIRE(plan.Meshes.size() == 1u);

        // --- target ------------------------------------------------------------------------
        OffscreenTarget fixture;
        {
            TextureDesc colourDesc;
            colourDesc.Name = "sr-cube-colour";
            colourDesc.Width = 64;
            colourDesc.Height = 64;
            colourDesc.Format = GraphicsFormat::Rgba32Float;
            colourDesc.IsShaderResource = true;
            fixture.Colour = Texture::Create(*device, colourDesc);
            REQUIRE(fixture.Colour != nullptr);

            TextureDesc depthDesc;
            depthDesc.Name = "sr-cube-depth";
            depthDesc.Width = 64;
            depthDesc.Height = 64;
            depthDesc.Format = GraphicsFormat::D32Float;
            fixture.Depth = Texture::Create(*device, depthDesc);
            REQUIRE(fixture.Depth != nullptr);

            RenderTargetDesc targetDesc;
            targetDesc.Name = "sr-cube-target";
            targetDesc.Colour = fixture.Colour;
            targetDesc.Depth = fixture.Depth;
            fixture.Target = RenderTarget::Create(*device, targetDesc);
            REQUIRE(fixture.Target != nullptr);
        }

        Ref<SceneRenderer> renderer = CreateRenderer(*device);
        REQUIRE(renderer != nullptr);

        // --- render ------------------------------------------------------------------------
        const bool rendered = renderer->RenderForward(plan, *fixture.Target, 0u, Vec4(0.0f, 0.0f, 0.0f, 1.0f));
        REQUIRE_MESSAGE(rendered, "RenderForward refused the frame; see log");
        device->WaitForIdle();

        CHECK(renderer->GetSubmissionCount() == 1u);
        CHECK(renderer->GetDrawCount() == 1u);
        // One mesh uploaded, one pipeline variant (one target, one cull mode).
        CHECK(renderer->GetCachedGeometryCount() == 1u);
        CHECK(renderer->GetForwardVariantCount() == 1u);

        // --- compare the centre texel against the CPU reference ------------------------------
        const Texel texel = ReadTexel(*device, *fixture.Colour, 32u, 32u);
        INFO("centre texel " << texel.Red << ", " << texel.Green << ", " << texel.Blue << ", a=" << texel.Alpha);

        // NaN would mean the pixel was never written — reported distinctly from "written but different", because those
        // are a transform/culling bug and a shading bug respectively.
        CHECK_FALSE(std::isnan(texel.Red));
        CHECK_FALSE(std::isnan(texel.Green));
        CHECK_FALSE(std::isnan(texel.Blue));
        CHECK(texel.Alpha == doctest::Approx(1.0f));

        const Vec3 worldSample = SolveCentreSample(cameraState, 0.5f);
        const Vec3 expected =
            ComputeForwardShadingCpu(plan.Pass, plan.Meshes[0].Draw, worldSample, Vec3(0.0f, 0.0f, 1.0f));
        INFO("CPU reference " << Describe(expected));
        // A black reference makes the comparison vacuous (0 vs 0 passes), so guard it: this is the assertion that
        // catches a light upload landing at the wrong offset while every pixel stays finite.
        CHECK(expected.X > 0.0f);
        CHECK(expected.Z > 0.0f);

        // Tolerance: same float32 maths, but the GPU interpolates and normalises a normal and may fuse differently.
        // 2e-3 is well under an 8-bit step yet catches a dropped term or a mis-uploaded block field (same reasoning as
        // SceneToPixelTests, which pins this shader path against the same reference).
        CHECK(RelativeError(texel.Red, expected.X) < 2.0e-3f);
        CHECK(RelativeError(texel.Green, expected.Y) < 2.0e-3f);
        CHECK(RelativeError(texel.Blue, expected.Z) < 2.0e-3f);
    }

    TEST_CASE("an empty plan still clears to the requested background")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN("no graphics device");
            return;
        }

        TextureDesc colourDesc;
        colourDesc.Name = "sr-clear-colour";
        colourDesc.Width = 16;
        colourDesc.Height = 16;
        colourDesc.Format = GraphicsFormat::Rgba32Float;
        colourDesc.IsShaderResource = true;
        Ref<Texture> colour = Texture::Create(*device, colourDesc);
        REQUIRE(colour != nullptr);

        RenderTargetDesc targetDesc;
        targetDesc.Name = "sr-clear-target";
        targetDesc.Colour = colour;
        Ref<RenderTarget> target = RenderTarget::Create(*device, targetDesc);
        REQUIRE(target != nullptr);

        Ref<SceneRenderer> renderer = CreateRenderer(*device);
        REQUIRE(renderer != nullptr);

        const Vec4 background(0.25f, 0.5f, 0.75f, 1.0f);
        const RenderPlan emptyPlan;
        CHECK(renderer->RenderForward(emptyPlan, *target, 0u, background));
        device->WaitForIdle();

        // A frame with zero draws must still produce a defined image. This is the pass's own guarantee
        // (SceneRenderer.h): attachment memory is undefined at creation, so "no geometry" and "no clear" are otherwise
        // indistinguishable.
        CHECK(renderer->GetDrawCount() == 0u);
        CHECK(renderer->GetSubmissionCount() == 1u);

        const Texel texel = ReadTexel(*device, *colour, 8u, 8u);
        INFO("cleared texel " << texel.Red << ", " << texel.Green << ", " << texel.Blue << ", a=" << texel.Alpha);
        CHECK(texel.Red == doctest::Approx(0.25f));
        CHECK(texel.Green == doctest::Approx(0.5f));
        CHECK(texel.Blue == doctest::Approx(0.75f));
    }

    TEST_CASE("a target without depth renders and warns once")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN("no graphics device");
            return;
        }

        TextureDesc colourDesc;
        colourDesc.Name = "sr-nodepth-colour";
        colourDesc.Width = 32;
        colourDesc.Height = 32;
        colourDesc.Format = GraphicsFormat::Rgba32Float;
        colourDesc.IsShaderResource = true;
        Ref<Texture> colour = Texture::Create(*device, colourDesc);
        REQUIRE(colour != nullptr);

        RenderTargetDesc targetDesc;
        targetDesc.Name = "sr-nodepth-target";
        targetDesc.Colour = colour;  // deliberately no depth
        Ref<RenderTarget> target = RenderTarget::Create(*device, targetDesc);
        REQUIRE(target != nullptr);
        REQUIRE_FALSE(target->HasDepth());

        Ref<SceneRenderer> renderer = CreateRenderer(*device);
        REQUIRE(renderer != nullptr);

        const CameraState cameraState = MakeCameraState();
        const RenderPlan plan = MakeTestPlan(cameraState, Vec3(0.5f, 0.5f, 0.5f), 2.0f);

        Test::ScopedLogCapture logs;
        CHECK(renderer->RenderForward(plan, *target, 0u, Vec4(0.0f, 0.0f, 0.0f, 1.0f)));
        device->WaitForIdle();

        // The frame renders (a depthless target is a legitimate mode), and the consequence — overlapping geometry
        // resolving by rasterisation order — is reported exactly once rather than every frame.
        CHECK(renderer->GetDrawCount() == 1u);
        CHECK(logs.GetWarningCount() == 1);
        CHECK(logs.AnyMessageContains("no depth attachment"));

        renderer->RenderForward(plan, *target, 0u, Vec4(0.0f, 0.0f, 0.0f, 1.0f));
        device->WaitForIdle();
        CHECK(logs.GetWarningCount() == 1);  // still one: warned once per renderer, not per frame
    }

    TEST_CASE("geometry uploads once per mesh and re-uploads only when released")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN("no graphics device");
            return;
        }

        TextureDesc colourDesc;
        colourDesc.Name = "sr-cache-colour";
        colourDesc.Width = 16;
        colourDesc.Height = 16;
        colourDesc.Format = GraphicsFormat::Rgba32Float;
        colourDesc.IsShaderResource = true;
        Ref<Texture> colour = Texture::Create(*device, colourDesc);
        REQUIRE(colour != nullptr);

        RenderTargetDesc targetDesc;
        targetDesc.Name = "sr-cache-target";
        targetDesc.Colour = colour;
        Ref<RenderTarget> target = RenderTarget::Create(*device, targetDesc);
        REQUIRE(target != nullptr);

        Ref<SceneRenderer> renderer = CreateRenderer(*device);
        REQUIRE(renderer != nullptr);

        const CameraState cameraState = MakeCameraState();
        const RenderPlan plan = MakeTestPlan(cameraState, Vec3(0.4f, 0.4f, 0.4f), 1.0f);
        const GeometryData* geometry = plan.Meshes[0].Draw.Geometry;
        REQUIRE(geometry != nullptr);

        CHECK_FALSE(renderer->HasGeometry(*geometry));

        for (UInt32 frame = 0; frame < 5u; ++frame)
        {
            REQUIRE(renderer->RenderForward(plan, *target, 0u));
        }
        device->WaitForIdle();

        // Five frames, one upload. A per-frame implementation would show five here (or leak), and this is the property
        // that keeps a 200-instance scene from uploading its cube 200 times per frame.
        CHECK(renderer->HasGeometry(*geometry));
        CHECK(renderer->GetCachedGeometryCount() == 1u);
        CHECK(renderer->GetDrawCount() == 5u);

        renderer->ReleaseGeometry(*geometry);
        CHECK_FALSE(renderer->HasGeometry(*geometry));
        CHECK(renderer->GetCachedGeometryCount() == 0u);

        // Releasing is not an error and the next frame simply uploads again.
        REQUIRE(renderer->RenderForward(plan, *target, 0u));
        device->WaitForIdle();
        CHECK(renderer->GetCachedGeometryCount() == 1u);

        // Releasing something never cached must be harmless (an asset layer calls it blindly on unload).
        TwoGeometries unrelated;
        renderer->ReleaseGeometry(unrelated.First);
        CHECK(renderer->GetCachedGeometryCount() == 1u);
    }

    TEST_CASE("pipeline caches are keyed by format, so a resize creates no new pipeline")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN("no graphics device");
            return;
        }

        Ref<SceneRenderer> renderer = CreateRenderer(*device);
        REQUIRE(renderer != nullptr);

        const CameraState cameraState = MakeCameraState();
        const RenderPlan plan = MakeTestPlan(cameraState, Vec3(0.6f, 0.2f, 0.2f), 1.0f);

        // Ten targets of the SAME format at different sizes: what a resize hands the renderer frame after frame. Each
        // is a distinct object, so an identity-keyed cache would grow to ten pipelines here (and keep nine dead ones).
        for (UInt32 index = 0; index < 10u; ++index)
        {
            TextureDesc colourDesc;
            colourDesc.Name = "sr-resize-colour";
            colourDesc.Width = 16 + index;
            colourDesc.Height = 16 + index;
            colourDesc.Format = GraphicsFormat::Rgba32Float;
            colourDesc.IsShaderResource = true;
            Ref<Texture> colour = Texture::Create(*device, colourDesc);
            REQUIRE(colour != nullptr);

            RenderTargetDesc targetDesc;
            targetDesc.Name = "sr-resize-target";
            targetDesc.Colour = colour;
            Ref<RenderTarget> target = RenderTarget::Create(*device, targetDesc);
            REQUIRE(target != nullptr);

            REQUIRE(renderer->RenderForward(plan, *target, 0u));
        }
        device->WaitForIdle();

        CHECK(renderer->GetForwardVariantCount() == 1u);
        CHECK(renderer->GetDrawCount() == 10u);

        // A genuinely different format must produce a second pipeline: the same code path that reuses on similarity has
        // to distinguish, or it would bind an incompatible pipeline and draw garbage.
        TextureDesc halfFloatDesc;
        halfFloatDesc.Name = "sr-half-colour";
        halfFloatDesc.Width = 16;
        halfFloatDesc.Height = 16;
        halfFloatDesc.Format = GraphicsFormat::Rgba16Float;
        halfFloatDesc.IsShaderResource = true;
        Ref<Texture> halfColour = Texture::Create(*device, halfFloatDesc);
        REQUIRE(halfColour != nullptr);

        RenderTargetDesc halfTargetDesc;
        halfTargetDesc.Name = "sr-half-target";
        halfTargetDesc.Colour = halfColour;
        Ref<RenderTarget> halfTarget = RenderTarget::Create(*device, halfTargetDesc);
        REQUIRE(halfTarget != nullptr);

        REQUIRE(renderer->RenderForward(plan, *halfTarget, 0u));
        device->WaitForIdle();
        CHECK(renderer->GetForwardVariantCount() == 2u);
    }

    TEST_CASE("frame slots own separate per-draw state")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN("no graphics device");
            return;
        }

        TextureDesc colourDesc;
        colourDesc.Name = "sr-slot-colour";
        colourDesc.Width = 16;
        colourDesc.Height = 16;
        colourDesc.Format = GraphicsFormat::Rgba32Float;
        colourDesc.IsShaderResource = true;
        Ref<Texture> colour = Texture::Create(*device, colourDesc);
        REQUIRE(colour != nullptr);

        RenderTargetDesc targetDesc;
        targetDesc.Name = "sr-slot-target";
        targetDesc.Colour = colour;
        Ref<RenderTarget> target = RenderTarget::Create(*device, targetDesc);
        REQUIRE(target != nullptr);

        const CameraState cameraState = MakeCameraState();
        const RenderPlan plan = MakeTestPlan(cameraState, Vec3(0.3f, 0.6f, 0.9f), 1.0f);

        // Three slots: one frame each, and each slot's pool grows on its own. This is what makes Graphics::FrameRing
        // mean something — a shared pool would let frame N+2 overwrite what frame N's draws are still reading, with no
        // error and no validation report to show for it (this host has no validation layer installed).
        Ref<SceneRenderer> renderer = CreateRenderer(*device, 3u);
        REQUIRE(renderer != nullptr);

        CHECK(renderer->GetDrawPoolSize(0u) == 0u);
        CHECK(renderer->GetDrawPoolSize(1u) == 0u);
        CHECK(renderer->GetDrawPoolSize(2u) == 0u);

        REQUIRE(renderer->RenderForward(plan, *target, 0u));
        device->WaitForIdle();
        CHECK(renderer->GetDrawPoolSize(0u) == 1u);
        CHECK(renderer->GetDrawPoolSize(1u) == 0u);  // untouched: the pools are not one pool

        REQUIRE(renderer->RenderForward(plan, *target, 1u));
        device->WaitForIdle();
        CHECK(renderer->GetDrawPoolSize(1u) == 1u);
        CHECK(renderer->GetDrawPoolSize(2u) == 0u);

        // Slot 0 is written again and its pool stays at one entry — reused, not reallocated.
        REQUIRE(renderer->RenderForward(plan, *target, 0u));
        device->WaitForIdle();
        CHECK(renderer->GetDrawPoolSize(0u) == 1u);

        // An out-of-range slot asserts AND refuses: clamping would silently write a slot that may be in flight, which
        // is the exact corruption the slots exist to prevent.
        {
            Test::ScopedAssertCapture capture;
            CHECK_FALSE(renderer->RenderForward(plan, *target, 3u));
            CHECK(capture.GetCaptureCount() >= 1);
        }

        const Texture* colourTexture = target->GetColourTexture();
        REQUIRE(colourTexture != nullptr);
        {
            Test::ScopedAssertCapture capture;
            CHECK_FALSE(renderer->BlitTonemap(*colourTexture, *target, 1.0f, 7u));
            CHECK(capture.GetCaptureCount() >= 1);
        }

        // A renderer with one slot rejects every non-zero slot rather than drawing into a ring it does not have.
        Ref<SceneRenderer> single = CreateRenderer(*device, 1u);
        REQUIRE(single != nullptr);
        {
            Test::ScopedAssertCapture capture;
            CHECK_FALSE(single->RenderForward(plan, *target, 2u));
            CHECK_FALSE(single->BlitTonemap(*colourTexture, *target, 1.0f, 1u));
            CHECK(capture.GetCaptureCount() >= 2);
        }
        // A slot index far past the end reports an empty pool rather than reading out of bounds.
        CHECK(renderer->GetDrawPoolSize(99u) == 0u);
    }

    TEST_CASE("BlitTonemap writes display-referred values matching LinearToDisplay")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN("no graphics device");
            return;
        }

        // A 4x4 HDR source with known radiance, uploaded through Texture::Update (synchronous by contract).
        constexpr UInt32 kSize = 4u;
        constexpr Float32 kExposure = 0.75f;

        TextureDesc sourceDesc;
        sourceDesc.Name = "sr-blit-source";
        sourceDesc.Width = kSize;
        sourceDesc.Height = kSize;
        sourceDesc.Format = GraphicsFormat::Rgba32Float;
        sourceDesc.IsRenderTarget = true;  // EXPERIMENT: was false
        sourceDesc.IsShaderResource = true;
        Ref<Texture> source = Texture::Create(*device, sourceDesc);
        REQUIRE(source != nullptr);

        std::vector<float> texels(static_cast<size_t>(kSize) * kSize * 4u, 0.0f);
        for (UInt32 y = 0; y < kSize; ++y)
        {
            for (UInt32 x = 0; x < kSize; ++x)
            {
                float* texel = texels.data() + (static_cast<size_t>(y) * kSize + x) * 4u;
                texel[0] = 0.05f + 0.3f * static_cast<float>(x);
                texel[1] = 0.2f + 0.3f * static_cast<float>(y);
                texel[2] = 1.7f;  // over-bright, so the tone curve's compression is actually exercised
                texel[3] = 1.0f;
            }
        }
        REQUIRE(source->Update(*device, texels.data(), texels.size() * sizeof(float)));

        TextureDesc targetDesc;
        targetDesc.Name = "sr-blit-target";
        targetDesc.Width = kSize;
        targetDesc.Height = kSize;
        targetDesc.Format = GraphicsFormat::Rgba32Float;
        targetDesc.IsShaderResource = true;
        Ref<Texture> colour = Texture::Create(*device, targetDesc);
        REQUIRE(colour != nullptr);

        RenderTargetDesc renderTargetDesc;
        renderTargetDesc.Name = "sr-blit-framebuffer";
        renderTargetDesc.Colour = colour;
        Ref<RenderTarget> display = RenderTarget::Create(*device, renderTargetDesc);
        REQUIRE(display != nullptr);

        Ref<SceneRenderer> renderer = CreateRenderer(*device);
        REQUIRE(renderer != nullptr);

        REQUIRE(renderer->BlitTonemap(*source, *display, kExposure));
        device->WaitForIdle();
        CHECK(renderer->GetSubmissionCount() == 1u);

        // Every texel compared: the blit covers the viewport at 1:1 with nearest filtering, so an orientation or scale
        // error shows up as a mismatch on specific texels rather than cancelling out.
        UInt32 compared = 0;
        for (UInt32 y = 0; y < kSize; ++y)
        {
            for (UInt32 x = 0; x < kSize; ++x)
            {
                const Texel texel = ReadTexel(*device, *colour, x, y);
                const float* sourceTexel = &texels[(static_cast<size_t>(y) * kSize + x) * 4u];
                const Vec3 expected = LinearToDisplay(Vec3(sourceTexel[0], sourceTexel[1], sourceTexel[2]), kExposure);

                INFO("texel (" << x << "," << y << ") gpu " << texel.Red << ", " << texel.Green << ", " << texel.Blue
                               << " cpu " << Describe(expected));
                // 8-bit output is not the constraint here (the target is RGBA32F), so this measures only the curve's
                // own transcendental differences between libm and this driver — same tolerance TestTonemapBlit settled
                // on.
                CHECK(RelativeError(texel.Red, expected.X) < 2.0e-3f);
                CHECK(RelativeError(texel.Green, expected.Y) < 2.0e-3f);
                CHECK(RelativeError(texel.Blue, expected.Z) < 2.0e-3f);
                ++compared;
            }
        }
        // A loop that compared nothing would pass every check above. This is the assertion that makes them real, and it
        // is here because a vacuously-green GPU suite already fooled this project once (session 14).
        CHECK(compared == kSize * kSize);
    }

    TEST_CASE("BlitTonemap refuses a source that cannot be sampled")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN("no graphics device");
            return;
        }

        TextureDesc notSampleable;
        notSampleable.Name = "sr-no-srv";
        notSampleable.Width = 4;
        notSampleable.Height = 4;
        notSampleable.Format = GraphicsFormat::Rgba32Float;
        notSampleable.IsRenderTarget = true;
        notSampleable.IsShaderResource = false;  // the point of the case
        Ref<Texture> source = Texture::Create(*device, notSampleable);
        REQUIRE(source != nullptr);

        TextureDesc colourDesc;
        colourDesc.Name = "sr-no-srv-target";
        colourDesc.Width = 4;
        colourDesc.Height = 4;
        colourDesc.Format = GraphicsFormat::Rgba32Float;
        colourDesc.IsShaderResource = true;
        Ref<Texture> colour = Texture::Create(*device, colourDesc);
        REQUIRE(colour != nullptr);

        RenderTargetDesc targetDesc;
        targetDesc.Name = "sr-no-srv-framebuffer";
        targetDesc.Colour = colour;
        Ref<RenderTarget> display = RenderTarget::Create(*device, targetDesc);
        REQUIRE(display != nullptr);

        Ref<SceneRenderer> renderer = CreateRenderer(*device);
        REQUIRE(renderer != nullptr);

        // Reported as an error naming the cause rather than letting NVRHI fail the binding set with a message about
        // views.
        Test::ScopedLogCapture logs;
        CHECK_FALSE(renderer->BlitTonemap(*source, *display, 1.0f));
        CHECK(logs.GetErrorCount() == 1);
        CHECK(logs.AnyMessageContains("IsShaderResource"));
        CHECK(renderer->GetSubmissionCount() == 0u);
    }

    TEST_CASE("RenderFrame runs both passes and lands display-referred output")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN("no graphics device");
            return;
        }

        const CameraState cameraState = MakeCameraState();
        const RenderPlan plan = MakeTestPlan(cameraState, Vec3(0.72f, 0.31f, 0.18f), 3.5f);

        auto makeTarget = [&](const char* name, GraphicsFormat format) {
            TextureDesc colourDesc;
            colourDesc.Name = name;
            colourDesc.Width = 64;
            colourDesc.Height = 64;
            colourDesc.Format = format;
            colourDesc.IsShaderResource = true;
            Ref<Texture> colourTexture = Texture::Create(*device, colourDesc);
            REQUIRE(colourTexture != nullptr);

            RenderTargetDesc targetDesc;
            targetDesc.Name = std::string(name) + "-fb";
            targetDesc.Colour = colourTexture;

            struct Pair
            {
                Ref<Texture> Colour;
                Ref<RenderTarget> Target;
            };
            return Pair{colourTexture, RenderTarget::Create(*device, targetDesc)};
        };

        auto hdr = makeTarget("sr-frame-hdr", GraphicsFormat::Rgba32Float);
        REQUIRE(hdr.Target != nullptr);
        auto display = makeTarget("sr-frame-display", GraphicsFormat::Rgba32Float);
        REQUIRE(display.Target != nullptr);

        Ref<SceneRenderer> renderer = CreateRenderer(*device);
        REQUIRE(renderer != nullptr);

        constexpr Float32 kExposure = 1.0f;
        REQUIRE(renderer->RenderFrame(plan, *hdr.Target, *display.Target, kExposure));
        device->WaitForIdle();

        // Two passes, two submissions: the forward pass then the tone map. A frame loop's ring is ended after this
        // returns, so both must be queued by now (SceneRenderer.h documents that).
        CHECK(renderer->GetSubmissionCount() == 2u);
        CHECK(renderer->GetDrawCount() == 1u);  // indexed draws only; the blit is a non-indexed draw

        // The displayed centre pixel is the tone-mapped scene radiance, which must be in range and match the chain.
        const Texel texel = ReadTexel(*device, *display.Colour, 32u, 32u);
        INFO("display texel " << texel.Red << ", " << texel.Green << ", " << texel.Blue << ", a=" << texel.Alpha);
        CHECK_FALSE(std::isnan(texel.Red));
        CHECK(texel.Red >= 0.0f);
        CHECK(texel.Red <= 1.0f);
        CHECK(texel.Green >= 0.0f);
        CHECK(texel.Blue > 0.0f);

        // Cross-check against the CPU reference run through the same display transform, so "the two passes compose" is
        // tested rather than merely "two passes ran".
        const Vec3 worldSample = SolveCentreSample(cameraState, 0.5f);
        const Vec3 hdrReference =
            ComputeForwardShadingCpu(plan.Pass, plan.Meshes[0].Draw, worldSample, Vec3(0.0f, 0.0f, 1.0f));
        const Vec3 expected = LinearToDisplay(hdrReference, kExposure);
        INFO("expected display " << Describe(expected));
        CHECK(RelativeError(texel.Red, expected.X) < 5.0e-3f);
        CHECK(RelativeError(texel.Green, expected.Y) < 5.0e-3f);
        CHECK(RelativeError(texel.Blue, expected.Z) < 5.0e-3f);
    }

    TEST_CASE("a plan whose geometry will not upload submits no partial frame")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN("no graphics device");
            return;
        }

        TextureDesc colourDesc;
        colourDesc.Name = "sr-empty-geo-colour";
        colourDesc.Width = 8;
        colourDesc.Height = 8;
        colourDesc.Format = GraphicsFormat::Rgba32Float;
        colourDesc.IsShaderResource = true;
        Ref<Texture> colour = Texture::Create(*device, colourDesc);
        REQUIRE(colour != nullptr);

        RenderTargetDesc targetDesc;
        targetDesc.Name = "sr-empty-geo-target";
        targetDesc.Colour = colour;
        Ref<RenderTarget> target = RenderTarget::Create(*device, targetDesc);
        REQUIRE(target != nullptr);

        Ref<SceneRenderer> renderer = CreateRenderer(*device);
        REQUIRE(renderer != nullptr);

        // A mesh entry claiming geometry that has no indices: not drawable in any meaningful sense, and the renderer
        // must drop it while still clearing (so the frame is defined) rather than submitting an indexed draw with a
        // zero count.
        RenderPlan plan;
        ForwardDrawItem draw;
        static GeometryData emptyGeometry = [] {
            GeometryData geometry;
            geometry.Vertices.push_back(Renderer::GeometryVertex(Vec3(0.0f), Vec3(0.0f, 1.0f, 0.0f), Vec2(0.0f)));
            return geometry;  // vertices but no indices: IsEmpty() is false, and an indexed draw would read nothing
        }();
        draw.Geometry = &emptyGeometry;

        RenderMeshEntry entry;
        entry.Draw = draw;
        plan.Meshes.push_back(entry);

        Test::ScopedLogCapture logs;
        CHECK(renderer->RenderForward(plan, *target, 0u, Vec4(0.125f, 0.125f, 0.125f, 1.0f)));
        device->WaitForIdle();

        CHECK(renderer->GetDrawCount() == 0u);
        CHECK(renderer->GetCachedGeometryCount() == 0u);
        CHECK(logs.AnyMessageContains("no vertices or indices"));

        // And the frame is still defined — cleared, not stale.
        const Texel texel = ReadTexel(*device, *colour, 4u, 4u);
        CHECK(texel.Red == doctest::Approx(0.125f));
    }

    TEST_CASE("a draw referencing a light outside the frame is reported and skipped")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN("no graphics device");
            return;
        }

        TextureDesc colourDesc;
        colourDesc.Name = "sr-badlight-colour";
        colourDesc.Width = 16;
        colourDesc.Height = 16;
        colourDesc.Format = GraphicsFormat::Rgba32Float;
        colourDesc.IsShaderResource = true;
        Ref<Texture> colour = Texture::Create(*device, colourDesc);
        REQUIRE(colour != nullptr);

        RenderTargetDesc targetDesc;
        targetDesc.Name = "sr-badlight-target";
        targetDesc.Colour = colour;
        Ref<RenderTarget> target = RenderTarget::Create(*device, targetDesc);
        REQUIRE(target != nullptr);

        Ref<SceneRenderer> renderer = CreateRenderer(*device);
        REQUIRE(renderer != nullptr);

        const CameraState cameraState = MakeCameraState();
        RenderPlan plan = MakeTestPlan(cameraState, Vec3(0.5f, 0.5f, 0.5f), 1.0f);
        // A dangling index: the kind of thing a scene edited under the renderer produces. Silently dropping it changes
        // lighting with no trace, so it is reported per occurrence.
        plan.Meshes[0].Draw.LightIndices.push_back(99u);

        Test::ScopedLogCapture logs;
        CHECK(renderer->RenderForward(plan, *target, 0u));
        device->WaitForIdle();

        CHECK(renderer->GetDrawCount() == 1u);  // the draw still happens, minus the bad index
        // Matched by content rather than by count: this fixture's target has no depth attachment either, so the
        // renderer's once-per-renderer warning about that is also in here — asserting a total would make this case fail
        // for an unrelated reason if any other diagnostic were ever added.
        CHECK(logs.GetWarningCount() >= 1);
        CHECK(logs.AnyMessageContains("outside the frame"));
    }

    TEST_CASE("more lights than the bound are reported once and truncated")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN("no graphics device");
            return;
        }

        TextureDesc colourDesc;
        colourDesc.Name = "sr-manylights-colour";
        colourDesc.Width = 8;
        colourDesc.Height = 8;
        colourDesc.Format = GraphicsFormat::Rgba32Float;
        colourDesc.IsShaderResource = true;
        Ref<Texture> colour = Texture::Create(*device, colourDesc);
        REQUIRE(colour != nullptr);

        RenderTargetDesc targetDesc;
        targetDesc.Name = "sr-manylights-target";
        targetDesc.Colour = colour;
        Ref<RenderTarget> target = RenderTarget::Create(*device, targetDesc);
        REQUIRE(target != nullptr);

        SceneRendererDesc desc;
        desc.MaxLightsPerDraw = 2u;
        Ref<SceneRenderer> renderer = SceneRenderer::Create(*device, desc);
        REQUIRE(renderer != nullptr);

        const CameraState cameraState = MakeCameraState();
        RenderPlan plan = MakeTestPlan(cameraState, Vec3(0.5f, 0.5f, 0.5f), 1.0f);
        for (UInt32 extra = 0; extra < 5u; ++extra)
        {
            ForwardLight light;
            light.Light.Type = LightComponent::LightType::Point;
            light.WorldPosition = Vec3(1.0f * static_cast<float>(extra), 1.0f, 1.0f);
            light.Light.Intensity = 1.0f;
            light.Light.Range = 5.0f;
            plan.Pass.Lights.push_back(light);
        }
        REQUIRE(plan.Pass.Lights.size() == 6u);

        Test::ScopedLogCapture logs;
        CHECK(renderer->RenderForward(plan, *target, 0u));
        device->WaitForIdle();
        CHECK(logs.AnyMessageContains("later lights are ignored"));

        const Size warningsAfterFirst = static_cast<Size>(logs.GetWarningCount());
        CHECK(renderer->RenderForward(plan, *target, 0u));
        device->WaitForIdle();
        // Reported once per renderer: five extra lights at 144 Hz would otherwise bury every other message.
        CHECK(static_cast<Size>(logs.GetWarningCount()) == warningsAfterFirst);
    }
}
