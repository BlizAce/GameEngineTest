#pragma once

#include "Hollowpoint/Math/MathCore.h"

namespace Hollowpoint::Math
{

// ---------------------------------------------------------------------------
// Color — floating point RGBA in the renderer's working space.
//
// The engine stores colors **linear** (the renderer works in HDR linear and tonemaps at the end), but
// authoring tools, textures from sRGB sources and CSS-style hex literals are sRGB. Use FromSRGBHex /
// ToSRGB for the boundary conversions; they implement the exact IEC 61966-2-1 piecewise transfer
// function rather than a 2.2 gamma approximation, so colours match browsers and Photoshop.
// ---------------------------------------------------------------------------

struct HP_MATH_API Color
{
    Float32 R = 1.0f;
    Float32 G = 1.0f;
    Float32 B = 1.0f;
    Float32 A = 1.0f;

    constexpr Color() = default;
    constexpr Color(Float32 r, Float32 g, Float32 b, Float32 a = 1.0f) : R(r), G(g), B(b), A(a) {}

    //! sRGB components in [0,1] converted to linear working space.
    [[nodiscard]] static Color FromSRGB(Float32 r, Float32 g, Float32 b, Float32 a = 1.0f);

    //! CSS-style "#rrggbb" or "#rrggbbaa" (case insensitive), interpreted as sRGB.
    //!  Returns White for malformed input rather than failing at runtime — colours are author data.
    [[nodiscard]] static Color FromSRGBHex(const char* hexText);

    //! Components in [0,1] interpreted as already-linear (no conversion).
    static constexpr Color FromLinear(Float32 r, Float32 g, Float32 b, Float32 a = 1.0f)
    {
        return Color(r, g, b, a);
    }

    [[nodiscard]] Color ToSRGB() const;        //!< Linear -> sRGB, components in [0,1].
    [[nodiscard]] Color ToLinear() const;      //!< sRGB -> linear (identity when already linear).
    [[nodiscard]] UInt32 ToRGBA8SRGB() const;  //!< Packed as R in the most significant byte.
    [[nodiscard]] static Color FromRGBA8SRGB(UInt32 packed);

    [[nodiscard]] constexpr Float32 GetLuminance() const
    {
        // Rec.709 weights, applied in linear space.
        return 0.2126f * R + 0.7152f * G + 0.0722f * B;
    }

    [[nodiscard]] Color WithAlpha(Float32 alpha) const
    {
        return Color(R, G, B, alpha);
    }
    [[nodiscard]] Color Premultiplied() const
    {
        return Color(R * A, G * A, B * A, A);
    }

    Color operator*(Float32 scalar) const
    {
        return Color(R * scalar, G * scalar, B * scalar, A * scalar);
    }
    Color operator*(const Color& other) const
    {
        return Color(R * other.R, G * other.G, B * other.B, A * other.A);
    }
    Color operator+(const Color& other) const
    {
        return Color(R + other.R, G + other.G, B + other.B, A + other.A);
    }
    Color operator-(const Color& other) const
    {
        return Color(R - other.R, G - other.G, B - other.B, A - other.A);
    }
    bool operator==(const Color&) const = default;

    // Named colours (sRGB values from the CSS specification, converted to linear).
    [[nodiscard]] static Color White();
    [[nodiscard]] static Color Black();
    [[nodiscard]] static Color Gray();
    [[nodiscard]] static Color Red();
    [[nodiscard]] static Color Green();
    [[nodiscard]] static Color Blue();
    [[nodiscard]] static Color CornflowerBlue();  //!< The classic clear colour of XNA demos.
};

[[nodiscard]] inline Color Lerp(Color from, Color to, Float32 t)
{
    return from + (to - from) * t;
}

//! Single sRGB channel -> linear. Values outside [0,1] are clamped (textures never exceed the range).
[[nodiscard]] HP_MATH_API Float32 SRGBToLinear(Float32 channel);

//! Single linear channel -> sRGB. Input is clamped to [0,1].
[[nodiscard]] HP_MATH_API Float32 LinearToSRGB(Float32 channel);

}  // namespace Hollowpoint::Math
