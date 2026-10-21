#include <algorithm>
#include <cmath>

#include "Hollowpoint/Math/MathFunctions.h"  // Math::Lerp(Vec3) — not declared by MathCore.h
#include "Hollowpoint/Renderer/ImageBasedLighting.h"
#include "Hollowpoint/Renderer/PbrShading.h"
#include "Hollowpoint/Renderer/ShadingConstants.h"

namespace Hollowpoint::Renderer
{

using Math::Vec2;

namespace
{

using ShadingConstants::Epsilon;
using ShadingConstants::MinGgxAlpha;
using ShadingConstants::Pi;

//! Builds an orthonormal tangent frame around `normal`.
//!
//! Required because the sampling patterns below are generated in a local hemisphere and rotated into world space. The
//! construction handles the two degenerate cases where the usual "cross with an axis" trick produces a zero vector:
//! when `normal` is parallel to the helper axis, a different axis is chosen. Silent NaNs here would poison every
//! convolution output at specific normals only — the kind of bug that survives review because it appears on perhaps one
//! cube-map texel in eight.
struct OrthonormalBasis
{
    Vec3 Tangent;
    Vec3 Bitangent;
    Vec3 Normal;
};

[[nodiscard]] OrthonormalBasis MakeBasis(const Vec3& normal)
{
    const Vec3 n = normal.Normalized();

    // Pick the world axis least aligned with n: its cross product with n has the largest magnitude and is therefore
    // numerically best conditioned.
    const Vec3 helper = std::fabs(n.X) < std::fabs(n.Y)
                            ? (std::fabs(n.X) < std::fabs(n.Z) ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 0.0f, 1.0f))
                            : (std::fabs(n.Y) < std::fabs(n.Z) ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(0.0f, 0.0f, 1.0f));

    Vec3 tangent = helper.Cross(n).Normalized();
    if (tangent.LengthSquared() <= 0.0f)
    {
        // n is zero-length or pathological; fall back to a fixed axis rather than propagating NaN.
        tangent = Vec3(1.0f, 0.0f, 0.0f);
    }

    const Vec3 bitangent = n.Cross(tangent).Normalized();

    return OrthonormalBasis{tangent, bitangent, n};
}

[[nodiscard]] Vec3 ToWorld(const OrthonormalBasis& basis, const Vec3& localDirection)
{
    return basis.Tangent * localDirection.X + basis.Bitangent * localDirection.Y + basis.Normal * localDirection.Z;
}

//! Inverse of ToWorld. The rows are the basis vectors, so a world direction projects onto each axis in turn.
[[nodiscard]] Vec3 ToLocal(const OrthonormalBasis& basis, const Vec3& worldDirection)
{
    return Vec3(basis.Tangent.Dot(worldDirection), basis.Bitangent.Dot(worldDirection),
                basis.Normal.Dot(worldDirection));
}

//! Two-dimensional low-discrepancy point in [0,1)^2 for a sample index: Roberts' R2 additive-recurrence sequence.
//!
//! Deterministic and stateless — the same index always yields the same point, so a convolution is reproducible across
//! runs and platforms without carrying generator state, which a PRNG would require.
//!
//! Both coordinates come from **different** irrational multipliers (successive powers of the reciprocal of the plastic
//! constant — Roberts' R2 sequence), and that is the entire point. An earlier version drew the second coordinate from
//! the *same* golden-ratio sequence at an index offset (`GoldenRatioSample(index + 0x9E3779B9u)`), which was wrong
//! twice over, and both failures were silent:
//!
//!   * with Float32 arithmetic, `static_cast<Float32>(index + 0x9E3779B9)` rounds every index in the range to the same
//!     float (ULP at 2.6e9 is ~256), so the second coordinate came out **identically zero for all 1024 samples** —
//!     measured, not suspected; and
//!   * even in exact arithmetic a shifted copy of one lattice is perfectly correlated with itself: it fills a line, not
//!     the square.
//!
//! A degenerate second coordinate collapses cosine-weighted hemisphere sampling onto a single meridian. Measured
//! against independent quadrature, that biased `IntegrateIrradiance` by up to **+100%** on directional environments
//! (the error vanished for axes lying in the surviving meridian's plane, which is how it survived reading), and skewed
//! the split-sum scale factor A by ~+28% at roughness 0.25, n·v = 0.1 — a material that reads slightly wrong under HDRI
//! lighting with no error anywhere.
//!
//! Arithmetic is double rather than float32 for the same reason in miniature: `index * alpha` must keep its fractional
//! part meaningful, and Float32 loses it once the product grows past ~2^24/alpha. The result is rounded back to Float32
//! because callers mix it into Float32 shading maths anyway.
[[nodiscard]] Vec2 SampleUnitSquare(UInt32 index, UInt32 seed)
{
    // Roberts 2018, "Alias-free 2D noise distributions": the pair below minimises bias for 2-dimensional additive
    // recurrence better than two independent golden-ratio multiples.
    constexpr double kAlphaX = 0.7548776662466927;
    constexpr double kAlphaY = 0.5698402909980532;

    // Cranley-Patterson rotation: offsetting the sequence index by the seed shifts the whole lattice without
    // disturbing its discrepancy, so two bakes with different seeds decorrelate while each stays reproducible.
    const double rotated = static_cast<double>(index) + static_cast<double>(seed) * kAlphaY;

    const double coordinateX = rotated * kAlphaX;
    const double coordinateY = rotated * kAlphaY;

    return Vec2(static_cast<Float32>(coordinateX - std::floor(coordinateX)),
                static_cast<Float32>(coordinateY - std::floor(coordinateY)));
}

[[nodiscard]] Vec3 CosineWeightedHemisphereDirection(Float32 randomA, Float32 randomB)
{
    // Malley's method: sample the projected disc uniformly and lift to the hemisphere. The resulting density is
    // cos(theta)/pi, which cancels the BRDF's cosine term exactly — hence zero variance for constant radiance.
    const Float32 radius = std::sqrt(randomA);
    const Float32 angle = 2.0f * Pi * randomB;

    const Vec3 local(std::cos(angle) * radius, std::sin(angle) * radius, std::sqrt(std::max(0.0f, 1.0f - randomA)));

    return local;
}

[[nodiscard]] Float32 PerceptualRoughnessToAlphaLocal(Float32 perceptualRoughness)
{
    // Same remap as PbrShading's — shared constant so the two cannot diverge into lobes of different widths.
    const Float32 clamped = std::clamp(perceptualRoughness, 0.0f, 1.0f);
    return std::max(clamped * clamped, MinGgxAlpha);
}

[[nodiscard]] Vec3 SampleGgxHalfVectorLocal(Float32 alpha, Float32 randomA, Float32 randomB)
{
    // Heitz 2018, sampling the visible normal distribution about +Z. Returned in the local frame; callers reflect a
    // local view about it and rotate only the resulting light direction into world space.
    const Float32 alphaSquared = alpha * alpha;

    const Float32 cosThetaSquared = (1.0f - randomA) / (1.0f + (alphaSquared - 1.0f) * randomA);
    const Float32 sinThetaSquared = std::max(0.0f, 1.0f - cosThetaSquared);

    const Float32 phi = 2.0f * Pi * randomB;

    return Vec3(std::cos(phi) * std::sqrt(sinThetaSquared), std::sin(phi) * std::sqrt(sinThetaSquared),
                std::sqrt(cosThetaSquared));
}

[[nodiscard]] Float32 Clamp01(Float32 value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

}  // namespace

EnvironmentSampler MakeConstantEnvironment(Vec3 radiance)
{
    return [radiance](const Vec3&) -> Vec3 { return radiance; };
}

EnvironmentSampler MakeDirectionalRampEnvironment(Vec3 axis, Vec3 scale)
{
    const Vec3 normalisedAxis = axis.Normalized();

    return [normalisedAxis, scale](const Vec3& direction) -> Vec3 {
        return scale * std::max(normalisedAxis.Dot(direction), 0.0f);
    };
}

EnvironmentSampler MakeGradientEnvironment(Vec3 below, Vec3 above, Vec3 axis)
{
    const Vec3 normalisedAxis = axis.Normalized();

    return [below, above, normalisedAxis](const Vec3& direction) -> Vec3 {
        const Float32 t = 0.5f * (normalisedAxis.Dot(direction) + 1.0f);
        return below + (above - below) * t;
    };
}

Vec3 IntegrateIrradiance(const EnvironmentSampler& environment, const Vec3& normal,
                         const IblConvolutionOptions& options)
{
    if (!environment)
    {
        return Vec3(0.0f, 0.0f, 0.0f);
    }

    const OrthonormalBasis basis = MakeBasis(normal);
    const UInt32 sampleCount = std::max<UInt32>(1u, options.SampleCount);

    Vec3 accumulated(0.0f, 0.0f, 0.0f);

    // Cosine-weighted importance sampling of `integral L(w) cos(theta) dw` over the hemisphere.
    //
    // Derivation (verified numerically before writing): with pdf = cos/pi, the estimator is
    //     E ~= (1/N) * sum( L(w_i) * cos_i / pdf_i ) = (1/N) * sum( L(w_i) * pi )
    // so every sample contributes `L * pi / N` — the cosines cancel entirely. That makes a constant environment
    // integrate to exactly its radiance for any N, turning normalisation into a check that cannot pass by luck
    // (quadrature-verified: L=(0.3,-0.5,0.2) integrates to pi*L = (0.942478, -1.570796, 0.628319)).
    // An earlier draft used 2*pi/N here — a factor-of-2 error caught by the exact-value test rather than review.
    //
    // Orientation is invisible to a constant environment. The ramp environments in the tests are checked against
    // quadrature computed inside the test file, independently of this estimator. Note there is **no** simple closed
    // form for a directional ramp in general: an earlier comment here claimed `pi * (n . axis)`, and brute-force
    // quadrature disproved it (aligned gives 2*pi/3 = 2.0944, not pi; at 54.7 degrees the ratio is 0.686; when the
    // ramp's zero half-sphere clips the horizon the formula even changes sign). Only the a == n case has a clean
    // analytic value — 2*pi/3 * scale, from integral cos^2(theta) sin(theta) dtheta dphi = 2*pi/3 — and the tests use
    // it as an exact anchor alongside quadrature for everything else.
    const Float32 weight = Pi / static_cast<Float32>(sampleCount);

    for (UInt32 index = 0; index < sampleCount; ++index)
    {
        const Vec2 randoms = SampleUnitSquare(index, options.Seed);

        const Vec3 localDirection = CosineWeightedHemisphereDirection(randoms.X, randoms.Y);
        const Vec3 worldDirection = ToWorld(basis, localDirection);

        accumulated = accumulated + environment(worldDirection) * weight;
    }

    return accumulated;
}

Vec3 PrefilterEnvironmentGGX(const EnvironmentSampler& environment, const Vec3& normal, const Vec3& viewDirection,
                             Float32 roughness, const IblConvolutionOptions& options)
{
    if (!environment)
    {
        return Vec3(0.0f, 0.0f, 0.0f);
    }

    const OrthonormalBasis basis = MakeBasis(normal);
    const UInt32 sampleCount = std::max<UInt32>(1u, options.SampleCount);
    const Float32 alpha = PerceptualRoughnessToAlphaLocal(roughness);

    // Everything below happens in the tangent-space frame built around `normal`: half vectors and reflections are
    // local, and only the final gather direction is rotated to world space. Mixing the two — reflecting a world-space
    // view with a locally-sampled half vector — produces a lobe that points somewhere plausible but wrong, which is
    // exactly the kind of error a symmetric test environment hides.
    const Vec3 localView = ToLocal(basis, viewDirection);

    // A view exactly along the normal has no well-defined tangent azimuth; the integral is rotation-invariant there, so
    // any tilt gives the same answer and nudging off-axis only avoids divide-by-zero in downstream trig.
    const Vec3 effectiveView = localView.Z > 0.99999f ? Vec3(1e-3f, 0.0f, 1.0f).Normalized() : localView;

    Vec3 accumulated(0.0f, 0.0f, 0.0f);
    Float32 weightSum = 0.0f;

    // Importance sampling over the half-vector gives `(sum L(l_i) * cos_i) / (sum cos_i)` — a weighted average of
    // sampled radiance. Dividing by the accumulated weights rather than by a closed-form integral is what makes this
    // return **radiance** instead of energy, and it is why a constant environment returns exactly that constant at
    // every roughness: numerator and denominator carry identical weighting and cancel. Without the division, rougher
    // surfaces would come out dimmer — readable as "the sky leaks energy", invisible in any single frame but obvious as
    // soon as two materials with different roughness sit side by side under the same lighting.
    for (UInt32 index = 0; index < sampleCount; ++index)
    {
        const Vec2 randoms = SampleUnitSquare(index, options.Seed);

        const Vec3 halfVector = SampleGgxHalfVectorLocal(alpha, randoms.X, randoms.Y);

        const Float32 dotViewHalf = effectiveView.Dot(halfVector);
        if (dotViewHalf <= 0.0f)
        {
            continue;
        }

        // Reflection about the half vector: l = 2(h·v)h - v, entirely in the local frame.
        const Vec3 reflected = halfVector * (2.0f * dotViewHalf) - effectiveView;

        // Only upper-hemisphere light contributes; a reflection below the horizon samples nothing.
        if (reflected.Z <= 0.0f)
        {
            continue;
        }

        const Float32 cosine = reflected.Z;
        const Vec3 worldDirection = ToWorld(basis, reflected.Normalized());

        accumulated = accumulated + environment(worldDirection) * cosine;
        weightSum += cosine;
    }

    if (weightSum <= Epsilon)
    {
        return Vec3(0.0f, 0.0f, 0.0f);
    }

    return accumulated / weightSum;
}

BrdfSplitSumFactors EvaluateBrdfSplitSum(Float32 cosineViewAngle, Float32 roughness,
                                         const IblConvolutionOptions& options)
{
    const Float32 nDotV = Clamp01(cosineViewAngle);
    const UInt32 sampleCount = std::max<UInt32>(1u, options.SampleCount);
    const Float32 alpha = PerceptualRoughnessToAlphaLocal(roughness);

    // Derivation of the split-sum factors (worked through and checked numerically rather than recalled):
    //
    //   ambient_spec = INT Phi_r(h) F(v,h) (l.n)/(v.h)^2 dh        over the hemisphere
    //   Phi_r        = D(h) G1(v.h) / (4 (n.l))                    (visibility-terminated form)
    //   substituting GGX and the half-vector Jacobian: Phi_r dh = G1(v.h)^2 D_v(h) (v.n)/(v.h)^2 dw_h
    //   sampling h from the visible-normal distribution D_v, whose pdf is D_v/(v.n), leaves the estimator weight as
    //   exactly **G1(v.h)^2** — every other factor cancels.
    //
    // Schlick decomposes as F = F0*(1-u) + u with u = (1 - v.h)^5, so
    //   A = E[ G1^2 (1-u) ],   B = E[ G1^2 u ].
    // Both are expectations under identical weights of complementary quantities, so **A + B == 1 identically**. The
    // tests assert that identity: it holds only when this weighting matches what PrefilterEnvironmentGGX assumes, so a
    // divergence between the two functions fails a test instead of quietly doubling or halving ambient specular energy
    // at a roughness nobody happened to inspect.
    //
    // G1 uses the IBL remap k = alpha/2 — *not* direct lighting's (roughness+1)^2/8. See PbrShading.h on why the two
    // forms differ and why using one where the other belongs is a silent error.
    Float32 scaleNumerator = 0.0f;
    Float32 biasNumerator = 0.0f;
    Float32 weightSum = 0.0f;

    // The integral is rotation-invariant about the normal, so a view tilted into the XZ plane by nDotV covers every
    // distinct case.
    const Vec3 localView(std::sqrt(std::max(0.0f, 1.0f - nDotV * nDotV)), 0.0f, nDotV);

    const Float32 k = alpha * 0.5f;

    for (UInt32 index = 0; index < sampleCount; ++index)
    {
        const Vec2 randoms = SampleUnitSquare(index, options.Seed);

        const Vec3 halfVector = SampleGgxHalfVectorLocal(alpha, randoms.X, randoms.Y);

        const Float32 dotViewHalf = localView.Dot(halfVector);
        if (dotViewHalf <= 0.0f || halfVector.Z <= 0.0f)
        {
            continue;
        }

        // Samples whose reflection leaves the hemisphere gather no light and are dropped rather than mirrored: folding
        // them back in would double-count the lower hemisphere.
        const Vec3 reflected = halfVector * (2.0f * dotViewHalf) - localView;
        if (reflected.Z <= 0.0f)
        {
            continue;
        }

        const Float32 geometry = dotViewHalf / (dotViewHalf * (1.0f - k) + k);
        const Float32 weight = geometry * geometry;

        const Float32 oneMinusDotViewHalf = Clamp01(1.0f - dotViewHalf);
        const Float32 u =
            oneMinusDotViewHalf * oneMinusDotViewHalf * oneMinusDotViewHalf * oneMinusDotViewHalf * oneMinusDotViewHalf;

        scaleNumerator += weight * (1.0f - u);
        biasNumerator += weight * u;
        weightSum += weight;
    }

    if (weightSum <= Epsilon)
    {
        return BrdfSplitSumFactors{};
    }

    BrdfSplitSumFactors factors;
    factors.Scale = scaleNumerator / weightSum;
    factors.Bias = biasNumerator / weightSum;

    return factors;
}

Vec3 AmbientLightingResult::Total() const noexcept
{
    return Diffuse + Specular;
}

AmbientLightingResult EvaluateAmbientLighting(const EnvironmentSampler& environment,
                                              const PbrMaterialParameters& material, const Vec3& normalShading,
                                              const Vec3& viewDirection, const IblConvolutionOptions& options)
{
    AmbientLightingResult result;

    if (!environment)
    {
        return result;
    }

    const Float32 metallic = Clamp01(material.Metallic);

    // Diffuse ambient: irradiance times the Lambertian BRDF. The 1/pi lives here rather than in IntegrateIrradiance so
    // that function keeps the clean "integral of L*cos" contract its tests assert against closed forms.
    const Vec3 irradiance = IntegrateIrradiance(environment, normalShading, options);
    const Vec3 diffuseAlbedo = material.BaseColor * (1.0f - metallic);

    result.Diffuse = irradiance * diffuseAlbedo / Pi;

    // Specular ambient: prefiltered radiance times the split-sum Fresnel factors.
    const Vec3 prefiltered =
        PrefilterEnvironmentGGX(environment, normalShading, viewDirection, material.Roughness, options);

    const Float32 nDotV = Clamp01(normalShading.Dot(viewDirection));
    const BrdfSplitSumFactors splitSum = EvaluateBrdfSplitSum(nDotV, material.Roughness, options);

    const Vec3 f0 = Math::Lerp(Vec3(material.DielectricF0, material.DielectricF0, material.DielectricF0),
                               material.BaseColor, metallic);

    result.Specular = prefiltered * (f0 * splitSum.Scale + Vec3(splitSum.Bias));

    return result;
}

}  // namespace Hollowpoint::Renderer
