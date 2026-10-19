#pragma once

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Math/MathCore.h"  // Vec3 (member Dot/Cross/Normalized), Float32

// ---------------------------------------------------------------------------
// The engine's shading model: Cook-Torrance microfacet BRDF with a metallic-roughness parameterisation.
//
// This is the CPU reference implementation. Its purpose is not to shade pixels (the GPU does that) but to define,
// in one readable place, exactly what "PBR" means for this engine — so that:
//
//   * the GLSL/HLSL port can be validated against it numerically, rather than judged by eye, and
//   * a disagreement between CPU and GPU is a bug report with an expected value, not a debate about appearance.
//
// Convention (deliberately matching glTF 2.0 / the Khronos PBR reference, so imported materials behave as artists
// expect):
//
//   * roughness is stored **perceptually** (as authored, 0..1) and remapped to GGX alpha via a = roughness^2.
//     Using roughness directly as alpha makes materials look far too glossy in the mid-range; the square is what
//     maps linear authoring onto the distribution's actual response.
//   * diffuse is Lambertian, specular is Cook-Torrance, and their sum is weighted by (1 - F) so energy is not
//     double-counted at grazing angles.
//   * Fresnel uses Schlick's approximation with F0 derived from a dielectric reflectance of 0.04 (the glTF default).
//
// Known deviation, stated rather than hidden: this is **single-scattering**. Multi-scatter GGX energy loss makes
// rough metals noticeably too dark at grazing angles, which is why a correction term is planned before shipping
// (Kelemen/Lazarov-style visibility compensation, or the Karbis multi-scatter table). Tests below assert against
// single-scattering analytic values, so adding the correction will be an explicit, visible change rather than a
// silent drift.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Renderer
{

using Math::Float32;
using Math::Vec3;

//! Material parameters in the engine's authoring space. Values follow glTF metallic-roughness semantics.
struct HP_RENDERER_API PbrMaterialParameters
{
    //! Linear-space base colour (albedo). Not sRGB: conversion happens at texture-sample time, and storing a
    //! gamma-encoded albedo here would double-apply it once lighting is added.
    Vec3 BaseColor{1.0f, 1.0f, 1.0f};

    //! Perceptual roughness, 0..1. Remapped internally; see MaterialDescriptor notes on the squared mapping.
    Float32 Roughness = 0.5f;

    //! 0 = dielectric, 1 = metal. Interpolating linearly is an approximation (real materials are neither between
    //! the extremes) but it is the convention every DCC tool exports, so imported assets look right.
    Float32 Metallic = 0.0f;

    //! Dielectric specular reflectance at normal incidence. 0.04 covers most non-metals; glTF allows up to 0.08.
    Float32 DielectricF0 = 0.04f;
};

//! Direct-lighting result, split so a renderer can accumulate or debug the lobes separately.
struct HP_RENDERER_API PbrDirectLightResult
{
    Vec3 Diffuse{};
    Vec3 Specular{};

    [[nodiscard]] HP_RENDERER_API Vec3 Total() const noexcept;
};

// ---------------------------------------------------------------------------
// Individual BRDF terms. Exposed because each is independently testable against a closed form, and because a
// renderer that wants only one of them (a debug view, a custom pass) should not have to recompute the rest.
// ---------------------------------------------------------------------------

//! Remaps authored perceptual roughness to GGX alpha: a = roughness^2. Clamped away from zero — an exact-zero
//! alpha makes the distribution divide by zero and produces NaNs that propagate through a whole frame.
[[nodiscard]] HP_RENDERER_API Float32 PerceptualRoughnessToAlpha(Float32 perceptualRoughness) noexcept;

//! Trowbridge-Reitz / GGX normal distribution D(h). Returns the density of microfacets oriented along H.
[[nodiscard]] HP_RENDERER_API Float32 DistributionGGX(const Vec3& normal, const Vec3& viewDirection,
                                                      const Vec3& halfVector, Float32 alpha) noexcept;

//! Smith-Schlick geometric shadowing-masking term for one direction: G = n dot d / (n dot d (1-k) + k).
//!
//! \param k the roughness-dependent remap. The direct-lighting form is (roughness + 1)^2 / 8 and the IBL form is
//! roughness^2 / 2; using one where the other belongs changes specular falloff subtly enough to survive review, so
//! callers pass k explicitly rather than letting a single helper guess.
[[nodiscard]] HP_RENDERER_API Float32 GeometrySchlickGGX(Float32 dotNormalDirection, Float32 k) noexcept;

//! Combined Smith visibility G(N,V,L) = G_Schlick(N,V) * G_Schlick(N,L).
[[nodiscard]] HP_RENDERER_API Float32 GeometrySmith(const Vec3& normal, const Vec3& viewDirection,
                                                    const Vec3& lightDirection, Float32 roughness) noexcept;

//! Schlick's Fresnel approximation: F0 + (1-F0)(1 - cosTheta)^5.
[[nodiscard]] HP_RENDERER_API Vec3 FresnelSchlick(Float32 cosineAngle, const Vec3& f0) noexcept;

//! Fresnel with roughness-aware grazing term (Lazarov's pre-integrated variant), used when sampling a prefiltered
//! environment so rough surfaces do not mirror sharp reflections at grazing angles.
[[nodiscard]] HP_RENDERER_API Vec3 FresnelSchlickRoughness(Float32 cosineAngle, const Vec3& f0,
                                                           Float32 roughness) noexcept;

//! Evaluates the BRDF for one punctual light.
//!
//! \param material Material parameters (perceptual roughness).
//! \param normalShading Shading normal, assumed unit length.
//! \param viewDirection Unit vector from the surface point toward the eye.
//! \param lightDirection Unit vector from the surface point toward the light.
//! \return Diffuse and specular contributions, each already multiplied by the geometric cosine term. The caller
//!         multiplies by radiance and integrates over solid angle; no 1/pi is applied here beyond what the BRDF
//!         itself contains, so a punctual light's intensity must carry its own falloff.
[[nodiscard]] HP_RENDERER_API PbrDirectLightResult EvaluateDirectLight(const PbrMaterialParameters& material,
                                                                       const Vec3& normalShading,
                                                                       const Vec3& viewDirection,
                                                                       const Vec3& lightDirection) noexcept;

}  // namespace Hollowpoint::Renderer
