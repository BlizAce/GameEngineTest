#include <cmath>
#include <cstdint>
#include <string>

#include <doctest/doctest.h>

#include "Hollowpoint/Renderer/ColorPipeline.h"

using namespace Hollowpoint;
using namespace Hollowpoint::Renderer;
using Math::Float32;
using Math::Vec3;

namespace
{

[[nodiscard]] bool Approximately(const Vec3& a, const Vec3& b, Float32 tolerance = 1e-4f)
{
    return std::fabs(a.X - b.X) <= tolerance && std::fabs(a.Y - b.Y) <= tolerance && std::fabs(a.Z - b.Z) <= tolerance;
}

[[nodiscard]] std::string ToString(const Vec3& v)
{
    return "(" + std::to_string(v.X) + ", " + std::to_string(v.Y) + ", " + std::to_string(v.Z) + ")";
}

[[nodiscard]] bool AllFinite(const Vec3& v)
{
    return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z);
}

}  // namespace

TEST_SUITE("Renderer/ColorPipeline")
{
    TEST_CASE("sRGB decode matches the IEC 61966-2-1 reference values")
    {
        // Hand-computed from the standard's piecewise definition:
        //   linear = encoded / 12.92                              for encoded <= 0.04045
        //   linear = ((encoded + 0.055) / 1.055) ^ 2.4             otherwise
        CHECK(SrgbToLinear(0.0f) == 0.0f);
        CHECK(SrgbToLinear(1.0f) == doctest::Approx(1.0f).epsilon(1e-5));

        // Mid grey: ((0.5 + 0.055)/1.055)^2.4 = 0.214047...
        CHECK(SrgbToLinear(0.5f) == doctest::Approx(0.214048f).epsilon(1e-4));

        // Three-quarter grey: ((0.75 + 0.055)/1.055)^2.4 = 0.522503...
        CHECK(SrgbToLinear(0.75f) == doctest::Approx(0.522503f).epsilon(1e-4));

        // In the linear toe: 0.02 / 12.92 = 0.0015480...
        CHECK(SrgbToLinear(0.02f) == doctest::Approx(0.0015480f).epsilon(1e-3));

        // The value that most visibly distinguishes sRGB from a plain gamma-2.2 curve. If someone "simplifies" this
        // to pow(x, 2.2) the mid-grey assertion fails: pow(0.5, 2.2) = 0.2176, outside tolerance of 0.2140.
        const Float32 midGrey = SrgbToLinear(0.5f);
        const bool notPlainGamma22 = std::fabs(midGrey - std::pow(0.5f, 2.2f)) > 2e-3f;
        CHECK(notPlainGamma22);
    }

    TEST_CASE("sRGB encode matches the standard and inverts decode")
    {
        // Hand-computed: linear 0.5 -> 1.055 * 0.5^(1/2.4) - 0.055 = 0.735357...
        CHECK(LinearToSrgb(0.5f) == doctest::Approx(0.735357f).epsilon(1e-4));

        CHECK(LinearToSrgb(0.0f) == 0.0f);
        CHECK(LinearToSrgb(1.0f) == doctest::Approx(1.0f).epsilon(1e-5));

        // Toe: linear 0.001 * 12.92 = 0.01292.
        CHECK(LinearToSrgb(0.001f) == doctest::Approx(0.01292f).epsilon(1e-3));

        // Round-trip over the whole domain. This is a genuine cross-check: encode and decode are separate branches, so
        // a wrong constant in one of them breaks the round trip even though the other could compensate nowhere.
        Float32 worstError = 0.0f;
        for (std::int32_t step = 0; step <= 1000; ++step)
        {
            const Float32 original = static_cast<Float32>(step) / 1000.0f;
            const Float32 roundTripped = SrgbToLinear(LinearToSrgb(original));
            worstError = std::max(worstError, std::fabs(roundTripped - original));
        }

        INFO("worst round-trip error: " << worstError);
        CHECK(worstError < 1e-4f);
    }

    TEST_CASE("the linear toe uses the standard's 12.92 slope exactly")
    {
        // The round-trip test above cannot pin this constant: encode divides by the slope and decode multiplies by it,
        // so any value at all round-trips perfectly — a wrong slope is self-cancelling and only a direct reference
        // value catches it. Verified against IEC 61966-2-1 rather than this implementation:
        //   linear(0.02) = 0.02 / 12.92       = 0.0015480...
        //   srgb(0.001)  = 0.001 * 12.92      = 0.01292
        //   linear(0.04045) == srgb-branch value at the breakpoint = 0.0031308 (both branches meet).
        CHECK(SrgbToLinear(0.02f) == doctest::Approx(0.02f / 12.92f).epsilon(1e-6));
        CHECK(LinearToSrgb(0.001f) == doctest::Approx(0.001f * 12.92f).epsilon(1e-6));

        // The breakpoint itself is defined so that both branches agree: 0.04045 / 12.92 = 0.0031308.
        CHECK(SrgbToLinear(SrgbConstants::SrgbLowerBound) ==
              doctest::Approx(SrgbConstants::LinearLowerBound).epsilon(1e-4));

        // Slope is not a round number someone might "tidy up", and 1/2.4 (a plausible alternative) differs.
        const Float32 toeSlope = SrgbToLinear(0.02f) / 0.02f;
        CHECK(std::fabs(toeSlope - (1.0f / 12.92f)) < 1e-5f);
    }

    TEST_CASE("the two branches meet at the breakpoint")
    {
        // A discontinuity at 0.04045 would produce a visible band in dark gradients (the region where 8-bit steps are
        // densest). Evaluate just below and above: they must agree far tighter than one 8-bit step (1/255).
        const Float32 decodeBelow = SrgbToLinear(0.0404f);
        const Float32 decodeAbove = SrgbToLinear(0.0405f);
        CHECK(std::fabs(decodeAbove - decodeBelow) < 1.0f / 255.0f * 0.01f);

        const Float32 encodeBelow = LinearToSrgb(0.00313f);
        const Float32 encodeAbove = LinearToSrgb(0.00314f);
        CHECK(std::fabs(encodeAbove - encodeBelow) < 1.0f / 255.0f);
    }

    TEST_CASE("out-of-range sRGB input is clamped and never produces NaN")
    {
        // The stated contract: negative texels from a mis-tagged texture must not raise a negative base to a
        // fractional power. pow() would return NaN there, which propagates through an accumulation buffer silently.
        CHECK(SrgbToLinear(-1.0f) == 0.0f);
        CHECK(SrgbToLinear(2.0f) == doctest::Approx(1.0f).epsilon(1e-5));
        CHECK(LinearToSrgb(-0.5f) == 0.0f);
        CHECK(LinearToSrgb(3.0f) == doctest::Approx(1.0f).epsilon(1e-5));

        const bool finite = std::isfinite(SrgbToLinear(-1.0f)) && std::isfinite(LinearToSrgb(-1.0f));
        CHECK(finite);
    }

    TEST_CASE("per-channel conversion does not mix channels")
    {
        // A luminance-preserving or channel-averaging variant would desaturate saturated colours. Feeding a pure red
        // must yield pure red linearly: any channel mixing shows up as non-zero G/B.
        const Vec3 red = SrgbToLinear(Vec3(1.0f, 0.0f, 0.0f));

        INFO(ToString(red));
        CHECK(red.X == doctest::Approx(1.0f).epsilon(1e-5));
        CHECK(red.Y == 0.0f);
        CHECK(red.Z == 0.0f);

        // And a grey must stay grey (equal channels in, equal out) — the mirror case: catches a variant that treats
        // channels independently but with mismatched constants.
        const Vec3 grey = LinearToSrgb(Vec3(0.5f, 0.5f, 0.5f));
        CHECK(Approximately(grey, Vec3(grey.X, grey.X, grey.X)));

        // Independently: each channel of the vector form must equal the scalar form applied to that channel.
        const Vec3 mixed{0.1f, 0.5f, 0.9f};
        const Vec3 vectorResult = SrgbToLinear(mixed);
        const bool channelsMatchScalars = vectorResult.X == doctest::Approx(SrgbToLinear(0.1f)).epsilon(1e-6) &&
                                          vectorResult.Y == doctest::Approx(SrgbToLinear(0.5f)).epsilon(1e-6) &&
                                          vectorResult.Z == doctest::Approx(SrgbToLinear(0.9f)).epsilon(1e-6);
        CHECK(channelsMatchScalars);
    }

    TEST_CASE("ACES tone map matches hand-computed values")
    {
        // f(x) = (x(2.51x + 0.03)) / (x(2.43x + 0.59) + 0.14), evaluated by hand:
        //   f(0.0) = 0 / 0.14 = 0
        CHECK(ToneMapAceFilmic(0.0f) == 0.0f);

        //   f(0.18) = 0.18*(2.51*0.18+0.03) / (0.18*(2.43*0.18+0.59)+0.14)
        //           = 0.18*0.48180 / (0.18*1.02740 + 0.14) = 0.0867240 / 0.3249320 = 0.26689...
        //   (0.18 is the standard "middle grey" reference in a physical renderer.)
        CHECK(ToneMapAceFilmic(0.18f) == doctest::Approx(0.266897f).epsilon(1e-4));

        //   f(1.0) = 1*(2.51+0.03) / (1*(2.43+0.59)+0.14) = 2.54/3.16 = 0.803797...
        CHECK(ToneMapAceFilmic(1.0f) == doctest::Approx(0.803797f).epsilon(1e-4));

        //   f(2.0) = 2*(5.02+0.03) / (2*(4.86+0.59)+0.14) = 10.1/11.04 = 0.914855...
        CHECK(ToneMapAceFilmic(2.0f) == doctest::Approx(0.914855f).epsilon(1e-4));

        // Toe slope: as x -> 0 the curve is (0.03/0.14)x = 0.214286x.
        CHECK(ToneMapAceFilmic(0.000001f) == doctest::Approx(0.214286f * 0.000001f).epsilon(1e-3));
    }

    TEST_CASE("ACES is an S-curve: it lifts mid-tones and compresses highlights")
    {
        // Crossings of f(x) == x, solved from 2.43x^2 - 1.92x + 0.11 = 0: x ~= 0.06219 and x ~= 0.72794. Inside that
        // interval the curve is above y = x (lifts), outside it is below (darkens). Asserting this shape catches a
        // tone map that is merely a brightness change, and it corrected two earlier wrong claims in the header — that
        // the curve "lifts shadows" (it darkens below 0.062) and that it "darkens everything below 0.375" (it lifts
        // above 0.062). Both statements were made without measuring; neither survived measurement.
        const Float32 lowerCrossing = 0.062186f;
        const Float32 upperCrossing = 0.727938f;

        CHECK(ToneMapAceFilmic(lowerCrossing * 0.5f) < lowerCrossing * 0.5f);  // deep shadow: darkened
        CHECK(ToneMapAceFilmic(0.3f) > 0.3f);                                  // mid-tone: lifted
        CHECK(ToneMapAceFilmic(0.5f) > 0.5f);                                  // mid-tone: lifted
        CHECK(ToneMapAceFilmic(2.0f) < 2.0f);                                  // highlight: compressed

        // The crossings themselves, to a tolerance that would catch a coefficient typo (a wrong 0.59 or 0.14 moves
        // them by more than this).
        CHECK(ToneMapAceFilmic(lowerCrossing) == doctest::Approx(lowerCrossing).epsilon(2e-3));
        CHECK(ToneMapAceFilmic(upperCrossing) == doctest::Approx(upperCrossing).epsilon(2e-3));
    }

    TEST_CASE("ACES is monotonic and bounded, including very large HDR values")
    {
        Float32 previous = ToneMapAceFilmic(0.0f);
        bool monotonic = true;
        bool bounded = true;

        const Float32 samples[] = {0.0f,  0.01f, 0.1f,   0.5f,    1.0f, 2.0f, 7.0f,
                                   7.24f, 8.0f,  100.0f, 1000.0f, 1e6f, 1e18f};

        for (const Float32 sample : samples)
        {
            const Float32 value = ToneMapAceFilmic(sample);
            if (value < previous - 1e-6f)
            {
                monotonic = false;
            }
            if (!(value >= 0.0f && value <= 1.0f))
            {
                bounded = false;
            }
            previous = value;
        }

        CHECK(monotonic);
        CHECK(bounded);
    }

    TEST_CASE("negative HDR renders black, not bright grey")
    {
        // The unclamped rational curve is NOT monotonic through the origin. Measured on the raw formula:
        //   f(-0.1) = +0.210,  f(-0.24) = +0.993
        // so without the clamp a negative intermediate (from a subtraction-based effect or signed blur) shows up as
        // bright grey/white — visible, with no error attached to trace it back.
        CHECK(ToneMapAceFilmic(-0.1f) == 0.0f);
        CHECK(ToneMapAceFilmic(-0.24f) == 0.0f);
        CHECK(ToneMapAceFilmic(-1000.0f) == 0.0f);

        // Vector form likewise.
        const Vec3 negative = ToneMapAceFilmic(Vec3(-0.1f, -5.0f, 0.5f));
        INFO(ToString(negative));
        CHECK(negative.X == 0.0f);
        CHECK(negative.Y == 0.0f);
        CHECK(negative.Z > 0.0f);
    }

    TEST_CASE("exposure is a linear-space multiply")
    {
        CHECK(ApplyExposure(0.5f, 2.0f) == doctest::Approx(1.0f));
        CHECK(ApplyExposure(0.5f, 0.0f) == 0.0f);

        // Exposure happens in linear space, before a curve that is expansive below ~0.3 and compressive above it. So
        // doubling exposure must *more than* double the displayed value in the toe and *less than* double it near
        // saturation. Asserting both pins the order far better than "they differ": if the multiply were moved after
        // ToneMapAceFilmic, the two ratios below would both be exactly 2 (below the clamp), failing each side.
        const Float32 toeDim = ToneMapAceFilmic(ApplyExposure(0.05f, 1.0f));
        const Float32 toeBright = ToneMapAceFilmic(ApplyExposure(0.05f, 2.0f));
        INFO("toe " << toeDim << " -> " << toeBright);
        CHECK(toeBright > toeDim * 2.0f);

        const Float32 highDim = ToneMapAceFilmic(ApplyExposure(3.0f, 1.0f));
        const Float32 highBright = ToneMapAceFilmic(ApplyExposure(3.0f, 2.0f));
        INFO("high " << highDim << " -> " << highBright);
        CHECK(highBright < highDim * 2.0f);
    }

    TEST_CASE("the full display chain composes exposure, tone map and sRGB in that order")
    {
        // Reference value computed independently by applying the three formulas in sequence:
        //   colour = 1.0, exposure = 1.0 -> ACES(1.0) = 0.8037975
        //   -> sRGB encode: 1.055 * 0.8037975^(1/2.4) - 0.055 = 0.9082305
        // (An earlier draft of this expectation said 0.9131 — wrong, because the encode step's *1.055 and -0.055 were
        // dropped while computing it mentally. Recomputed with an independent tool before trusting it.)
        const Vec3 display = LinearToDisplay(Vec3(1.0f, 1.0f, 1.0f), 1.0f);

        INFO(ToString(display));
        CHECK(display.X == doctest::Approx(0.908230f).epsilon(1e-4));

        // Black stays black (a common failure is the sRGB toe lifting true black to a visible grey).
        const Vec3 black = LinearToDisplay(Vec3(0.0f, 0.0f, 0.0f), 1.0f);
        CHECK(black.X == 0.0f);
        CHECK(black.Y == 0.0f);
        CHECK(black.Z == 0.0f);

        // A very bright value saturates to white without NaN or a wrap-around (which would show as dark pixels in the
        // middle of a highlight — the classic "inverted bloom" artefact).
        const Vec3 sun = LinearToDisplay(Vec3(1000.0f, 2000.0f, 5000.0f), 1.0f);
        INFO(ToString(sun));
        CHECK(AllFinite(sun));
        CHECK(sun.X > 0.99f);
        CHECK(sun.Y > 0.99f);

        // Zero exposure must produce black rather than leaving the unexposed image visible.
        const Vec3 noExposure = LinearToDisplay(Vec3(0.5f, 0.5f, 0.5f), 0.0f);
        CHECK(noExposure.X == 0.0f);

        // Monotonic in exposure across a wide range: an inversion here would make the exposure slider non-monotonic.
        Float32 previous = -1.0f;
        bool monotonic = true;
        for (Float32 exposure = 0.0f; exposure <= 4.0f; exposure += 0.05f)
        {
            const Float32 value = LinearToDisplay(Vec3(0.3f, 0.3f, 0.3f), exposure).X;
            if (value < previous - 1e-6f)
            {
                monotonic = false;
            }
            previous = value;
        }
        CHECK(monotonic);
    }

    TEST_CASE("no path produces NaN or infinities for hostile inputs")
    {
        // The conversions clamp their domain, so they must be finite for anything. Tone mapping now clamps its input
        // at the saturation point, which is what makes 1e30/1e38 safe: before that clamp the rational form overflowed
        // to inf/inf and returned NaN (asserted here because a NaN in an HDR buffer silently destroys every pixel it
        // touches downstream, with no error raised anywhere).
        const Float32 hostile[] = {-1e30f, -1.0f, 0.0f, 1e-38f, 1.0f, 1e30f, 1e38f};

        for (const Float32 value : hostile)
        {
            INFO("input " << value);

            CHECK(std::isfinite(SrgbToLinear(value)));
            CHECK(std::isfinite(LinearToSrgb(value)));
            CHECK(std::isfinite(ToneMapAceFilmic(value)));
            CHECK(ToneMapAceFilmic(value) >= 0.0f);
            CHECK(ToneMapAceFilmic(value) <= 1.0f);

            // Exposure is a plain multiply with no domain clamp, so it legitimately overflows at absurd magnitudes
            // (1e38 * 1e30). Only the plausible range must stay finite — tone mapping clamps what comes after it.
            if (std::fabs(value) <= 1e6f)
            {
                const Float32 exposed = ApplyExposure(value, 1000.0f);
                CHECK(std::isfinite(exposed));
            }
        }

        // The full chain must be finite and in range even for an absurd HDR pixel, because that is what a shader pass
        // hands it after a bug upstream — the display path is the last place a NaN can be caught.
        const Vec3 extreme = LinearToDisplay(Vec3(1e30f, -5.0f, 1e-30f), 4.0f);
        CHECK(AllFinite(extreme));
        const bool xInRange = extreme.X >= 0.0f && extreme.X <= 1.0f;
        CHECK(xInRange);
        CHECK(extreme.Y == 0.0f);
    }
}
