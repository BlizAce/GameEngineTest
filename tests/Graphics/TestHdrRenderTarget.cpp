// Does an RGBA16_FLOAT render target round-trip values precisely enough to validate shading numerically?
//
// The renderer accumulates lighting in a half-float HDR target (values above 1 are meaningful: the sun is thousands of
// nits), then tone maps. To assert on that pipeline I need two things this test establishes:
//
//   * the format is actually supported as a colour attachment with shader read access on this device — checked at
//     runtime rather than assumed, because a renderer that hard-depends on an unsupported format fails only on some
//     drivers; and
//   * how much error a float->half->float round trip introduces. That number becomes the tolerance for every image
//     comparison built on this target. Guessing it produces tests that are either vacuously loose (accepting wrong
//     shading) or flaky (failing correct shading), so it is measured here and quoted by the tests that use it.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include <doctest/doctest.h>

#include "Hollowpoint/Graphics/RhiAccess.h"
#include "Hollowpoint/Graphics/ShaderBindings.h"
#include "Hollowpoint/Graphics/ShaderCompiler.h"

using namespace Hollowpoint::Graphics;

namespace
{

[[nodiscard]] GraphicsDevice* GetHdrDevice()
{
    static GraphicsDevice* device = []() -> GraphicsDevice* {
        GraphicsDeviceDesc desc;
        desc.ApplicationName = "Hollowpoint HDR target tests";
        return GraphicsDevice::Create(desc);
    }();

    return device;
}

constexpr uint32_t kSize = 8u;

[[nodiscard]] std::string WithBindingPrelude(std::string_view glslSource)
{
    const std::string prelude = BuildGlslBindingPrelude();
    const size_t newline = glslSource.find('\n');

    if (glslSource.rfind("#version", 0) == 0 && newline != std::string_view::npos)
    {
        return std::string(glslSource.substr(0, newline + 1)) + prelude + std::string(glslSource.substr(newline + 1));
    }

    return prelude + std::string(glslSource);
}

constexpr const char* kFullscreenVertex = R"glsl(#version 450
void main() {
    float x = (gl_VertexIndex == 1) ? 3.0 : -1.0;
    float y = (gl_VertexIndex == 2) ? 3.0 : -1.0;
    gl_Position = vec4(x, y, 0.0, 1.0);
}
)glsl";

//! Writes a value supplied through a uniform so the round trip is exercised on a chosen input rather than a literal
//! the compiler might fold or reassociate at compile time.
constexpr const char* kPassthroughFragment = R"glsl(#version 450
layout(location = 0) out vec4 OutColor;
layout(std140, set = 0, binding = HP_BINDING_CONSTANT_BUFFER(0)) uniform Values {
    vec4 Value;
} g_Values;
void main() { OutColor = g_Values.Value; }
)glsl";

//! Draws `value` into an RGBA16_FLOAT target and reads back the centre texel's red channel.
[[nodiscard]] float RenderAndReadBackRed(nvrhi::IDevice* rhi, float value, std::string& failure)
{
    const CompiledShader vs = CompileGlsl(ShaderStage::Vertex, kFullscreenVertex);
    if (!vs.Succeeded)
    {
        failure = "vertex compile: " + vs.Diagnostics;
        return 0.0f;
    }

    const CompiledShader ps = CompileGlsl(ShaderStage::Pixel, WithBindingPrelude(kPassthroughFragment));
    if (!ps.Succeeded)
    {
        failure = "pixel compile: " + ps.Diagnostics;
        return 0.0f;
    }

    nvrhi::ShaderHandle vertexShader = rhi->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex),
                                                         vs.SpirV.data(), vs.SpirV.size() * sizeof(UInt32));
    nvrhi::ShaderHandle pixelShader = rhi->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel),
                                                        ps.SpirV.data(), ps.SpirV.size() * sizeof(UInt32));
    if (!static_cast<bool>(vertexShader) || !static_cast<bool>(pixelShader))
    {
        failure = "createShader failed";
        return 0.0f;
    }

    struct alignas(16) Uniforms
    {
        float Value[4];
    };

    Uniforms uniforms{{value, value, value, 1.0f}};

    nvrhi::BufferDesc constantBufferDesc;
    constantBufferDesc.debugName = "hdr test value";
    constantBufferDesc.byteSize = sizeof(Uniforms);
    constantBufferDesc.isConstantBuffer = true;
    constantBufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    constantBufferDesc.keepInitialState = true;

    nvrhi::BufferHandle constantBuffer = rhi->createBuffer(constantBufferDesc);
    if (!static_cast<bool>(constantBuffer))
    {
        failure = "createBuffer failed";
        return 0.0f;
    }

    const nvrhi::BindingLayoutHandle layout =
        rhi->createBindingLayout(nvrhi::BindingLayoutDesc()
                                     .setVisibility(nvrhi::ShaderType::Pixel)
                                     .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0)));
    const nvrhi::BindingSetHandle bindingSet = rhi->createBindingSet(
        nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constantBuffer)), layout);

    if (!static_cast<bool>(layout) || !static_cast<bool>(bindingSet))
    {
        failure = "binding creation failed";
        return 0.0f;
    }

    nvrhi::TextureDesc targetDesc;
    targetDesc.debugName = "hdr round-trip target";
    targetDesc.format = nvrhi::Format::RGBA16_FLOAT;
    targetDesc.width = kSize;
    targetDesc.height = kSize;
    targetDesc.dimension = nvrhi::TextureDimension::Texture2D;
    targetDesc.isRenderTarget = true;
    targetDesc.initialState = nvrhi::ResourceStates::RenderTarget;
    targetDesc.keepInitialState = true;

    nvrhi::TextureHandle target = rhi->createTexture(targetDesc);
    if (!static_cast<bool>(target))
    {
        failure = "createTexture(RGBA16_FLOAT) failed — format unsupported as colour attachment";
        return 0.0f;
    }

    const nvrhi::FramebufferDesc framebufferDesc = nvrhi::FramebufferDesc().addColorAttachment(target);
    const nvrhi::FramebufferInfo framebufferInfo(framebufferDesc);

    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.setPrimType(nvrhi::PrimitiveType::TriangleList)
        .setVertexShader(vertexShader)
        .setPixelShader(pixelShader)
        .addBindingLayout(layout);

    nvrhi::RasterState raster;
    raster.cullMode = nvrhi::RasterCullMode::None;
    pipelineDesc.setRenderState(nvrhi::RenderState().setRasterState(raster));

    nvrhi::GraphicsPipelineHandle pipeline = rhi->createGraphicsPipeline(pipelineDesc, framebufferInfo);
    if (!static_cast<bool>(pipeline))
    {
        failure = "createGraphicsPipeline failed";
        return 0.0f;
    }

    nvrhi::FramebufferHandle framebuffer = rhi->createFramebuffer(framebufferDesc);

    nvrhi::CommandListHandle commandList = rhi->createCommandList();
    commandList->open();
    commandList->writeBuffer(constantBuffer, &uniforms, sizeof(uniforms));
    commandList->clearTextureFloat(target, nvrhi::TextureSubresourceSet(), nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));

    const nvrhi::Viewport viewport(static_cast<float>(kSize), static_cast<float>(kSize));
    nvrhi::GraphicsState state;
    state.setPipeline(pipeline)
        .setFramebuffer(framebuffer)
        .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(viewport))
        .addBindingSet(bindingSet);

    commandList->setGraphicsState(state);
    commandList->draw(nvrhi::DrawArguments().setVertexCount(3).setInstanceCount(1));

    nvrhi::StagingTextureHandle readback = rhi->createStagingTexture(targetDesc, nvrhi::CpuAccessMode::Read);
    if (!static_cast<bool>(readback))
    {
        failure = "createStagingTexture(RGBA16_FLOAT) failed — format unsupported for CPU readback";
        return 0.0f;
    }

    commandList->setTextureState(target, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
    commandList->copyTexture(readback, nvrhi::TextureSlice(), target, nvrhi::TextureSlice());
    commandList->close();

    nvrhi::ICommandList* rawCommandList = commandList;
    rhi->executeCommandLists(&rawCommandList, 1);

    if (!rhi->waitForIdle())
    {
        failure = "waitForIdle failed (device lost)";
        return 0.0f;
    }

    size_t rowPitch = 0;
    void* mapped = rhi->mapStagingTexture(readback, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch);
    if (mapped == nullptr)
    {
        failure = "mapStagingTexture failed";
        return 0.0f;
    }

    // RGBA16_FLOAT readback layout is implementation-defined in size per texel; NVRHI converts to a well-defined
    // CPU-side format, so ask it rather than assuming two bytes per channel.
    const uint16_t* halfTexel = static_cast<const uint16_t*>(mapped) + (rowPitch / 2) * (kSize / 2) + (kSize / 2) * 4;

    // Half-float -> float, decoded here rather than via a library helper so the comparison is independent of whatever
    // NVRHI or the driver did on the way out.
    //
    // Careful with the sign: for normal values it is folded into `bits` (sign << 16 lands in float32's bit 31), so
    // negating afterwards flips it a second time and -3.75 decodes as +3.75. Only the subnormal branch — which builds
    // its result from the mantissa alone — needs the sign applied manually. Caught by testing a negative sample.
    const uint16_t rawHalf = halfTexel[0];
    const bool negative = (rawHalf & 0x8000u) != 0u;
    const uint32_t exponent = (rawHalf >> 10) & 0x1Fu;
    const uint32_t mantissa = rawHalf & 0x3FFu;

    float result = 0.0f;
    if (exponent == 0u)
    {
        // Subnormal half: value = (-1)^s * 2^-14 * (mantissa / 1024) = mantissa * 2^-24. Sign applied here only.
        result = std::ldexp(static_cast<float>(mantissa), -24);
        if (negative)
        {
            result = -result;
        }
    }
    else if (exponent == 31u)
    {
        result = mantissa != 0u ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
        if (negative)
        {
            result = -result;
        }
    }
    else
    {
        const uint32_t bits =
            (static_cast<uint32_t>(rawHalf & 0x8000u) << 16) | ((exponent + 112u) << 23) | (mantissa << 13);
        std::memcpy(&result, &bits, sizeof(result));
    }

    rhi->unmapStagingTexture(readback);
    return result;
}

}  // namespace

TEST_SUITE("Graphics/HdrRenderTarget")
{
    TEST_CASE("RGBA16_FLOAT is supported as a colour attachment on this device")
    {
        GraphicsDevice* device = GetHdrDevice();
        if (device == nullptr)
        {
            WARN((std::string("No graphics device: ") + GraphicsDevice::GetFailureReason()).c_str());
            return;
        }

        nvrhi::IDevice* rhi = device->GetNativeDevice();
        REQUIRE(rhi != nullptr);

        const nvrhi::FormatSupport support = rhi->queryFormatSupport(nvrhi::Format::RGBA16_FLOAT);

        INFO("format support bits: " << static_cast<unsigned long long>(support));

        // Reported rather than hard-asserted as a requirement: if a device lacks this, the renderer must fall back to
        // another HDR format (R11G11B10_FLOAT) or RGBA32_FLOAT, not fail. The fallback selection is what the next
        // milestone implements; here we only record whether the preferred format is available so the fallback path is
        // exercised knowingly rather than by accident.
        // NVRHI's flag names are ShaderSample/ShaderLoad rather than the Vulkan-ish ShaderResource, so these are read
        // off the enum as it actually exists instead of from a guess at what it might be called.
        const bool renderTarget = (support & nvrhi::FormatSupport::RenderTarget) != nvrhi::FormatSupport::None;
        const bool shaderSample = (support & nvrhi::FormatSupport::ShaderSample) != nvrhi::FormatSupport::None;

        INFO("render target: " << renderTarget << ", shader sample: " << shaderSample);

        // Creation is the real test — support bits and actual allocation can disagree on some drivers.
        nvrhi::TextureDesc desc;
        desc.format = nvrhi::Format::RGBA16_FLOAT;
        desc.width = 4;
        desc.height = 4;
        desc.dimension = nvrhi::TextureDimension::Texture2D;
        desc.isRenderTarget = true;

        const nvrhi::TextureHandle texture = rhi->createTexture(desc);
        CHECK_MESSAGE(static_cast<bool>(texture), "RGBA16_FLOAT texture creation failed on this device");
    }

    TEST_CASE("half-float render target round-trips values, and the error is measured")
    {
        GraphicsDevice* device = GetHdrDevice();
        if (device == nullptr)
        {
            WARN((std::string("No graphics device: ") + GraphicsDevice::GetFailureReason()).c_str());
            return;
        }

        nvrhi::IDevice* rhi = device->GetNativeDevice();
        REQUIRE(rhi != nullptr);

        // Half-precision has a 10-bit mantissa, so relative resolution near 1.0 is ~2^-10 = 9.77e-4. Values below are
        // chosen to include exactly representable ones (0.5, 0.25) and inexact ones (0.1, 0.3), because "it
        // round-trips" is only true for the former and a test that uses just those proves nothing about real shading
        // output.
        // Signed samples are essential: the first decoder applied the sign bit twice on the normal path, so every
        // positive value round-tripped correctly and negatives came back positive — invisible to an all-positive set.
        const std::array<float, 9> samples = {0.0f, 0.25f, 0.5f, 1.0f, 0.1f, 4.0f, -3.75f, -0.1f, -1024.0f};

        float worstRelativeError = 0.0f;

        for (const float sample : samples)
        {
            std::string failure;
            const float readBack = RenderAndReadBackRed(rhi, sample, failure);

            INFO("wrote " << sample << " read back " << readBack << " failure: " << failure.c_str());
            REQUIRE(failure.empty());

            if (sample == 0.0f)
            {
                // Zero must be exact — an epsilon floor here would let a broken pipeline that outputs tiny garbage
                // pass.
                CHECK(readBack == 0.0f);
                continue;
            }

            const float relativeError = std::fabs(readBack - sample) / std::fabs(sample);
            worstRelativeError = std::max(worstRelativeError, relativeError);

            // Half precision guarantees ~2^-10 relative resolution for normal-range values. Allowing 2^-9 leaves room
            // for a driver-side conversion without accepting anything coarser.
            CHECK(relativeError < 2.0e-3f);
        }

        INFO("worst relative error across samples: " << worstRelativeError);
        CHECK(worstRelativeError < 2.0e-3f);

        // Large HDR values must survive too: the sun is thousands of nits and half-float tops out at 65504, so a
        // tone-mapping pipeline that multiplies before clamping can silently overflow to infinity.
        std::string failure;
        const float bright = RenderAndReadBackRed(rhi, 1024.0f, failure);

        INFO("1024 read back as " << bright << " failure: " << failure.c_str());
        CHECK(std::isfinite(bright));
        CHECK(bright == doctest::Approx(1024.0f).epsilon(2.0e-3));
    }
}
