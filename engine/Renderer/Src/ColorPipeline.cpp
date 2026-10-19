#include <algorithm>
#include <cmath>

#include "Hollowpoint/Renderer/ColorPipeline.h"
#include "Hollowpoint/Renderer/ShadingConstants.h"

namespace Hollowpoint::Renderer
{

using namespace SrgbConstants;

Float32 SrgbToLinear(Float32 encoded) noexcept
{
    // Defined on [0,1] only. A negative texel (mis-tagged signed texture, or a value already linear and subtracted)
    // must not reach pow() with a fractional exponent — that yields NaN, which then propagates through every
    // downstream accumulation and is nearly impossible to trace back to a texture import flag.
    const Float32 clamped = std::clamp(encoded, 0.0f, 1.0f);

    if (clamped <= SrgbLowerBound)
    {
        return clamped / LinearSegmentSlope;
    }

    return std::pow((clamped + GammaOffset) / GammaScale, GammaExponent);
}

Float32 LinearToSrgb(Float32 linear) noexcept
{
    const Float32 clamped = std::clamp(linear, 0.0f, 1.0f);

    if (clamped <= LinearLowerBound)
    {
        return clamped * LinearSegmentSlope;
    }

    return GammaScale * std::pow(clamped, 1.0f / GammaExponent) - GammaOffset;
}

Vec3 SrgbToLinear(const Vec3& encoded) noexcept
{
    // Per-channel by definition. A luminance-preserving conversion would desaturate: mixing channels to preserve a
    // scalar that display photometry does not ask for is a common "why does everything look washed out" bug, so no
    // such entry point exists here.
    return Vec3{SrgbToLinear(encoded.X), SrgbToLinear(encoded.Y), SrgbToLinear(encoded.Z)};
}

Vec3 LinearToSrgb(const Vec3& linear) noexcept
{
    return Vec3{LinearToSrgb(linear.X), LinearToSrgb(linear.Y), LinearToSrgb(linear.Z)};
}

Float32 ToneMapAceFilmic(Float32 hdrValue) noexcept
{
    // Negative HDR reaches this point legitimately (a subtraction-based effect or a signed blur can produce it). It is
    // clamped to zero first because the rational curve is NOT monotonic through the origin: measured, f(-0.1) = +0.210
    // and f(-0.24) = +0.993, so an unclamped negative value renders as bright grey/white instead of black — a very
    // visible artefact with no error attached. The numerator's root is x = -0.03/2.51 ~= -0.012, beyond which the sign
    // flips; the denominator has no real roots (complex pair at -0.121 +/- 0.207i), so it never blows up.
    //
    // Upper clamp at 8.0: the curve saturates at x ~= 7.2417 and is monotonic up to there, so every input above 8 maps
    // to exactly 1 either way — but evaluating the rational form past ~1.16e19 overflows float32 (2.51x^2 exceeds the
    // range), producing inf/inf = NaN, which propagates through blending and destroys a whole frame with no error. One
    // max() makes that unreachable; huge values occur in practice from an unnormalised HDRI or a bug upstream.
    constexpr Float32 kSaturationPoint = AceFilmic.SaturationPoint;
    const Float32 x = std::clamp(hdrValue, 0.0f, kSaturationPoint);

    // Coefficients come from ShadingConstants so the generated GLSL uses identical numbers — see AceFilmicCoefficients.
    const Float32 numerator = x * (AceFilmic.NumeratorLinear * x + AceFilmic.NumeratorConstant);
    const Float32 denominator =
        x * (AceFilmic.DenominatorLinear * x + AceFilmic.DenominatorMiddle) + AceFilmic.DenominatorConstant;

    // For x >= 0 the denominator starts at 0.14 and increases (its derivative 4.86x + 0.59 is positive there), so no
    // zero guard is needed — the curve is smooth across its whole intended domain.
    return std::clamp(numerator / denominator, 0.0f, 1.0f);
}

Vec3 ToneMapAceFilmic(const Vec3& hdrColor) noexcept
{
    return Vec3{ToneMapAceFilmic(hdrColor.X), ToneMapAceFilmic(hdrColor.Y), ToneMapAceFilmic(hdrColor.Z)};
}

Float32 ApplyExposure(Float32 linearColor, Float32 exposure) noexcept
{
    // Exposure is a scene-linear multiply. Doing it after tone mapping would scale an already-compressed image (which
    // changes contrast, not brightness), and doing it after sRGB encoding multiplies in gamma space and visibly
    // crushes shadows — hence a dedicated function rather than a step inside one monolithic shader.
    return linearColor * exposure;
}

Vec3 ApplyExposure(const Vec3& linearColor, Float32 exposure) noexcept
{
    return linearColor * exposure;
}

Vec3 LinearToDisplay(const Vec3& hdrColor, Float32 exposure) noexcept
{
    // The one canonical order: exposure -> tone map -> sRGB encode. Composed here so a CPU reference image and the
    // GPU pass cannot drift apart by each choosing its own sequence.
    const Vec3 exposed = ApplyExposure(hdrColor, exposure);
    const Vec3 toneMapped = ToneMapAceFilmic(exposed);

    return LinearToSrgb(toneMapped);
}

}  // namespace Hollowpoint::Renderer
