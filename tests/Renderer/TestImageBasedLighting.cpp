// Tests for the image-based lighting CPU reference (engine/Renderer/Src/ImageBasedLighting.cpp).
//
// Method, per the lesson recorded in notes/EXPERIMENTS.md: every expected value below is either a hand-derived closed
// form or a quadrature computed independently of the estimator under test — never the estimator's own output. Property
// checks (positivity, monotonicity, A+B==1) are included but no single one of them pins shape; each test case states
// which defect class it exists to catch.
//
// Background: this suite was written after measurement overturned two claims made in the implementation's comments:
//   1. The 2D sampler's second coordinate was identically zero (Float32 cannot distinguish index + a 2.6e9 offset),
//      collapsing hemisphere sampling onto one meridian — up to +100% error on directional environments, invisible for
//      axes lying in that meridian's plane. The oblique-ramp cases below are the regression net for that class.
//   2. A "closed form" of pi*(n·axis) for ramp irradiance is wrong (aligned gives 2*pi/3; general orientations have no
//      simple closed form once the ramp's zero half-sphere clips the horizon). The helper quadrature below re-derives
//      ground truth from scratch and self-checks against the one analytic case.

#include <cmath>
#include <string>

#include <doctest/doctest.h>

#include "Hollowpoint/Renderer/ImageBasedLighting.h"
#include "Hollowpoint/Renderer/PbrShading.h"

using namespace Hollowpoint;
using namespace Hollowpoint::Renderer;
using Math::Float32;
using Math::Vec3;

namespace
{

constexpr double kPiD = 3.1415926535897932384626433832795;

[[nodiscard]] bool Approximately(const Vec3& a, const Vec3& b, Float32 tolerance)
{
    return std::fabs(a.X - b.X) <= tolerance && std::fabs(a.Y - b.Y) <= tolerance && std::fabs(a.Z - b.Z) <= tolerance;
}

//! Absolute-tolerance comparison. doctest's Approx offers only relative epsilon, and several checks below need a
//! tolerance in absolute units (near-zero expected values make a relative check vacuous or impossible).
[[nodiscard]] bool NearAbsolute(Float32 value, double expected, Float32 tolerance)
{
    return std::fabs(static_cast<double>(value) - expected) <= static_cast<double>(tolerance);
}

[[nodiscard]] std::string ToString(const Vec3& v)
{
    return "(" + std::to_string(v.X) + ", " + std::to_string(v.Y) + ", " + std::to_string(v.Z) + ")";
}

//! Independent dense quadrature of `integral L(w) max(n.w,0) dw` over the hemisphere about `normal`.
//!
//! Deliberately shares no code with the implementation: its own frame construction, double-precision grid, midpoint
//! rule. Self-checks against the analytic aligned-ramp value (2*pi/3) inside the test that uses it most, so a bug in
//! this helper cannot silently bless a bug in the estimator.
[[nodiscard]] double QuadratureIrradiance(const EnvironmentSampler& environment, const Vec3& normal, int stepsTheta,
                                          int stepsPhi)
{
    const Vec3 n = normal.Normalized();

    const Vec3 helper = std::fabs(n.X) < 0.9f ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 0.0f, 1.0f);
    const Vec3 tangent = helper.Cross(n).Normalized();
    const Vec3 bitangent = n.Cross(tangent).Normalized();

    double total = 0.0;
    const double dTheta = kPiD / 2.0 / stepsTheta;
    const double dPhi = 2.0 * kPiD / stepsPhi;

    for (int i = 0; i < stepsTheta; ++i)
    {
        const double theta = (i + 0.5) * dTheta;
        const double sinTheta = std::sin(theta);
        const double cosTheta = std::cos(theta);

        for (int j = 0; j < stepsPhi; ++j)
        {
            const double phi = (j + 0.5) * dPhi;
            const Vec3 direction(
                tangent.X * (std::cos(phi) * sinTheta) + bitangent.X * (std::sin(phi) * sinTheta) + n.X * cosTheta,
                tangent.Y * (std::cos(phi) * sinTheta) + bitangent.Y * (std::sin(phi) * sinTheta) + n.Y * cosTheta,
                tangent.Z * (std::cos(phi) * sinTheta) + bitangent.Z * (std::sin(phi) * sinTheta) + n.Z * cosTheta);

            const double cosine = std::max(0.0, static_cast<double>(n.Dot(direction)));
            total += static_cast<double>(environment(direction).X) * cosine * sinTheta * dTheta * dPhi;
        }
    }

    return total;
}

[[nodiscard]] IblConvolutionOptions WithSamples(Math::UInt32 sampleCount)
{
    IblConvolutionOptions options;
    options.SampleCount = sampleCount;
    return options;
}

}  // namespace

TEST_SUITE("Renderer/ImageBasedLighting")
{
    TEST_CASE("cosine-weighted irradiance of a constant environment is exact for any sample count")
    {
        // Zero-variance identity: with pdf = cos/pi the estimator reduces to mean(L)*pi, so a constant L integrates to
        // pi*L with no dependence on the sampling pattern. This pins the normalisation exactly — the historical
        // factor-of-2 draft error (2*pi/N instead of pi/N) fails here at every N, and so does any future change that
        // moves the 1/pi between IntegrateIrradiance and its callers.
        const Vec3 sky(0.4f, -0.5f, 0.2f);  // negative channel on purpose: sign handling must survive too
        const EnvironmentSampler environment = MakeConstantEnvironment(sky);

        for (Math::UInt32 sampleCount : {1u, 7u, 64u, 1024u})
        {
            const Vec3 result = IntegrateIrradiance(environment, Vec3(0.4f, 0.8f, -0.4f), WithSamples(sampleCount));
            INFO("N=" << sampleCount << " got " << ToString(result));
            CHECK(NearAbsolute(result.X, kPiD * sky.X, 2e-5f));
            CHECK(NearAbsolute(result.Y, kPiD * sky.Y, 2e-5f));
            CHECK(NearAbsolute(result.Z, kPiD * sky.Z, 2e-5f));
        }
    }

    TEST_CASE("irradiance of a ramp aligned with the normal equals the hand-derived 2*pi/3")
    {
        // int_hemi cos^2(theta) dw = 2*pi/3 — exact anchor for MakeDirectionalRampEnvironment(axis == normal, scale 1).
        // The helper quadrature is checked against the same value first: if the helper is wrong, nothing downstream in
        // this file can be trusted.
        const EnvironmentSampler ramp = MakeDirectionalRampEnvironment(Vec3(0.0f, 0.0f, 1.0f), Vec3(1.0f, 1.0f, 1.0f));

        const double helperValue = QuadratureIrradiance(ramp, Vec3(0.0f, 0.0f, 1.0f), 600, 1200);
        const double analytic = 2.0 * kPiD / 3.0;
        INFO("quadrature " << helperValue << " vs analytic 2pi/3 " << analytic);
        CHECK(helperValue == doctest::Approx(analytic).epsilon(1e-4));

        const Vec3 result = IntegrateIrradiance(ramp, Vec3(0.0f, 0.0f, 1.0f), WithSamples(8192));
        INFO("estimator " << ToString(result));
        CHECK(result.X == doctest::Approx(analytic).epsilon(1e-3));
    }

    TEST_CASE("irradiance of oblique ramps matches independent quadrature")
    {
        // The regression net for the collapsed-second-dimension defect: when both random coordinates came from one
        // 1D sequence, samples lived on a single meridian and this comparison blew up (+100% for axis=(x+z)/sqrt2)
        // while aligned cases stayed green. Tolerances are loose relative to measured error (<= ~1.6e-3 at N=1024 for
        // smooth orientations, ~1.5e-2 where the ramp's horizon clip concentrates quadrature error) but orders of
        // magnitude tighter than any defect this catches in practice.
        struct Case
        {
            const char* Name;
            Vec3 Normal;
            Vec3 Axis;
            double Tolerance;  // relative
        };

        const double invRoot2 = 1.0 / std::sqrt(2.0);
        const double invRoot3 = 1.0 / std::sqrt(3.0);
        const Case cases[] = {
            {"54.7 degrees", Vec3(0.577350269f, 0.577350269f, 0.577350269f),
             Vec3(static_cast<Float32>(invRoot2), 0.0f, static_cast<Float32>(invRoot2)), 0.02},
            {"horizon clip", Vec3(0.0f, -0.707106781f, 0.707106781f), Vec3(0.0f, 1.0f, 0.0f), 0.10},
            {"steep oblique", Vec3(0.948683298f, -0.316227766f, 0.0f),
             Vec3(static_cast<Float32>(invRoot3), static_cast<Float32>(invRoot3), -0.877930621f), 0.05},
        };

        for (const Case& testCase : cases)
        {
            const EnvironmentSampler ramp = MakeDirectionalRampEnvironment(testCase.Axis, Vec3(1.0f, 1.0f, 1.0f));

            const double truth = QuadratureIrradiance(ramp, testCase.Normal, 900, 1800);
            const Vec3 estimate = IntegrateIrradiance(ramp, testCase.Normal, WithSamples(1024));

            INFO(testCase.Name << ": quadrature " << truth << ", estimator " << ToString(estimate));
            REQUIRE(std::fabs(truth) > 1e-4);  // a near-zero truth would make the relative check meaningless
            CHECK(estimate.X == doctest::Approx(truth).epsilon(testCase.Tolerance));
        }
    }

    TEST_CASE("more samples reduce the error against quadrature (convergence, not luck)")
    {
        // Low-discrepancy sequences are not monotone per-sample, but their envelope must contract: if higher N does not
        // get closer to an independent truth, the estimator is biased and no tolerance choice makes tests honest.
        const EnvironmentSampler ramp =
            MakeDirectionalRampEnvironment(Vec3(0.707106781f, 0.0f, 0.707106781f), Vec3(1.0f, 1.0f, 1.0f));
        const Vec3 normal(0.577350269f, 0.577350269f, 0.577350269f);
        const double truth = QuadratureIrradiance(ramp, normal, 1200, 2400);

        const double errorLow = std::fabs(IntegrateIrradiance(ramp, normal, WithSamples(256)).X - truth);
        const double errorHigh = std::fabs(IntegrateIrradiance(ramp, normal, WithSamples(8192)).X - truth);

        INFO("error at N=256: " << errorLow << ", at N=8192: " << errorHigh << ", truth " << truth);
        CHECK(errorHigh < errorLow);
    }

    TEST_CASE("the seed decorrelates sample lattices without breaking convergence")
    {
        // Seed exists so two convolutions (e.g. neighbouring mip levels) can avoid correlated errors. A degenerate
        // sequence would return identical values for every seed; a PRNG-quality stream returns different ones that all
        // converge to the same truth. Measured: N=256 outputs differ per seed and each lands within ~0.6% of truth.
        const EnvironmentSampler ramp =
            MakeDirectionalRampEnvironment(Vec3(0.707106781f, 0.0f, 0.707106781f), Vec3(1.0f, 1.0f, 1.0f));
        const Vec3 normal(0.577350269f, 0.577350269f, 0.577350269f);
        const double truth = QuadratureIrradiance(ramp, normal, 1200, 2400);

        Float32 previous = 0.0f;
        for (Math::UInt32 seed : {0x5EED1234u, 1u, 2u, 3u})
        {
            IblConvolutionOptions options;
            options.SampleCount = 256;
            options.Seed = seed;

            const Vec3 result = IntegrateIrradiance(ramp, normal, options);
            INFO("seed " << seed << " -> " << ToString(result) << ", truth " << truth);
            CHECK(result.X == doctest::Approx(truth).epsilon(0.05));
            if (seed != 0x5EED1234u)
            {
                CHECK(result.X != previous);  // identical output for every seed = seed ignored or lattice collapsed
            }
            previous = result.X;
        }
    }

    TEST_CASE("convolution is deterministic: repeated calls are bitwise identical")
    {
        // Image tests bake these convolutions once and compare pixels later; any run-to-run drift makes them flaky.
        const EnvironmentSampler environment =
            MakeGradientEnvironment(Vec3(0.1f, 0.1f, 0.1f), Vec3(1.0f, 1.0f, 1.0f), Vec3(0.0f, 1.0f, 0.0f));
        const IblConvolutionOptions options = WithSamples(2048);

        const Vec3 first = IntegrateIrradiance(environment, Vec3(0.3f, 0.6f, -0.7f), options);
        const Vec3 second = IntegrateIrradiance(environment, Vec3(0.3f, 0.6f, -0.7f), options);

        CHECK(first.X == second.X);
        CHECK(first.Y == second.Y);
        CHECK(first.Z == second.Z);
    }

    TEST_CASE("prefilter of a constant environment returns that constant at every roughness and view")
    {
        // Radiance-normalised convolution (divide by the accumulated weights): numerator and denominator carry
        // identical weighting, so a constant passes through unchanged. Without the normalisation, rough surfaces come
        // out dimmer — "the sky leaks energy" — which reads as materials disagreeing under identical lighting.
        const Vec3 sky(0.7f, 0.2f, 0.9f);
        const EnvironmentSampler environment = MakeConstantEnvironment(sky);

        for (Float32 roughness : {0.0f, 0.1f, 0.5f, 1.0f})
        {
            for (const Vec3& view : {Vec3(0.0f, 0.0f, 1.0f), Vec3(0.707106781f, 0.0f, 0.707106781f)})
            {
                const Vec3 result =
                    PrefilterEnvironmentGGX(environment, Vec3(0.0f, 0.0f, 1.0f), view, roughness, WithSamples(1024));
                INFO("roughness " << roughness << " view " << ToString(view) << " -> " << ToString(result));
                CHECK(Approximately(result, sky, 1e-4f));
            }
        }
    }

    TEST_CASE("prefilter concentrates toward the reflection direction as roughness falls")
    {
        // Orientation sensitivity: with the ramp axis perpendicular to the normal, a mirror (roughness 0) samples the
        // dark half-sphere and returns ~L(+Z) = 0; roughening spreads the lobe into the lit half. Measured at N=4096:
        // 0.001, 0.051, 0.133, 0.212 for roughness 0, 0.25, 0.5, 1.0. Catches a lobe that ignores roughness or points
        // at the wrong hemisphere — both of which leave constant-environment tests green.
        const EnvironmentSampler ramp = MakeDirectionalRampEnvironment(Vec3(0.0f, 1.0f, 0.0f), Vec3(1.0f, 1.0f, 1.0f));
        const IblConvolutionOptions options = WithSamples(4096);

        Float32 previous = -1.0f;
        for (Float32 roughness : {0.0f, 0.25f, 0.5f, 1.0f})
        {
            const Vec3 result =
                PrefilterEnvironmentGGX(ramp, Vec3(0.0f, 0.0f, 1.0f), Vec3(0.0f, 0.0f, 1.0f), roughness, options);
            INFO("roughness " << roughness << " -> " << ToString(result));
            CHECK(result.X > previous);  // strictly increasing: sharper lobe sees less of the lit half
            previous = result.X;
        }

        CHECK(PrefilterEnvironmentGGX(ramp, Vec3(0.0f, 0.0f, 1.0f), Vec3(0.0f, 0.0f, 1.0f), 0.0f, options).X < 0.01f);

        // And the mirror limit toward a lit direction returns that direction's radiance: axis = +Z = reflection of the
        // view about the normal, L = 1 there.
        const EnvironmentSampler rampFront =
            MakeDirectionalRampEnvironment(Vec3(0.0f, 0.0f, 1.0f), Vec3(1.0f, 1.0f, 1.0f));
        const Vec3 mirror =
            PrefilterEnvironmentGGX(rampFront, Vec3(0.0f, 0.0f, 1.0f), Vec3(0.0f, 0.0f, 1.0f), 0.0f, options);
        INFO("mirror limit " << ToString(mirror));
        CHECK(Approximately(mirror, Vec3(1.0f, 1.0f, 1.0f), 2e-4f));
    }

    TEST_CASE("split-sum factors satisfy A + B == 1")
    {
        // The guard the header promises: Scale and Bias are expectations of complementary Fresnel terms under identical
        // weights, so their sum is 1 by construction. If EvaluateBrdfSplitSum's weighting ever drifts from what
        // PrefilterEnvironmentGGX assumes, ambient specular silently doubles or halves — visible only as materials
        // that "look slightly wrong" under HDRI lighting.
        for (Float32 cosine : {0.0f, 0.1f, 0.5f, 0.75f, 1.0f})
        {
            for (Float32 roughness : {0.05f, 0.25f, 0.5f, 1.0f})
            {
                const BrdfSplitSumFactors factors = EvaluateBrdfSplitSum(cosine, roughness, WithSamples(4096));
                INFO("cos " << cosine << " rough " << roughness << " A " << factors.Scale << " B " << factors.Bias);
                CHECK(NearAbsolute(factors.Scale + factors.Bias, 1.0, 1e-4));
            }
        }
    }

    TEST_CASE("split-sum matches the analytic mirror limit as roughness goes to zero")
    {
        // At roughness 0 the GGX lobe collapses onto the reflection direction, so
        //     A -> 1 - (1 - n.v)^5   and   B -> (1 - n.v)^5
        // — an exact value independent of any sampling choice. Measured agreement at N=4096: <= 2e-5 for n.v >= 0.1.
        // The collapsed-sampler defect showed here as +4% at n.v = 0.1 (A = 0.4268 vs analytic 0.40951), so this test
        // is load-bearing, not decoration. n.v = 0 is excluded: every light reflection leaves the hemisphere and the
        // truncated estimator has no closed-form limit there.
        for (Float32 cosine : {1.0f, 0.9f, 0.75f, 0.5f, 0.25f, 0.1f})
        {
            const BrdfSplitSumFactors factors = EvaluateBrdfSplitSum(cosine, 0.0f, WithSamples(4096));
            const Float32 u = std::pow(1.0f - cosine, 5);

            INFO("cos " << cosine << " A " << factors.Scale << " analytic " << (1.0f - u));
            CHECK(NearAbsolute(factors.Scale, 1.0f - u, 1e-4));
            CHECK(NearAbsolute(factors.Bias, u, 1e-4));
        }
    }

    TEST_CASE("split-sum clamps its cosine argument instead of extrapolating")
    {
        // Scripts and animation curves produce out-of-range cosines routinely; the function's contract is to clamp,
        // and bit-exact equality against the boundary values proves no polynomial extrapolation happens.
        const IblConvolutionOptions options = WithSamples(1024);

        CHECK(EvaluateBrdfSplitSum(2.0f, 0.5f, options).Scale == EvaluateBrdfSplitSum(1.0f, 0.5f, options).Scale);
        CHECK(EvaluateBrdfSplitSum(-3.0f, 0.5f, options).Scale == EvaluateBrdfSplitSum(0.0f, 0.5f, options).Scale);
    }

    TEST_CASE("gradient environment keeps every convolution strictly positive")
    {
        // The header sells MakeGradientEnvironment as the "no zeros" environment — a ramp reaching zero over half the
        // sphere lets a convolution that drops a hemisphere look correct. If either convolution ever zeroes out a
        // region this environment lights, that claim (and whatever relied on it) breaks.
        const EnvironmentSampler environment =
            MakeGradientEnvironment(Vec3(0.1f, 0.1f, 0.1f), Vec3(1.0f, 1.0f, 1.0f), Vec3(0.0f, 1.0f, 0.0f));

        for (const Vec3& normal : {Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f)})
        {
            const Vec3 irradiance = IntegrateIrradiance(environment, normal, WithSamples(1024));
            INFO("normal " << ToString(normal) << " -> " << ToString(irradiance));
            CHECK(irradiance.X > 0.0f);
            CHECK(irradiance.Y > 0.0f);
            CHECK(irradiance.Z > 0.0f);

            const Vec3 prefiltered = PrefilterEnvironmentGGX(environment, normal, normal, 0.5f, WithSamples(1024));
            CHECK(prefiltered.X > 0.0f);
        }
    }

    TEST_CASE("degenerate inputs return zeros or finite values rather than NaN")
    {
        // A NaN from a convolution propagates through the whole frame and is nearly impossible to trace afterwards, so
        // every guard here converts a would-be NaN into a defined value. SampleCount=0 must behave like 1 (measured:
        // bitwise equal), and a zero-length normal must still integrate a constant environment to pi*L.
        const EnvironmentSampler empty;
        CHECK(IntegrateIrradiance(empty, Vec3(0.0f, 0.0f, 1.0f)).X == 0.0f);
        CHECK(PrefilterEnvironmentGGX(empty, Vec3(0.0f, 0.0f, 1.0f), Vec3(0.0f, 0.0f, 1.0f), 0.5f).X == 0.0f);

        const EnvironmentSampler constant = MakeConstantEnvironment(Vec3(1.0f, 1.0f, 1.0f));
        const Vec3 zeroNormalResult = IntegrateIrradiance(constant, Vec3(0.0f, 0.0f, 0.0f), WithSamples(64));
        CHECK(std::isfinite(zeroNormalResult.X));
        CHECK(std::isfinite(zeroNormalResult.Y));
        CHECK(zeroNormalResult.X == doctest::Approx(kPiD).epsilon(1e-4));

        const Vec3 zeroSampleCount = IntegrateIrradiance(constant, Vec3(0.0f, 0.0f, 1.0f), WithSamples(0));
        const Vec3 oneSampleCount = IntegrateIrradiance(constant, Vec3(0.0f, 0.0f, 1.0f), WithSamples(1));
        CHECK(zeroSampleCount.X == oneSampleCount.X);

        // Prefilter with the view below the horizon: no light can reflect, and the contract is zero rather than NaN.
        const Vec3 belowHorizon =
            PrefilterEnvironmentGGX(constant, Vec3(0.0f, 0.0f, 1.0f), Vec3(0.0f, 0.0f, -1.0f), 0.5f, WithSamples(256));
        CHECK(belowHorizon.X == 0.0f);
    }

    TEST_CASE("ambient lighting on a constant environment reduces to albedo times radiance")
    {
        // The diffuse half of the split sum: irradiance (pi*L) times albedo/pi must give exactly L*albedo — the 1/pi
        // placed in EvaluateAmbientLighting cancels IntegrateIrradiance's pi. Getting that placement wrong by a factor
        // of pi is the single most common IBL bug and it hides under arbitrary exposure adjustments, so the exact
        // value is asserted here rather than tuned later against a reference render.
        const Vec3 sky(0.4f, 0.5f, 0.6f);
        const EnvironmentSampler environment = MakeConstantEnvironment(sky);

        PbrMaterialParameters material;
        material.BaseColor = Vec3(0.8f, 0.3f, 0.2f);
        material.Metallic = 0.0f;
        material.Roughness = 0.4f;

        const AmbientLightingResult result = EvaluateAmbientLighting(environment, material, Vec3(0.0f, 0.0f, 1.0f),
                                                                     Vec3(0.0f, 0.0f, 1.0f), WithSamples(4096));

        INFO("diffuse " << ToString(result.Diffuse));
        CHECK(Approximately(
            result.Diffuse,
            Vec3(sky.X * material.BaseColor.X, sky.Y * material.BaseColor.Y, sky.Z * material.BaseColor.Z), 2e-5f));

        // A pure metal has exactly zero diffuse — the metallic term must remove it entirely, not attenuate it.
        PbrMaterialParameters metal = material;
        metal.Metallic = 1.0f;
        const AmbientLightingResult metalResult = EvaluateAmbientLighting(environment, metal, Vec3(0.0f, 0.0f, 1.0f),
                                                                          Vec3(0.0f, 0.0f, 1.0f), WithSamples(1024));
        CHECK(metalResult.Diffuse.X == 0.0f);
        CHECK(metalResult.Diffuse.Y == 0.0f);
        CHECK(metalResult.Diffuse.Z == 0.0f);

        // Specular closes the loop with the split-sum factors the GPU path will use: prefiltered (== sky for a
        // constant environment) times (F0*A + B), F0 = BaseColor at metallic 1.
        const BrdfSplitSumFactors factors = EvaluateBrdfSplitSum(1.0f, material.Roughness, WithSamples(4096));
        const Float32 expectedX = sky.X * (material.BaseColor.X * factors.Scale + factors.Bias);
        INFO("specular " << ToString(metalResult.Specular) << " expected X " << expectedX);
        CHECK(NearAbsolute(metalResult.Specular.X, expectedX, 1e-4));

        // Empty environment: everything zero, and Total() agrees with the components.
        const EnvironmentSampler empty;
        const AmbientLightingResult zero =
            EvaluateAmbientLighting(empty, material, Vec3(0.0f, 0.0f, 1.0f), Vec3(0.0f, 0.0f, 1.0f));
        CHECK(zero.Diffuse.X == 0.0f);
        CHECK(zero.Specular.X == 0.0f);
        const AmbientLightingResult sum = result;
        CHECK(NearAbsolute(sum.Total().X, result.Diffuse.X + result.Specular.X, 1e-9));
    }
}
