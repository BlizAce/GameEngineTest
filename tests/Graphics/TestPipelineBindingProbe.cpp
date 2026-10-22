// Probes for the invisible assumptions underneath every GPU test in this project.
//
// These are not unit tests of behaviour so much as measurements of the environment: what a constant-buffer slot
// delivers through Pipeline::Create's generated binding layout, and which vertical direction the framebuffer uses on
// this driver. Both were written while debugging "the forward pass renders exact zero" — a symptom that turned out to
// be a texel-mapping error in the test, not an engine bug, and that stayed invisible because the centre pixel of a
// symmetric scene looks correct under either orientation. Keeping them means the next person who hits a mirrored or
// zero-filled image gets a one-line answer instead of a day.
//
//   1. Slots reach the fragment stage (and vertex-visible slots reach the vertex stage) through the layout built by
//      Pipeline::Create — with no layout attached at all, bindDescriptorSets dereferences a null handle and SIGSEGVs
//      inside the driver, which is what started this investigation.
//   2. NDC +Y maps to the FIRST readback rows (Vulkan's top-left origin). Measured here because every world→texel
//      mapping in the test suite depends on it and nothing else would catch a change: the failure looks like wrong
//      lighting, not like flipped coordinates.

#include "TestSupport.h"

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
#include "Hollowpoint/Renderer/ForwardRender.h"

using namespace Hollowpoint;
using namespace Hollowpoint::Graphics;

namespace
{
constexpr UInt32 kProbeSize = 16;

[[nodiscard]] GraphicsDevice* GetProbeDevice()
{
    static GraphicsDevice* device = []() -> GraphicsDevice* {
        GraphicsDeviceDesc desc;
        desc.ApplicationName = "Hollowpoint uniform probe";
        return GraphicsDevice::Create(desc);
    }();
    return device;
}

struct alignas(16) ProbeBlockA
{
    float V[4];
};

struct alignas(16) ProbeBlockC
{
    int Count;
    float Pad[3];
};


// TEMP helpers: the real generators with the final write rewritten to diagnostics. Kept out of the engine so the
// probe cannot influence what it measures beyond string surgery on the emitted text.
[[nodiscard]] std::string BuildForwardVertexGlslProbe()
{
    // The real generator, verbatim — no reimplementation in the probe, so it measures what ships.
    return Hollowpoint::Renderer::BuildForwardVertexGlsl();
}

[[nodiscard]] std::string BuildForwardFragmentGlslProbe()
{
    std::string source = Hollowpoint::Renderer::BuildForwardFragmentGlsl(8u);

    // Replace the final write with four diagnostic channels (documented at the call site).
    const std::string needle = "OutColor = vec4(accumulated, 1.0);";
    const std::size_t position = source.rfind(needle);
    if (position != std::string::npos)
    {
        source.replace(position, needle.size(),
                       "OutColor = vec4(accumulated.r, float(drawLightIndices.LightCount) / 16.0, "
                       "lightArray.PositionAndIntensity[0].w / 16.0, dot(normalShading, viewDirection));");
    }
    return source;
}

}  // namespace

TEST_SUITE("Probe")
{
    TEST_CASE("constant buffers reach the fragment stage through Pipeline::Create's layout")
    {
        GraphicsDevice* device = GetProbeDevice();
        if (device == nullptr)
        {
            WARN("no graphics device for the uniform probe");
            return;
        }
        nvrhi::IDevice* rhi = device->GetNativeDevice();
        REQUIRE(rhi != nullptr);

        ProbeBlockA blockA{};
        blockA.V[0] = 0.25f;
        ProbeBlockC blockC{};
        blockC.Count = 7;

        auto upload = [&](const char* name, const void* data, size_t size) -> Core::Ref<Buffer> {
            BufferDesc desc;
            desc.Name = name;
            desc.ByteSize = static_cast<UInt32>(size);
            desc.Usage = BufferUsage::Constant;
            Core::Ref<Buffer> buffer = Buffer::Create(*device, desc);
            if (buffer == nullptr)
            {
                return nullptr;
            }

            nvrhi::CommandListHandle commandList = rhi->createCommandList();
            commandList->open();
            commandList->writeBuffer(buffer->GetNativeBuffer(), data, size);
            commandList->close();
            nvrhi::ICommandList* raw = commandList;
            rhi->executeCommandLists(&raw, 1);
            device->WaitForIdle();
            return buffer;
        };

        ProbeBlockA blockScale{};
        blockScale.V[0] = 1.0f;  // vertex scale: identity, so geometry lands exactly where NDC says

        Core::Ref<Buffer> bufferA = upload("probe a", &blockA, sizeof(blockA));
        Core::Ref<Buffer> bufferScale = upload("probe scale", &blockScale, sizeof(blockScale));
        Core::Ref<Buffer> bufferC = upload("probe c", &blockC, sizeof(blockC));
        REQUIRE(bufferA != nullptr);
        REQUIRE(bufferScale != nullptr);
        REQUIRE(bufferC != nullptr);

        const std::string header = "#version 450\n" + BuildGlslBindingPrelude();

        // Slot 1 read by the VERTEX stage only: proves a vertex-visible block transforms geometry (if the layout's
        // visibility were wrong, the cube would collapse to a point and nothing would rasterise).
        std::string vertexSource = header;
        vertexSource += "layout(location = 0) in vec3 InPosition;\n";
        vertexSource +=
            "layout(set = 0, binding = HP_BINDING_CONSTANT_BUFFER(1)) uniform ScaleBlock { float Scale; } "
            "scaleBlock;\n";
        vertexSource += "void main() { gl_Position = vec4(InPosition * scaleBlock.Scale, 1.0); }\n";

        std::string fragmentSource = header;
        fragmentSource += "layout(location = 0) out vec4 OutColor;\n";
        fragmentSource +=
            "layout(set = 0, binding = HP_BINDING_CONSTANT_BUFFER(0)) uniform BlockA { vec4 Value; } blockA;\n";
        fragmentSource +=
            "layout(set = 0, binding = HP_BINDING_CONSTANT_BUFFER(2)) uniform BlockC { int Count; float Pad[3]; } "
            "blockC;\n";
        // Channels: R = slot-0 value, G = slot-2 count normalised, B = constant 1 (proves the shader ran), A = 1.
        fragmentSource += "void main() { OutColor = vec4(blockA.Value.r, float(blockC.Count) / 16.0, 1.0, 1.0); }\n";

        const CompiledShader compiledVertex = CompileGlsl(ShaderStage::Vertex, vertexSource);
        REQUIRE_MESSAGE(compiledVertex.Succeeded, compiledVertex.Diagnostics);
        const CompiledShader compiledFragment = CompileGlsl(ShaderStage::Pixel, fragmentSource);
        REQUIRE_MESSAGE(compiledFragment.Succeeded, compiledFragment.Diagnostics);

        TextureDesc textureDesc;
        textureDesc.Name = "probe target";
        textureDesc.Width = kProbeSize;
        textureDesc.Height = kProbeSize;
        textureDesc.Format = GraphicsFormat::Rgba32Float;
        Core::Ref<Texture> texture = Texture::Create(*device, textureDesc);
        REQUIRE(texture != nullptr);

        RenderTargetDesc renderTargetDesc;
        renderTargetDesc.Name = "probe rt";
        renderTargetDesc.Colour = texture;
        Core::Ref<RenderTarget> renderTarget = RenderTarget::Create(*device, renderTargetDesc);
        REQUIRE(renderTarget != nullptr);

        PipelineDesc pipelineDesc;
        pipelineDesc.Name = "probe";
        pipelineDesc.VertexShader.SpirV = compiledVertex.SpirV;
        pipelineDesc.PixelShader.SpirV = compiledFragment.SpirV;
        // Deliberately the same shape the forward pass uses: slots 0 and 2 pixel-only, slot 1 vertex-visible.
        pipelineDesc.Bindings = {
            BindingSlotDesc{BindingSlotDesc::Kind::ConstantBuffer, 0},
            BindingSlotDesc{BindingSlotDesc::Kind::ConstantBuffer, 1},
            BindingSlotDesc{BindingSlotDesc::Kind::ConstantBuffer, 2},
        };
        pipelineDesc.BindingsVisibleToVertex = true;
        pipelineDesc.BindingsVisibleToPixel = true;
        pipelineDesc.TriangleCulling = CullMode::None;
        pipelineDesc.IsDepthEnabled = false;
        // Full-screen triangle in NDC directly: no projection matrix involved, so coverage cannot be the variable.
        pipelineDesc.VertexAttributes = {VertexAttribute{"InPosition", GraphicsFormat::Rgb32Float, 0, 12}};

        Core::Ref<Pipeline> pipeline = Pipeline::Create(*device, pipelineDesc, *renderTarget);
        REQUIRE_MESSAGE(pipeline != nullptr, "probe pipeline creation failed");
        REQUIRE(pipeline->GetBindingLayout() != nullptr);

        const nvrhi::BindingSetHandle bindingSet =
            rhi->createBindingSet(nvrhi::BindingSetDesc()
                                      .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, bufferA->GetNativeBuffer()))
                                      .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, bufferScale->GetNativeBuffer()))
                                      .addItem(nvrhi::BindingSetItem::ConstantBuffer(2, bufferC->GetNativeBuffer())),
                                  pipeline->GetBindingLayout());
        REQUIRE(static_cast<bool>(bindingSet));

        // Three vertices covering the whole viewport.
        static const float kTriangle[9] = {
            -1.0f, -1.0f, 0.0f,  //
            3.0f,  -1.0f, 0.0f,  //
            -1.0f, 3.0f,  0.0f,  //
        };

        BufferDesc vertexBufferDesc;
        vertexBufferDesc.Name = "probe vertices";
        vertexBufferDesc.ByteSize = sizeof(kTriangle);
        vertexBufferDesc.Usage = BufferUsage::Vertex;
        Core::Ref<Buffer> vertexBuffer = Buffer::Create(*device, vertexBufferDesc);
        REQUIRE(vertexBuffer != nullptr);

        nvrhi::CommandListHandle commandList = rhi->createCommandList();
        commandList->open();
        commandList->writeBuffer(vertexBuffer->GetNativeBuffer(), kTriangle, sizeof(kTriangle));
        commandList->clearTextureFloat(
            texture->GetNativeTexture(), nvrhi::TextureSubresourceSet(),
            nvrhi::Color(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN(),
                         std::numeric_limits<float>::quiet_NaN(), 0.0f));

        nvrhi::GraphicsState state;
        state.setPipeline(pipeline->GetNativePipeline())
            .setFramebuffer(renderTarget->GetNativeFramebuffer())
            .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                nvrhi::Viewport(static_cast<float>(kProbeSize), static_cast<float>(kProbeSize))))
            .addVertexBuffer(nvrhi::VertexBufferBinding{vertexBuffer->GetNativeBuffer(), 0, 0})
            .addBindingSet(bindingSet);

        commandList->setGraphicsState(state);
        commandList->draw(nvrhi::DrawArguments().setVertexCount(3).setInstanceCount(1));
        commandList->close();

        nvrhi::ICommandList* raw = commandList;
        rhi->executeCommandLists(&raw, 1);
        device->WaitForIdle();

        UInt32 rowPitchBytes = 0;
        const std::vector<UInt8> bytes = texture->ReadBack(*device, rowPitchBytes);
        REQUIRE_FALSE(bytes.empty());

        const auto* floats = reinterpret_cast<const float*>(bytes.data());
        const size_t perRow = rowPitchBytes / sizeof(float);
        const float* centre = floats + perRow * (kProbeSize / 2) + (kProbeSize / 2) * 4u;

        INFO("centre texel: R(slot0)=" << centre[0] << " G(slot2 count/16)=" << centre[1] << " B=" << centre[2]
                                       << " A=" << centre[3]);

        CHECK(centre[2] == 1.0f);                           // shader ran
        CHECK(centre[0] == doctest::Approx(0.25f));         // slot 0 delivered
        CHECK(centre[1] == doctest::Approx(7.0f / 16.0f));  // slot 2 delivered
    }
}

TEST_SUITE("Probe")
{
    TEST_CASE("real forward fragment shader exposes its light-loop internals")
    {
        GraphicsDevice* device = GetProbeDevice();
        if (device == nullptr)
        {
            WARN("no graphics device");
            return;
        }
        nvrhi::IDevice* rhi = device->GetNativeDevice();
        REQUIRE(rhi != nullptr);

        // Real engine shaders, with only the final write replaced by diagnostics:
        //   R = accumulated.r (the real answer)
        //   G = LightCount / 16
        //   B = first light's intensity as uploaded
        //   A = dot(normalShading, viewDirection) — proves varyings arrive sane
        std::string fragmentSource = BuildForwardFragmentGlslProbe();

        const std::string header = "#version 450\n" + BuildGlslBindingPrelude();
        std::string vertexSource = header + BuildForwardVertexGlslProbe();
        fragmentSource = header + fragmentSource;

        const CompiledShader compiledVertex = CompileGlsl(ShaderStage::Vertex, vertexSource);
        REQUIRE_MESSAGE(compiledVertex.Succeeded, compiledVertex.Diagnostics.substr(0, 400));
        const CompiledShader compiledFragment = CompileGlsl(ShaderStage::Pixel, fragmentSource);
        REQUIRE_MESSAGE(compiledFragment.Succeeded, compiledFragment.Diagnostics.substr(0, 600));

        // --- uniform data, mirroring the forward test -------------------------
        struct alignas(16) FrameU
        {
            float ViewProjection[16];
            float CameraPositionAndPad[4];
        };
        struct alignas(16) TransformU
        {
            float ModelMatrix[16];
            float NormalMatrix[16];
        };
        constexpr int kMaxLights = 8;
        struct alignas(16) LightsU
        {
            float PositionAndIntensity[kMaxLights][4];
            float ColorTypeAndRange[kMaxLights][4];
            float RangeAndSpotAngles[kMaxLights][4];
            float DirectionAndPad[kMaxLights][4];
        };
        struct alignas(16) MaterialU
        {
            float BaseColor[4];
            float MaterialParams[4];
        };
        struct alignas(16) IndicesU
        {
            int LightIndices[(kMaxLights + 3) / 4][4];
            int LightCount;
        };

        FrameU frame{};
        for (int i = 0; i < 4; ++i)
        {
            frame.ViewProjection[i * 4 + i] = 1.0f;  // identity projection: NDC passthrough
        }
        frame.CameraPositionAndPad[2] = 5.0f;

        TransformU transform{};
        for (int i = 0; i < 4; ++i)
        {
            transform.ModelMatrix[i * 4 + i] = 1.0f;
            transform.NormalMatrix[i * 4 + i] = 1.0f;
        }

        LightsU lights{};
        // One directional light pointing at -Z (toward the viewer-facing face), full intensity.
        lights.ColorTypeAndRange[0][0] = 1.0f;
        lights.ColorTypeAndRange[0][1] = 1.0f;
        lights.ColorTypeAndRange[0][2] = 1.0f;
        lights.ColorTypeAndRange[0][3] = 0.0f;  // type: directional
        lights.PositionAndIntensity[0][3] = 2.0f;
        lights.DirectionAndPad[0][2] = -1.0f;

        MaterialU material{};
        material.BaseColor[0] = 0.8f;
        material.BaseColor[1] = 0.4f;
        material.BaseColor[2] = 0.2f;
        material.MaterialParams[0] = 0.5f;
        material.MaterialParams[1] = 0.0f;

        IndicesU indices{};
        indices.LightIndices[0][0] = 0;
        indices.LightCount = 1;

        auto upload = [&](const char* name, const void* data, size_t size) -> Core::Ref<Buffer> {
            BufferDesc desc;
            desc.Name = name;
            desc.ByteSize = static_cast<UInt32>(size);
            desc.Usage = BufferUsage::Constant;
            Core::Ref<Buffer> buffer = Buffer::Create(*device, desc);
            if (buffer == nullptr)
            {
                return nullptr;
            }
            nvrhi::CommandListHandle commandList = rhi->createCommandList();
            commandList->open();
            commandList->writeBuffer(buffer->GetNativeBuffer(), data, size);
            commandList->close();
            nvrhi::ICommandList* raw = commandList;
            rhi->executeCommandLists(&raw, 1);
            device->WaitForIdle();
            return buffer;
        };

        Core::Ref<Buffer> bFrame = upload("f-frame", &frame, sizeof(frame));
        Core::Ref<Buffer> bTransform = upload("f-transform", &transform, sizeof(transform));
        Core::Ref<Buffer> bLights = upload("f-lights", &lights, sizeof(lights));
        Core::Ref<Buffer> bMaterial = upload("f-material", &material, sizeof(material));
        Core::Ref<Buffer> bIndices = upload("f-indices", &indices, sizeof(indices));
        REQUIRE(bFrame != nullptr);
        REQUIRE(bTransform != nullptr);
        REQUIRE(bLights != nullptr);
        REQUIRE(bMaterial != nullptr);
        REQUIRE(bIndices != nullptr);

        TextureDesc textureDesc;
        textureDesc.Name = "forward probe target";
        textureDesc.Width = kProbeSize;
        textureDesc.Height = kProbeSize;
        textureDesc.Format = GraphicsFormat::Rgba32Float;
        Core::Ref<Texture> texture = Texture::Create(*device, textureDesc);
        REQUIRE(texture != nullptr);

        RenderTargetDesc renderTargetDesc;
        renderTargetDesc.Colour = texture;
        Core::Ref<RenderTarget> renderTarget = RenderTarget::Create(*device, renderTargetDesc);
        REQUIRE(renderTarget != nullptr);

        PipelineDesc pipelineDesc;
        pipelineDesc.Name = "forward probe";
        pipelineDesc.VertexShader.SpirV = compiledVertex.SpirV;
        pipelineDesc.PixelShader.SpirV = compiledFragment.SpirV;
        for (UInt32 slot = 0; slot < 5; ++slot)
        {
            pipelineDesc.Bindings.push_back(BindingSlotDesc{BindingSlotDesc::Kind::ConstantBuffer, slot});
        }
        pipelineDesc.BindingsVisibleToVertex = true;
        pipelineDesc.BindingsVisibleToPixel = true;
        pipelineDesc.TriangleCulling = CullMode::None;
        pipelineDesc.IsDepthEnabled = false;
        pipelineDesc.VertexAttributes = {
            VertexAttribute{"InPosition", GraphicsFormat::Rg32Float, 0, 32},
            VertexAttribute{"InNormal", GraphicsFormat::Rgb32Float, 12, 32},
            VertexAttribute{"InUv", GraphicsFormat::Rg32Float, 24, 32},
        };

        Core::Ref<Pipeline> pipeline = Pipeline::Create(*device, pipelineDesc, *renderTarget);
        REQUIRE_MESSAGE(pipeline != nullptr, "forward probe pipeline failed");

        const nvrhi::BindingSetHandle bindingSet =
            rhi->createBindingSet(nvrhi::BindingSetDesc()
                                      .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, bFrame->GetNativeBuffer()))
                                      .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, bTransform->GetNativeBuffer()))
                                      .addItem(nvrhi::BindingSetItem::ConstantBuffer(2, bLights->GetNativeBuffer()))
                                      .addItem(nvrhi::BindingSetItem::ConstantBuffer(3, bMaterial->GetNativeBuffer()))
                                      .addItem(nvrhi::BindingSetItem::ConstantBuffer(4, bIndices->GetNativeBuffer())),
                                  pipeline->GetBindingLayout());
        REQUIRE(static_cast<bool>(bindingSet));

        // One full-screen triangle whose normal is +Z and position sits at the origin.
        struct Vertex
        {
            float Position[3];
            float Normal[3];
            float Uv[2];
        };
        static const Vertex kVerts[3] = {
            {{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
            {{3.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
            {{-1.0f, 3.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
        };

        BufferDesc vertexBufferDesc;
        vertexBufferDesc.Name = "forward probe vertices";
        vertexBufferDesc.ByteSize = sizeof(kVerts);
        vertexBufferDesc.Usage = BufferUsage::Vertex;
        Core::Ref<Buffer> vertexBuffer = Buffer::Create(*device, vertexBufferDesc);
        REQUIRE(vertexBuffer != nullptr);

        nvrhi::CommandListHandle commandList = rhi->createCommandList();
        commandList->open();
        commandList->writeBuffer(vertexBuffer->GetNativeBuffer(), kVerts, sizeof(kVerts));
        commandList->clearTextureFloat(
            texture->GetNativeTexture(), nvrhi::TextureSubresourceSet(),
            nvrhi::Color(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN(),
                         std::numeric_limits<float>::quiet_NaN(), 0.0f));

        nvrhi::GraphicsState state;
        state.setPipeline(pipeline->GetNativePipeline())
            .setFramebuffer(renderTarget->GetNativeFramebuffer())
            .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                nvrhi::Viewport(static_cast<float>(kProbeSize), static_cast<float>(kProbeSize))))
            .addVertexBuffer(nvrhi::VertexBufferBinding{vertexBuffer->GetNativeBuffer(), 0, 0})
            .addBindingSet(bindingSet);

        commandList->setGraphicsState(state);
        commandList->draw(nvrhi::DrawArguments().setVertexCount(3).setInstanceCount(1));
        commandList->close();

        nvrhi::ICommandList* raw = commandList;
        rhi->executeCommandLists(&raw, 1);
        device->WaitForIdle();

        UInt32 rowPitchBytes = 0;
        const std::vector<UInt8> bytes = texture->ReadBack(*device, rowPitchBytes);
        REQUIRE_FALSE(bytes.empty());

        const auto* floats = reinterpret_cast<const float*>(bytes.data());
        const size_t perRow = rowPitchBytes / sizeof(float);
        const float* centre = floats + perRow * (kProbeSize / 2) + (kProbeSize / 2) * 4u;

        INFO("R(accum)=" << centre[0] << " G(count/16)=" << centre[1] << " B(light0 intensity)=" << centre[2]
                         << " A(dotNV)=" << centre[3]);
        CHECK(centre[1] == doctest::Approx(1.0 / 16.0));  // LightCount reached the shader
        CHECK(centre[2] == doctest::Approx(2.0 / 16.0));  // intensity from the light array
        CHECK(centre[0] > 0.0f);                          // and the BRDF produced light
    }
}

TEST_SUITE("Probe")
{
    TEST_CASE("Vulkan framebuffer Y orientation: which readback rows does NDC +Y cover")
    {
        GraphicsDevice* device = GetProbeDevice();
        if (device == nullptr)
        {
            WARN("no graphics device");
            return;
        }
        nvrhi::IDevice* rhi = device->GetNativeDevice();
        REQUIRE(rhi != nullptr);

        // One quad covering exactly the NDC upper half (y in [0,1]), drawn as a constant green with no varyings —
        // an earlier version of this probe used per-vertex flat colours and its provoking vertex made every pixel
        // take one colour, so the red/green logic it "measured" was meaningless. Coverage alone answers the question:
        // whichever readback row band turns green is where NDC +Y landed. This decides the texel mapping used by
        // every position-asserting test in the suite (Vulkan's spec says y_f = (1 - y_ndc)/2 * height, i.e. NDC +Y
        // lands at framebuffer row 0 — but this machine has no validation layer and its RHI sits between us and the
        // spec, so it is measured here once and pinned).

        const std::string header = "#version 450\n";
        const std::string vertexSource = header + R"glsl(
layout(location = 0) in vec2 InPosition;
void main() { gl_Position = vec4(InPosition, 0.0, 1.0); }
)glsl";
        const std::string fragmentSource = header + R"glsl(
layout(location = 0) out vec4 OutColor;
void main() { OutColor = vec4(0.0, 1.0, 0.0, 1.0); }
)glsl";

        const CompiledShader compiledVertex = CompileGlsl(ShaderStage::Vertex, vertexSource);
        REQUIRE(compiledVertex.Succeeded);
        const CompiledShader compiledFragment = CompileGlsl(ShaderStage::Pixel, fragmentSource);
        REQUIRE(compiledFragment.Succeeded);

        TextureDesc textureDesc;
        textureDesc.Name = "orientation probe";
        textureDesc.Width = kProbeSize;
        textureDesc.Height = kProbeSize;
        textureDesc.Format = GraphicsFormat::Rgba32Float;
        Core::Ref<Texture> texture = Texture::Create(*device, textureDesc);
        REQUIRE(texture != nullptr);

        RenderTargetDesc renderTargetDesc;
        renderTargetDesc.Colour = texture;
        Core::Ref<RenderTarget> renderTarget = RenderTarget::Create(*device, renderTargetDesc);
        REQUIRE(renderTarget != nullptr);

        PipelineDesc pipelineDesc;
        pipelineDesc.Name = "orientation probe";
        pipelineDesc.VertexShader.SpirV = compiledVertex.SpirV;
        pipelineDesc.PixelShader.SpirV = compiledFragment.SpirV;
        pipelineDesc.VertexAttributes = {VertexAttribute{"InPosition", GraphicsFormat::Rg32Float, 0, 8}};
        pipelineDesc.TriangleCulling = CullMode::None;
        pipelineDesc.IsDepthEnabled = false;

        Core::Ref<Pipeline> pipeline = Pipeline::Create(*device, pipelineDesc, *renderTarget);
        REQUIRE(pipeline != nullptr);

        struct V
        {
            float X, Y;
        };
        static const V kUpperHalf[6] = {
            {-1.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f},  //
            {-1.0f, 0.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f},
        };

        BufferDesc vertexBufferDesc;
        vertexBufferDesc.ByteSize = sizeof(kUpperHalf);
        vertexBufferDesc.Usage = BufferUsage::Vertex;
        Core::Ref<Buffer> vertexBuffer = Buffer::Create(*device, vertexBufferDesc);
        REQUIRE(vertexBuffer != nullptr);

        nvrhi::CommandListHandle commandList = rhi->createCommandList();
        commandList->open();
        commandList->writeBuffer(vertexBuffer->GetNativeBuffer(), kUpperHalf, sizeof(kUpperHalf));
        // Clear to a colour that cannot be produced by the fragment shader (green only), so "covered vs not" is
        // unambiguous.
        commandList->clearTextureFloat(texture->GetNativeTexture(), nvrhi::TextureSubresourceSet(),
                                       nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
        nvrhi::GraphicsState state;
        state.setPipeline(pipeline->GetNativePipeline())
            .setFramebuffer(renderTarget->GetNativeFramebuffer())
            .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                nvrhi::Viewport(static_cast<float>(kProbeSize), static_cast<float>(kProbeSize))))
            .addVertexBuffer(nvrhi::VertexBufferBinding{vertexBuffer->GetNativeBuffer(), 0, 0});
        commandList->setGraphicsState(state);
        commandList->draw(nvrhi::DrawArguments().setVertexCount(6).setInstanceCount(1));
        commandList->close();

        nvrhi::ICommandList* raw = commandList;
        rhi->executeCommandLists(&raw, 1);
        device->WaitForIdle();

        UInt32 rowPitchBytes = 0;
        const std::vector<UInt8> bytes = texture->ReadBack(*device, rowPitchBytes);
        REQUIRE_FALSE(bytes.empty());

        const auto* floats = reinterpret_cast<const float*>(bytes.data());
        const size_t perRow = rowPitchBytes / sizeof(float);
        const auto greenAtRow = [&](UInt32 row) {
            const float* texel = floats + perRow * row + (kProbeSize / 2) * 4u;
            return texel[1] > 0.5f;
        };

        UInt32 firstGreenRow = kProbeSize;
        UInt32 lastGreenRow = 0;
        UInt32 greenCount = 0;
        for (UInt32 row = 0; row < kProbeSize; ++row)
        {
            if (greenAtRow(row))
            {
                firstGreenRow = std::min(firstGreenRow, row);
                lastGreenRow = std::max(lastGreenRow, row);
                ++greenCount;
            }
        }

        INFO("green spans readback rows [" << firstGreenRow << ", " << lastGreenRow << "] of [0, " << (kProbeSize - 1)
                                           << "]");

        // Half the image covered, and it must be exactly one contiguous half — anything else means coverage or
        // readback ordering is not what every mapping test assumes.
        CHECK(greenCount == kProbeSize / 2);
        const bool coversFirstHalf = firstGreenRow == 0u && lastGreenRow == kProbeSize / 2 - 1u;
        const bool coversSecondHalf = firstGreenRow == kProbeSize / 2 && lastGreenRow == kProbeSize - 1u;
        CHECK(coversFirstHalf != coversSecondHalf);

        // Vulkan spec: y_f = (1 - y_ndc)/2 * height, so NDC +Y lands at row 0 and readback is stored top-down. If a
        // future backend/upgrade inverts this, fail HERE — naming orientation — instead of letting every world→texel
        // assertion mirror silently.
        CHECK_MESSAGE(coversFirstHalf,
                      "NDC +Y did NOT map to the first readback rows: framebuffer orientation "
                      "changed from the Vulkan top-left convention this engine assumes");
    }
}
