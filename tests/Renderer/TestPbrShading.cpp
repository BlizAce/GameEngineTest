#include <cmath>
#include <string>

#include <doctest/doctest.h>

#include "Hollowpoint/Renderer/PbrShading.h"

using namespace Hollowpoint;
using namespace Hollowpoint::Renderer;
using Math::Float32;
using Math::Vec3;

namespace
{

// ---------------------------------------------------------------------------
// Every expected value below is derived from the BRDF's closed form by hand (the derivation is in the comment),
// NOT produced by running the implementation. That independence is what makes these tests able to fail: a test
// whose expectation is copied from the code under test cannot detect anything.
// ---------------------------------------------------------------------------

[[nodiscard]] bool Approximately(const Vec3& a, const Vec3& b, Float32 tolerance = 1e-4f)
{
    const Float32 dx = std::fabs(a.X - b.X);
    const Float32 dy = std::fabs(a.Y - b.Y);
    const Float32 dz = std::fabs(a.Z - b.Z);
    return dx <= tolerance && dy <= tolerance && dz <= tolerance;
}

[[nodiscard]] std::string ToString(const Vec3& v)
{
    return "(" + std::to_string(v.X) + ", " + std::to_string(v.Y) + ", " + std::to_string(v.Z) + ")";
}

constexpr Float32 kPi = 3.14159265358979323846f;

const Vec3 kUp{0.0f, 1.0f, 0.0f};
const Vec3 kViewStraightOn{0.0f, 1.0f, 0.0f};

}  // namespace

TEST_SUITE("Renderer/Pbr")
{
    TEST_CASE("perceptual roughness maps to GGX alpha by squaring")
    {
        // a = r^2. Hand-computed:
        CHECK(PerceptualRoughnessToAlpha(1.0f) == doctest::Approx(1.0f));
        CHECK(PerceptualRoughnessToAlpha(0.5f) == doctest::Approx(0.25f));
        CHECK(PerceptualRoughnessToAlpha(0.25f) == doctest::Approx(0.0625f));

        // Identity would be the bug: assert it is NOT linear at a mid value where the difference is large.
        const Float32 atHalf = PerceptualRoughnessToAlpha(0.5f);
        const bool notIdentity = std::fabs(atHalf - 0.5f) > 0.1f;
        CHECK(notIdentity);
    }

    TEST_CASE("roughness is clamped and never produces a zero (NaN-producing) alpha")
    {
        // Out-of-range authoring must not yield alpha <= 0: the GGX denominator would go to zero and NaN would
        // propagate through an accumulation buffer.
        const Float32 fromNegative = PerceptualRoughnessToAlpha(-1.0f);
        const Float32 fromZero = PerceptualRoughnessToAlpha(0.0f);
        const Float32 fromAboveOne = PerceptualRoughnessToAlpha(5.0f);

        CHECK(fromNegative > 0.0f);
        CHECK(fromZero > 0.0f);
        CHECK(fromAboveOne == doctest::Approx(1.0f));

        // And the result is finite — a NaN would silently poison every downstream pixel.
        const bool allFinite = std::isfinite(fromNegative) && std::isfinite(fromZero) && std::isfinite(fromAboveOne);
        CHECK(allFinite);
    }

    TEST_CASE("GGX at normal incidence equals 1 / (pi * a^2)")
    {
        // With N == H, the TR form reduces: D = a^2 / (pi * ((a^2 - 1) + 1)^2) = a^2 / (pi * a^4) = 1 / (pi * a^2).
        const Float32 alpha = 0.25f;
        const Float32 expected = 1.0f / (kPi * alpha * alpha);

        const Float32 actual = DistributionGGX(kUp, kViewStraightOn, kUp, alpha);

        INFO("expected " << expected << " actual " << actual);
        CHECK(actual == doctest::Approx(expected));
    }

    TEST_CASE("a smoother surface has a higher peak and a steeper falloff")
    {
        // Physical requirement, independent of the formula: lower roughness concentrates energy, so the peak rises.
        const Float32 smooth = DistributionGGX(kUp, kViewStraightOn, kUp, 0.05f);
        const Float32 rough = DistributionGGX(kUp, kViewStraightOn, kUp, 0.5f);

        CHECK(smooth > rough);

        // And away from the peak the ordering can invert (energy is redistributed, not removed): at a wide half-angle
        // the rough surface spreads more light. Asserting only "smooth is higher everywhere" would be wrong physics.
        const Vec3 tiltedHalf = Vec3(0.7071f, 0.7071f, 0.0f);
        const Float32 smoothWide = DistributionGGX(kUp, kViewStraightOn, tiltedHalf, 0.05f);
        const Float32 roughWide = DistributionGGX(kUp, kViewStraightOn, tiltedHalf, 0.5f);

        CHECK(roughWide > smoothWide);
    }

    TEST_CASE("Fresnel hits exactly F0 head-on and 1 at grazing")
    {
        const Vec3 f0{0.04f, 0.04f, 0.04f};

        // (1 - cos)^5 with cos == 1 gives zero, so the result is exactly F0; with cos == 0 it is F0 + (1-F0) = 1.
        const Vec3 headOn = FresnelSchlick(1.0f, f0);
        const Vec3 grazing = FresnelSchlick(0.0f, f0);

        CHECK(Approximately(headOn, f0));
        CHECK(Approximately(grazing, Vec3(1.0f, 1.0f, 1.0f)));
    }

    TEST_CASE("Fresnel matches Schlick's polynomial at an interior angle")
    {
        // Endpoints alone do not pin the shape: exponent 4 vs 5 passes both. Hand-computed for cos = 0.5, F0 = 0.04:
        //   F = 0.04 + (1 - 0.04) * (1 - 0.5)^5 = 0.04 + 0.96 / 32 = 0.07 exactly.
        // Exponent 4 would give 0.04 + 0.96/16 = 0.10 — well outside tolerance.
        const Vec3 f0{0.04f, 0.04f, 0.04f};
        const Vec3 value = FresnelSchlick(0.5f, f0);

        INFO(ToString(value));
        CHECK(value.X == doctest::Approx(0.07f).epsilon(1e-3));
        CHECK(value.Y == doctest::Approx(0.07f).epsilon(1e-3));

        // A tinted F0 must be evaluated per channel (metals reflect their colour), not averaged across RGB.
        const Vec3 tinted{0.9f, 0.2f, 0.1f};
        const Vec3 tintedValue = FresnelSchlick(0.5f, tinted);
        // R: 0.9 + 0.1/32 = 0.903125; G: 0.2 + 0.8/32 = 0.225; B: 0.1 + 0.9/32 = 0.128125
        CHECK(tintedValue.X == doctest::Approx(0.903125f).epsilon(1e-4));
        CHECK(tintedValue.Y == doctest::Approx(0.225f).epsilon(1e-4));
        CHECK(tintedValue.Z == doctest::Approx(0.128125f).epsilon(1e-4));
    }

    TEST_CASE("Fresnel is monotonically increasing toward grazing")
    {
        const Vec3 f0{0.04f, 0.04f, 0.04f};

        Float32 previous = FresnelSchlick(1.0f, f0).X;
        bool monotonic = true;

        for (Float32 cosine = 0.9f; cosine >= -0.001f; cosine -= 0.1f)
        {
            const Float32 value = FresnelSchlick(cosine, f0).X;
            if (value < previous - 1e-6f)
            {
                monotonic = false;
            }
            previous = value;
        }

        CHECK(monotonic);
    }

    TEST_CASE("roughness-aware Fresnel saturates a rough surface instead of going chrome")
    {
        const Vec3 f0{0.04f, 0.04f, 0.04f};

        // At grazing with high roughness the result must stay near 1 - roughness (0.1 for roughness 0.9), not climb
        // to 1.0 as plain Schlick does. This is the "chrome rim on matte material" artefact.
        const Vec3 roughGrazing = FresnelSchlickRoughness(0.0f, f0, 0.9f);

        INFO(ToString(roughGrazing));
        CHECK(roughGrazing.X < 0.2f);

        // A smooth surface still goes to (nearly) 1 at grazing.
        const Vec3 smoothGrazing = FresnelSchlickRoughness(0.0f, f0, 0.05f);
        CHECK(smoothGrazing.X > 0.9f);
    }

    TEST_CASE("Smith geometry is zero for a back-facing direction and one at normal incidence")
    {
        // Back-facing: no microfacets contribute along a direction behind the surface.
        CHECK(GeometrySchlickGGX(-0.5f, 0.1f) == 0.0f);
        CHECK(GeometrySchlickGGX(0.0f, 0.1f) == 0.0f);

        // At normal incidence (n.d == 1): G = 1 / (1*(1-k) + k) = 1 exactly, for any k. Asserting this independently
        // of k catches a denominator that omits a term.
        CHECK(GeometrySchlickGGX(1.0f, 0.0f) == doctest::Approx(1.0f));
        CHECK(GeometrySchlickGGX(1.0f, 0.3f) == doctest::Approx(1.0f));
        CHECK(GeometrySchlickGGX(1.0f, 0.9f) == doctest::Approx(1.0f));

        // Hand-computed for n.d = 0.5, k = 0.25: 0.5 / (0.5*0.75 + 0.25) = 0.5 / 0.625 = 0.8.
        CHECK(GeometrySchlickGGX(0.5f, 0.25f) == doctest::Approx(0.8f));
    }

    TEST_CASE("direct lighting is zero when the light is behind the surface")
    {
        const PbrMaterialParameters material{};

        // Light below the horizon: must contribute nothing rather than negative light (which would subtract from the
        // frame and show up as dark halos).
        //
        // The direction is deliberately NOT antiparallel to the view. With L == -V the half vector V+L is the zero
        // vector, Fresnel saturates to 1, and (1 - F) zeroes the diffuse term on its own — so a mutant that simply
        // took abs(cosine) survived this test when it used straight-down light. Rejecting back-facing light has to be
        // exercised in a configuration where nothing else happens to mask it.
        const Vec3 belowHorizon = Vec3(0.7f, -0.7f, 0.0f).Normalized();

        const PbrDirectLightResult result = EvaluateDirectLight(material, kUp, kViewStraightOn, belowHorizon);

        INFO(ToString(result.Total()));
        CHECK(result.Diffuse.X == 0.0f);
        CHECK(result.Specular.X == 0.0f);
        CHECK(result.Total().X == 0.0f);

        // Just past the horizon in the other direction it must contribute, or the surface would be black at every
        // grazing light angle. Together with the case above this pins the rejection to the sign of the cosine rather
        // than to a magnitude threshold.
        const Vec3 justAboveHorizon = Vec3(0.99f, 0.14f, 0.0f).Normalized();
        const PbrDirectLightResult lit = EvaluateDirectLight(material, kUp, kViewStraightOn, justAboveHorizon);
        CHECK(lit.Total().X > 0.0f);
    }

    TEST_CASE("a pure metal has no diffuse component")
    {
        PbrMaterialParameters material;
        material.Metallic = 1.0f;
        material.BaseColor = Vec3(0.8f, 0.2f, 0.1f);

        const PbrDirectLightResult result = EvaluateDirectLight(material, kUp, kViewStraightOn, kUp);

        // Diffuse is weighted by (1 - metallic), so metallic == 1 gives exactly zero. A non-zero diffuse here would
        // make metals look like chalk painted with a specular highlight.
        INFO(ToString(result.Diffuse));
        const bool diffuseIsZero = std::fabs(result.Diffuse.X) < 1e-6f && std::fabs(result.Diffuse.Y) < 1e-6f &&
                                   std::fabs(result.Diffuse.Z) < 1e-6f;
        CHECK(diffuseIsZero);

        // And the metal still has specular.
        const bool specularPresent = result.Specular.X > 0.0f;
        CHECK(specularPresent);
    }

    TEST_CASE("a dielectric keeps its diffuse albedo and a faint untinted specular")
    {
        PbrMaterialParameters material;
        material.Metallic = 0.0f;
        material.BaseColor = Vec3(0.7f, 0.3f, 0.2f);

        const PbrDirectLightResult result = EvaluateDirectLight(material, kUp, kViewStraightOn, kUp);

        // Lambertian diffuse at normal incidence: albedo / pi, minus the small specular energy term. Hand estimate:
        // 0.7 / pi ~= 0.2228, reduced by (1 - F) with F ~= 0.04 -> ~0.2139. Tolerance is loose because the exact
        // Fresnel at this configuration is itself computed; the point is the order of magnitude and that it tracks
        // albedo/pi rather than, say, albedo (a missing /pi is a classic 3x-too-bright bug).
        const Float32 lambertEstimate = material.BaseColor.X / kPi;
        INFO("diffuse " << ToString(result.Diffuse) << " lambert estimate " << lambertEstimate);

        CHECK(result.Diffuse.X > lambertEstimate * 0.8f);
        CHECK(result.Diffuse.X < lambertEstimate * 1.05f);

        // Specular must be present but small (dielectric F0 = 0.04).
        CHECK(result.Specular.X > 0.0f);
        CHECK(result.Specular.X < result.Diffuse.X);
    }

    TEST_CASE("diffuse falls off with the cosine of the incidence angle")
    {
        const PbrMaterialParameters material;

        // Light straight up (cos = 1) versus 60 degrees off (cos = 0.5). Lambert's cosine law: half the irradiance.
        const PbrDirectLightResult headOn = EvaluateDirectLight(material, kUp, kViewStraightOn, kUp);
        const Vec3 lightAt60 = Vec3(0.8660254f, 0.5f, 0.0f).Normalized();
        const PbrDirectLightResult oblique = EvaluateDirectLight(material, kUp, kViewStraightOn, lightAt60);

        // Diffuse ratio should be ~2:1 from the cosine alone (the specular term changes too, so it is compared on
        // diffuse only).
        const Float32 ratio = headOn.Diffuse.X / oblique.Diffuse.X;
        INFO("ratio " << ratio);
        CHECK(ratio == doctest::Approx(2.0f).epsilon(0.05f));
    }

    TEST_CASE("rougher surfaces spread specular energy and lower its peak")
    {
        PbrMaterialParameters smooth;
        smooth.Roughness = 0.1f;
        PbrMaterialParameters rough;
        rough.Roughness = 0.9f;

        // Head-on configuration: N == V == L, so H == N and the distribution peak dominates. Smoother => higher peak.
        const PbrDirectLightResult smoothResult = EvaluateDirectLight(smooth, kUp, kViewStraightOn, kUp);
        const PbrDirectLightResult roughResult = EvaluateDirectLight(rough, kUp, kViewStraightOn, kUp);

        INFO("smooth " << ToString(smoothResult.Specular) << " rough " << ToString(roughResult.Specular));
        CHECK(smoothResult.Specular.X > roughResult.Specular.X);
    }

    TEST_CASE("the result is always finite for extreme inputs")
    {
        // NaNs in a frame buffer are catastrophic and hard to trace; the shading model must not create them even for
        // degenerate geometry. These inputs are deliberately hostile: near-antiparallel view/light, zero roughness,
        // out-of-range metallic.
        const Vec3 cases[][3] = {
            {kUp, Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f)},
            {kUp, Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f)},
            {Vec3(0.0f, 0.0f, 1.0f), Vec3(0.0f, 1.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f)},
        };

        for (const auto& testCase : cases)
        {
            PbrMaterialParameters material;
            material.Roughness = 0.0f;
            material.Metallic = 2.0f;  // Out of range on purpose.

            const PbrDirectLightResult result = EvaluateDirectLight(material, testCase[0], testCase[1], testCase[2]);

            const bool finite = std::isfinite(result.Diffuse.X) && std::isfinite(result.Diffuse.Y) &&
                                std::isfinite(result.Diffuse.Z) && std::isfinite(result.Specular.X) &&
                                std::isfinite(result.Specular.Y) && std::isfinite(result.Specular.Z);

            INFO(ToString(testCase[0]) << " / " << ToString(testCase[1]) << " / " << ToString(testCase[2]));
            CHECK(finite);
        }
    }

    TEST_CASE("energy never goes negative")
    {
        // Negative light subtracts from the frame and produces dark halos; it happens when a cosine leaks into a
        // denominator or a Fresnel term is mis-signed.
        for (Float32 roughness = 0.0f; roughness <= 1.0f; roughness += 0.1f)
        {
            for (Float32 metallic = 0.0f; metallic <= 1.0f; metallic += 0.25f)
            {
                for (Float32 elevation = 0.0f; elevation <= kPi * 0.5f; elevation += 0.2f)
                {
                    PbrMaterialParameters material;
                    material.Roughness = roughness;
                    material.Metallic = metallic;

                    const Vec3 light = Vec3(std::cos(elevation), std::sin(elevation), 0.0f).Normalized();
                    const PbrDirectLightResult result = EvaluateDirectLight(material, kUp, kViewStraightOn, light);

                    INFO("r=" << roughness << " m=" << metallic << " elev=" << elevation);

                    const bool nonNegative = result.Diffuse.X >= 0.0f && result.Diffuse.Y >= 0.0f &&
                                             result.Diffuse.Z >= 0.0f && result.Specular.X >= 0.0f &&
                                             result.Specular.Y >= 0.0f && result.Specular.Z >= 0.0f;
                    CHECK(nonNegative);
                }
            }
        }
    }
}
