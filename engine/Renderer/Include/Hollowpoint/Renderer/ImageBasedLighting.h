#pragma once

#include <functional>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Math/MathCore.h"        // Vec3, Float32, UInt32 aliases
#include "Hollowpoint/Renderer/PbrShading.h"  // PbrMaterialParameters

// ---------------------------------------------------------------------------
// Image-based lighting: the CPU reference for environment convolution.
//
// Ambient light from an environment map is a hemisphere integral per shaded point, which is far too expensive to
// evaluate directly. The standard solution (Karis 2014) splits it into two precomputed convolutions plus a cheap
// runtime combination:
//
//     diffuse   ≈ Irradiance(n)                        — cosine-weighted hemisphere integral, once per normal
//     specular  ≈ Prefilter(env, n, v, roughness) × (F0 · A + B)
//
// where A and B depend only on (n·v, roughness) — a 2D lookup at runtime.
//
// This file implements the convolutions and derives A/B from their exact integral definitions rather than copying the
// variants commonly found in tutorials, which differ from one another in their geometry terms and are frequently
// inconsistent between the prefilter and the LUT (see the derivation in ImageBasedLighting.cpp). That consistency is
// not cosmetic: if the two convolutions use different weights, energy leaks or doubles at roughness values nobody can
// see in isolation, and it shows up as materials that look "slightly wrong" under HDRI lighting with no error anywhere.
//
// RHI-free like the other Renderer references: an environment is a callable from direction to radiance, so tests use
// analytic environments with closed-form answers and the GPU path can later wrap a real texture in the same shape.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Renderer
{

using Math::Float32;
using Math::UInt32;
using Math::Vec3;

//! Radiance arriving at the scene from a direction, which is assumed unit length by implementations.
//!
//! A callable rather than an image type so the convolution code has no notion of texture formats, mip levels or
//! coordinate conventions: those belong to whoever builds the sampler, not to the maths being validated.
using EnvironmentSampler = std::function<Vec3(const Vec3& direction)>;

//! Uniform sky of the given radiance. Not a curiosity: it is the exact ground truth for testing convolution
//! normalisation, because a correctly normalised convolution returns this same radiance for *any* roughness and any
//! normal (see ImageBasedLightingTests).
[[nodiscard]] HP_RENDERER_API EnvironmentSampler MakeConstantEnvironment(Vec3 radiance);

//! `L(d) = scale * max(dot(d, axis), 0)` — a cosine ramp toward `axis`.
//!
//! This is the workhorse test environment: unlike a constant it depends on direction (so it catches hemisphere and
//! normal misalignment). Its irradiance has an exact analytic value only when `axis` parallels the normal
//! (`2*pi/3 * scale`); every other orientation must be checked against quadrature, because no simple closed form
//! exists once the ramp's zero half-sphere clips the sampling horizon. In the zero-roughness limit the specular
//! convolution must return exactly `L` sampled at the reflection direction.
[[nodiscard]] HP_RENDERER_API EnvironmentSampler MakeDirectionalRampEnvironment(Vec3 axis, Vec3 scale);

//! Two-colour gradient along an axis: `mix(below, above, 0.5 * (dot(d, axis) + 1))`.
//!
//! Smooth and strictly positive everywhere, which makes it the environment to use when a test needs "no zeros" — a
//! ramp reaches zero over half the sphere, so a convolution that silently drops a hemisphere can look correct against
//! it in some configurations.
[[nodiscard]] HP_RENDERER_API EnvironmentSampler MakeGradientEnvironment(Vec3 below, Vec3 above, Vec3 axis);

//! Convolution options. Sample counts are explicit rather than hidden defaults because accuracy and cost trade off
//! directly, and a convolution's error budget should be a decision at the call site.
struct IblConvolutionOptions
{
    //! Hemisphere samples per output value.
    UInt32 SampleCount = 1024;

    //! Deterministic seed for the sample sequence. Fixed by default so results are reproducible: an environment
    //! convolution that changed between runs would make image tests flaky and would make a rendered frame flicker if
    //! this were ever used at runtime for progressive refinement.
    UInt32 Seed = 0x5EED1234u;
};

//! Cosine-weighted hemisphere integral: `∫ L(ω) max(n·ω, 0) dω`.
//!
//! This is the irradiance a Lambertian surface needs — with no 1/π folded in, because the BRDF's own 1/π stays in the
//! shading model (PbrShading) where its tests already cover it. Splitting the two keeps each testable against its own
//! closed form; folding them here would mean neither place has a checkable contract.
//!
//! Sampling is cosine-weighted, which makes a constant environment integrate with **zero variance** — the estimator is
//! exact for any sample set. That is a strong normalisation check but a weak orientation check (a misaligned hemisphere
//! still returns the constant), which is why the test suite also integrates directional ramps against quadrature
//! computed independently in `tests/Renderer/TestImageBasedLighting.cpp`. Directional ramps have no simple closed form
//! in general — only the axis-parallel-to-normal case does (`2π/3 · scale`) — so quadrature, not a formula, is the
//! reference for everything oblique.
[[nodiscard]] HP_RENDERER_API Vec3 IntegrateIrradiance(const EnvironmentSampler& environment, const Vec3& normal,
                                                       const IblConvolutionOptions& options = {});

//! GGX-weighted specular convolution, normalised so it returns radiance (not energy).
//!
//! \param viewDirection Unit vector from the surface toward the eye. Passing the normal itself is the standard
//!        cube-map approximation — valid because a cube map is indexed by the reflection vector and cannot vary with
//!        view; the resulting error grows at grazing angles, which is why this signature takes the direction
//!        explicitly rather than baking that shortcut (a later GPU path can choose to bake it deliberately).
//! \param roughness Perceptual roughness, remapped to GGX alpha exactly as PbrShading does — the two must agree or the
//!        specular lobe width will not match between direct and ambient lighting.
[[nodiscard]] HP_RENDERER_API Vec3 PrefilterEnvironmentGGX(const EnvironmentSampler& environment, const Vec3& normal,
                                                           const Vec3& viewDirection, Float32 roughness,
                                                           const IblConvolutionOptions& options = {});

//! The `(A, B)` pair of the split-sum approximation: ambient specular ≈ prefilteredRadiance × (F0·A + B).
//!
//! Derived from Schlick's Fresnel decomposition `F = F0·(1 - (1-v·h)^5) + (1-v·h)^5`, which makes A and B the
//! normalised weighted averages of those two Fresnel factors over the GGX lobe. Because they are averaged with the
//! same weights that PrefilterEnvironmentGGX uses, `A + B == 1` identically — asserted in the tests as a guard against
//! the two convolutions drifting apart (a real and easy regression, since they live in different functions).
struct HP_RENDERER_API BrdfSplitSumFactors
{
    Float32 Scale = 0.0f;  //!< Multiplies F0.
    Float32 Bias = 0.0f;   //!< Added unconditionally.
};

//! \param cosineViewAngle Cosine of the angle between the surface normal and the view direction, clamped to [0,1].
[[nodiscard]] HP_RENDERER_API BrdfSplitSumFactors EvaluateBrdfSplitSum(Float32 cosineViewAngle, Float32 roughness,
                                                                       const IblConvolutionOptions& options = {});

//! Full ambient contribution for a surface, combining the convolutions above — the CPU equivalent of what the GPU
//! per-pixel ambient term will compute.
struct HP_RENDERER_API AmbientLightingResult
{
    Vec3 Diffuse{};
    Vec3 Specular{};

    [[nodiscard]] Vec3 Total() const noexcept;
};

[[nodiscard]] HP_RENDERER_API AmbientLightingResult EvaluateAmbientLighting(const EnvironmentSampler& environment,
                                                                            const PbrMaterialParameters& material,
                                                                            const Vec3& normalShading,
                                                                            const Vec3& viewDirection,
                                                                            const IblConvolutionOptions& options = {});

}  // namespace Hollowpoint::Renderer
