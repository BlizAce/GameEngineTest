#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "Hollowpoint/Core/FileSystem.h"
#include "Hollowpoint/Graphics/GraphicsTypes.h"
#include "Hollowpoint/Graphics/Texture.h"
#include "Hollowpoint/Runtime/Screenshot.h"

// stb_image_write is a header-only implementation with one TU define. Kept in this .cpp so the symbol appears once and
// no engine header sees stb's macros.
#if defined(__GNUC__) || defined(__clang__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored \
        "-Wall"  // stb headers are not written to the engine's warning level (third_party policy)
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION 1
#include <stb_image_write.h>

#if defined(__GNUC__) || defined(__clang__)
    #pragma GCC diagnostic pop
#endif

namespace Hollowpoint::Runtime
{

using Core::Size;
using Core::UInt16;
using Core::UInt32;
using Core::UInt8;
using Graphics::GraphicsDevice;
using Graphics::GraphicsFormat;
using Graphics::Texture;

namespace
{

//! Decodes one IEEE 754 half to float.
/**
 * Written out rather than pulled from a library because nothing else in the engine needs it yet, and the alternative —
 * treating the bits as a float or truncating — produces values that are wrong by orders of magnitude while staying
 * finite, which is the worst kind of image bug: a screenshot that looks like a slightly dark render.
 *
 * Subnormals (exponent 0, mantissa nonzero) are handled by the same path as zero scaled by 2^-14; NaN and infinity
 * carry their exponent pattern through. Both branches below are exercised by HalfToFloatTests rather than trusted.
 */
[[nodiscard]] float HalfToFloat(UInt16 bits) noexcept
{
    const UInt32 sign = static_cast<UInt32>(bits & 0x8000u) << 16u;
    const UInt32 exponent = static_cast<UInt32>(bits & 0x7C00u) >> 10u;
    const UInt32 mantissa = static_cast<UInt32>(bits & 0x03FFu);

    UInt32 assembled = 0u;
    if (exponent == 0u)
    {
        if (mantissa == 0u)
        {
            assembled = sign;  // +/- zero
        }
        else
        {
            // Subnormal half: renormalise by shifting the mantissa until the leading one reaches the implicit position
            // and decrementing the exponent to match. A loop over ten bits is cheap and obviously correct; the
            // arithmetic trick is faster and easier to get wrong by one bit at exactly the boundary this path exists
            // for.
            UInt32 shiftedMantissa = mantissa;
            UInt32 shiftedExponent = 127u - 15u + 1u;
            while ((shiftedMantissa & 0x0400u) == 0u)
            {
                shiftedMantissa <<= 1u;
                --shiftedExponent;
            }
            assembled = sign | (shiftedExponent << 23u) | ((shiftedMantissa & 0x03FFu) << 13u);
        }
    }
    else if (exponent == 0x1Fu)
    {
        assembled = sign | 0x7F800000u | (mantissa << 13u);  // Inf or NaN
    }
    else
    {
        assembled = sign | ((exponent + (127u - 15u)) << 23u) | (mantissa << 13u);
    }

    return std::bit_cast<float>(assembled);
}

[[nodiscard]] UInt32 BytesPerPixel(GraphicsFormat format) noexcept
{
    switch (format)
    {
        case GraphicsFormat::Rgba8Unorm:
        case GraphicsFormat::Bgra8Unorm:
        case GraphicsFormat::Rgba8Srgb:
            return 4u;
        case GraphicsFormat::Rgba16Float:
            return 8u;
        case GraphicsFormat::Rgba32Float:
            return 16u;
        default:
            return 0u;
    }
}

[[nodiscard]] float ClampToUnit(float value) noexcept
{
    if (std::isnan(value))
    {
        // NaN reaching a screenshot means an unwritten texel or a divide-by-zero in shading. Turning it into black
        // would hide it inside a plausible image, so it becomes magenta — impossible to mistake for content, and
        // visible in the file itself rather than only in a log line someone has to correlate by timestamp.
        return -1.0f;  // caller maps the sentinel to magenta
    }
    return std::clamp(value, 0.0f, 1.0f);
}

[[nodiscard]] unsigned char ToByte(float value) noexcept
{
    if (value < 0.0f)
    {
        return 0u;  // sentinel (NaN source) handled by the caller's per-channel check below
    }
    // Rounded, not truncated: truncating 254.99 to 254 makes an all-white image read as very slightly grey and forces
    // every comparison against a reference PNG to carry an off-by-one tolerance for no reason.
    return static_cast<unsigned char>(std::clamp(value * 255.0f + 0.5f, 0.0f, 255.0f));
}

//! Reads one texel's channels in engine order (R,G,B,A) from a row of `format`.
void UnpackTexel(const unsigned char* source, GraphicsFormat format, float (&outRgba)[4])
{
    switch (format)
    {
        case GraphicsFormat::Rgba8Unorm:
            outRgba[0] = static_cast<float>(source[0]) / 255.0f;
            outRgba[1] = static_cast<float>(source[1]) / 255.0f;
            outRgba[2] = static_cast<float>(source[2]) / 255.0f;
            outRgba[3] = static_cast<float>(source[3]) / 255.0f;
            break;

        case GraphicsFormat::Rgba8Srgb:
            // Stored sRGB-encoded, and PNG is conventionally read as sRGB by viewers — so the bytes pass through
            // unchanged. Decoding to linear here would make the saved file darker than what was on screen: hardware
            // applies that decode at sample time, and a screenshot captures storage, not a sampled value.
            outRgba[0] = static_cast<float>(source[0]) / 255.0f;
            outRgba[1] = static_cast<float>(source[1]) / 255.0f;
            outRgba[2] = static_cast<float>(source[2]) / 255.0f;
            outRgba[3] = static_cast<float>(source[3]) / 255.0f;
            break;

        case GraphicsFormat::Bgra8Unorm:
            // The swapchain's format on this host (Swapchain.cpp prefers B8G8R8A8_UNORM), so getting the swap wrong
            // produces a red/blue-inverted screenshot that is otherwise pixel-perfect — the kind of bug that survives
            // because every texel still holds "a colour".
            outRgba[0] = static_cast<float>(source[2]) / 255.0f;
            outRgba[1] = static_cast<float>(source[1]) / 255.0f;
            outRgba[2] = static_cast<float>(source[0]) / 255.0f;
            outRgba[3] = static_cast<float>(source[3]) / 255.0f;
            break;

        case GraphicsFormat::Rgba16Float:
        {
            UInt16 components[4] = {};
            std::memcpy(components, source, sizeof(components));
            for (int channel = 0; channel < 4; ++channel)
            {
                outRgba[channel] = HalfToFloat(components[channel]);
            }
            break;
        }

        case GraphicsFormat::Rgba32Float:
            std::memcpy(outRgba, source, sizeof(float) * 4u);
            break;

        default:
            outRgba[0] = outRgba[1] = outRgba[2] = outRgba[3] = 0.0f;
            break;
    }
}

[[nodiscard]] std::string DescribeFormat(GraphicsFormat format)
{
    switch (format)
    {
        case GraphicsFormat::Unknown:
            return "Unknown";
        case GraphicsFormat::Rgba8Unorm:
            return "Rgba8Unorm";
        case GraphicsFormat::Bgra8Unorm:
            return "Bgra8Unorm";
        case GraphicsFormat::Rgba8Srgb:
            return "Rgba8Srgb";
        case GraphicsFormat::Rgba16Float:
            return "Rgba16Float";
        case GraphicsFormat::Rgba32Float:
            return "Rgba32Float";
        default:
            return "a format with no PNG conversion";
    }
}

[[nodiscard]] bool HasParentDirectory(const std::string& path)
{
    const std::size_t separator = path.find_last_of("/\\");
    if (separator == std::string::npos)
    {
        return true;  // "out.png" lands in the current directory, which exists by definition of having run from it
    }
    if (separator == 0u)
    {
        return true;  // "/tmp/out.png" — root exists
    }
    const std::string parent = path.substr(0, separator);
    return Core::DirectoryExists(parent);
}

[[nodiscard]] std::string ParentDirectory(const std::string& path)
{
    const std::size_t separator = path.find_last_of("/\\");
    if (separator == std::string::npos)
    {
        return ".";
    }
    return path.substr(0, separator);
}

}  // namespace

bool ConvertToRgba8(const void* source, Size byteCount, UInt32 rowPitchBytes, GraphicsFormat format, UInt32 width,
                    UInt32 height, std::vector<unsigned char>& outRgba8, std::string& outError)
{
    outError.clear();

    const UInt32 bytesPerPixel = BytesPerPixel(format);
    if (source == nullptr)
    {
        outError = "screenshot conversion given no source pixels";
        return false;
    }
    if (width == 0u || height == 0u)
    {
        outError = "screenshot conversion given a " + std::to_string(width) + "x" + std::to_string(height) + " extent";
        return false;
    }
    if (bytesPerPixel == 0u)
    {
        outError = "no PNG conversion for format " + DescribeFormat(format);
        return false;
    }
    // Row pitch must at least hold a row of texels. A smaller pitch means the readback and this call disagree about the
    // texture, and walking rows with it would read past the buffer's end on later rows while producing an image for the
    // first.
    if (rowPitchBytes < width * bytesPerPixel)
    {
        outError = "row pitch " + std::to_string(rowPitchBytes) + " is smaller than a row of " + std::to_string(width) +
                   " texels at " + DescribeFormat(format);
        return false;
    }
    const Size needed = static_cast<Size>(rowPitchBytes) * height;
    if (byteCount < needed)
    {
        outError = "screenshot buffer holds " + std::to_string(byteCount) + " bytes, needs " + std::to_string(needed);
        return false;
    }

    outRgba8.assign(static_cast<size_t>(width) * height * 4u, 0u);
    const auto* rows = static_cast<const unsigned char*>(source);

    for (UInt32 y = 0; y < height; ++y)
    {
        const unsigned char* rowStart = rows + static_cast<size_t>(y) * rowPitchBytes;
        unsigned char* outRow = outRgba8.data() + static_cast<size_t>(y) * width * 4u;

        for (UInt32 x = 0; x < width; ++x)
        {
            float texel[4] = {};
            UnpackTexel(rowStart + static_cast<size_t>(x) * bytesPerPixel, format, texel);

            const float red = ClampToUnit(texel[0]);
            const float green = ClampToUnit(texel[1]);
            const float blue = ClampToUnit(texel[2]);
            const float alpha = ClampToUnit(texel[3]);

            // Indexed by x, not a fixed [0..3]. Writing every texel of a row to the same four bytes leaves one visible
            // column and a black image whose raw readback looks perfectly correct — which is exactly how this first
            // showed up: the GPU target held a full scene while the PNG held column zero. Caught by writing the
            // converted buffer back through a decoder rather than trusting the call to return true.
            unsigned char* out = outRow + static_cast<size_t>(x) * 4u;

            // NaN sentinel from ClampToUnit: magenta. See the comment there for why not black.
            const bool isNanSource = red < 0.0f || green < 0.0f || blue < 0.0f;
            if (isNanSource)
            {
                out[0] = 255u;
                out[1] = 0u;
                out[2] = 255u;
                out[3] = 255u;
            }
            else
            {
                out[0] = ToByte(red);
                out[1] = ToByte(green);
                out[2] = ToByte(blue);
                // Alpha is carried through rather than forced to 255: a screenshot of a target with meaningful alpha
                // (an editor overlay, an authored RGBA texture) should keep it, and PNG supports both. For opaque
                // renders it is already 1.0.
                out[3] = ToByte(alpha);
            }
        }
    }

    return true;
}

bool WriteScreenshot(GraphicsDevice& device, Texture& texture, const std::string& path, std::string& outError)
{
    outError.clear();

    if (path.empty())
    {
        outError = "screenshot path is empty";
        return false;
    }

    // Checked before the readback rather than after stb fails: a capture that took a pipeline stall to produce and then
    // could not be written is worth reporting precisely, and "stbi_write_png returned 0" says nothing about why.
    if (!HasParentDirectory(path))
    {
        outError = "screenshot directory '" + ParentDirectory(path) + "' does not exist";
        return false;
    }

    const auto& desc = texture.GetDesc();

    // ReadBack blocks on the GPU and returns bytes with the row pitch they were stored at. A failure is reported by an
    // empty vector, which cannot be distinguished from a legitimately empty image except by the extent — hence checking
    // dimensions first so an empty result really does mean the copy failed.
    UInt32 rowPitchBytes = 0u;
    const std::vector<UInt8> pixels = texture.ReadBack(device, rowPitchBytes);
    if (pixels.empty())
    {
        outError = "could not read back '" + desc.Name + "' (staging allocation failed, or the device was lost)";
        return false;
    }

    std::vector<unsigned char> rgba8;
    if (!ConvertToRgba8(pixels.data(), pixels.size(), rowPitchBytes, desc.Format, desc.Width, desc.Height, rgba8,
                        outError))
    {
        // Prefix the texture's identity: a run that captures several targets gets one message naming the one that
        // broke.
        outError = "'" + desc.Name + "': " + outError;
        return false;
    }

    // stb writes top-down, matching the row order ConvertToRgba8 produced (see the header note on Y convention).
    const int stride = static_cast<int>(desc.Width) * 4;
    if (stbi_write_png(path.c_str(), static_cast<int>(desc.Width), static_cast<int>(desc.Height), 4, rgba8.data(),
                       stride) == 0)
    {
        outError = "could not write '" + path + "' (disk full, or the path is not a file)";
        return false;
    }

    return true;
}

}  // namespace Hollowpoint::Runtime
