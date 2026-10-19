#pragma once

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Math/MathCore.h"

// ---------------------------------------------------------------------------
// Light components and the CPU reference for light transport quantities.
//
// Like PbrShading.h and ColorPipeline.h this is RHI-free: it defines, in one place, exactly what a light's radiance is
// as a function of position, so the GPU lighting pass can be validated numerically instead of by eye. It also fixes
// the unit conventions, which is where engine lighting usually goes wrong — an intensity that means "lux" in one pass
// and "candela" in another produces a scene that cannot be lit consistently no matter how good the shaders are.
//
// Units (chosen so values behave sanely for both architectural interiors and outdoor scenes):
//   * Directional lights carry **irradiance-like** flux density: Color * Intensity, applied unchanged at every point.
//     Distance is meaningless for a light at infinity, which is why the sun's strength does not fall off with a Range.
//   * Point and spot lights carry **luminous intensity** (candela): Color * Intensity / (4π) would be lumens/steradian
//     bookkeeping; here Intensity is already per-steradian, and LuminousPowerToIntensity converts from the lumens an
//     artist reads on a packaging label.
//   * Colour is linear and **not** premultiplied by intensity. Keeping them separate means an animator can fade a
//     light without touching its colour, and a 16-bit HDR value stays meaningful when Intensity exceeds 1.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Renderer
{

using Math::Float32;
using Math::Vec3;

//! A light source in the scene. One component type with a mode tag rather than three component types: every consumer
//! (culling, shadow assignment, the GPU light buffer, the editor inspector, scripting) then handles one shape and
//! switches on LightType, instead of maintaining three parallel code paths that drift apart.
struct HP_RENDERER_API LightComponent
{
    enum class LightType : unsigned char
    {
        Directional,
        Point,
        Spot,
    };

    LightType Type = LightType::Point;

    //! Linear light colour, 0..1 per channel recommended (intensity carries the magnitude). Values above 1 are legal
    //! and behave as a multiplier, but Intensity exists so that is not the only way to express brightness.
    Vec3 Color = Vec3(1.0f, 1.0f, 1.0f);

    //! Magnitude in the units described by Type (irradiance-like for Directional, candela for Point/Spot).
    Float32 Intensity = 1.0f;

    //! Range of influence in world units for Point/Spot. Zero or negative means "unbounded" — useful for a fill light
    //! that should affect the whole scene, at the cost of the optimisation that a finite range allows (a light with a
    //! bounded range can be culled per-tile; an unbounded one cannot).
    Float32 Range = 0.0f;

    //! Full inner cone angle in radians where the spot is at full strength. Ignored for non-spot lights.
    Float32 SpotInnerAngleRadians = 0.6108652f;  // 35 degrees

    //! Full outer cone angle in radians; outside this the light contributes exactly zero. Must be >= inner.
    Float32 SpotOuterAngleRadians = 0.7853982f;  // 45 degrees

    //! Direction the light points, for directional and spot lights. Not normalised on write; consumers normalise.
    Vec3 Direction = Vec3(0.0f, -1.0f, 0.0f);

    //! When false the light is skipped by collection but keeps its state (same rationale as CameraComponent::Active).
    bool Active = true;
};

//! Distance falloff for a point or spot light: inverse-square with a smooth window to zero at Range.
//!
//! The window is w(d) = clamp(1 - (d/range)^4, 0, 1)^2, multiplied by 1/d^2. Two separate properties, easy to
//! conflate — an earlier version of this comment credited the wrong one:
//!   * **Smoothness at the boundary comes from the outer square**, not from the ^4. Measured with a finite difference,
//!     (1 - x^n)^2 has slope ~0 at x = 1 for n = 1, 2 and 4 alike (-1e-7, -4e-7, -1.6e-6), whereas the *unsquared*
//!     (1 - x^4) has slope -4 there. So squaring is what removes the visible ring a linear fade draws on a floor; the
//!     inner exponent only reshapes the falloff between centre and edge.
//!   * The ^4 specifically keeps the window near 1 for most of the range — w(0.5·range) = (1 - 0.5^4)^2 = 0.8789,
//!     versus 0.5625 for n = 2 and 0.25 for n = 1 — so the light looks inverse-square out to near its edge and then
//!     drops, rather than dimming early like a linear fade. That shape is what the tests pin with an interior value;
//!     smoothness and monotonicity alone cannot distinguish the exponents.
//!   * The inverse-square core is physical, and clamping d^2 from below keeps a surface point coinciding with the
//!     light finite instead of infinite.
//!
//! \param distance Distance from the light to the shaded point. Negative values are treated as their magnitude.
//! \param range Influence radius; <= 0 means unbounded (no window, pure inverse square).
[[nodiscard]] HP_RENDERER_API Float32 DistanceAttenuation(Float32 distance, Float32 range) noexcept;

//! Angular profile of a spot light: 1 inside the inner cone, 0 outside the outer cone, smoothstep in between.
//!
//! \param dotViewSpotAxis Dot product of (normalised point-to-light direction... see note) and the spot's axis.
//!        Pass the cosine between the vector **from the shaded point toward the light** and the spot's forward axis
//!        negated — i.e. dot(normalize(lightPosition - pointPosition), -spotDirection). Both are usually negative of
//!        each other; getting this backwards lights the outside of the cone and is invisible until something moves.
//! \param innerAngleRadians Full inner cone angle (>= 0, <= outer).
//! \param outerAngleRadians Full outer cone angle (<= pi).
[[nodiscard]] HP_RENDERER_API Float32 SpotAngleAttenuation(Float32 dotViewSpotAxis, Float32 innerAngleRadians,
                                                           Float32 outerAngleRadians) noexcept;

//! Radiance arriving at a point from a point light, before the surface's BRDF and cosine terms.
//!
//! Contract, stated because double-counting here is a silent 2x brightness error: the returned value is incident
//! radiance only. EvaluateDirectLight multiplies by N·L internally, so callers must not apply the cosine again.
[[nodiscard]] HP_RENDERER_API Vec3 EvaluatePointLightRadiance(const LightComponent& light, Float32 distance) noexcept;

//! Radiance arriving at a point from a spot light: point-light radiance scaled by the angular profile.
//!
//! \param dotViewSpotAxis As documented on SpotAngleAttenuation.
[[nodiscard]] HP_RENDERER_API Vec3 EvaluateSpotLightRadiance(const LightComponent& light, Float32 distance,
                                                             Float32 dotViewSpotAxis) noexcept;

//! Radiance arriving at any point in the scene from a directional light (position-independent by definition).
[[nodiscard]] HP_RENDERER_API Vec3 EvaluateDirectionalLightRadiance(const LightComponent& light) noexcept;

//! Converts luminous power (lumens, as printed on a real lamp) to intensity (candela) for a given light shape.
//!
//! A point light radiates into the full sphere, so I = P / 4π. A spot concentrates the same power into its cone,
//! Ω = 2π(1 - cos(θ/2)), giving I = P / Ω — which is why a 1000 lm spotlight is far brighter on-axis than a 1000 lm
//! bulb. The convention here divides by the **emission cone** solid angle (the physical reading). Note that some
//! engines divide spot power by 4π as well, treating the cone as a mask rather than a concentrator; if this engine
//! ever matches one of those for asset compatibility, the change belongs in this one function.
[[nodiscard]] HP_RENDERER_API Float32 LuminousPowerToIntensity(Float32 luminousPowerLumens,
                                                               LightComponent::LightType type,
                                                               Float32 spotOuterAngleRadians) noexcept;

}  // namespace Hollowpoint::Renderer
