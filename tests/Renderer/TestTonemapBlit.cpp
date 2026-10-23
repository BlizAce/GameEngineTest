// Tests for the tonemap blit: its CPU reference, its generated shader, and — on a GPU host — the rendered result
// compared against ColorPipeline::LinearToDisplay pixel by pixel.
//
// The point of the whole file is that three things must agree: ColorPipeline's C++ display transform, the shared GLSL
// generator's HpLinearToDisplay, and this pass which calls it. Two of those were already tested independently; this is
// where the third gets pinned to them on real hardware.

#include "TestSupport.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Graphics/Buffer.h"
#include "Hollowpoint/Graphics/GraphicsDevice.h"
#include "Hollowpoint/Graphics/Pipeline.h"
#include "Hollowpoint/Graphics/RenderTarget.h"
#include "Hollowpoint/Graphics/RhiAccess.h"
#include "Hollowpoint/Graphics/ShaderBindings.h"
#include "Hollowpoint/Graphics/ShaderCompiler.h"
#include "Hollowpoint/Graphics/Texture.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Renderer/ColorPipeline.h"
#include "Hollowpoint/Renderer/TonemapBlit.h"

using namespace Hollowpoint;
using namespace Hollowpoint::Graphics;
using namespace Hollowpoint::Renderer;
using Math::Float32;
using Math::Vec3;

namespace
{

[[nodiscard]] bool Approximately(const Vec3& a, const Vec3& b, Float32 tolerance)
{
    return std::abs(a.X - b.X) <= tolerance && std::abs(a.Y - b.Y) <= tolerance && std::abs(a.Z - b.Z) <= tolerance;
}

[[nodiscard]] std::string Describe(const Vec3& v)
{
    return "(" + std::to_string(v.X) + ", " + std::to_string(v.Y) + ", " + std::to_string(v.Z) + ")";
}

[[nodiscard]] GraphicsDevice* GetDevice()
{
    static GraphicsDevice* device = []() -> GraphicsDevice* {
        GraphicsDeviceDesc desc;
        desc.ApplicationName = "Hollowpoint tonemap blit tests";
        return GraphicsDevice::Create(desc);
    }();

    return device;
}

}  // namespace

TEST_SUITE("Renderer/TonemapBlit")
{
    TEST_CASE("the CPU reference is exactly ColorPipeline's display transform")
    {
        // The blit must not introduce its own order or clamping: exposure -> ACES -> sRGB, nothing else. Comparing
        // against LinearToDisplay directly means a change to either shows up here rather than as an image nobody can
        // explain.
        const std::vector<Float32> exposures = {0.0f, 0.25f, 1.0f, 4.0f};
        const std::vector<Vec3> colors = {
            Vec3(0.0f),
            Vec3(0.001f),
            Vec3(0.18f),
            Vec3(1.0f),
            Vec3(4.0f, 2.0f, 0.5f),
            Vec3(1000.0f, 800.0f, 600.0f),  // sun-bright: tone mapping must compress, not clip channel-wise
        };

        for (const Float32 exposure : exposures)
        {
            for (const Vec3& color : colors)
            {
                INFO("exposure " << exposure << " colour " << Describe(color));
                const Vec3 expected = LinearToDisplay(color, exposure);
                const Vec3 actual = ComputeDisplayColourCpu(color, exposure);
                CHECK(actual.X == doctest::Approx(expected.X).epsilon(1e-6).scale(1.0f));
                CHECK(actual.Y == doctest::Approx(expected.Y).epsilon(1e-6).scale(1.0f));
                CHECK(actual.Z == doctest::Approx(expected.Z).epsilon(1e-6).scale(1.0f));
            }
        }
    }

    TEST_CASE("display output stays in range and keeps hue relationships monotonic")
    {
        // Not a re-test of tone mapping (ColorPipelineTests owns that) — these are properties the BLIT depends on:
        // output must be display-representable, because it is written to an 8-bit-capable target with no second clamp.
        for (Float32 exposure = 0.0f; exposure <= 8.0f; exposure += 0.5f)
        {
            INFO("exposure " << exposure);
            const Vec3 result = ComputeDisplayColourCpu(Vec3(1000.0f, 500.0f, 250.0f), exposure);

            CHECK(result.X >= 0.0f);
            CHECK(result.X <= 1.0f);
            CHECK(result.Y >= 0.0f);
            CHECK(result.Z >= 0.0f);

            // Brighter exposure must never darken any channel: a non-monotonic tone curve would produce banding and
            // flicker under auto-exposure, which is exactly what a blit pass gets blamed for later.
            const Vec3 brighter = ComputeDisplayColourCpu(Vec3(1000.0f, 500.0f, 250.0f), exposure + 0.25f);
            CHECK(brighter.X >= result.X - 1e-6f);
            CHECK(brighter.Y >= result.Y - 1e-6f);
            CHECK(brighter.Z >= result.Z - 1e-6f);
        }
    }

    TEST_CASE("the generated blit shaders compile and contain no colour maths of their own")
    {
        const std::string header = "#version 450\n" + BuildGlslBindingPrelude();

        const CompiledShader vertex = CompileGlsl(ShaderStage::Vertex, header + BuildTonemapBlitVertexGlsl());
        INFO(vertex.Diagnostics);
        CHECK(vertex.Succeeded);

        const CompiledShader fragment = CompileGlsl(ShaderStage::Pixel, header + BuildTonemapBlitFragmentGlsl());
        INFO(fragment.Diagnostics);
        CHECK(fragment.Succeeded);

        // The blit must delegate every colour operation. Hand-typed exposure/ACES/sRGB here would drift from the CPU
        // reference and produce an image that looks plausible on one platform and wrong on another.
        const std::string source = BuildTonemapBlitFragmentGlsl();
        const std::size_t mainStart = source.find("void main()");
        REQUIRE(mainStart != std::string::npos);
        const std::string body = source.substr(mainStart);

        CHECK(body.find("HpLinearToDisplay") != std::string::npos);

        // No ACES polynomial coefficients typed into the blit's own main(): those live in the shared generator only.
        CHECK(body.find("0.15005") == std::string::npos);
        CHECK(body.find("0.59719") == std::string::npos);

        // Both resources bound through the macros, never a raw number (a raw binding reads as zero/black silently —
        // session 12's most expensive failure mode). Checked against the whole source because the texture binding is
        // emitted before main().
        CHECK(source.find("HP_BINDING_TEXTURE") != std::string::npos);
        CHECK(source.find("HP_BINDING_SAMPLER") != std::string::npos);
        CHECK(source.find("HP_BINDING_CONSTANT_BUFFER") != std::string::npos);
    }

    TEST_CASE("the blit renders a scene texture matching the CPU display transform (GPU)")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN((std::string("No graphics device on this host: ") + GraphicsDevice::GetFailureReason()).c_str());
            return;
        }

        nvrhi::IDevice* rhi = device->GetNativeDevice();
        REQUIRE(rhi != nullptr);

        constexpr UInt32 kSize = 16;
        constexpr Float32 kExposure = 0.75f;

        // --- Source texture: a known HDR ramp -------------------------------------------------------------------
        // Each texel's radiance is a function of its coordinates, so expected values are computed per pixel rather
        // than stored and the mapping cannot silently shift.
        //
        // Integers and simple fractions throughout. That matters less than it usually would — this texture is
        // RGBA32_FLOAT and Update/ReadBack copy bytes verbatim, so nothing here rounds — but it keeps the expected
        // values hand-checkable, which is what actually paid off: the first failure of this test read as "gpu (0, 1,
        // 0.0768) vs cpu (0, 0, 0.0768)", and recognising 1.0 as exactly LinearToDisplay(20 * 0.75) — the value of
        // sourceAt(0, 15), i.e. the LAST row — is what identified a vertical flip in one step rather than a dozen.
        //
        // Asymmetry in y is the property being relied on: a gradient that reads the same top-to-bottom would pass with
        // flipped UVs, which is precisely how the bug got through review.
        std::vector<float> sourceTexels(static_cast<size_t>(kSize) * kSize * 4u);
        auto sourceAt = [&](UInt32 x, UInt32 y) -> Vec3 {
            // Red spans zero to sun-bright in coarse steps so the tone curve is exercised across its whole range
            // (threshold, mid, and saturation all in one row); green and blue stay small and vary per texel.
            //
            // Note red exceeds 2048 for x >= 9, which is the largest consecutive integer binary16 can hold — so these
            // values are exact in float32 but NOT round-trip-stable if this test's format were ever switched to a
            // half-float one. That is acceptable because the point of RGBA32_FLOAT here is that no storage rounding
            // exists at all, letting the comparison use a tight tolerance; switching formats would mean re-deriving the
            // ramp (and loosening it), which this comment exists to say out loud.
            const float red = static_cast<float>(x * 255u);       // 0, 255, ... 3825
            const float green = static_cast<float>(y) / 4.0f;     // 0 .. 3.75, exact quarters in any format
            const float blue = static_cast<float>((x + y) % 8u);  // 0..7 integers
            return Vec3(red, green, blue);
        };

        for (UInt32 y = 0; y < kSize; ++y)
        {
            for (UInt32 x = 0; x < kSize; ++x)
            {
                const Vec3 colour = sourceAt(x, y);
                float* texel = sourceTexels.data() + (static_cast<size_t>(y) * kSize + x) * 4u;
                texel[0] = colour.X;
                texel[1] = colour.Y;
                texel[2] = colour.Z;
                texel[3] = 1.0f;
            }
        }

        TextureDesc sourceDesc;
        sourceDesc.Name = "blit source";
        sourceDesc.Width = kSize;
        sourceDesc.Height = kSize;
        sourceDesc.Format = GraphicsFormat::Rgba32Float;
        sourceDesc.IsShaderResource = true;
        Core::Ref<Texture> sourceTexture = Texture::Create(*device, sourceDesc);
        REQUIRE(sourceTexture != nullptr);

        // Upload the ramp. Skipping this is a mistake worth naming: without it the source holds undefined memory and
        // the blit faithfully produces the display transform *of garbage* — every downstream number is then plausible
        // and only the comparison catches it. Update() returns false on a short buffer for exactly that reason.
        REQUIRE(sourceTexture->Update(*device, sourceTexels.data(), sourceTexels.size() * sizeof(float)));

        // --- Destination --------------------------------------------------------------------------------------------
        TextureDesc targetDesc;
        targetDesc.Name = "blit target";
        targetDesc.Width = kSize;
        targetDesc.Height = kSize;
        targetDesc.Format = GraphicsFormat::Rgba32Float;
        Core::Ref<Texture> targetTexture = Texture::Create(*device, targetDesc);
        REQUIRE(targetTexture != nullptr);

        RenderTargetDesc renderTargetDesc;
        renderTargetDesc.Name = "blit rt";
        renderTargetDesc.Colour = targetTexture;
        Core::Ref<RenderTarget> renderTarget = RenderTarget::Create(*device, renderTargetDesc);
        REQUIRE(renderTarget != nullptr);

        // --- Uniforms -----------------------------------------------------------------------------------------------
        struct alignas(16) ExposureBlock
        {
            float Exposure;
        };
        const ExposureBlock exposureBlock{kExposure};

        BufferDesc exposureBufferDesc;
        exposureBufferDesc.Name = "blit exposure";
        exposureBufferDesc.ByteSize = sizeof(ExposureBlock);
        exposureBufferDesc.Usage = BufferUsage::Constant;
        Core::Ref<Buffer> exposureBuffer = Buffer::Create(*device, exposureBufferDesc);
        REQUIRE(exposureBuffer != nullptr);

        // --- Shaders -----------------------------------------------------------------------------------------------
        const std::string header = "#version 450\n" + BuildGlslBindingPrelude();
        const CompiledShader vertex = CompileGlsl(ShaderStage::Vertex, header + BuildTonemapBlitVertexGlsl());
        REQUIRE_MESSAGE(vertex.Succeeded, vertex.Diagnostics);
        const CompiledShader fragment = CompileGlsl(ShaderStage::Pixel, header + BuildTonemapBlitFragmentGlsl());
        REQUIRE_MESSAGE(fragment.Succeeded, fragment.Diagnostics);

        // --- Pipeline ----------------------------------------------------------------------------------------------
        PipelineDesc pipelineDesc;
        pipelineDesc.Name = "tonemap blit";
        pipelineDesc.VertexShader.SpirV = vertex.SpirV;
        pipelineDesc.PixelShader.SpirV = fragment.SpirV;
        // No vertex attributes: the triangle is synthesised from gl_VertexID. Declaring none keeps createInputLayout
        // from expecting buffers that do not exist.
        pipelineDesc.Bindings = {
            BindingSlotDesc{BindingSlotDesc::Kind::ConstantBuffer, TonemapBlitBindings::ExposureBuffer},
            BindingSlotDesc{BindingSlotDesc::Kind::SampledTexture, TonemapBlitBindings::SourceTexture},
            BindingSlotDesc{BindingSlotDesc::Kind::Sampler, TonemapBlitBindings::SourceSampler},
        };
        pipelineDesc.BindingsVisibleToPixel = true;
        // No depth for a full-screen copy: no attachment exists, and enabling the test against an undefined buffer is
        // the flakiness trap documented in session 14.
        pipelineDesc.IsDepthEnabled = false;
        pipelineDesc.TriangleCulling = CullMode::None;

        Core::Ref<Pipeline> pipeline = Pipeline::Create(*device, pipelineDesc, *renderTarget);
        REQUIRE_MESSAGE(pipeline != nullptr, "blit pipeline creation failed");

        // --- Sampler (nearest: exact 1:1 sampling is what the comparison assumes) -----------------------------------
        // NVRHI's filters are bools, not an enum: true means "lerp" (linear) and false means point. An earlier draft
        // here guessed FilteringMode values — worth stating because the names read as though one existed. False on
        // magFilter is what makes this an exact 1:1 copy; mip filtering off since the source has a single level.
        nvrhi::SamplerDesc samplerDesc;
        samplerDesc.setAllFilters(false);
        samplerDesc.addressU = nvrhi::SamplerAddressMode::Clamp;
        samplerDesc.addressV = nvrhi::SamplerAddressMode::Clamp;
        const nvrhi::SamplerHandle sampler = rhi->createSampler(samplerDesc);
        REQUIRE(static_cast<bool>(sampler));

        const nvrhi::BindingSetHandle bindingSet = rhi->createBindingSet(
            nvrhi::BindingSetDesc()
                .addItem(nvrhi::BindingSetItem::ConstantBuffer(TonemapBlitBindings::ExposureBuffer,
                                                               exposureBuffer->GetNativeBuffer()))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(TonemapBlitBindings::SourceTexture,
                                                            sourceTexture->GetNativeTexture()))
                .addItem(nvrhi::BindingSetItem::Sampler(TonemapBlitBindings::SourceSampler, sampler)),
            pipeline->GetBindingLayout());
        REQUIRE(static_cast<bool>(bindingSet));

        // --- Record -------------------------------------------------------------------------------------------------
        nvrhi::CommandListHandle commandList = rhi->createCommandList();
        commandList->open();
        commandList->writeBuffer(exposureBuffer->GetNativeBuffer(), &exposureBlock, sizeof(exposureBlock));

        // NaN clear: an unwritten texel fails loudly instead of reading as a plausible black pixel.
        commandList->clearTextureFloat(
            targetTexture->GetNativeTexture(), nvrhi::TextureSubresourceSet(),
            nvrhi::Color(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN(),
                         std::numeric_limits<float>::quiet_NaN(), 0.0f));

        nvrhi::GraphicsState state;
        state.setPipeline(pipeline->GetNativePipeline())
            .setFramebuffer(renderTarget->GetNativeFramebuffer())
            .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                nvrhi::Viewport(static_cast<float>(kSize), static_cast<float>(kSize))))
            .addBindingSet(bindingSet);

        commandList->setGraphicsState(state);
        commandList->draw(nvrhi::DrawArguments().setVertexCount(3).setInstanceCount(1));
        commandList->close();

        nvrhi::ICommandList* raw = commandList;
        rhi->executeCommandLists(&raw, 1);
        device->WaitForIdle();

        // --- Read back and compare -----------------------------------------------------------------------------------
        UInt32 rowPitchBytes = 0;
        const std::vector<UInt8> output = targetTexture->ReadBack(*device, rowPitchBytes);
        REQUIRE_FALSE(output.empty());

        const auto* floats = reinterpret_cast<const float*>(output.data());
        const size_t perRow = rowPitchBytes / sizeof(float);

        UInt32 compared = 0;
        std::string firstFailure;

        for (UInt32 y = 0; y < kSize; ++y)
        {
            for (UInt32 x = 0; x < kSize; ++x)
            {
                const float* texel = floats + perRow * y + static_cast<size_t>(x) * 4u;
                REQUIRE_MESSAGE(!std::isnan(texel[0]), "texel (" << x << "," << y << ") never written");

                // The blit covers the viewport exactly, so texel (x,y) of the output samples texel (x,y) of the
                // source at 1:1 with nearest filtering. Note the source was authored with y growing the same way the
                // framebuffer reads back; since this pass must reproduce its input's layout, any orientation error
                // shows up as a mismatch on every off-diagonal texel rather than cancelling out.
                const Vec3 expected = LinearToDisplay(sourceAt(x, y), kExposure);
                const Vec3 actual(texel[0], texel[1], texel[2]);

                ++compared;

                // Display values are 0..1 and the inputs are exact float32, so the only error left is the tone curve's
                // own transcendental evaluation (exp/pow differ slightly between CPU libm and lavapipe's software
                // path). A wrong constant, a swapped channel or a mis-sampled texel all shift by more than 0.05 —
                // measured mismatches from real bugs ran to ~1.0 — so this window is tight without hiding anything.
                const Float32 tolerance = 2.0e-3f;

                if (!Approximately(actual, expected, tolerance) && firstFailure.empty())
                {
                    firstFailure = "texel (" + std::to_string(x) + "," + std::to_string(y) + ") source " +
                                   Describe(sourceAt(x, y)) + "\n  gpu " + Describe(actual) + "\n  cpu " +
                                   Describe(expected);
                }
            }
        }

        INFO("first mismatch:\n" << firstFailure);
        CHECK_MESSAGE(firstFailure.empty(), "blit output disagrees with the CPU display transform");
        CHECK(compared == kSize * kSize);
    }
}
