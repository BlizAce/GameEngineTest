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
#include <cstring>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Graphics/Buffer.h"
#include "Hollowpoint/Graphics/GraphicsDevice.h"
#include "Hollowpoint/Graphics/Pipeline.h"
#include "Hollowpoint/Graphics/RenderTarget.h"
#include "Hollowpoint/Graphics/RhiAccess.h"  // command recording stays NVRHI's by design (see Buffer.h)
#include "Hollowpoint/Graphics/ShaderBindings.h"
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

    TEST_CASE("texture upload round-trips through the staging path")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN("No graphics device; upload needs one");
            return;
        }

        // RGBA32_FLOAT so the comparison is exact: half-float storage would make this test measure its own rounding.
        constexpr UInt32 kSize = 8u;
        TextureDesc desc;
        desc.Name = "roundtrip";
        desc.Width = kSize;
        desc.Height = kSize;
        desc.Format = GraphicsFormat::Rgba32Float;
        desc.IsShaderResource = true;  // what a sampled texture needs, and Update must not disturb it
        Core::Ref<Texture> texture = Texture::Create(*device, desc);
        REQUIRE(texture != nullptr);

        // Every texel distinct, so a row/column swap or a pitch error moves pixels rather than hiding. Values are all
        // exactly representable in half floats as well — the upload path copies bytes verbatim, but a test value that
        // survives both storage formats keeps this test valid if the default format ever changes under it.
        std::vector<float> texels(static_cast<size_t>(kSize) * kSize * 4u);
        for (UInt32 y = 0; y < kSize; ++y)
        {
            for (UInt32 x = 0; x < kSize; ++x)
            {
                float* texel = texels.data() + (static_cast<size_t>(y) * kSize + x) * 4u;
                texel[0] = static_cast<float>(x);
                texel[1] = static_cast<float>(y);
                texel[2] = static_cast<float>((x * 3u + y * 5u) % 17u);
                texel[3] = 1.0f;
            }
        }

        REQUIRE(texture->Update(*device, texels.data(), texels.size() * sizeof(float)));

        UInt32 rowPitchBytes = 0;
        const std::vector<UInt8> readBack = texture->ReadBack(*device, rowPitchBytes);
        REQUIRE_FALSE(readBack.empty());
        REQUIRE(rowPitchBytes >= kSize * 4u * sizeof(float));

        const auto* floats = reinterpret_cast<const float*>(readBack.data());
        const size_t perRow = rowPitchBytes / sizeof(float);
        for (UInt32 y = 0; y < kSize; ++y)
        {
            for (UInt32 x = 0; x < kSize; ++x)
            {
                const float* actual = floats + perRow * y + static_cast<size_t>(x) * 4u;
                const float* expected = texels.data() + (static_cast<size_t>(y) * kSize + x) * 4u;
                INFO("texel (" << x << "," << y << ")");
                // Exact: this path copies bytes through two stages with no arithmetic, so any difference is a bug —
                // pitch, state, or the copy region. Tolerances here would hide exactly what is being tested.
                CHECK(actual[0] == expected[0]);
                CHECK(actual[1] == expected[1]);
                CHECK(actual[2] == expected[2]);
                CHECK(actual[3] == expected[3]);
            }
        }

        // Still sampleable after the update: the path transitions to CopyDest and back, and a texture left in a
        // transfer layout fails later binds with an error that names neither this call nor the reason.
        UInt32 secondPitch = 0;
        CHECK_FALSE(texture->ReadBack(*device, secondPitch).empty());
    }

    TEST_CASE("texture upload honours a padded source pitch")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN("No graphics device; upload needs one");
            return;
        }

        // A 4x2 image whose rows carry 16 bytes of trailing junk — what an importer hands over when a file's rows are
        // aligned to something wider than the image. The wrapper must copy rowBytes per row and ignore the padding;
        // copying byteCount linearly instead produces a sheared image, which is the kind of bug that survives because
        // square power-of-two test images never have padding.
        constexpr UInt32 kWidth = 4u;
        constexpr UInt32 kHeight = 2u;
        constexpr UInt32 kTexelBytes = 16u;  // RGBA32_FLOAT
        constexpr UInt32 kPitch = kWidth * kTexelBytes + 16u;

        TextureDesc desc;
        desc.Name = "padded upload";
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.Format = GraphicsFormat::Rgba32Float;
        Core::Ref<Texture> texture = Texture::Create(*device, desc);
        REQUIRE(texture != nullptr);

        std::vector<unsigned char> padded(static_cast<size_t>(kPitch) * kHeight, 0xABu);  // junk in the padding
        for (UInt32 y = 0; y < kHeight; ++y)
        {
            for (UInt32 x = 0; x < kWidth; ++x)
            {
                auto* texel = padded.data() + static_cast<size_t>(kPitch) * y + static_cast<size_t>(kTexelBytes) * x;
                const float values[4] = {static_cast<float>(x), static_cast<float>(y), 0.5f, 1.0f};
                std::memcpy(texel, values, sizeof(values));
            }
        }

        REQUIRE(texture->Update(*device, padded.data(), padded.size(), kPitch));

        UInt32 rowPitchBytes = 0;
        const std::vector<UInt8> readBack = texture->ReadBack(*device, rowPitchBytes);
        REQUIRE_FALSE(readBack.empty());
        const auto* floats = reinterpret_cast<const float*>(readBack.data());
        const size_t perRow = rowPitchBytes / sizeof(float);

        for (UInt32 y = 0; y < kHeight; ++y)
        {
            for (UInt32 x = 0; x < kWidth; ++x)
            {
                const float* actual = floats + perRow * y + static_cast<size_t>(x) * 4u;
                INFO("texel (" << x << "," << y << ")");
                CHECK(actual[0] == static_cast<float>(x));
                CHECK(actual[1] == static_cast<float>(y));
                CHECK(actual[2] == 0.5f);
            }
        }
    }

    TEST_CASE("texture upload rejects a buffer that is too small")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN("No graphics device; upload needs one");
            return;
        }

        TextureDesc desc;
        desc.Name = "short upload";
        desc.Width = 16;
        desc.Height = 16;
        desc.Format = GraphicsFormat::Rgba32Float;
        Core::Ref<Texture> texture = Texture::Create(*device, desc);
        REQUIRE(texture != nullptr);

        // Half the bytes needed. Copying anyway reads past the caller's buffer — an ASan catch at best and a silently
        // half-uploaded texture at worst, which surfaces as a wrong image far from here.
        std::vector<float> tooSmall(16u * 4u, 1.0f);

        {
            Test::ScopedLogCapture logs;
            CHECK_FALSE(texture->Update(*device, tooSmall.data(), tooSmall.size() * sizeof(float)));
            CHECK(logs.AnyMessageContains("bytes supplied"));
        }

        // A pitch narrower than one row is rejected for the same reason: the copy would read overlapping rows and
        // smear rather than fail.
        std::vector<float> enoughBytes(16u * 16u * 4u, 1.0f);
        {
            Test::ScopedLogCapture logs;
            CHECK_FALSE(texture->Update(*device, enoughBytes.data(), enoughBytes.size() * sizeof(float), 8u));
            CHECK(logs.AnyMessageContains("narrower than one row"));
        }

        // Null pixels with a non-zero count is a caller bug worth catching rather than memcpy'ing from.
        {
            Test::ScopedLogCapture logs;
            CHECK_FALSE(texture->Update(*device, nullptr, 64u));
            CHECK(logs.AnyMessageContains("null pixels"));
        }
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

    TEST_CASE("a sampled-only texture has a defined image layout and survives upload, sampling and readback")
    {
        // Regression test. Texture::Create used to give every non-render-target texture an initial state of
        // ResourceStates::Common — which NVRHI's Vulkan backend maps to vk::ImageLayout::eUndefined. With
        // keepInitialState set (as it is for all engine textures), CommandList::close() emits a barrier back to that
        // initial state, so the first command list touching such a texture wrote a barrier whose new layout was
        // UNDEFINED: an assert in NVRHI debug builds, silent corruption otherwise. It stayed latent because every
        // existing test left IsRenderTarget at its default of true; the tone-map blit's sampled source was the first
        // non-RT texture created.
        //
        // The case therefore does all three things a sampled texture needs — upload into it, sample it from a shader,
        // read it back — because each transitions the layout and any one of them alone would pass with a broken initial
        // state.
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN("no graphics device");
            return;
        }

        constexpr UInt32 kSize = 4u;

        TextureDesc desc;
        desc.Name = "sampled-only";
        desc.Width = kSize;
        desc.Height = kSize;
        desc.Format = GraphicsFormat::Rgba32Float;
        desc.IsRenderTarget = false;  // the configuration that used to produce an undefined layout
        desc.IsShaderResource = true;
        Core::Ref<Texture> texture = Texture::Create(*device, desc);
        REQUIRE(texture != nullptr);

        std::vector<float> texels(static_cast<size_t>(kSize) * kSize * 4u, 0.0f);
        for (UInt32 y = 0; y < kSize; ++y)
        {
            for (UInt32 x = 0; x < kSize; ++x)
            {
                float* texel = texels.data() + (static_cast<size_t>(y) * kSize + x) * 4u;
                texel[0] = static_cast<float>(x) / 4.0f;
                texel[1] = static_cast<float>(y) / 4.0f;
                texel[2] = 0.5f;
                texel[3] = 1.0f;
            }
        }

        // Upload: transitions to CopyDest and back.
        REQUIRE(texture->Update(*device, texels.data(), texels.size() * sizeof(float)));

        // Sample it from a real fragment shader into a render target. This is the step that reproduces the original
        // bug: the barrier emitted at close() targets the initial state, so it fires here rather than at upload.
        nvrhi::IDevice* rhi = device->GetNativeDevice();
        REQUIRE(rhi != nullptr);

        const std::string header = "#version 450\n" + BuildGlslBindingPrelude();
        // A full-screen triangle plus a texel-exact sample: uv comes from gl_FragCoord so texel (x,y) of the output
        // reads texel (x,y) of the source under nearest filtering, which is what makes the assertion below
        // discriminating. The binding macros come from BuildGlslBindingPrelude rather than numeric bindings — a
        // hand-written number is the silent-zero failure ShaderBindings.h exists to prevent.
        const CompiledShader vertex =
            CompileGlsl(ShaderStage::Vertex,
                        header +
                            "void main(){\n"
                            "  vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
                            "  gl_Position = vec4(corner * 2.0f - 1.0f, 0.0f, 1.0f);\n"
                            "}");
        REQUIRE_MESSAGE(vertex.Succeeded, vertex.Diagnostics);

        // The extent is a literal rather than textureSize(): querying a bare texture2D needs
        // GL_EXT_samplerless_texture_functions, and requesting an extension to save writing one number into the source
        // would make this test depend on driver feature support for no benefit.
        const std::string extent = std::to_string(kSize) + ".0, " + std::to_string(kSize) + ".0";
        const CompiledShader fragment = CompileGlsl(
            ShaderStage::Pixel, header +
                                    "layout(binding = HP_BINDING_TEXTURE(0)) uniform texture2D SourceTexture;\n"
                                    "layout(binding = HP_BINDING_SAMPLER(0)) uniform sampler SourceSampler;\n"
                                    "layout(location = 0) out vec4 OutColour;\n"
                                    "void main(){\n"
                                    "  const vec2 kExtent = vec2(" +
                                    extent +
                                    ");\n"
                                    "  vec2 uv = (floor(gl_FragCoord.xy) + vec2(0.5)) / kExtent;\n"
                                    "  OutColour = texture(sampler2D(SourceTexture, SourceSampler), uv);\n"
                                    "}");
        REQUIRE_MESSAGE(fragment.Succeeded, fragment.Diagnostics);

        TextureDesc targetDesc;
        targetDesc.Name = "sampled-only-target";
        targetDesc.Width = kSize;
        targetDesc.Height = kSize;
        targetDesc.Format = GraphicsFormat::Rgba32Float;
        Core::Ref<Texture> colour = Texture::Create(*device, targetDesc);
        REQUIRE(colour != nullptr);

        RenderTargetDesc framebufferDesc;
        framebufferDesc.Name = "sampled-only-framebuffer";
        framebufferDesc.Colour = colour;
        Core::Ref<RenderTarget> target = RenderTarget::Create(*device, framebufferDesc);
        REQUIRE(target != nullptr);

        PipelineDesc pipelineDesc;
        pipelineDesc.Name = "sampled-only-pipeline";
        pipelineDesc.VertexShader.SpirV = vertex.SpirV;
        pipelineDesc.PixelShader.SpirV = fragment.SpirV;
        pipelineDesc.Bindings = {
            BindingSlotDesc{BindingSlotDesc::Kind::SampledTexture, 0},
            BindingSlotDesc{BindingSlotDesc::Kind::Sampler, 0},
        };
        pipelineDesc.BindingsVisibleToPixel = true;
        pipelineDesc.IsDepthEnabled = false;
        pipelineDesc.TriangleCulling = CullMode::None;

        Core::Ref<Pipeline> pipeline = Pipeline::Create(*device, pipelineDesc, *target);
        REQUIRE(pipeline != nullptr);

        nvrhi::SamplerDesc samplerDesc;
        samplerDesc.setAllFilters(false);
        samplerDesc.addressU = nvrhi::SamplerAddressMode::Clamp;
        samplerDesc.addressV = nvrhi::SamplerAddressMode::Clamp;
        const nvrhi::SamplerHandle sampler = rhi->createSampler(samplerDesc);
        REQUIRE(static_cast<bool>(sampler));

        const nvrhi::BindingSetHandle bindingSet =
            rhi->createBindingSet(nvrhi::BindingSetDesc()
                                      .addItem(nvrhi::BindingSetItem::Texture_SRV(0, texture->GetNativeTexture()))
                                      .addItem(nvrhi::BindingSetItem::Sampler(0, sampler)),
                                  pipeline->GetBindingLayout());
        REQUIRE(static_cast<bool>(bindingSet));

        nvrhi::CommandListHandle commandList = rhi->createCommandList();
        commandList->open();
        commandList->clearTextureFloat(colour->GetNativeTexture(), nvrhi::TextureSubresourceSet(),
                                       nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));

        nvrhi::GraphicsState state;
        state.setPipeline(pipeline->GetNativePipeline())
            .setFramebuffer(target->GetNativeFramebuffer())
            .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                nvrhi::Viewport(static_cast<float>(kSize), static_cast<float>(kSize))))
            .addBindingSet(bindingSet);
        commandList->setGraphicsState(state);
        commandList->draw(nvrhi::DrawArguments().setVertexCount(3).setInstanceCount(1));
        commandList->close();

        nvrhi::ICommandList* raw = commandList;
        rhi->executeCommandLists(&raw, 1);
        device->WaitForIdle();

        // Readback: the third transition. Texel (3,3) is the far corner and the most discriminating — a layout problem
        // typically leaves everything but the sampled region at the clear value.
        UInt32 rowPitchBytes = 0;
        const std::vector<UInt8> output = colour->ReadBack(*device, rowPitchBytes);
        REQUIRE_FALSE(output.empty());

        const auto* floats = reinterpret_cast<const float*>(output.data());
        const Size perRow = rowPitchBytes / sizeof(float);
        const float* texelOut = floats + perRow * (kSize - 1u) + static_cast<Size>(kSize - 1u) * 4u;

        INFO("sampled texel " << texelOut[0] << ", " << texelOut[1] << ", " << texelOut[2] << ", " << texelOut[3]);
        // Nearest sampling of a same-size target: the value must match the uploaded texel, which also proves the upload
        // landed in the layout the shader then read.
        CHECK(texelOut[0] == doctest::Approx(0.75f).epsilon(0.02));
        CHECK(texelOut[1] == doctest::Approx(0.75f).epsilon(0.02));
        CHECK(texelOut[3] == doctest::Approx(1.0f));

        // A non-render-target texture with no usage at all has no legal layout to be created in, and says so rather
        // than producing a resource that cannot be touched.
        {
            TextureDesc unusable;
            unusable.Name = "no-usage";
            unusable.Width = 4;
            unusable.Height = 4;
            unusable.Format = GraphicsFormat::Rgba32Float;
            unusable.IsRenderTarget = false;
            unusable.IsShaderResource = false;

            // Refused with a logged reason rather than asserted: nothing about the arguments is malformed (a zero-size
            // texture asserts), this is a configuration that simply cannot be honoured, and an editor creating textures
            // from author data must get a null back rather than a dialog.
            Test::ScopedLogCapture logs;
            CHECK(Texture::Create(*device, unusable) == nullptr);
            CHECK(logs.GetErrorCount() == 1);
            CHECK(logs.AnyMessageContains("no usage"));
        }
    }
}
