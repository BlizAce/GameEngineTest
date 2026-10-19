#pragma once

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Math/MathCore.h"

// ---------------------------------------------------------------------------
// Colour space conversion and tone mapping — the CPU reference for the display pipeline.
//
// Everything between a material's albedo and the framebuffer is linear: lighting math in sRGB is simply wrong (it
// averages in perceptual rather than physical light, so highlights clip early and mid-tones go muddy). These helpers
// define the single place where that boundary is crossed, which matters twice over:
//
//   * the GPU shader port must agree with them numerically, and
//   * a texture loaded as sRGB but sampled as linear (or converted twice) is one of the most common and least visible
//     colour bugs there is — pinning the conversion in code makes it testable rather than a matter of taste.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Renderer
{

using Math::Float32;
using Math::Vec3;

//! sRGB transfer function constants (IEC 61966-2-1). Exposed so tests and tooling reference the same numbers the
//! implementation uses rather than repeating them.
namespace SrgbConstants
{
//! Linear value below which the transfer function is linear rather than a power curve. The two branches meet here.
constexpr Float32 LinearLowerBound = 0.0031308f;

//! The sRGB-side counterpart of LinearLowerBound: 12.92 * 0.0031308 ~= 0.04045.
constexpr Float32 SrgbLowerBound = 0.04045f;

//! Slope of the linear segment near black. The power curve alone has an infinite derivative at zero, which would make
//! dark-region quantisation and dithering behave badly; the linear toe is deliberate, not an approximation.
constexpr Float32 LinearSegmentSlope = 12.92f;

//! Power of the non-linear segment.
constexpr Float32 GammaExponent = 2.4f;

//! Offset of the power-curve fit.
constexpr Float32 GammaOffset = 0.055f;

//! Scale of the power-curve fit.
constexpr Float32 GammaScale = 1.055f;

}  // namespace SrgbConstants

//! sRGB-encoded value (as stored in an 8-bit texture or shown on a monitor) to scene-linear light.
//!
//! Input is clamped to [0,1]: the function is only defined there, and feeding it a negative texel from a
//! mis-tagged signed texture should not produce a NaN from raising a negative base to a fractional power.
[[nodiscard]] HP_RENDERER_API Float32 SrgbToLinear(Float32 encoded) noexcept;

//! Scene-linear light to an sRGB-encoded value for display. Inverse of SrgbToLinear over [0,1].
[[nodiscard]] HP_RENDERER_API Float32 LinearToSrgb(Float32 linear) noexcept;

//! Per-channel sRGB decode. Colour conversion is per-channel by definition — luminance-preserving variants would
//! desaturate saturated colours, so there is deliberately no "convert as one value" entry point.
[[nodiscard]] HP_RENDERER_API Vec3 SrgbToLinear(const Vec3& encoded) noexcept;

//! Per-channel sRGB encode.
[[nodiscard]] HP_RENDERER_API Vec3 LinearToSrgb(const Vec3& linear) noexcept;

// ---------------------------------------------------------------------------
// Tone mapping.
//
// The engine renders HDR (values well above 1.0 are meaningful and expected: a sun is thousands of nits). Before the
// image can be shown it must be compressed into [0,1]. This uses Narkowicz's rational approximation of the ACES
// output transform — chosen because it is cheap, monotonic, keeps highlights from clipping to flat white, and is what
// most real-time engines ship. It is an approximation of the true ACES pipeline (no separate RRT/ODT, no gamut
// mapping), which is a deliberate quality/perf trade rather than an error; if full ACES is wanted later it replaces
// this function without touching its callers.
// ---------------------------------------------------------------------------

//! Narkowicz ACES filmic approximation: (x(2.51x + 0.03)) / (x(2.43x + 0.59) + 0.14), clamped to [0,1].
//!
//! Measured properties (computed, not assumed — two earlier drafts of this comment stated these wrongly):
//!   * toe slope as x -> 0 is 0.03/0.14 = 0.2143;
//!   * the curve crosses y == x at x ~= 0.0622 and x ~= 0.7279, so it **lifts** everything between them and
//!     **darkens** outside — i.e. it is an S/contrast curve: deep shadows are pushed down, mid-tones lifted,
//!     highlights compressed toward white. Neither "it lifts shadows" nor "it darkens everything below 0.375" is right;
//!   * saturation to 1.0 happens at x ~= 7.2417, where numerator and denominator polynomials cross (the latter has no
//!     real roots, so it never blows up); above that the curve would exceed 1 and is clamped.
//!
//! Applied per channel: saturated highlights roll off in hue rather than greying out, at the cost of some hue shifting
//! near saturation — inherent to per-channel tone mapping, not a bug in this implementation.
[[nodiscard]] HP_RENDERER_API Float32 ToneMapAceFilmic(Float32 hdrValue) noexcept;

//! Per-channel variant.
[[nodiscard]] HP_RENDERER_API Vec3 ToneMapAceFilmic(const Vec3& hdrColor) noexcept;

//! Applies exposure in scene-linear space, before tone mapping and before sRGB encoding.
//!
//! Order matters: exposure after tone mapping would scale an already-compressed image (changing contrast rather than
//! brightness), and exposure after sRGB encoding is a gamma-space multiply that visibly crushes shadows. Exposing this
//! as its own function makes the required order part of the API rather than a comment inside one monolithic shader.
[[nodiscard]] HP_RENDERER_API Float32 ApplyExposure(Float32 linearColor, Float32 exposure) noexcept;

[[nodiscard]] HP_RENDERER_API Vec3 ApplyExposure(const Vec3& linearColor, Float32 exposure) noexcept;

//! The full linear-to-display chain: exposure, tone map, sRGB encode. Provided so a CPU reference image and the GPU
//! pass cannot drift apart by each composing the steps in their own order.
[[nodiscard]] HP_RENDERER_API Vec3 LinearToDisplay(const Vec3& hdrColor, Float32 exposure) noexcept;

}  // namespace Hollowpoint::Renderer
