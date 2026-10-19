// A full draw: compile GLSL, build a graphics pipeline, rasterise a triangle into an offscreen target, and assert on
// the pixels that come back. This is the test that turns "the renderer probably works" into an observation — every
// later stage (PBR, shadows, SSAO, tonemapping) is judged by exactly this mechanism, so it is established here rather
// than assumed.
//
// GPU-touching tests live in their own executables; see tests/CMakeLists.txt for why leak checking is off for them on
// this host.

#include "TestSupport.h"

#include <array>
#include <cstdint>
#include <string>

#include <doctest/doctest.h>

#include "Hollowpoint/Graphics/RhiAccess.h"
#include "Hollowpoint/Graphics/ShaderCompiler.h"

using namespace Hollowpoint::Graphics;

namespace
{

//! One Vulkan device per process: creation costs ~100 ms and several tests need it.
[[nodiscard]] GraphicsDevice* GetDevice()
{
    static GraphicsDevice* device = []() -> GraphicsDevice* {
        GraphicsDeviceDesc desc;
        desc.ApplicationName = "Hollowpoint draw tests";
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

//! Edges extend well past the target so rasterisation covers every pixel, and each vertex carries a different colour
//! so interpolation is observable rather than a flat fill.
constexpr std::array<Vertex, 3> kVertices = {{
    {{0.0f, -1.4f}, {1.0f, 0.0f, 0.0f}},
    {{-1.4f, 1.4f}, {0.0f, 1.0f, 0.0f}},
    {{1.4f, 1.4f}, {0.0f, 0.0f, 1.0f}},
}};

//! A clear colour no interpolation of pure R/G/B can produce, so "nothing drew" is distinguishable from "drew the
//! wrong thing".
constexpr float kClearR = 0.1f;
constexpr float kClearG = 0.2f;
constexpr float kClearB = 0.9f;

constexpr uint32_t kSize = 16;

[[nodiscard]] const uint8_t* TexelAt(const uint8_t* pixels, size_t rowPitch, uint32_t x, uint32_t y)
{
    return pixels + static_cast<size_t>(rowPitch) * y + static_cast<size_t>(x) * 4u;
}

[[nodiscard]] bool IsClearColor(const uint8_t* texel)
{
    // UNORM8 of (0.1, 0.2, 0.9) is roughly (26, 51, 230); tolerance covers rounding mode differences.
    return texel[0] < 40 && texel[1] > 35 && texel[1] < 70 && texel[2] > 200;
}

}  // namespace

TEST_SUITE("Graphics/Draw")
{
    TEST_CASE("a triangle rasterises into an offscreen render target")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN((std::string("No graphics device on this host: ") + GraphicsDevice::GetFailureReason()).c_str());
            return;
        }

        nvrhi::IDevice* rhi = device->GetNativeDevice();
        REQUIRE(rhi != nullptr);

        // --- Compile the shaders --------------------------------------------
        const CompiledShader vs = CompileGlsl(ShaderStage::Vertex, kTriangleVertex);
        INFO(vs.Diagnostics);
        REQUIRE(vs.Succeeded);

        const CompiledShader ps = CompileGlsl(ShaderStage::Pixel, kTriangleFragment);
        INFO(ps.Diagnostics);
        REQUIRE(ps.Succeeded);

        nvrhi::ShaderHandle vertexShader =
            rhi->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex), vs.SpirV.data(),
                              vs.SpirV.size() * sizeof(UInt32));
        REQUIRE(static_cast<bool>(vertexShader));

        nvrhi::ShaderHandle pixelShader = rhi->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel),
                                                            ps.SpirV.data(), ps.SpirV.size() * sizeof(UInt32));
        REQUIRE(static_cast<bool>(pixelShader));

        // --- Input layout ---------------------------------------------------
        // elementStride must be set explicitly and must agree across attributes sharing a buffer slot: the default
        // of 0 makes every vertex read element zero, i.e. a degenerate zero-area triangle that rasterises nothing
        // and reports no error (observed: the whole target stayed the clear colour). NVRHI asserts on a mismatch
        // between attributes in the same slot, so this is stated once per attribute.
        const std::array<nvrhi::VertexAttributeDesc, 2> attributes = {{
            nvrhi::VertexAttributeDesc()
                .setName("InPosition")
                .setFormat(nvrhi::Format::RG32_FLOAT)
                .setOffset(0)
                .setElementStride(sizeof(Vertex))
                .setBufferIndex(0),
            nvrhi::VertexAttributeDesc()
                .setName("InColor")
                .setFormat(nvrhi::Format::RGB32_FLOAT)
                .setOffset(sizeof(float) * 2)
                .setElementStride(sizeof(Vertex))
                .setBufferIndex(0),
        }};

        nvrhi::InputLayoutHandle inputLayout = rhi->createInputLayout(attributes.data(), 2, vertexShader);
        REQUIRE(static_cast<bool>(inputLayout));

        // --- Vertex buffer --------------------------------------------------
        nvrhi::BufferDesc vertexBufferDesc;
        vertexBufferDesc.debugName = "draw-test vertices";
        vertexBufferDesc.byteSize = kVertices.size() * sizeof(Vertex);
        vertexBufferDesc.isVertexBuffer = true;
        vertexBufferDesc.initialState = nvrhi::ResourceStates::CopyDest;
        vertexBufferDesc.keepInitialState = true;

        nvrhi::BufferHandle vertexBuffer = rhi->createBuffer(vertexBufferDesc);
        REQUIRE(static_cast<bool>(vertexBuffer));

        // --- Render target --------------------------------------------------
        nvrhi::TextureDesc targetDesc;
        targetDesc.debugName = "draw-test target";
        targetDesc.format = nvrhi::Format::RGBA8_UNORM;
        targetDesc.width = kSize;
        targetDesc.height = kSize;
        targetDesc.dimension = nvrhi::TextureDimension::Texture2D;
        targetDesc.isRenderTarget = true;
        targetDesc.initialState = nvrhi::ResourceStates::RenderTarget;
        targetDesc.keepInitialState = true;

        nvrhi::TextureHandle target = rhi->createTexture(targetDesc);
        REQUIRE(static_cast<bool>(target));

        // --- Pipeline -------------------------------------------------------
        const nvrhi::FramebufferDesc framebufferDesc = nvrhi::FramebufferDesc().addColorAttachment(target);
        const nvrhi::FramebufferInfo framebufferInfo(framebufferDesc);

        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.setPrimType(nvrhi::PrimitiveType::TriangleList)
            .setInputLayout(inputLayout)
            .setVertexShader(vertexShader)
            .setPixelShader(pixelShader);

        // Culling disabled so the result does not depend on which winding a Y-down framebuffer treats as
        // front-facing. Investigated as a candidate cause of an empty frame and ruled out (Back culling still draws
        // this triangle), but stated explicitly anyway: relying on the default makes the test's meaning depend on a
        // library default that could change under it.
        nvrhi::RasterState raster;
        raster.cullMode = nvrhi::RasterCullMode::None;
        pipelineDesc.setRenderState(nvrhi::RenderState().setRasterState(raster));

        nvrhi::GraphicsPipelineHandle pipeline = rhi->createGraphicsPipeline(pipelineDesc, framebufferInfo);
        REQUIRE_MESSAGE(static_cast<bool>(pipeline), "Pipeline creation failed");

        nvrhi::FramebufferHandle framebuffer = rhi->createFramebuffer(framebufferDesc);
        REQUIRE(static_cast<bool>(framebuffer));

        // --- Record and submit ---------------------------------------------
        nvrhi::CommandListHandle commandList = rhi->createCommandList();
        REQUIRE(static_cast<bool>(commandList));

        commandList->open();

        // writeBuffer stages and copies internally; this NVRHI revision has no staging-buffer handle type.
        commandList->writeBuffer(vertexBuffer, kVertices.data(), kVertices.size() * sizeof(Vertex));
        commandList->clearTextureFloat(target, nvrhi::TextureSubresourceSet(),
                                       nvrhi::Color(kClearR, kClearG, kClearB, 1.0f));

        // This NVRHI revision is state-based: no beginRenderPass. Pipeline, framebuffer, viewport and vertex buffers
        // are assembled into a GraphicsState submitted immediately before draw().
        const nvrhi::Viewport viewport(static_cast<float>(kSize), static_cast<float>(kSize));

        nvrhi::GraphicsState state;
        state.setPipeline(pipeline)
            .setFramebuffer(framebuffer)
            .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(viewport))
            .addVertexBuffer(nvrhi::VertexBufferBinding{vertexBuffer, 0, 0});

        commandList->setGraphicsState(state);
        commandList->draw(nvrhi::DrawArguments().setVertexCount(3).setInstanceCount(1));

        // Readback needs an explicit state transition: NVRHI tracks resource state and an untracked use is a
        // validation error rather than something silently fixed up.
        nvrhi::StagingTextureHandle readback = rhi->createStagingTexture(targetDesc, nvrhi::CpuAccessMode::Read);
        REQUIRE(static_cast<bool>(readback));

        commandList->setTextureState(target, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
        commandList->copyTexture(readback, nvrhi::TextureSlice(), target, nvrhi::TextureSlice());
        commandList->close();

        nvrhi::ICommandList* rawCommandList = commandList;
        rhi->executeCommandLists(&rawCommandList, 1);
        device->WaitForIdle();

        // --- Inspect the pixels --------------------------------------------
        size_t rowPitch = 0;
        void* mapped = rhi->mapStagingTexture(readback, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch);
        REQUIRE(mapped != nullptr);

        const uint8_t* pixels = static_cast<const uint8_t*>(mapped);
        const uint8_t* centre = TexelAt(pixels, rowPitch, kSize / 2, kSize / 2);

        INFO("centre texel: " << static_cast<int>(centre[0]) << " " << static_cast<int>(centre[1]) << " "
                              << static_cast<int>(centre[2]));

        // The centre lies inside the triangle, so it must have been overwritten. Comparing against the clear colour
        // (rather than an exact expected blend) keeps the test robust to a driver's interpolation and sRGB handling
        // while still failing decisively when nothing rasterised — which is the regression that matters here.
        const bool centreOverwritten = !IsClearColor(centre);
        CHECK(centreOverwritten);

        // Alpha comes straight from the fragment shader's literal 1.0, so it is asserted exactly: a wrong alpha
        // would silently break every later blend operation.
        CHECK(centre[3] == 255);

        // Colour must vary across a row. A single flat colour would mean the vertex colours never reached the
        // fragment shader (e.g. all vertices reading element zero), which the centre check alone cannot detect.
        const uint8_t* left = TexelAt(pixels, rowPitch, 2, 4);
        const uint8_t* right = TexelAt(pixels, rowPitch, kSize - 3, 4);
        const bool varies = left[0] != right[0] || left[1] != right[1] || left[2] != right[2];
        CHECK(varies);

        // Row pitch must be at least one row of texels; a smaller pitch corrupts every readback beyond row 0 while
        // still returning plausible pixels at offset 0.
        CHECK(rowPitch >= kSize * 4u);

        rhi->unmapStagingTexture(readback);
    }
}
