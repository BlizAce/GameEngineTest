#include <cmath>
#include <cstdlib>

#include <doctest/doctest.h>

#include "Hollowpoint/Math/Color.h"
#include "Hollowpoint/Math/MathFunctions.h"

using namespace Hollowpoint::Math;

TEST_SUITE("Math/Color")
{
    TEST_CASE("sRGB endpoints map exactly to linear endpoints")
    {
        CHECK(SRGBToLinear(0.0f) == 0.0f);
        CHECK(SRGBToLinear(1.0f) == doctest::Approx(1.0f));
        CHECK(LinearToSRGB(0.0f) == 0.0f);
        CHECK(LinearToSRGB(1.0f) == doctest::Approx(1.0f));

        // The mid-grey knee of the sRGB curve: 0.04045 -> 0.0031308 exactly (both directions).
        CHECK(SRGBToLinear(0.04045f) == doctest::Approx(0.0031308f).epsilon(1e-4f));
        CHECK(LinearToSRGB(0.0031308f) == doctest::Approx(0.04045f).epsilon(1e-4f));
    }

    TEST_CASE("sRGB conversion is monotonic and inverts itself")
    {
        Float32 previousLinear = -1.0f;
        for (int step = 0; step <= 255; ++step)
        {
            const Float32 srgb = static_cast<Float32>(step) / 255.0f;
            const Float32 linear = SRGBToLinear(srgb);

            CHECK(linear >= previousLinear);
            previousLinear = linear;

            // Round-tripping a channel must reproduce it within one 8-bit step.
            CHECK(LinearToSRGB(linear) == doctest::Approx(srgb).epsilon(1e-4f).scale(1.0f));
        }
    }

    TEST_CASE("the piecewise curve is used, not a plain gamma 2.2 approximation")
    {
        // A pure pow(x, 2.2) gives 0.00137 for sRGB 0.05; the spec's linear segment gives 0.00394 —
        // nearly three times brighter. Pinning this catches an accidental switch to the cheap power
        // approximation, which visibly crushes shadow detail.
        CHECK(SRGBToLinear(0.05f) == doctest::Approx(0.0039359f).epsilon(1e-4f).scale(1.0f));
    }

    TEST_CASE("out-of-range channels are clamped rather than producing NaN")
    {
        CHECK(SRGBToLinear(-1.0f) == 0.0f);
        CHECK(SRGBToLinear(2.0f) == doctest::Approx(1.0f));
        CHECK(LinearToSRGB(-0.5f) == 0.0f);
        CHECK(LinearToSRGB(3.0f) == doctest::Approx(1.0f));
    }

    TEST_CASE("FromSRGBHex parses CSS colour strings")
    {
        const Color white = Color::FromSRGBHex("#FFFFFF");
        CHECK(ApproximatelyEqual(white.R, 1.0f, 1e-6f));
        CHECK(ApproximatelyEqual(white.A, 1.0f, 1e-6f));

        const Color black = Color::FromSRGBHex("000000");
        CHECK(ApproximatelyEqual(black.R, 0.0f, 1e-6f));

        // Case insensitive, and shorthand expands by duplicating the nibble (#f80 -> #ff8800).
        CHECK(Color::FromSRGBHex("#ffffff") == Color::FromSRGBHex("#FFFFFF"));
        const Color shorthand = Color::FromSRGBHex("#f80");
        const Color expanded = Color::FromSRGBHex("#ff8800");
        CHECK(shorthand == expanded);

        // Alpha is parsed when present.
        const Color halfTransparent = Color::FromSRGBHex("#ff000080");
        CHECK(halfTransparent.A == doctest::Approx(128.0f / 255.0f).epsilon(1e-3f).scale(1.0f));

        // Malformed input falls back to white (a loud, visible default) instead of asserting.
        CHECK(Color::FromSRGBHex("#12345") == Color::White());
        CHECK(Color::FromSRGBHex("zzzzzz") == Color::White());
        CHECK(Color::FromSRGBHex(nullptr) == Color::White());
    }

    TEST_CASE("hex colours decode through the sRGB curve, not raw")
    {
        // #808080 is mid-grey in sRGB: 128/255 = 0.502 -> linear ~0.216, definitely not 0.5.
        const Color grey = Color::FromSRGBHex("#808080");
        CHECK(grey.R == doctest::Approx(0.2158f).epsilon(1e-3f).scale(1.0f));
    }

    TEST_CASE("RGBA8 packing round-trips through sRGB")
    {
        // The inputs are sRGB components (what an image file or editor shows), which is the normal way
        // to author a colour; the engine stores them linearly.
        const Color original = Color::FromSRGB(0.25f, 0.5f, 0.75f, 1.0f);
        const UInt32 packed = original.ToRGBA8SRGB();

        // R is the most significant byte, so an opaque colour ends in 0xFF alpha.
        // FromSRGB interprets its arguments as sRGB and stores them linearised, so encoding back must
        // return the original sRGB bytes: 0.25 -> 64, 0.5 -> 128, 0.75 -> 191 (round-trip identity).
        CHECK((packed & 0xFFu) == 0xFFu);
        CHECK(((packed >> 24) & 0xFFu) == 64u);
        CHECK(((packed >> 16) & 0xFFu) == 128u);
        CHECK(((packed >> 8) & 0xFFu) == 191u);

        // Decoding quantises to an 8-bit step, which is worth up to ~0.004 in linear space near the
        // dark end of the curve — that is the precision loss being characterised here.
        const Color decoded = Color::FromRGBA8SRGB(packed);
        CHECK(ApproximatelyEqual(decoded.R, original.R, 5e-3f));
        CHECK(ApproximatelyEqual(decoded.G, original.G, 5e-3f));
        CHECK(ApproximatelyEqual(decoded.B, original.B, 5e-3f));
    }

    TEST_CASE("encoding a linear colour applies the transfer function")
    {
        // Linear 0.25 encodes to sRGB 137/255 — what an image viewer shows for a surface reflecting
        // 25% of the light. Storing linear bytes instead makes everything look washed out, so this is
        // pinned independently of the round-trip test above.
        const Color linear = Color::FromLinear(0.25f, 0.5f, 0.75f, 1.0f);
        const UInt32 packed = linear.ToRGBA8SRGB();

        // Compared with a tolerance of one code value: the exact byte depends on whether the transfer
        // function's result lands just above or below the x.5 rounding boundary in float precision
        // (0.75 linear sits exactly on it), and ±1 LSB is imperceptible and not worth pinning down.
        auto byteOf = [](UInt32 value, int shift) { return static_cast<int>((value >> shift) & 0xFFu); };
        CHECK(std::abs(byteOf(packed, 24) - 137) <= 1);
        CHECK(std::abs(byteOf(packed, 16) - 188) <= 1);
        CHECK(std::abs(byteOf(packed, 8) - 225) <= 1);
    }

    TEST_CASE("ToSRGB and ToLinear are inverses within 8-bit precision")
    {
        const Color linear = Color::FromLinear(0.1f, 0.4f, 0.9f, 1.0f);
        const Color roundTripped = linear.ToSRGB().ToLinear();

        CHECK(ApproximatelyEqual(roundTripped.R, 0.1f, 1e-3f));
        CHECK(ApproximatelyEqual(roundTripped.G, 0.4f, 1e-3f));
        CHECK(ApproximatelyEqual(roundTripped.B, 0.9f, 1e-3f));
    }

    TEST_CASE("luminance follows Rec.709 weights in linear space")
    {
        CHECK(Color::Black().GetLuminance() == doctest::Approx(0.0f));
        CHECK(Color::White().GetLuminance() == doctest::Approx(1.0f).epsilon(1e-4f));

        // Green is the brightest primary under Rec.709, blue the darkest.
        CHECK(Color::Green().GetLuminance() > Color::Red().GetLuminance());
        CHECK(Color::Red().GetLuminance() > Color::Blue().GetLuminance());
    }

    TEST_CASE("alpha helpers")
    {
        const Color color(0.2f, 0.4f, 0.6f, 1.0f);
        CHECK(color.WithAlpha(0.5f).A == doctest::Approx(0.5f));
        CHECK(color.WithAlpha(0.5f).R == doctest::Approx(0.2f));

        const Color premultiplied = color.WithAlpha(0.5f).Premultiplied();
        CHECK(premultiplied.R == doctest::Approx(0.1f));
        CHECK(premultiplied.A == doctest::Approx(0.5f));
    }

    TEST_CASE("arithmetic and interpolation")
    {
        const Color a(0.2f, 0.4f, 0.6f, 1.0f);
        const Color b(0.6f, 0.8f, 1.0f, 1.0f);

        CHECK(ApproximatelyEqual((a + b).R, 0.8f, 1e-6f));
        CHECK(ApproximatelyEqual((b - a).R, 0.4f, 1e-6f));
        CHECK(ApproximatelyEqual((a * 2.0f).R, 0.4f, 1e-6f));
        CHECK(ApproximatelyEqual(Lerp(a, b, 0.5f).G, 0.6f, 1e-6f));
    }
}
