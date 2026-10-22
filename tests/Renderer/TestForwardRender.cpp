// Tests for the forward pass's two halves that can be checked without a GPU:
//
//   1. The CPU reference (ComputeForwardShadingCpu) — its light loop, conventions and edge cases, pinned against
//      independently computed expectations rather than by re-calling the same code path.
//   2. The generated GLSL — it compiles, it calls only the shared Hp* shading functions, every uniform block goes
//      through the binding-macro convention, and the light-array sizing follows the requested maximum.
//
// The GPU-vs-CPU numeric equivalence for a rendered cube is a separate test (TestForwardRenderGpu.cpp): this file
// needs no device, so it runs — with leak checking — everywhere the shading references do.

#include "TestSupport.h"

#include <cmath>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Graphics/ShaderBindings.h"
#include "Hollowpoint/Graphics/ShaderCompiler.h"
#include "Hollowpoint/Renderer/ForwardRender.h"
#include "Hollowpoint/Renderer/Geometry.h"

using namespace Hollowpoint;
using namespace Hollowpoint::Renderer;
using Math::Float32;
using Math::Vec3;

namespace
{

constexpr Float32 kEpsilon = 1.0e-4f;

[[nodiscard]] bool Approximately(const Vec3& a, const Vec3& b, Float32 tolerance = kEpsilon)
{
    return std::abs(a.X - b.X) <= tolerance && std::abs(a.Y - b.Y) <= tolerance && std::abs(a.Z - b.Z) <= tolerance;
}

[[nodiscard]] std::string Describe(const Vec3& v)
{
    return "(" + std::to_string(v.X) + ", " + std::to_string(v.Y) + ", " + std::to_string(v.Z) + ")";
}

//! A frame with the camera on +Z looking at the origin, and one face of a unit cube at z = +0.5 facing it.
struct BasicScene
{
    ForwardPassParams Params;
    ForwardDrawItem Draw;
    Renderer::GeometryData Cube = Renderer::MakeCube();

    BasicScene()
    {
        Params.CameraPosition = Vec3(0.0f, 0.0f, 3.0f);
        Draw.Geometry = &Cube;
        Draw.Material.BaseColor = Vec3(0.8f, 0.2f, 0.1f);
        Draw.Material.Roughness = 0.5f;
        Draw.Material.Metallic = 0.0f;
    }
};

}  // namespace

TEST_SUITE("Renderer/ForwardRender")
{
    TEST_CASE("a draw with no lights renders exactly black")
    {
        BasicScene scene;
        const Vec3 result =
            ComputeForwardShadingCpu(scene.Params, scene.Draw, Vec3(0.0f, 0.0f, 0.5f), Vec3(0.0f, 0.0f, 1.0f));

        // Not "near black" — exactly zero. Any ambient term leaking in here would silently mask every future test
        // that expects darkness, so the exactness is the point.
        CHECK(result == Vec3(0.0f));
    }

    TEST_CASE("a directional light at normal incidence matches radiance times the BRDF evaluated independently")
    {
        BasicScene scene;

        ForwardLight entry;
        entry.Light.Type = LightComponent::LightType::Directional;
        entry.Light.Color = Vec3(1.0f, 0.5f, 0.25f);
        entry.Light.Intensity = 3.0f;
        entry.WorldDirection = Vec3(0.0f, 0.0f, -1.0f);  // points at the face from +Z
        scene.Params.Lights.push_back(entry);
        scene.Draw.LightIndices = {0};

        const Vec3 worldPosition(0.0f, 0.0f, 0.5f);
        const Vec3 result = ComputeForwardShadingCpu(scene.Params, scene.Draw, worldPosition, Vec3(0.0f, 0.0f, 1.0f));

        // Expected value assembled from the primitives by hand: radiance × BRDF at exactly normal incidence.
        // dotNL = 1, dotNV = 1 — the one configuration where the geometric-terms denominators are known exactly.
        const Vec3 viewDirection(0.0f, 0.0f, 1.0f);
        const Vec3 lightDirection(0.0f, 0.0f, 1.0f);
        const PbrDirectLightResult brdf =
            EvaluateDirectLight(scene.Draw.Material, Vec3(0.0f, 0.0f, 1.0f), viewDirection, lightDirection);
        const Vec3 expected = entry.Light.Color * (entry.Light.Intensity * 1.0f) * brdf.Total();

        INFO("got " << Describe(result) << " expected " << Describe(expected));
        CHECK(Approximately(result, expected));

        // Sanity that the reference is not vacuously returning the BRDF: an unoccluded white-lit dielectric must be
        // brighter than zero and dimmer than radiance (the BRDF scatters a fraction).
        CHECK(result.X > 0.0f);
        CHECK(result.X < entry.Light.Color.X * entry.Light.Intensity);
    }

    TEST_CASE("doubling intensity doubles the result exactly")
    {
        BasicScene scene;

        ForwardLight entry;
        entry.Light.Type = LightComponent::LightType::Point;
        entry.WorldPosition = Vec3(1.0f, 1.0f, 2.0f);
        entry.Light.Intensity = 2.0f;
        entry.Light.Range = 10.0f;
        scene.Params.Lights.push_back(entry);
        scene.Draw.LightIndices = {0};

        const Vec3 worldPosition(0.0f, 0.0f, 0.5f);
        const Vec3 single = ComputeForwardShadingCpu(scene.Params, scene.Draw, worldPosition, Vec3(0.0f, 0.0f, 1.0f));

        scene.Params.Lights[0].Light.Intensity = 4.0f;
        const Vec3 doubled = ComputeForwardShadingCpu(scene.Params, scene.Draw, worldPosition, Vec3(0.0f, 0.0f, 1.0f));

        // Linearity in radiance is a structural property: if it fails, something inside the loop applies intensity
        // non-linearly (a squared falloff on top of attenuation, say) — an error that hides inside "looks brighter".
        CHECK(doubled.X == doctest::Approx(single.X * 2.0f).epsilon(1e-3));
        CHECK(doubled.Y == doctest::Approx(single.Y * 2.0f).epsilon(1e-3));
    }

    TEST_CASE("a light behind the surface contributes exactly zero")
    {
        BasicScene scene;

        ForwardLight entry;
        entry.Light.Type = LightComponent::LightType::Directional;
        entry.WorldDirection = Vec3(0.0f, 0.0f, 1.0f);  // points away from the +Z-facing face
        scene.Params.Lights.push_back(entry);
        scene.Draw.LightIndices = {0};

        const Vec3 result =
            ComputeForwardShadingCpu(scene.Params, scene.Draw, Vec3(0.0f, 0.0f, 0.5f), Vec3(0.0f, 0.0f, 1.0f));
        CHECK(result == Vec3(0.0f));
    }

    TEST_CASE("inactive lights are skipped and dangling indices contribute nothing")
    {
        BasicScene scene;

        ForwardLight lit;
        lit.Light.Type = LightComponent::LightType::Directional;
        lit.WorldDirection = Vec3(0.0f, 0.0f, -1.0f);
        lit.Light.Active = false;  // switched off in the editor but state kept
        scene.Params.Lights.push_back(lit);

        scene.Draw.LightIndices = {0};
        CHECK(ComputeForwardShadingCpu(scene.Params, scene.Draw, Vec3(0.0f), Vec3(0.0f, 0.0f, 1.0f)) == Vec3(0.0f));

        // Dangling index: asserted (a wiring bug) and skipped in release — the release path must render, not read
        // out of bounds. The assert capture proves the debug report fires too.
        scene.Draw.LightIndices = {7};
        {
            Test::ScopedAssertCapture asserts;
            const Vec3 result = ComputeForwardShadingCpu(scene.Params, scene.Draw, Vec3(0.0f), Vec3(0.0f, 0.0f, 1.0f));
            // Skipping (rendering nothing) is the safe action after reporting; HP_ASSERT fires in every build
            // configuration by design (engine invariants are not debug-only), so the capture check holds here too.
            CHECK(result == Vec3(0.0f));
            CHECK(asserts.GetCaptureCount() >= 1);
        }
    }

    TEST_CASE("a spot light is full strength inside the cone and exactly zero outside")
    {
        BasicScene scene;

        ForwardLight spot;
        spot.Light.Type = LightComponent::LightType::Spot;
        spot.WorldPosition = Vec3(0.0f, 0.0f, 2.0f);
        spot.WorldDirection = Vec3(0.0f, 0.0f, -1.0f);  // straight down the -Z axis
        spot.Light.Range = 0.0f;                        // unbounded: isolates the cone behaviour from falloff
        spot.Light.SpotInnerAngleRadians = 0.35f;       // 20 degrees
        spot.Light.SpotOuterAngleRadians = 0.5f;        // 28.6 degrees

        scene.Params.Lights.push_back(spot);
        scene.Draw.LightIndices = {0};

        // On-axis: the surface→light vector is +Z, negated against the axis gives cos = 1 — inside the inner cone.
        const Vec3 onAxis =
            ComputeForwardShadingCpu(scene.Params, scene.Draw, Vec3(0.0f, 0.0f, 0.5f), Vec3(0.0f, 0.0f, 1.0f));
        CHECK(onAxis.X > 0.0f);

        // Off-axis at 45 degrees from the axis: outside the outer cone, contribution exactly zero — not small.
        // "Small but nonzero" would mean the smoothstep window is misapplied and lights leak past their edge.
        scene.Params.Lights[0].WorldPosition = Vec3(2.0f, 0.0f, 2.0f);
        const Vec3 offAxis =
            ComputeForwardShadingCpu(scene.Params, scene.Draw, Vec3(0.0f, 0.0f, 0.5f), Vec3(0.0f, 0.0f, 1.0f));
        CHECK(offAxis == Vec3(0.0f));
    }

    TEST_CASE("camera coincident with the surface returns black instead of NaN")
    {
        BasicScene scene;

        ForwardLight entry;
        entry.Light.Type = LightComponent::LightType::Directional;
        entry.WorldDirection = Vec3(0.0f, 0.0f, -1.0f);
        scene.Params.Lights.push_back(entry);
        scene.Draw.LightIndices = {0};

        scene.Params.CameraPosition = Vec3(0.0f, 0.0f, 0.5f);  // exactly on the surface
        const Vec3 result =
            ComputeForwardShadingCpu(scene.Params, scene.Draw, Vec3(0.0f, 0.0f, 0.5f), Vec3(0.0f, 0.0f, 1.0f));

        // A NaN here propagates through tone mapping into a black screen with no clue where it came from; the
        // documented answer is black, which is at least a defined pixel.
        CHECK(result == Vec3(0.0f));
    }

    TEST_CASE("world normals use the inverse transpose — non-uniform scale proves it")
    {
        // The classic test for this: scale x by 2 and take a face whose local normal is (1,1,0)/sqrt(2). The true
        // world normal of the stretched surface is NOT the transformed normal direction; hand-computed values:
        //   inverse-transpose path: n = (0.5, 1.0, 0) normalised = (0.44721, 0.89443, 0)
        //   naive matrix path:    n = (2.0, 1.0, 0) normalised = (0.89443, 0.44721, 0)
        // If someone "simplifies" this function to use the model matrix directly, exactly one of these fails.
        const Mat4 stretch = Mat4::Scale(Vec3(2.0f, 1.0f, 1.0f));
        const Vec3 localNormal(0.70710678f, 0.70710678f, 0.0f);

        const Vec3 world = ComputeWorldNormal(stretch, localNormal).Normalized();
        CHECK(world.X == doctest::Approx(0.44721f).epsilon(1e-3));
        CHECK(world.Y == doctest::Approx(0.89443f).epsilon(1e-3));
        CHECK(world.Z == doctest::Approx(0.0f).epsilon(1e-3));

        // Uniform scale must leave normals alone (the common case): scaling by 5 changes nothing about orientation.
        const Vec3 uniform = ComputeWorldNormal(Mat4::Scale(Vec3(5.0f)), localNormal).Normalized();
        CHECK(uniform.X == doctest::Approx(localNormal.X).epsilon(1e-3));
        CHECK(uniform.Y == doctest::Approx(localNormal.Y).epsilon(1e-3));

        // A zero scale axis has no surface: asserted, and the safe fallback keeps the direction instead of NaN.
        {
            Test::ScopedAssertCapture asserts;
            const Vec3 degenerate = ComputeWorldNormal(Mat4::Scale(Vec3(0.0f, 1.0f, 1.0f)), localNormal);
            CHECK_FALSE(std::isnan(degenerate.X));
            CHECK(asserts.GetCaptureCount() >= 1);
        }
    }

    TEST_CASE("the generated forward shaders compile through glslang")
    {
        // Assembly order is the documented contract: #version first, then binding macros, then stage body.
        const std::string header = "#version 450\n" + Graphics::BuildGlslBindingPrelude();

        const Graphics::CompiledShader vertex =
            Graphics::CompileGlsl(Graphics::ShaderStage::Vertex, header + BuildForwardVertexGlsl());
        INFO(vertex.Diagnostics);
        CHECK(vertex.Succeeded);

        for (const UInt32 lightCount : {1u, 8u, 64u})
        {
            const Graphics::CompiledShader fragment =
                Graphics::CompileGlsl(Graphics::ShaderStage::Pixel, header + BuildForwardFragmentGlsl(lightCount));
            INFO("light count " << lightCount << ": " << fragment.Diagnostics);
            CHECK(fragment.Succeeded);
        }
    }

    TEST_CASE("the generated fragment shader calls the shared shading functions and no lighting of its own")
    {
        const std::string source = BuildForwardFragmentGlsl(8u);
        const std::size_t mainStart = source.find("void main()");
        REQUIRE(mainStart != std::string::npos);
        const std::string body = source.substr(mainStart);

        // Every shading quantity must come from the generated shared model. If someone hand-writes GGX inside main,
        // CPU/GPU equivalence silently becomes two implementations again — the thing the generator exists to stop.
        CHECK(body.find("HpShadeDirect") != std::string::npos);
        CHECK(body.find("HpDistanceAttenuation") != std::string::npos);
        CHECK(body.find("HpSpotAngleAttenuation") != std::string::npos);

        // No tone map here: the pass writes scene-linear HDR and the blit owns display conversion. A second tone
        // map applied at both stages darkens everything into mush — visible, but only after someone ships a frame.
        CHECK(body.find("HpLinearToDisplay") == std::string::npos);
        CHECK(body.find("HpToneMapAceFilmic") == std::string::npos);

        // No hand-typed GGX machinery inside main (D/G/F are the generator's job).
        CHECK(body.find("DistributionGGX") == std::string::npos);
        CHECK(body.find("FresnelSchlick") == std::string::npos);
    }

    TEST_CASE("every uniform block goes through the binding-macro convention")
    {
        for (const std::string& source : {BuildForwardVertexGlsl(), BuildForwardFragmentGlsl(8u)})
        {
            // Raw `binding = <number>` means someone bypassed HP_BINDING_*, and on NVRHI/Vulkan the uniform then
            // reads all zeros with no error of any kind (session 12's worst bug). The macro is the only legal form.
            std::size_t position = 0;
            while ((position = source.find("binding =", position)) != std::string::npos)
            {
                const std::string after = source.substr(position, 40);
                CHECK_MESSAGE(after.find("HP_BINDING_CONSTANT_BUFFER") != std::string::npos,
                              "raw binding number in generated shader: ..." << after.substr(0, 30));
                position += 7;
            }

            // Both stages declare `set = 0`; a second set needs pipeline-layout work the engine has not done yet.
            CHECK(source.find("set = 0") != std::string::npos);
        }
    }

    TEST_CASE("light array sizing follows the requested maximum")
    {
        const std::string eight = BuildForwardFragmentGlsl(8u);
        CHECK(eight.find("PositionAndIntensity[8]") != std::string::npos);
        // Index block packs four ints per ivec4: 8 indices fit in two vectors, and the ceiling division must round
        // up — a truncating (count / 4) would drop the last lights for any count that is not a multiple of four.
        CHECK(eight.find("LightIndices[2]") != std::string::npos);

        const std::string nine = BuildForwardFragmentGlsl(9u);
        CHECK(nine.find("PositionAndIntensity[9]") != std::string::npos);
        CHECK(nine.find("LightIndices[3]") != std::string::npos);  // ceil(9/4) — the truncation trap, pinned

        // The out-of-range guard must compare against the same generated constant, not a hand-typed copy.
        CHECK(eight.find("index >= 8") != std::string::npos);
    }
}
