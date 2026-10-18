#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

#include "Hollowpoint/Math/Color.h"
#include "Hollowpoint/Math/MathFunctions.h"

namespace Hollowpoint::Math
{

Float32 SRGBToLinear(Float32 channel)
{
    const Float32 value = Clamp(channel, 0.0f, 1.0f);
    // IEC 61966-2-1: linear segment below 0.04045 to avoid the visible banding a pure power curve
    // produces in near-black gradients.
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

Float32 LinearToSRGB(Float32 channel)
{
    const Float32 value = Clamp(channel, 0.0f, 1.0f);
    return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

Color Color::FromSRGB(Float32 r, Float32 g, Float32 b, Float32 a)
{
    // Alpha is never gamma-decoded: it is a coverage/opacity factor, not light.
    return Color(SRGBToLinear(r), SRGBToLinear(g), SRGBToLinear(b), Clamp(a, 0.0f, 1.0f));
}

namespace
{

[[nodiscard]] int HexDigitValue(char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

//! Reads two hex digits and returns the byte value, or -1 when the text is exhausted/malformed.
[[nodiscard]] int ReadHexByte(std::string_view text, std::size_t& offset)
{
    if (offset + 2 > text.size())
    {
        return -1;
    }
    const int high = HexDigitValue(text[offset]);
    const int low = HexDigitValue(text[offset + 1]);
    if (high < 0 || low < 0)
    {
        return -1;
    }
    offset += 2;
    return (high << 4) | low;
}

}  // namespace

Color Color::FromSRGBHex(const char* hexText)
{
    if (hexText == nullptr)
    {
        return White();
    }

    std::string_view text(hexText);
    if (!text.empty() && text.front() == '#')
    {
        text.remove_prefix(1);
    }

    // Accept #rgb, #rrggbb and #rrggbbaa; anything else falls back to white with a visible default so
    // a typo in an asset shows up as a bright surface rather than a black hole.
    std::size_t offset = 0;
    Float32 channels[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    if (text.size() == 3)
    {
        for (int channel = 0; channel < 3; ++channel)
        {
            const int value = HexDigitValue(text[static_cast<std::size_t>(channel)]);
            if (value < 0)
            {
                return White();
            }
            channels[channel] = static_cast<Float32>((value << 4) | value) / 255.0f;
        }
    }
    else if (text.size() == 6 || text.size() == 8)
    {
        for (int channel = 0; channel < 4; ++channel)
        {
            if (channel == 3 && text.size() == 6)
            {
                break;
            }
            const int value = ReadHexByte(text, offset);
            if (value < 0)
            {
                return White();
            }
            channels[channel] = static_cast<Float32>(value) / 255.0f;
        }
    }
    else
    {
        return White();
    }

    return FromSRGB(channels[0], channels[1], channels[2], channels[3]);
}

Color Color::ToSRGB() const
{
    return Color(LinearToSRGB(R), LinearToSRGB(G), LinearToSRGB(B), Clamp(A, 0.0f, 1.0f));
}

Color Color::ToLinear() const
{
    return Color(SRGBToLinear(R), SRGBToLinear(G), SRGBToLinear(B), Clamp(A, 0.0f, 1.0f));
}

UInt32 Color::ToRGBA8SRGB() const
{
    auto toByte = [](Float32 value) -> UInt32 {
        const Float32 clamped = Clamp(value, 0.0f, 1.0f);
        // +0.5 rounds to nearest, which is what image encoders and the GPU's UNORM conversion do.
        return static_cast<UInt32>(clamped * 255.0f + 0.5f);
    };

    const Color srgb = ToSRGB();
    return (toByte(srgb.R) << 24) | (toByte(srgb.G) << 16) | (toByte(srgb.B) << 8) | toByte(srgb.A);
}

Color Color::FromRGBA8SRGB(UInt32 packed)
{
    const Float32 r = static_cast<Float32>((packed >> 24) & 0xFFu) / 255.0f;
    const Float32 g = static_cast<Float32>((packed >> 16) & 0xFFu) / 255.0f;
    const Float32 b = static_cast<Float32>((packed >> 8) & 0xFFu) / 255.0f;
    const Float32 a = static_cast<Float32>(packed & 0xFFu) / 255.0f;
    return FromSRGB(r, g, b, a);
}

Color Color::White()
{
    return FromSRGB(1.0f, 1.0f, 1.0f);
}
Color Color::Black()
{
    return FromSRGB(0.0f, 0.0f, 0.0f);
}
Color Color::Gray()
{
    return FromSRGB(0.5f, 0.5f, 0.5f);
}
Color Color::Red()
{
    return FromSRGB(1.0f, 0.0f, 0.0f);
}
Color Color::Green()
{
    return FromSRGB(0.0f, 1.0f, 0.0f);
}
Color Color::Blue()
{
    return FromSRGB(0.0f, 0.0f, 1.0f);
}
Color Color::CornflowerBlue()
{
    return FromSRGB(0.392f, 0.584f, 0.929f);
}

}  // namespace Hollowpoint::Math
