// The Graphics resource layer under test: engine-level Buffer/Texture/RenderTarget/Pipeline, with no raw NVRHI
// resource creation anywhere in the test. This is what makes M1 more than a compile: a triangle drawn through the
// wrappers and read back through Texture::ReadBack proves the flag/state choices those wrappers make are correct —
// every one of them exists because getting it wrong was silent (see their headers).
//
// The pixel assertions deliberately mirror TestDraw.cpp's triangle (same vertices, same clear colour): if the two
// ever disagree, either the wrappers changed behaviour or the reference test did, and both are news.

#include "TestSupport.h"

#include <array>
#include <cstdint>
#include <string>

#include <doctest/doctest.h>

#include "Hollowpoint/Graphics/Buffer.h"
#include "Hollowpoint/Graphics/GraphicsDevice.h"
#include "Hollowpoint/Graphics/Pipeline.h"
#include "Hollowpoint/Graphics/RenderTarget.h"
#include "Hollowpoint/Graphics/RhiAccess.h"  // command recording stays NVRHI's by design (see Buffer.h)
#include "Hollowpoint/Graphics/ShaderCompiler.h"
#include "Hollowpoint/Graphics/Texture.h"

using namespace Hollowpoint;
using namespace Hollowpoint::Graphics;

namespace
{

[[nodiscard]] GraphicsDevice* GetDevice()
{
    static GraphicsDevice* device = []() -> GraphicsDevice* {
        GraphicsDeviceDesc desc;
        desc.ApplicationName = "Hollowpoint resource tests";
        return GraphicsDevice::Create(desc);
    }();

    return device;
}

constexpr const char* kTriangleVertex = R"glsl(#version 450
layout(location = 0) in vec2 InPosition;
layout(location = 1) in vec3 InColor;
layout(location = 0) out vec3 OutColor;
void main() {
    OutColor = InColor;
    gl_Position = vec4(InPosition, 0.0, 1.0);
}
)glsl";

constexpr const char* kTriangleFragment = R"glsl(#version 450
layout(location = 0) in vec3 InColor;
layout(location = 0) out vec4 OutColor;
void main() { OutColor = vec4(InColor, 1.0); }
)glsl";

struct Vertex
{
    float Position[2];
    float Color[3];
};

constexpr std::array<Vertex, 3> kVertices = {{
    {{0.0f, -1.4f}, {1.0f, 0.0f, 0.0f}},
    {{-1.4f, 1.4f}, {0.0f, 1.0f, 0.0f}},
    {{1.4f, 1.4f}, {0.0f, 0.0f, 1.0f}},
}};

//! Same unmixable clear colour as TestDraw: no interpolation of R/G/B vertices produces it, so "nothing drew" stays
//! distinguishable from "drew the wrong thing".
constexpr float kClearR = 0.1f;
constexpr float kClearG = 0.2f;
constexpr float kClearB = 0.9f;

constexpr UInt32 kSize = 16;

}  // namespace

TEST_SUITE("Graphics/Resources")
{
    TEST_CASE("a triangle drawn through the resource wrappers rasterises and reads back")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN((std::string("No graphics device on this host: ") + GraphicsDevice::GetFailureReason()).c_str());
            return;
        }

        nvrhi::IDevice* rhi = device->GetNativeDevice();
        REQUIRE(rhi != nullptr);

        // --- Shaders (existing compiler layer) --------------------------------
        const CompiledShader vs = CompileGlsl(ShaderStage::Vertex, kTriangleVertex);
        INFO(vs.Diagnostics);
        REQUIRE(vs.Succeeded);

        const CompiledShader ps = CompileGlsl(ShaderStage::Pixel, kTriangleFragment);
        INFO(ps.Diagnostics);
        REQUIRE(ps.Succeeded);

        // --- Colour + depth textures through the wrapper ----------------------
        TextureDesc colourDesc;
        colourDesc.Name = "resource-test colour";
        colourDesc.Width = kSize;
        colourDesc.Height = kSize;
        colourDesc.Format = GraphicsFormat::Rgba8Unorm;
        Core::Ref<Texture> colour = Texture::Create(*device, colourDesc);
        REQUIRE(colour != nullptr);
        CHECK(colour->IsValid());

        TextureDesc depthDesc;
        depthDesc.Name = "resource-test depth";
        depthDesc.Width = kSize;
        depthDesc.Height = kSize;
        depthDesc.Format = GraphicsFormat::D32Float;
        Core::Ref<Texture> depth = Texture::Create(*device, depthDesc);
        REQUIRE(depth != nullptr);

        // --- Framebuffer: both attachments, sizes matched ----------------------
        RenderTargetDesc targetDesc;
        targetDesc.Name = "resource-test target";
        targetDesc.Colour = colour;
        targetDesc.Depth = depth;
        Core::Ref<RenderTarget> target = RenderTarget::Create(*device, targetDesc);
        REQUIRE(target != nullptr);
        CHECK(target->HasDepth());
        CHECK(target->GetWidth() == kSize);
        CHECK(target->GetHeight() == kSize);

        // --- Pipeline paired to that target ------------------------------------
        PipelineDesc pipelineDesc;
        pipelineDesc.Name = "resource-test triangle";
        pipelineDesc.VertexShader.SpirV = vs.SpirV;
        pipelineDesc.VertexShader.Name = "resource-test vs";
        pipelineDesc.PixelShader.SpirV = ps.SpirV;
        pipelineDesc.PixelShader.Name = "resource-test ps";
        pipelineDesc.VertexAttributes = {
            // Formats must match the GLSL input types: `in vec2` reads Rg32Float, `in vec3` reads Rgb32Float. A
            // 16-bit format here reads garbage with no error — the same class of silent mismatch as stride 0.
            VertexAttribute{"InPosition", GraphicsFormat::Rg32Float, 0, sizeof(Vertex)},
            VertexAttribute{"InColor", GraphicsFormat::Rgb32Float, sizeof(float) * 2, sizeof(Vertex)},
        };
        // Same reasoning as TestDraw: culling off so the result cannot depend on which winding a Y-down framebuffer
        // treats as front-facing.
        pipelineDesc.TriangleCulling = CullMode::None;

        Core::Ref<Pipeline> pipeline = Pipeline::Create(*device, pipelineDesc, *target);
        REQUIRE_MESSAGE(pipeline != nullptr, "Pipeline creation failed through the wrapper");
        CHECK(pipeline->IsValid());

        // --- Vertex buffer + upload --------------------------------------------
        BufferDesc vertexBufferDesc;
        vertexBufferDesc.Name = "resource-test vertices";
        vertexBufferDesc.ByteSize = kVertices.size() * sizeof(Vertex);
        vertexBufferDesc.Usage = BufferUsage::Vertex;
        Core::Ref<Buffer> vertexBuffer = Buffer::Create(*device, vertexBufferDesc);
        REQUIRE(vertexBuffer != nullptr);

        nvrhi::CommandListHandle commandList = rhi->createCommandList();
        REQUIRE(static_cast<bool>(commandList));
        commandList->open();

        // Upload through the RHI directly: recording is deliberately NVRHI's territory (Buffer.h documents why).
        commandList->writeBuffer(vertexBuffer->GetNativeBuffer(), kVertices.data(), kVertices.size() * sizeof(Vertex));
        commandList->clearTextureFloat(colour->GetNativeTexture(), nvrhi::TextureSubresourceSet(),
                                       nvrhi::Color(kClearR, kClearG, kClearB, 1.0f));

        // Depth must be cleared before anything tests against it: attachment initial contents are undefined per
        // Vulkan, so a LessEqual test against undefined depth is driver luck — green on one run or one GPU and
        // failing on another. Clear to far (1.0); the triangle sits at z = 0 and must then always pass.
        commandList->clearDepthStencilTexture(depth->GetNativeTexture(), nvrhi::AllSubresources, true, 1.0f, false, 0);

        const nvrhi::Viewport viewport(static_cast<float>(kSize), static_cast<float>(kSize));

        nvrhi::GraphicsState state;
        state.setPipeline(pipeline->GetNativePipeline())
            .setFramebuffer(target->GetNativeFramebuffer())
            .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(viewport))
            .addVertexBuffer(nvrhi::VertexBufferBinding{vertexBuffer->GetNativeBuffer(), 0, 0});

        commandList->setGraphicsState(state);
        commandList->draw(nvrhi::DrawArguments().setVertexCount(3).setInstanceCount(1));
        commandList->close();

        nvrhi::ICommandList* raw = commandList;
        rhi->executeCommandLists(&raw, 1);
        device->WaitForIdle();

        // --- Read back through the wrapper's helper -----------------------------
        UInt32 rowPitch = 0;
        std::vector<UInt8> pixels = colour->ReadBack(*device, rowPitch);
        REQUIRE_FALSE(pixels.empty());
        CHECK(rowPitch >= kSize * 4u);

        const auto texelAt = [&](UInt32 x, UInt32 y) -> const UInt8* {
            return pixels.data() + static_cast<size_t>(rowPitch) * y + static_cast<size_t>(x) * 4u;
        };

        const UInt8* centre = texelAt(kSize / 2, kSize / 2);
        INFO("centre texel: " << static_cast<int>(centre[0]) << " " << static_cast<int>(centre[1]) << " "
                              << static_cast<int>(centre[2]));

        // Centre is inside the triangle: must not still be the clear colour (clear-ish means nothing rasterised —
        // the exact failure every silent flag mistake produces).
        const bool centreOverwritten = !(centre[0] < 40 && centre[1] > 35 && centre[1] < 70 && centre[2] > 200);
        CHECK(centreOverwritten);

        // Fragment shader writes alpha as a literal 1.0: exact, because a wrong alpha breaks every later blend.
        CHECK(centre[3] == 255);

        // Colour must vary across the triangle: flat would mean vertex attributes never reached the shader —
        // e.g. an element stride of zero (the silent-degenerate-mesh trap the wrappers assert against).
        const UInt8* left = texelAt(2, 4);
        const UInt8* right = texelAt(kSize - 3, 4);
        const bool varies = left[0] != right[0] || left[1] != right[1] || left[2] != right[2];
        CHECK(varies);
    }

    TEST_CASE("wrappers reject invalid configurations instead of creating broken resources")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN("No graphics device; wrapper validation paths need one to create anything valid");
            return;
        }

        // Depth attachment smaller than colour: Vulkan requires matching sizes, and the wrapper catches it with a
        // readable error rather than an opaque createFramebuffer failure.
        TextureDesc colourDesc;
        colourDesc.Name = "mismatch-test colour";
        colourDesc.Width = 16;
        colourDesc.Height = 16;
        colourDesc.Format = GraphicsFormat::Rgba8Unorm;
        Core::Ref<Texture> colour = Texture::Create(*device, colourDesc);
        REQUIRE(colour != nullptr);

        TextureDesc depthDesc;
        depthDesc.Name = "mismatch-test depth";
        depthDesc.Width = 8;  // deliberately wrong
        depthDesc.Height = 8;
        depthDesc.Format = GraphicsFormat::D32Float;
        Core::Ref<Texture> depth = Texture::Create(*device, depthDesc);
        REQUIRE(depth != nullptr);

        RenderTargetDesc targetDesc;
        targetDesc.Name = "mismatch-test";
        targetDesc.Colour = colour;
        targetDesc.Depth = depth;

        {
            Test::ScopedAssertCapture asserts;  // HP_ASSERT_ARGUMENT fires on the programmer-error path
            Test::ScopedLogCapture logs;
            Core::Ref<RenderTarget> bad = RenderTarget::Create(*device, targetDesc);
            CHECK(bad == nullptr);
            // The assert must be followed by a refusal (assertion = report, never guard), and the log is what an
            // editor surfaces to a user — both halves are part of the contract.
            CHECK(asserts.GetCaptureCount() >= 1);
            CHECK(logs.AnyMessageContains("attachments must match"));
        }
    }
}
