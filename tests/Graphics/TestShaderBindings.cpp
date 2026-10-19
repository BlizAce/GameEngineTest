// The resource-binding convention end-to-end: constants, generated GLSL prelude, and a real constant buffer reaching
// a fragment shader through NVRHI's Vulkan binding model.
//
// Why three test cases rather than one: validation layers are NOT installed on this machine, so API misuse produces no
// diagnostics at all — a wrong binding simply reads zeros and looks exactly like a shading bug. The only way to
// localise such a fault is to test each link in the chain independently:
//
//   1. upload -> buffer contents (does writeBuffer land?)                 [no shader involved]
//   2. draw with a literal colour, no bindings (does the pass rasterise?) [no uniform involved]
//   3. draw reading the uniform (does the binding connect?)               [the actual question]
//
// Each prints the pixels it saw, so a failure names the link that broke instead of just "black". Case 3 is what found
// that NVRHI offsets Vulkan bindings by resource type (constant buffers at 256+slot), which ShaderBindings.h now
// encodes — see its header comment for the full story.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include <doctest/doctest.h>

#include "Hollowpoint/Graphics/RhiAccess.h"
#include "Hollowpoint/Graphics/ShaderBindings.h"
#include "Hollowpoint/Graphics/ShaderCompiler.h"

using namespace Hollowpoint::Graphics;

namespace
{

[[nodiscard]] GraphicsDevice* GetProbeDevice()
{
    static GraphicsDevice* device = []() -> GraphicsDevice* {
        GraphicsDeviceDesc desc;
        desc.ApplicationName = "Hollowpoint binding probe";
        return GraphicsDevice::Create(desc);
    }();

    return device;
}

constexpr uint32_t kSize = 8u;

//! Matches the shader's std140 block, which is NOT what C++ default layout produces.
//!
//! std140 base alignment is the largest member's (16 for vec4): Tint occupies 0..15 and Scalar aligns to 4 at offset
//! 16, so the block is 20 bytes of data rounded up to a multiple of 16 -> **32**. A plain C++ struct gives 20, because
//! C++ only rounds to its own alignment (4 here). The padding member makes the two agree, which matters twice over:
//! the buffer must be large enough for the GPU to read the whole block, and a CPU-side array of these (a light array)
//! must stride at the same rate the shader indexes it. A mismatch here reads neighbour elements' bytes — values that
//! look plausible but wrong, which is the worst kind of bug to find visually.
struct ProbeUniforms
{
    float Tint[4] = {0.25f, 0.5f, 0.75f, 0.625f};
    float Scalar = 0.375f;

    //! Explicit std140 tail padding (bytes 20..31). Never read by the shader; exists so sizeof matches the block.
    float Padding[3] = {0.0f, 0.0f, 0.0f};
};

static_assert(sizeof(ProbeUniforms) == 32, "std140 {vec4; float} block is 32 bytes");
static_assert(alignof(float) == 4, "layout reasoning above assumes 4-byte floats");

constexpr const char* kFullscreenVertex = R"glsl(#version 450
void main() {
    // Three-vertex fullscreen triangle from gl_VertexIndex alone (Vulkan spelling; gl_VertexID does not exist here).
    float x = (gl_VertexIndex == 1) ? 3.0 : -1.0;
    float y = (gl_VertexIndex == 2) ? 3.0 : -1.0;
    gl_Position = vec4(x, y, 0.0, 1.0);
}
)glsl";

//! Fragment shader reading the uniform block. `set`/`binding` are left to NVRHI (no explicit qualifiers): this
//! revision maps binding layouts to descriptor sets itself, and hardcoding a set index that disagrees with its
//! layout is exactly the silent-zero failure this probe exists to find.
constexpr const char* kUniformFragment = R"glsl(#version 450
layout(location = 0) out vec4 OutColor;
layout(std140, set = 0, binding = HP_BINDING_CONSTANT_BUFFER(0)) uniform ProbeBlock {
    vec4 Tint;
    float Scalar;
} g_Probe;
void main() { OutColor = vec4(g_Probe.Tint.rgb, g_Probe.Scalar); }
)glsl";

//! Same signature, but never touches the uniform: isolates "the pass rasterises at all" from "the binding works".
constexpr const char* kLiteralFragment = R"glsl(#version 450
layout(location = 0) out vec4 OutColor;
void main() { OutColor = vec4(0.25, 0.5, 0.75, 0.625); }
)glsl";

//! Inserts the engine's binding-prelude directly after a shader's `#version` directive, which must remain the first
//! non-comment token. Prepending it would be rejected with "'#version' must occur first in shader" (observed).
[[nodiscard]] std::string SourceWithBindingPrelude(std::string_view glslSource)
{
    const std::string prelude = BuildGlslBindingPrelude();

    const size_t newline = glslSource.find('\n');
    if (glslSource.rfind("#version", 0) == 0 && newline != std::string_view::npos)
    {
        return std::string(glslSource.substr(0, newline + 1)) + prelude + std::string(glslSource.substr(newline + 1));
    }

    return prelude + std::string(glslSource);
}

[[nodiscard]] nvrhi::ShaderHandle MakeShader(nvrhi::IDevice* rhi, nvrhi::ShaderType type,
                                             const CompiledShader& compiled)
{
    return rhi->createShader(nvrhi::ShaderDesc().setShaderType(type), compiled.SpirV.data(),
                             compiled.SpirV.size() * sizeof(UInt32));
}

//! Renders with the given fragment shader and optional binding layout/set, returning the centre texel.
struct RenderResult
{
    bool Ok = false;
    std::string Failure;
    std::array<uint8_t, 4> Centre{};
};

[[nodiscard]] RenderResult RenderProbe(nvrhi::IDevice* rhi, const char* fragmentSource,
                                       nvrhi::IBindingLayout* bindingLayout, nvrhi::IBindingSet* bindingSet)
{
    RenderResult result;

    const CompiledShader vs = CompileGlsl(ShaderStage::Vertex, kFullscreenVertex);
    if (!vs.Succeeded)
    {
        result.Failure = "vertex compile: " + vs.Diagnostics;
        return result;
    }

    // The binding macros come from the engine's generated prelude; shaders never hardcode a binding number.
    const CompiledShader ps = CompileGlsl(ShaderStage::Pixel, SourceWithBindingPrelude(fragmentSource));
    if (!ps.Succeeded)
    {
        result.Failure = "pixel compile: " + ps.Diagnostics;
        return result;
    }

    nvrhi::ShaderHandle vertexShader = MakeShader(rhi, nvrhi::ShaderType::Vertex, vs);
    nvrhi::ShaderHandle pixelShader = MakeShader(rhi, nvrhi::ShaderType::Pixel, ps);

    if (!static_cast<bool>(vertexShader) || !static_cast<bool>(pixelShader))
    {
        result.Failure = "createShader failed";
        return result;
    }

    nvrhi::TextureDesc targetDesc;
    targetDesc.debugName = "probe target";
    targetDesc.format = nvrhi::Format::RGBA8_UNORM;
    targetDesc.width = kSize;
    targetDesc.height = kSize;
    targetDesc.dimension = nvrhi::TextureDimension::Texture2D;
    targetDesc.isRenderTarget = true;
    targetDesc.initialState = nvrhi::ResourceStates::RenderTarget;
    targetDesc.keepInitialState = true;

    nvrhi::TextureHandle target = rhi->createTexture(targetDesc);
    if (!static_cast<bool>(target))
    {
        result.Failure = "createTexture failed";
        return result;
    }

    const nvrhi::FramebufferDesc framebufferDesc = nvrhi::FramebufferDesc().addColorAttachment(target);
    const nvrhi::FramebufferInfo framebufferInfo(framebufferDesc);

    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.setPrimType(nvrhi::PrimitiveType::TriangleList)
        .setVertexShader(vertexShader)
        .setPixelShader(pixelShader);

    if (bindingLayout != nullptr)
    {
        pipelineDesc.addBindingLayout(bindingLayout);
    }

    nvrhi::RasterState raster;
    raster.cullMode = nvrhi::RasterCullMode::None;
    pipelineDesc.setRenderState(nvrhi::RenderState().setRasterState(raster));

    nvrhi::GraphicsPipelineHandle pipeline = rhi->createGraphicsPipeline(pipelineDesc, framebufferInfo);
    if (!static_cast<bool>(pipeline))
    {
        result.Failure = "createGraphicsPipeline failed";
        return result;
    }

    nvrhi::FramebufferHandle framebuffer = rhi->createFramebuffer(framebufferDesc);
    if (!static_cast<bool>(framebuffer))
    {
        result.Failure = "createFramebuffer failed";
        return result;
    }

    nvrhi::CommandListHandle commandList = rhi->createCommandList();
    if (!static_cast<bool>(commandList))
    {
        result.Failure = "createCommandList failed";
        return result;
    }

    commandList->open();
    // A clear colour that no shader output can produce, so "nothing drew" is visible as (26, 230, 51) rather than
    // being confused with a black draw. Note the fullscreen triangle covers every texel, so this only survives when
    // the draw itself fails — which is precisely the distinction being made.
    commandList->clearTextureFloat(target, nvrhi::TextureSubresourceSet(), nvrhi::Color(0.1f, 0.9f, 0.2f, 1.0f));

    const nvrhi::Viewport viewport(static_cast<float>(kSize), static_cast<float>(kSize));

    nvrhi::GraphicsState state;
    state.setPipeline(pipeline)
        .setFramebuffer(framebuffer)
        .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(viewport));

    if (bindingSet != nullptr)
    {
        state.addBindingSet(bindingSet);
    }

    commandList->setGraphicsState(state);
    commandList->draw(nvrhi::DrawArguments().setVertexCount(3).setInstanceCount(1));

    nvrhi::StagingTextureHandle readback = rhi->createStagingTexture(targetDesc, nvrhi::CpuAccessMode::Read);
    if (!static_cast<bool>(readback))
    {
        result.Failure = "createStagingTexture failed";
        return result;
    }

    commandList->setTextureState(target, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
    commandList->copyTexture(readback, nvrhi::TextureSlice(), target, nvrhi::TextureSlice());
    commandList->close();

    nvrhi::ICommandList* rawCommandList = commandList;
    rhi->executeCommandLists(&rawCommandList, 1);

    if (!rhi->waitForIdle())
    {
        result.Failure = "waitForIdle failed (device lost — an illegal command was submitted)";
        return result;
    }

    size_t rowPitch = 0;
    void* mapped = rhi->mapStagingTexture(readback, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch);
    if (mapped == nullptr)
    {
        result.Failure = "mapStagingTexture failed";
        return result;
    }

    const uint8_t* pixels = static_cast<const uint8_t*>(mapped);
    const size_t offset = rowPitch * (kSize / 2) + (kSize / 2) * 4;
    result.Centre = {pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3]};

    rhi->unmapStagingTexture(readback);

    result.Ok = true;
    return result;
}

[[nodiscard]] std::string ToString(const std::array<uint8_t, 4>& texel)
{
    return "(" + std::to_string(texel[0]) + ", " + std::to_string(texel[1]) + ", " + std::to_string(texel[2]) + ", " +
           std::to_string(texel[3]) + ")";
}

[[nodiscard]] bool ApproximatelyTexel(const std::array<uint8_t, 4>& texel, const std::array<int, 4>& expected,
                                      int tolerance = 1)
{
    for (int channel = 0; channel < 4; ++channel)
    {
        if (std::abs(static_cast<int>(texel[static_cast<size_t>(channel)]) - expected[static_cast<size_t>(channel)]) >
            tolerance)
        {
            return false;
        }
    }

    return true;
}

}  // namespace

TEST_SUITE("Graphics/ShaderBindings")
{
    TEST_CASE("writeBuffer uploads constant-buffer contents (verified by readback)")
    {
        GraphicsDevice* device = GetProbeDevice();
        if (device == nullptr)
        {
            WARN((std::string("No graphics device: ") + GraphicsDevice::GetFailureReason()).c_str());
            return;
        }

        nvrhi::IDevice* rhi = device->GetNativeDevice();
        REQUIRE(rhi != nullptr);

        // Reported, not asserted: whether the validation layer is present changes how a *later* failure must be read.
        // With it absent (the case on this machine) an illegal command produces no diagnostic and pixels simply stay at
        // whatever the last write left them — so "no errors printed" carries no information, and only these staged
        // assertions can localise a fault.
        INFO("validation layer: " << (device->IsValidationEnabled() ? "enabled" : "NOT INSTALLED"));

        ProbeUniforms uniforms;
        uniforms.Tint[0] = 0.125f;
        uniforms.Scalar = 0.875f;

        nvrhi::BufferDesc bufferDesc;
        bufferDesc.debugName = "probe upload";
        bufferDesc.byteSize = sizeof(ProbeUniforms);
        bufferDesc.isConstantBuffer = true;
        bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
        bufferDesc.keepInitialState = true;

        nvrhi::BufferHandle buffer = rhi->createBuffer(bufferDesc);
        REQUIRE_MESSAGE(static_cast<bool>(buffer), "createBuffer(constant) failed");

        nvrhi::CommandListHandle commandList = rhi->createCommandList();
        REQUIRE(static_cast<bool>(commandList));

        commandList->open();
        commandList->writeBuffer(buffer, &uniforms, sizeof(uniforms));
        commandList->close();

        nvrhi::ICommandList* rawCommandList = commandList;
        rhi->executeCommandLists(&rawCommandList, 1);
        device->WaitForIdle();

        // Read the same bytes back through a staging buffer. This is an independent check of the upload path: if this
        // fails, every uniform in the engine is broken and no shader debugging is worth doing yet.
        nvrhi::BufferDesc stagingDesc;
        stagingDesc.debugName = "probe upload readback";
        stagingDesc.byteSize = sizeof(ProbeUniforms);
        stagingDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
        stagingDesc.initialState = nvrhi::ResourceStates::CopyDest;
        stagingDesc.keepInitialState = true;

        nvrhi::BufferHandle staging = rhi->createBuffer(stagingDesc);
        if (!static_cast<bool>(staging))
        {
            // No read-back path for constant buffers on this driver: report rather than silently pass.
            WARN("could not create a CPU-readable staging buffer to verify the upload");
            return;
        }

        nvrhi::CommandListHandle copyList = rhi->createCommandList();
        REQUIRE(static_cast<bool>(copyList));

        copyList->open();
        copyList->setBufferState(buffer, nvrhi::ResourceStates::CopySource);
        copyList->copyBuffer(staging, 0, buffer, 0, sizeof(ProbeUniforms));
        copyList->close();

        nvrhi::ICommandList* rawCopyList = copyList;
        rhi->executeCommandLists(&rawCopyList, 1);
        device->WaitForIdle();

        void* mapped = rhi->mapBuffer(staging, nvrhi::CpuAccessMode::Read);
        REQUIRE(mapped != nullptr);

        ProbeUniforms readBack{};
        std::memcpy(&readBack, mapped, sizeof(ProbeUniforms));

        rhi->unmapBuffer(staging);

        INFO("tint0 " << readBack.Tint[0] << " scalar " << readBack.Scalar);
        CHECK(readBack.Tint[0] == doctest::Approx(0.125f));
        CHECK(readBack.Tint[1] == doctest::Approx(0.5f));
        CHECK(readBack.Scalar == doctest::Approx(0.875f));
    }

    TEST_CASE("a pass with no bindings rasterises (control for the binding test)")
    {
        GraphicsDevice* device = GetProbeDevice();
        if (device == nullptr)
        {
            WARN((std::string("No graphics device: ") + GraphicsDevice::GetFailureReason()).c_str());
            return;
        }

        nvrhi::IDevice* rhi = device->GetNativeDevice();
        REQUIRE(rhi != nullptr);

        const RenderResult result = RenderProbe(rhi, kLiteralFragment, nullptr, nullptr);
        INFO("centre texel: " << ToString(result.Centre));
        INFO("failure: " << result.Failure);

        REQUIRE(result.Ok);
        CHECK_MESSAGE(ApproximatelyTexel(result.Centre, {64, 128, 191, 159}),
                      "literal-colour pass did not draw — the pipeline/raster path is broken, so a later binding "
                      "failure would prove nothing");
    }

    TEST_CASE("a constant buffer reaches the fragment shader")
    {
        GraphicsDevice* device = GetProbeDevice();
        if (device == nullptr)
        {
            WARN((std::string("No graphics device: ") + GraphicsDevice::GetFailureReason()).c_str());
            return;
        }

        nvrhi::IDevice* rhi = device->GetNativeDevice();
        REQUIRE(rhi != nullptr);

        // --- Constant buffer, uploaded before the draw command list ---------
        ProbeUniforms uniforms;

        nvrhi::BufferDesc constantBufferDesc;
        constantBufferDesc.debugName = "probe uniforms";
        constantBufferDesc.byteSize = sizeof(ProbeUniforms);
        constantBufferDesc.isConstantBuffer = true;
        constantBufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
        constantBufferDesc.keepInitialState = true;

        nvrhi::BufferHandle constantBuffer = rhi->createBuffer(constantBufferDesc);
        REQUIRE(static_cast<bool>(constantBuffer));

        {
            nvrhi::CommandListHandle uploadList = rhi->createCommandList();
            REQUIRE(static_cast<bool>(uploadList));
            uploadList->open();
            uploadList->writeBuffer(constantBuffer, &uniforms, sizeof(uniforms));
            uploadList->close();

            nvrhi::ICommandList* rawUpload = uploadList;
            rhi->executeCommandLists(&rawUpload, 1);
            device->WaitForIdle();
        }

        const nvrhi::BindingLayoutHandle layout =
            rhi->createBindingLayout(nvrhi::BindingLayoutDesc()
                                         .setVisibility(nvrhi::ShaderType::Pixel)
                                         .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0)));
        REQUIRE_MESSAGE(static_cast<bool>(layout), "createBindingLayout failed");

        const nvrhi::BindingSetHandle bindingSet = rhi->createBindingSet(
            nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constantBuffer)), layout);
        REQUIRE_MESSAGE(static_cast<bool>(bindingSet), "createBindingSet failed");

        const RenderResult result = RenderProbe(rhi, kUniformFragment, layout, bindingSet);
        INFO("centre texel: " << ToString(result.Centre));
        INFO("failure: " << result.Failure);

        REQUIRE(result.Ok);

        // Expected UNORM8 of (0.25, 0.5, 0.75) with alpha 0.375: 64, 128, 191, 96.
        CHECK_MESSAGE(ApproximatelyTexel(result.Centre, {64, 128, 191, 96}),
                      "uniform did not reach the shader; all-zero means the binding connected to nothing");
    }
}
