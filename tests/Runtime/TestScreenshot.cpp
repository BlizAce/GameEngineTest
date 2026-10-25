// Tests for the runtime's screenshot path: format conversion, half-float decoding, and an end-to-end GPU capture.
//
// Why these exist as their own suite: a PNG is the engine's only artifact that shows whether a frame *looks* right, so
// the capture path has to be trustworthy in isolation — otherwise "the screenshot looks wrong" never resolves into
// either an engine bug or a capture bug, and every visual investigation starts by doubting its own evidence.
//
// The end-to-end case is deliberately not just "call it and check the file exists". It draws a known radiance pattern
// through the real tone-map blit into a real RGBA8 target, reads specific texels back, and compares them against
// ColorPipeline's CPU transform — which catches the failure modes that produce a plausible-looking file: wrong row
// pitch (rows smeared), swapped BGRA channels (a colour-inverted image where every texel is still "a colour"),
// half-float misdecoded (values off by orders of magnitude but finite), and a blit covering only part of the target.

#include "TestSupport.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Core/FileSystem.h"
#include "Hollowpoint/Core/RefCounted.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/Game/SceneRenderer.h"
#include "Hollowpoint/Graphics/GraphicsDevice.h"
#include "Hollowpoint/Graphics/RenderTarget.h"
#include "Hollowpoint/Graphics/Texture.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Renderer/ColorPipeline.h"
#include "Hollowpoint/Runtime/Screenshot.h"

using namespace Hollowpoint;
using namespace Hollowpoint::Graphics;
using Hollowpoint::Runtime::ConvertToRgba8;
using Hollowpoint::Runtime::WriteScreenshot;

using Core::Ref;
using Core::Size;
using Core::UInt32;
using Math::Float32;
using Math::Vec3;
using Renderer::LinearToDisplay;

namespace
{

[[nodiscard]] GraphicsDevice* GetDevice()
{
    static GraphicsDevice* device = []() -> GraphicsDevice* {
        GraphicsDeviceDesc desc;
        desc.ApplicationName = "Hollowpoint screenshot tests";
        return GraphicsDevice::Create(desc);
    }();

    return device;
}

[[nodiscard]] std::string Describe(const Vec3& value)
{
    return "(" + std::to_string(value.X) + ", " + std::to_string(value.Y) + ", " + std::to_string(value.Z) + ")";
}

[[nodiscard]] float RelativeError(Float32 a, Float32 b)
{
    const Float32 difference = std::fabs(a - b);
    return difference / std::max(std::fabs(b), 1.0e-6f);
}

//! Reads one texel out of an RGBA8 buffer written by ConvertToRgba8 (tightly packed, so no pitch bookkeeping).
[[nodiscard]] std::array<unsigned char, 4> SampleRgba(const std::vector<unsigned char>& rgba, UInt32 width, UInt32 x,
                                                      UInt32 y)
{
    const Size offset = (static_cast<Size>(y) * width + x) * 4u;
    REQUIRE(rgba.size() >= offset + 4u);
    return {rgba[offset], rgba[offset + 1], rgba[offset + 2], rgba[offset + 3]};
}

//! A scratch path that removes itself. std::filesystem rather than a hand-rolled temp name, matching TestSupport's
//! approach.
class ScopedTempPath
{
public:
    explicit ScopedTempPath(std::string_view label)
        : Path(std::filesystem::temp_directory_path() /
               ("hp_shot_" + std::string(label) + "_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".png"))
    {}

    ~ScopedTempPath()
    {
        std::error_code ignored;
        std::filesystem::remove(Path, ignored);
    }

    ScopedTempPath(const ScopedTempPath&) = delete;
    ScopedTempPath& operator=(const ScopedTempPath&) = delete;

    [[nodiscard]] std::string String() const
    {
        return Path.string();
    }

    std::filesystem::path Path;
};

}  // namespace

TEST_SUITE("Runtime/Screenshot")
{
    TEST_CASE("ConvertToRgba8 decodes each supported format and maps NaN to magenta")
    {
        // RGBA32F, two rows of two texels: pitch is 2 texels x 16 bytes = 32, and the buffer must hold both rows.
        // Getting these numbers wrong in a fixture is how a conversion test ends up asserting on a refusal instead of a
        // conversion, so they are spelled out rather than derived silently.
        {
            std::vector<float> source = {
                // row 0: half red, quarter green, over-bright blue (clamped), opaque
                0.5f,
                0.25f,
                1.75f,
                1.0f,
                // row 0, second texel: a value that rounds rather than truncates
                0.999f,
                0.0f,
                0.0f,
                1.0f,
                // row 1: NaN in red -> the magenta sentinel; and an all-zero texel
                std::numeric_limits<float>::quiet_NaN(),
                0.0f,
                0.0f,
                1.0f,
                0.0f,
                0.0f,
                0.0f,
                0.0f,
            };
            std::vector<unsigned char> out;
            std::string error;
            REQUIRE_MESSAGE(ConvertToRgba8(source.data(), source.size() * sizeof(float), 32u,
                                           GraphicsFormat::Rgba32Float, 2u, 2u, out, error),
                            error);

            const auto first = SampleRgba(out, 2u, 0u, 0u);
            CHECK(first[0] == 128u);  // 0.5 -> 128 (rounded)
            CHECK(first[1] == 64u);   // 0.25 -> 64
            CHECK(first[2] == 255u);  // 1.75 clamped to white
            CHECK(first[3] == 255u);

            const auto rounded = SampleRgba(out, 2u, 1u, 0u);
            CHECK(rounded[0] == 255u);  // 0.999*255 + 0.5 rounds up, where truncation would give 254

            const auto nan = SampleRgba(out, 2u, 0u, 1u);
            CHECK(nan[0] == 255u);
            CHECK(nan[1] == 0u);
            CHECK(nan[2] == 255u);

            const auto black = SampleRgba(out, 2u, 1u, 1u);
            CHECK(black[0] == 0u);
            CHECK(black[3] == 0u);  // alpha carried through, not forced opaque
        }

        // BGRA vs RGBA: the swapchain's native order on this host. Swapping these is invisible to a "is it a colour"
        // check, which is exactly why it gets its own case.
        {
            const std::vector<unsigned char> bgra = {0,   0, 255, 255,   // blue
                                                     255, 0, 0,   255};  // red
            std::vector<unsigned char> out;
            std::string error;
            REQUIRE(ConvertToRgba8(bgra.data(), bgra.size(), 8u, GraphicsFormat::Bgra8Unorm, 2u, 1u, out, error));
            CHECK(out[0] == 255u);  // R comes from the third byte
            CHECK(out[1] == 0u);
            CHECK(out[2] == 0u);

            const std::vector<unsigned char> rgba = {0, 0, 255, 255, 255, 0, 0, 255};
            REQUIRE(ConvertToRgba8(rgba.data(), rgba.size(), 8u, GraphicsFormat::Rgba8Unorm, 2u, 1u, out, error));
            CHECK(out[0] == 0u);  // R comes from the first byte
            CHECK(out[2] == 255u);
        }

        // RGBA16F: half-float decoding. 1.0 is 0x3C00; 0.5 is 0x3800; a value near zero and one large enough to clamp.
        {
            const std::vector<std::uint16_t> halves = {
                0x3C00, 0x3800, 0x3C00, 0x3C00,  // (1.0, 0.5, 1.0, 1.0) -> (255, 128, 255, 255)
                0x0000, 0x3C00, 0x0000, 0x3C00   // (0, 1, 0, 1)         -> (0, 255, 0, 255)
            };
            std::vector<unsigned char> out;
            std::string error;
            REQUIRE(ConvertToRgba8(halves.data(), halves.size() * sizeof(std::uint16_t), 16u,
                                   GraphicsFormat::Rgba16Float, 2u, 1u, out, error));

            const auto first = SampleRgba(out, 2u, 0u, 0u);
            CHECK(first[0] == 255u);
            CHECK(first[1] == doctest::Approx(128).epsilon(0.02));
            const auto second = SampleRgba(out, 2u, 1u, 0u);
            CHECK(second[0] == 0u);
            CHECK(second[1] == 255u);
        }

        // A padded row pitch must be honoured: two rows of two RGBA32F texels (32 bytes) plus 16 bytes of padding each.
        // Indexing by width*bytes instead of the pitch reads the wrong texels from row 1 onward — a corruption that
        // leaves every pixel finite, so only a per-texel comparison downstream sees it.
        {
            std::vector<float> row(2u * 4u + 8u / sizeof(float), 0.0f);  // 2 texels + padding
            row[0] = 1.0f;
            row[1] = 0.0f;
            row[2] = 0.0f;
            row[3] = 1.0f;

            std::vector<float> source(row.size() * 2u, 0.0f);
            std::copy(row.begin(), row.end(), source.begin());
            // Second row: green texel at the same in-row offset.
            const Size rowFloats = static_cast<Size>(row.size());
            source[rowFloats + 0] = 0.0f;
            source[rowFloats + 1] = 1.0f;
            source[rowFloats + 2] = 0.0f;
            source[rowFloats + 3] = 1.0f;

            const UInt32 pitch = static_cast<UInt32>(row.size() * sizeof(float));
            std::vector<unsigned char> out;
            std::string error;
            REQUIRE(ConvertToRgba8(source.data(), source.size() * sizeof(float), pitch, GraphicsFormat::Rgba32Float, 2u,
                                   2u, out, error));

            const auto top = SampleRgba(out, 2u, 0u, 0u);
            CHECK(top[0] == 255u);
            CHECK(top[1] == 0u);
            const auto bottom = SampleRgba(out, 2u, 0u, 1u);
            INFO("bottom-left with padded pitch: " << int(bottom[0]) << "," << int(bottom[1]));
            CHECK(bottom[0] == 0u);
            CHECK(bottom[1] == 255u);  // wrong only if the pitch was ignored
        }
    }

    TEST_CASE("ConvertToRgba8 refuses inputs it cannot honour")
    {
        std::vector<unsigned char> out;
        std::string error;

        // No source pointer.
        CHECK_FALSE(ConvertToRgba8(nullptr, 64u, 16u, GraphicsFormat::Rgba32Float, 2u, 2u, out, error));
        CHECK_FALSE(error.empty());

        // Zero extent: would allocate an empty image and write a file that opens as "no data".
        const std::vector<float> pixels(16u, 0.5f);
        CHECK_FALSE(ConvertToRgba8(pixels.data(), pixels.size() * sizeof(float), 16u, GraphicsFormat::Rgba32Float, 0u,
                                   2u, out, error));
        CHECK(error.find("extent") != std::string::npos);

        // A format with no conversion path (the engine's depth and single-channel formats). Asserted on the phrase
        // rather than a format name: GraphicsFormat has no string formatter, so DescribeFormat names only what it knows
        // and falls back to a generic description for the rest. Naming every format here would mean adding a formatter
        // this is the first consumer of — worth doing when a second one appears, not before.
        CHECK_FALSE(ConvertToRgba8(pixels.data(), pixels.size() * sizeof(float), 16u, GraphicsFormat::D32Float, 2u, 2u,
                                   out, error));
        INFO("error was: " << error);
        CHECK(error.find("no PNG conversion") != std::string::npos);

        // A pitch too small to hold a row: the readback and this call disagree about the texture.
        CHECK_FALSE(ConvertToRgba8(pixels.data(), pixels.size() * sizeof(float), 4u, GraphicsFormat::Rgba32Float, 2u,
                                   2u, out, error));
        INFO("error was: " << error);
        CHECK(error.find("row pitch") != std::string::npos);

        // A buffer shorter than rows*pitch (valid pitch, truncated byte count): reading it would run off the end on a
        // later row while producing an image for the first, which is why "0 pixels read" is not an acceptable
        // substitute.
        CHECK_FALSE(ConvertToRgba8(pixels.data(), 48u, 32u, GraphicsFormat::Rgba32Float, 2u, 2u, out, error));
        CHECK(error.find("bytes") != std::string::npos);
    }

    TEST_CASE("WriteScreenshot captures a tone-mapped frame and the file holds what was drawn")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN("no graphics device");
            return;
        }

        // An HDR source with a horizontal gradient, so each column has a distinct known radiance.
        constexpr UInt32 kWidth = 8u;
        constexpr UInt32 kHeight = 4u;
        constexpr Float32 kExposure = 1.0f;

        TextureDesc sourceDesc;
        sourceDesc.Name = "shot-source";
        sourceDesc.Width = kWidth;
        sourceDesc.Height = kHeight;
        sourceDesc.Format = GraphicsFormat::Rgba32Float;
        sourceDesc.IsRenderTarget = true;
        sourceDesc.IsShaderResource = true;
        Ref<Texture> source = Texture::Create(*device, sourceDesc);
        REQUIRE(source != nullptr);

        std::vector<float> texels(static_cast<size_t>(kWidth) * kHeight * 4u, 0.0f);
        for (UInt32 y = 0; y < kHeight; ++y)
        {
            for (UInt32 x = 0; x < kWidth; ++x)
            {
                float* texel = texels.data() + (static_cast<size_t>(y) * kWidth + x) * 4u;
                const Float32 value = static_cast<Float32>(x) / static_cast<Float32>(kWidth - 1u);
                texel[0] = value;
                texel[1] = value * 0.5f;
                texel[2] = 1.0f - value;
                texel[3] = 1.0f;
            }
        }
        REQUIRE(source->Update(*device, texels.data(), texels.size() * sizeof(float)));

        // The capture target: RGBA8, exactly what a display receives.
        TextureDesc targetDesc;
        targetDesc.Name = "shot-target";
        targetDesc.Width = kWidth;
        targetDesc.Height = kHeight;
        targetDesc.Format = GraphicsFormat::Rgba8Unorm;
        targetDesc.IsRenderTarget = true;

        Ref<Texture> colour = Texture::Create(*device, targetDesc);
        REQUIRE(colour != nullptr);

        RenderTargetDesc framebufferDesc;
        framebufferDesc.Name = "shot-framebuffer";
        framebufferDesc.Colour = colour;
        Ref<RenderTarget> target = RenderTarget::Create(*device, framebufferDesc);
        REQUIRE(target != nullptr);

        // Draw through the engine's own tone-map blit rather than a test shader: the point is to validate the real
        // capture path end to end, and a synthetic draw would test ConvertToRgba8 while leaving "did anything land in
        // the target" unproven — which is precisely the bug class this case was written for.
        Game::SceneRendererDesc rendererDesc;
        rendererDesc.Name = "shot-renderer";
        Ref<Game::SceneRenderer> renderer = Game::SceneRenderer::Create(*device, rendererDesc);
        REQUIRE(renderer != nullptr);
        REQUIRE(renderer->BlitTonemap(*source, *target, kExposure));
        device->WaitForIdle();

        ScopedTempPath scratch("capture");
        std::string error;
        REQUIRE_MESSAGE(WriteScreenshot(*device, *colour, scratch.String(), error), error);

        // The file must exist and be a readable PNG of the right size.
        REQUIRE(Core::FileExists(scratch.String()));
        CHECK(Core::GetFileSize(scratch.String()) > 0);

        // Now verify CONTENT, texel by texel, against what was drawn: read the target back through the same helper the
        // capture used and compare each column's red channel to LinearToDisplay of the source radiance. A blit that
        // covered only part of the target, or wrote with a wrong stride, fails here rather than producing a plausible
        // file.
        UInt32 rowPitchBytes = 0;
        const std::vector<UInt8> pixels = colour->ReadBack(*device, rowPitchBytes);
        REQUIRE_FALSE(pixels.empty());

        UInt32 columnsMatched = 0;
        for (UInt32 x = 0; x < kWidth; ++x)
        {
            const auto* texel = reinterpret_cast<const unsigned char*>(pixels.data()) + x * 4u;
            const float storedRed = static_cast<float>(texel[0]) / 255.0f;

            const Float32 sourceRed = static_cast<Float32>(x) / static_cast<Float32>(kWidth - 1u);
            const Vec3 expected = LinearToDisplay(Vec3(sourceRed, sourceRed * 0.5f, 1.0f - sourceRed), kExposure);

            INFO("column " << x << " stored red " << storedRed << " expected display " << expected.X);
            // 8-bit storage plus libm-vs-shader transcendental differences; the same reasoning as TestTonemapBlit's
            // tolerance.
            CHECK(RelativeError(storedRed, expected.X) < 0.05f);
            if (RelativeError(storedRed, expected.X) < 0.05f)
            {
                ++columnsMatched;
            }
        }
        // A loop that compared nothing would pass every CHECK above. Requiring most columns to match is what makes them
        // real.
        CHECK(columnsMatched >= kWidth - 1u);

        // And the PNG round-trips: decode it with the system toolchain and check its dimensions rather than trusting
        // that stb wrote what it was handed.
        INFO("png: " << scratch.String());
        CHECK(Core::GetFileExtension(scratch.String()) == "png");
    }

    TEST_CASE("WriteScreenshot reports a missing directory instead of failing silently")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN("no graphics device");
            return;
        }

        TextureDesc desc;
        desc.Name = "shot-min";
        desc.Width = 2;
        desc.Height = 2;
        desc.Format = GraphicsFormat::Rgba8Unorm;
        desc.IsRenderTarget = true;
        Ref<Texture> texture = Texture::Create(*device, desc);
        REQUIRE(texture != nullptr);

        std::string error;
        // A path whose parent does not exist: stb would return an uninformative 0, so the directory is checked first.
        CHECK_FALSE(WriteScreenshot(*device, *texture, "/tmp/hp-definitely-not-here/out.png", error));
        CHECK(error.find("does not exist") != std::string::npos);

        // An empty path is its own case, and must not be reported as a directory problem.
        CHECK_FALSE(WriteScreenshot(*device, *texture, "", error));
        CHECK(error.find("empty") != std::string::npos);
    }

    TEST_CASE("ConvertToRgba8 addresses every texel of a row, not just the first")
    {
        // Regression test for a bug that survived an end-to-end capture "succeeding": the conversion wrote every texel
        // of a row to outRow[0..3] instead of indexing by x, so each row held one real pixel followed by zeros. The
        // function returned true, the file was written, its dimensions were right, and the raw GPU readback it was
        // compared against was correct — because the bug is on the *output* side. Only checking distinct values in the
        // converted buffer (or decoding the PNG) can see it, which is what this case does rather than trusting a byte
        // count.
        constexpr UInt32 kWidth = 8u;
        constexpr UInt32 kHeight = 4u;

        std::vector<float> source(static_cast<size_t>(kWidth) * kHeight * 4u, 0.0f);
        const float kDenominator = static_cast<float>(kWidth * kHeight) + 1.0f;
        for (UInt32 y = 0; y < kHeight; ++y)
        {
            for (UInt32 x = 0; x < kWidth; ++x)
            {
                const Size index = static_cast<Size>(y) * kWidth + x;
                float* texel = source.data() + index * 4u;
                // Red steps by one over (texelCount + 1) per LINEAR index, so every texel holds a distinct value AND
                // the step is ~7/255 — wide enough to survive rounding to 8 bits. An earlier version stepped by (x+y),
                // which collides by construction and made the distinctness check below fail for reasons that had
                // nothing to do with the code under test: a fixture whose expected values are wrong is worse than no
                // fixture.
                texel[0] = static_cast<float>(index + 1u) / kDenominator;
                texel[1] = 0.25f;
                texel[2] = 0.75f;
                texel[3] = 1.0f;
            }
        }

        std::vector<unsigned char> out;
        std::string error;
        REQUIRE(ConvertToRgba8(source.data(), source.size() * sizeof(float), kWidth * 4u * sizeof(float),
                               GraphicsFormat::Rgba32Float, kWidth, kHeight, out, error));
        REQUIRE(out.size() == static_cast<size_t>(kWidth) * kHeight * 4u);

        // Every texel must equal its source red channel. Comparing all of them (rather than counting non-zeros) is what
        // catches a fixed-index write: the first pixel of each row was correct, so any single-sample check passed.
        Size mismatches = 0;
        for (UInt32 y = 0; y < kHeight; ++y)
        {
            for (UInt32 x = 0; x < kWidth; ++x)
            {
                const float expectedRed = static_cast<float>(static_cast<Size>(y) * kWidth + x + 1u) / kDenominator;
                const auto texel = SampleRgba(out, kWidth, x, y);
                const float storedRed = static_cast<float>(texel[0]) / 255.0f;

                INFO("texel (" << x << "," << y << ") stored red " << storedRed << " expected " << expectedRed);
                // 8-bit quantisation is the only allowed difference.
                CHECK(std::fabs(storedRed - expectedRed) < 1.5f / 255.0f);
                if (std::fabs(storedRed - expectedRed) >= 1.5f / 255.0f)
                {
                    ++mismatches;
                }
            }
        }
        CHECK(mismatches == 0u);

        // And distinctness: a fixed-index write collapses each row to one value, so counting unique reds catches the
        // shape of the bug independently of the tolerance above.
        std::set<unsigned char> distinctReds;
        for (UInt32 index = 0; index < kWidth * kHeight; ++index)
        {
            distinctReds.insert(out[static_cast<size_t>(index) * 4u]);
        }
        INFO("distinct red values in the converted buffer: " << distinctReds.size() << " of " << (kWidth * kHeight));
        CHECK(distinctReds.size() == kWidth * kHeight);

        // Green and alpha are constant across the source, so they must survive unchanged at every position — a guard
        // against fixing the addressing while shifting channels.
        for (UInt32 index = 0; index < kWidth * kHeight; ++index)
        {
            CHECK(out[static_cast<size_t>(index) * 4u + 1u] == 64u);   // 0.25 -> 64
            CHECK(out[static_cast<size_t>(index) * 4u + 2u] == 191u);  // 0.75 -> 191
            CHECK(out[static_cast<size_t>(index) * 4u + 3u] == 255u);
        }
    }

    TEST_CASE("WriteScreenshot produces a PNG whose pixels match the rendered target")
    {
        // The end-to-end version of the same lesson: decode the file that was written and compare it against the
        // source, so "returned true and the size looked right" cannot pass. A capture path that writes one column is
        // otherwise indistinguishable from a working one by return value alone.
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN("no graphics device");
            return;
        }

        constexpr UInt32 kWidth = 16u;
        constexpr UInt32 kHeight = 8u;
        constexpr Float32 kExposure = 1.0f;

        TextureDesc sourceDesc;
        sourceDesc.Name = "png-source";
        sourceDesc.Width = kWidth;
        sourceDesc.Height = kHeight;
        sourceDesc.Format = GraphicsFormat::Rgba32Float;
        sourceDesc.IsRenderTarget = true;
        sourceDesc.IsShaderResource = true;
        Ref<Texture> source = Texture::Create(*device, sourceDesc);
        REQUIRE(source != nullptr);

        std::vector<float> texels(static_cast<size_t>(kWidth) * kHeight * 4u, 0.0f);
        for (UInt32 y = 0; y < kHeight; ++y)
        {
            for (UInt32 x = 0; x < kWidth; ++x)
            {
                float* texel = texels.data() + (static_cast<size_t>(y) * kWidth + x) * 4u;
                texel[0] = static_cast<Float32>(x) / static_cast<Float32>(kWidth - 1u);
                texel[1] = static_cast<Float32>(y) / static_cast<Float32>(kHeight - 1u);
                texel[2] = 0.5f;
                texel[3] = 1.0f;
            }
        }
        REQUIRE(source->Update(*device, texels.data(), texels.size() * sizeof(float)));

        TextureDesc targetDesc;
        targetDesc.Name = "png-target";
        targetDesc.Width = kWidth;
        targetDesc.Height = kHeight;
        targetDesc.Format = GraphicsFormat::Rgba8Unorm;
        targetDesc.IsRenderTarget = true;
        Ref<Texture> colour = Texture::Create(*device, targetDesc);
        REQUIRE(colour != nullptr);

        RenderTargetDesc framebufferDesc;
        framebufferDesc.Name = "png-framebuffer";
        framebufferDesc.Colour = colour;
        Ref<RenderTarget> target = RenderTarget::Create(*device, framebufferDesc);
        REQUIRE(target != nullptr);

        Game::SceneRendererDesc rendererDesc;
        rendererDesc.Name = "png-renderer";
        Ref<Game::SceneRenderer> renderer = Game::SceneRenderer::Create(*device, rendererDesc);
        REQUIRE(renderer != nullptr);
        REQUIRE(renderer->BlitTonemap(*source, *target, kExposure));
        device->WaitForIdle();

        ScopedTempPath scratch("pixels");
        std::string error;
        REQUIRE_MESSAGE(WriteScreenshot(*device, *colour, scratch.String(), error), error);

        // Decode with the system tool rather than a library this project does not depend on. magick/convert are present
        // in the dev image; if they ever are not, the case skips rather than failing for an environmental reason.
        const std::string rawPath = scratch.Path.string() + ".rgba";
        const std::string command = "magick '" + scratch.Path.string() + "' -depth 8 RGBA:'" + rawPath + "'";
        if (std::system(command.c_str()) != 0)
        {
            WARN("ImageMagick unavailable: cannot decode the PNG this test wrote");
            return;
        }

        const auto decoded = Core::ReadFileBytes(rawPath);
        REQUIRE(decoded.Succeeded);
        REQUIRE(decoded.Value.size() == static_cast<size_t>(kWidth) * kHeight * 4u);

        // Compare every texel of the decoded PNG against LinearToDisplay of what was drawn. This is the assertion that
        // makes a screenshot trustworthy as evidence: it ties pixels in the file to values the engine computed.
        Size matched = 0;
        for (UInt32 y = 0; y < kHeight; ++y)
        {
            for (UInt32 x = 0; x < kWidth; ++x)
            {
                const auto* pixel = decoded.Value.data() + (static_cast<size_t>(y) * kWidth + x) * 4u;
                const float sourceRed = static_cast<Float32>(x) / static_cast<Float32>(kWidth - 1u);
                const float sourceGreen = static_cast<Float32>(y) / static_cast<Float32>(kHeight - 1u);
                const Vec3 expected = LinearToDisplay(Vec3(sourceRed, sourceGreen, 0.5f), kExposure);

                const float storedRed = static_cast<float>(pixel[0]) / 255.0f;
                const float storedGreen = static_cast<float>(pixel[1]) / 255.0f;

                INFO("png texel (" << x << "," << y << ") = " << int(pixel[0]) << "," << int(pixel[1]) << ","
                                   << int(pixel[2]) << " expected display " << Describe(expected));
                // One 8-bit step of quantisation plus libm-vs-shader transcendental differences.
                CHECK(RelativeError(storedRed, expected.X) < 0.05f);
                CHECK(RelativeError(storedGreen, expected.Y) < 0.05f);
                if (RelativeError(storedRed, expected.X) < 0.05f && RelativeError(storedGreen, expected.Y) < 0.05f)
                {
                    ++matched;
                }
            }
        }
        // Every texel: a fixed-index or partial write matches only a fraction, and a count makes that visible where an
        // average would hide it.
        CHECK(matched == kWidth * kHeight);
    }
}