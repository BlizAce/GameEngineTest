#include <cmath>
#include <limits>
#include <string>

#include <doctest/doctest.h>

#include "Hollowpoint/Renderer/Lights.h"

using namespace Hollowpoint;
using namespace Hollowpoint::Renderer;
using Math::Float32;
using Math::Vec3;

namespace
{

constexpr Float32 kPi = 3.14159265358979323846f;

[[nodiscard]] bool Approximately(const Vec3& a, const Vec3& b, Float32 tolerance = 1e-4f)
{
    return std::fabs(a.X - b.X) <= tolerance && std::fabs(a.Y - b.Y) <= tolerance && std::fabs(a.Z - b.Z) <= tolerance;
}

[[nodiscard]] std::string ToString(const Vec3& v)
{
    return "(" + std::to_string(v.X) + ", " + std::to_string(v.Y) + ", " + std::to_string(v.Z) + ")";
}

[[nodiscard]] LightComponent MakePointLight(Float32 range = 0.0f)
{
    LightComponent light;
    light.Type = LightComponent::LightType::Point;
    light.Color = Vec3(1.0f, 1.0f, 1.0f);
    light.Intensity = 1.0f;
    light.Range = range;

    return light;
}

}  // namespace

TEST_SUITE("Renderer/Lights")
{
    TEST_CASE("point light falls off with inverse square of distance")
    {
        // Unbounded range -> pure 1/d^2. Hand-computed (called through the component as well as directly, so a
        // mistake in how EvaluatePointLightRadiance passes Range through is covered too):
        CHECK(DistanceAttenuation(1.0f, 0.0f) == doctest::Approx(1.0f));
        CHECK(DistanceAttenuation(2.0f, 0.0f) == doctest::Approx(0.25f));
        CHECK(DistanceAttenuation(4.0f, 0.0f) == doctest::Approx(0.0625f));
        CHECK(DistanceAttenuation(10.0f, 0.0f) == doctest::Approx(0.01f));

        // Doubling distance must quarter the light: the ratio is exact for pure inverse square and is independent of
        // any single expected value, so it catches a wrong exponent that a point check might miss (e.g. 1/d^3 also
        // passes "further is dimmer" but gives a ratio of 8).
        const Float32 ratio = DistanceAttenuation(2.0f, 0.0f) / DistanceAttenuation(4.0f, 0.0f);
        INFO("ratio " << ratio);
        CHECK(ratio == doctest::Approx(4.0f).epsilon(1e-5));

        // Same through the component path: radiance must equal colour * intensity * attenuation.
        const LightComponent light = MakePointLight();
        const Vec3 viaComponent = EvaluatePointLightRadiance(light, 2.0f);
        CHECK(viaComponent.X == doctest::Approx(DistanceAttenuation(2.0f, light.Range)).epsilon(1e-6));
    }

    TEST_CASE("a bounded light reaches exactly zero at its range")
    {
        const Float32 range = 10.0f;

        // Inside the window the inverse-square core dominates, so a point well inside is close to 1/d^2.
        const Float32 nearCentre = DistanceAttenuation(1.0f, range);
        CHECK(nearCentre == doctest::Approx(1.0f).epsilon(0.05f));

        // At and beyond the range: exactly zero. Asserted exactly (not approximately) because a light that keeps a
        // faint tail past its declared range breaks per-tile light culling — the cull uses Range to decide inclusion,
        // so any contribution outside it is light that can appear or vanish depending on which tiles include the light.
        CHECK(DistanceAttenuation(range, range) == 0.0f);
        CHECK(DistanceAttenuation(range * 1.5f, range) == 0.0f);
    }

    TEST_CASE("the range window has no visible edge (zero slope at the boundary)")
    {
        // The reason for the (1 - x^4)^2 window rather than a linear fade. A linear window (1 - d/range) has slope
        // -1/range at the boundary, which draws a hard-edged circle on a floor; this window's slope must be far
        // smaller than the linear one's at the same place.
        const Float32 range = 10.0f;
        const Float32 step = 0.001f;

        const Float32 slopeAtEdge =
            (DistanceAttenuation(range - step, range) - DistanceAttenuation(range, range)) / step;
        const Float32 linearSlopeMagnitude = 1.0f / range;

        INFO("slope at edge " << slopeAtEdge);
        CHECK(std::fabs(slopeAtEdge) < linearSlopeMagnitude * 0.05f);

        // Window shape, pinned by an interior value: at half range the ^4 window is (1 - 0.5^4)^2 = 0.8789 of its
        // unwindowed inverse-square value. This is the assertion that distinguishes the exponent — smoothness at the
        // edge holds for (1-x^n)^2 with any n, so "no ring" and monotonicity together cannot catch a changed exponent
        // (a mutant swapping ^4 for ^2 survived until this check existed). Measured: 0.5625 for n=2, 0.25 for n=1.
        const Float32 halfRange = range * 0.5f;
        const Float32 windowedAtHalf = DistanceAttenuation(halfRange, range);
        const Float32 unwindowedAtHalf = DistanceAttenuation(halfRange, 0.0f);

        const Float32 windowFraction = windowedAtHalf / unwindowedAtHalf;
        INFO("window fraction at half range " << windowFraction);
        CHECK(windowFraction == doctest::Approx(0.878906f).epsilon(1e-4));

        // And the approach to zero is smooth rather than a knee: the slope just inside must be smaller in magnitude
        // than it is further in (energy leaving smoothly).
        const Float32 slopeMid =
            (DistanceAttenuation(range * 0.5f, range) - DistanceAttenuation(range * 0.5f + step, range)) / step;

        INFO("slope at mid-range " << slopeMid);
        CHECK(std::fabs(slopeMid) > std::fabs(slopeAtEdge));
    }

    TEST_CASE("attenuation is monotonic decreasing with distance")
    {
        const Float32 ranges[] = {0.0f, 5.0f, 10.0f, 100.0f};

        for (const Float32 range : ranges)
        {
            Float32 previous = -1.0f;
            bool monotonic = true;

            for (Float32 d = 0.05f; d <= 20.0f; d += 0.05f)
            {
                const Float32 value = DistanceAttenuation(d, range);
                if (value > previous + 1e-9f && previous >= 0.0f)
                {
                    monotonic = false;
                }
                previous = value;
            }

            INFO("range " << range);
            CHECK(monotonic);
        }
    }

    TEST_CASE("a surface point at the light is finite, not infinite")
    {
        // A lamp touching a table puts shaded points at distance 0 (and NaN/Inf there blooms into a full-screen white
        // flash). The implementation treats lights as small emissive spheres, so attenuation is bounded.
        const Float32 atLight = DistanceAttenuation(0.0f, 0.0f);

        INFO("attenuation at d=0: " << atLight);
        CHECK(std::isfinite(atLight));
        CHECK(atLight > 0.0f);
        CHECK(atLight <= 1e4f);

        // Negative distance (reachable from a sign slip in a caller) must not produce NaN or a negative light.
        const Float32 negative = DistanceAttenuation(-3.0f, 0.0f);
        CHECK(negative == doctest::Approx(DistanceAttenuation(3.0f, 0.0f)));

        // And it must be non-negative for every sample — a negative light subtracts from the frame and shows as dark
        // halos around lights.
        bool nonNegative = true;
        for (Float32 d = 0.0f; d <= 50.0f; d += 0.1f)
        {
            if (DistanceAttenuation(d, 7.5f) < 0.0f)
            {
                nonNegative = false;
            }
        }
        CHECK(nonNegative);
    }

    TEST_CASE("spot profile is 1 inside the inner cone and 0 outside the outer")
    {
        // Inner 35 deg, outer 45 deg (the defaults). Inside the cone the dot product with the negated axis is ~1.
        const Float32 innerHalfCosine = std::cos(0.6108652f * 0.5f);  // cos(17.5 deg) ~= 0.95372
        const Float32 outerHalfCosine = std::cos(0.7853982f * 0.5f);  // cos(22.5 deg) ~= 0.92388

        CHECK(SpotAngleAttenuation(1.0f, 0.6108652f, 0.7853982f) == doctest::Approx(1.0f));
        CHECK(SpotAngleAttenuation(innerHalfCosine, 0.6108652f, 0.7853982f) == doctest::Approx(1.0f).epsilon(1e-4));

        // Exactly at the outer boundary: zero.
        CHECK(SpotAngleAttenuation(outerHalfCosine, 0.6108652f, 0.7853982f) == doctest::Approx(0.0f).epsilon(1e-4));

        // Beyond it (smaller cosine = wider angle): still zero.
        CHECK(SpotAngleAttenuation(0.0f, 0.6108652f, 0.7853982f) == 0.0f);
        CHECK(SpotAngleAttenuation(-1.0f, 0.6108652f, 0.7853982f) == 0.0f);

        // Half-way through the penumbra in cosine space is exactly 0.5 for smoothstep at its midpoint.
        const Float32 midCosine = (innerHalfCosine + outerHalfCosine) * 0.5f;
        CHECK(SpotAngleAttenuation(midCosine, 0.6108652f, 0.7853982f) == doctest::Approx(0.5f).epsilon(1e-4));
    }

    TEST_CASE("spot profile increases monotonically toward the cone axis")
    {
        Float32 previous = -1.0f;
        bool monotonic = true;

        // Walking from outside the cone (low cosine) to inside it (cosine 1).
        for (Float32 dot = 0.80f; dot <= 1.001f; dot += 0.005f)
        {
            const Float32 value = SpotAngleAttenuation(dot, 0.6108652f, 0.7853982f);
            if (value < previous - 1e-6f)
            {
                monotonic = false;
            }
            previous = value;
        }

        CHECK(monotonic);
    }

    TEST_CASE("a degenerate cone (inner == outer) does not divide by zero")
    {
        // Reachable from a slider that clamps inner to outer, or a scene file with both at 0. The implementation must
        // produce a finite step rather than NaN.
        const Float32 onAxis = SpotAngleAttenuation(1.0f, 0.5f, 0.5f);
        const Float32 offAxis = SpotAngleAttenuation(0.1f, 0.5f, 0.5f);

        INFO("on " << onAxis << " off " << offAxis);
        CHECK(std::isfinite(onAxis));
        CHECK(std::isfinite(offAxis));
        CHECK(onAxis >= 0.0f);
        CHECK(onAxis <= 1.0f);
        CHECK(offAxis >= 0.0f);
        CHECK(offAxis <= 1.0f);

        // Inverted cone (inner > outer) likewise, and still bounded.
        const Float32 inverted = SpotAngleAttenuation(0.95f, 1.0f, 0.2f);
        CHECK(std::isfinite(inverted));
    }

    TEST_CASE("point light radiance scales with colour and intensity")
    {
        LightComponent light = MakePointLight();
        light.Color = Vec3(1.0f, 0.5f, 0.25f);
        light.Intensity = 4.0f;

        const Vec3 atTwo = EvaluatePointLightRadiance(light, 2.0f);

        // Colour * Intensity / d^2 = (1, 0.5, 0.25) * 4 / 4 = (1, 0.5, 0.25).
        INFO(ToString(atTwo));
        CHECK(Approximately(atTwo, Vec3(1.0f, 0.5f, 0.25f)));

        // Halving intensity must halve radiance exactly — the property an animator relies on when fading a light, and
        // the reason colour and intensity are stored separately rather than premultiplied.
        light.Intensity = 2.0f;
        const Vec3 faded = EvaluatePointLightRadiance(light, 2.0f);

        CHECK(faded.X == doctest::Approx(atTwo.X * 0.5f).epsilon(1e-5));
        CHECK(faded.Y == doctest::Approx(atTwo.Y * 0.5f).epsilon(1e-5));
    }

    TEST_CASE("spot radiance is point radiance times the angular profile")
    {
        LightComponent light = MakePointLight();
        light.Type = LightComponent::LightType::Spot;
        light.Intensity = 3.0f;
        light.Range = 20.0f;

        // On axis: profile is 1, so spot radiance must equal point radiance exactly. Asserting equality (rather than a
        // fresh expected value) pins that the angular term is applied multiplicatively and not added or double-applied.
        const Vec3 onAxis = EvaluateSpotLightRadiance(light, 5.0f, 1.0f);
        const Vec3 asPoint = EvaluatePointLightRadiance(light, 5.0f);

        INFO(ToString(onAxis) << " vs " << ToString(asPoint));
        CHECK(Approximately(onAxis, asPoint));

        // Outside the cone: exactly zero, while the same light at the same distance on axis is not.
        const Vec3 outside = EvaluateSpotLightRadiance(light, 5.0f, 0.0f);
        CHECK(outside.X == 0.0f);
        CHECK(outside.Y == 0.0f);

        // And the profile must never exceed 1: a spot brighter on its edge than its axis would mean the angular term
        // is being applied with the wrong normalisation.
        bool bounded = true;
        for (Float32 dot = -1.0f; dot <= 1.0f; dot += 0.01f)
        {
            const Float32 scale = SpotAngleAttenuation(dot, light.SpotInnerAngleRadians, light.SpotOuterAngleRadians);
            if (scale > 1.0f + 1e-6f)
            {
                bounded = false;
            }
        }
        CHECK(bounded);
    }

    TEST_CASE("directional light is position-independent")
    {
        LightComponent sun;
        sun.Type = LightComponent::LightType::Directional;
        sun.Color = Vec3(1.0f, 0.9f, 0.7f);
        sun.Intensity = 5.0f;

        // Same radiance everywhere — that is what "at infinity" means. A directional light carrying distance falloff
        // would make the far side of an outdoor scene black while a nearby wall was blown out.
        const Vec3 here = EvaluateDirectionalLightRadiance(sun);
        CHECK(Approximately(here, Vec3(5.0f, 4.5f, 3.5f)));

        // Range must not be consulted for directional lights even if someone sets it.
        sun.Range = 10.0f;
        CHECK(Approximately(EvaluateDirectionalLightRadiance(sun), here));
    }

    TEST_CASE("lumens convert to candela by the emission cone's solid angle")
    {
        // Point: I = P / 4π. For 1000 lm: 1000 / 12.56637 = 79.577...
        const Float32 pointIntensity = LuminousPowerToIntensity(1000.0f, LightComponent::LightType::Point, 0.0f);
        INFO("point " << pointIntensity);
        CHECK(pointIntensity == doctest::Approx(79.5775f).epsilon(1e-4));

        // Spot with a 45-degree full cone: Ω = 2π(1 - cos 22.5°) = 0.47830, so I = 1000 / 0.47830 = 2090.8...
        const Float32 spotIntensity = LuminousPowerToIntensity(1000.0f, LightComponent::LightType::Spot, 0.7853982f);

        INFO("spot " << spotIntensity);
        CHECK(spotIntensity == doctest::Approx(2090.8f).epsilon(1e-3));

        // Physical requirement: narrowing the cone must increase on-axis intensity (same power, smaller solid angle),
        // and a 180-degree full cone is a hemisphere with Ω = 2π giving I = P / 2π = twice the point value.
        const Float32 narrowerSpot = LuminousPowerToIntensity(1000.0f, LightComponent::LightType::Spot, 0.3490659f);
        CHECK(narrowerSpot > spotIntensity);

        const Float32 hemisphere = LuminousPowerToIntensity(1000.0f, LightComponent::LightType::Spot, kPi);
        CHECK(hemisphere == doctest::Approx(1000.0f / (2.0f * kPi)).epsilon(1e-4));

        // Zero-width cone must not produce infinity (it would write Inf into a uniform buffer and NaN the frame).
        const Float32 degenerate = LuminousPowerToIntensity(1000.0f, LightComponent::LightType::Spot, 0.0f);
        INFO("degenerate " << degenerate);
        CHECK(std::isfinite(degenerate));
        CHECK(degenerate > 0.0f);
    }

    TEST_CASE("no light evaluation produces NaN or infinity for hostile inputs")
    {
        const Float32 hostileDistances[] = {0.0f, -0.0f, 1e-8f, 1e8f, 1e30f};
        const Float32 hostileRanges[] = {0.0f, -1.0f, 1e-8f, 1e30f, 1e38f};

        for (const Float32 distance : hostileDistances)
        {
            for (const Float32 range : hostileRanges)
            {
                const Float32 attenuation = DistanceAttenuation(distance, range);

                INFO("d " << distance << " range " << range);
                CHECK(std::isfinite(attenuation));
                CHECK(attenuation >= 0.0f);
            }
        }

        // Non-finite cone angles from a corrupt scene file.
        const Float32 weirdAngles[] = {-1.0f, 0.0f, kPi * 4.0f};
        for (const Float32 angle : weirdAngles)
        {
            const Float32 value = SpotAngleAttenuation(0.9f, angle, angle);
            INFO("angle " << angle);
            CHECK(std::isfinite(value));
            CHECK(value >= 0.0f);
            CHECK(value <= 1.0f);
        }

        // A light with non-finite intensity must not propagate NaN through radiance.
        LightComponent light = MakePointLight();
        light.Intensity = std::numeric_limits<Float32>::infinity();
        const Vec3 radiance = EvaluatePointLightRadiance(light, 1.0f);
        // Inf * finite is Inf, which clamps away downstream; what must not happen is a NaN (which has no defined
        // clamp).
        CHECK(!std::isnan(radiance.X));
    }
}
