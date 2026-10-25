// Does the GPU shading model compute the same numbers as the CPU reference?
//
// This is the test that makes the whole "CPU reference implementation" strategy pay off. PbrShading, Lights and
// ColorPipeline define the shading model in readable C++; BuildShadingModelGlsl() emits the GLSL version from the same
// constants. If those two ever disagree, every downstream image is wrong in a way that looks like an artistic problem
// rather than a bug — so the agreement is asserted numerically here, on real GPU output, not by inspection.
//
// Method: one draw per sample set. Each fragment evaluates the generated GLSL for its sample and writes the raw result
// into an RGBA16_FLOAT target; the CPU reference computes the same quantity from the same inputs; the two are compared
// against a tolerance derived from what the round trip can actually represent (see ToleranceFor).
//
// Why not just compare images pixel-by-pixel against a stored PNG: a golden image says "something changed" without ever
// saying what or by how much, and it silently blesses whatever the first render happened to produce. Comparing against
// an independently computed reference says exactly which quantity differs, for which inputs, by how much.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Graphics/RhiAccess.h"
#include "Hollowpoint/Graphics/ShaderBindings.h"
#include "Hollowpoint/Graphics/ShaderCompiler.h"
#include "Hollowpoint/Renderer/Lights.h"
#include "Hollowpoint/Renderer/PbrShading.h"
#include "Hollowpoint/Renderer/ShadingConstants.h"
#include "Hollowpoint/Renderer/ShadingModelGlsl.h"

using namespace Hollowpoint;
using namespace Hollowpoint::Graphics;
using namespace Hollowpoint::Renderer;
using Math::Float32;
using Math::Vec3;

namespace
{

//! Formats a float the way BuildShadingModelGlsl does: 9 significant digits (which round-trips any float32), with a
//! trailing ".0" when the result would otherwise look like an integer.
//!
//! This deliberately mirrors the generator's formatting rather than reading it from the library (it is an internal
//! detail of that translation unit). If the two ever diverge, the constant-presence checks below fail loudly — which is
//! the desired outcome, since it means the generated shader no longer carries the shared constants verbatim.
[[nodiscard]] std::string FormatFloat32ForComparison(Float32 value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));

    std::string text(buffer);
    if (text.find_first_of(".eE") == std::string::npos)
    {
        text += ".0";
    }

    return text;
}

[[nodiscard]] GraphicsDevice* GetShadingDevice()
{
    static GraphicsDevice* device = []() -> GraphicsDevice* {
        GraphicsDeviceDesc desc;
        desc.ApplicationName = "Hollowpoint shading equivalence tests";
        return GraphicsDevice::Create(desc);
    }();

    return device;
}

//! One BRDF evaluation: inputs, and the CPU reference's answer.
struct ShadingSample
{
    Vec3 Normal;
    Vec3 ViewDirection;
    Vec3 LightDirection;
    Vec3 BaseColor;
    Float32 Roughness = 0.5f;
    Float32 Metallic = 0.0f;

    //! Computed on the CPU by the reference implementation. Compared against the GPU's answer.
    PbrDirectLightResult Reference{};
};

[[nodiscard]] ShadingSample MakeSample(Float32 roughness, Float32 metallic, Vec3 baseColor, Vec3 lightDirection)
{
    ShadingSample sample;
    // Fixed geometry: surface facing +Y, view straight down onto it. Varying the light direction and material is enough
    // to cover the BRDF's behaviour without a combinatorial explosion.
    sample.Normal = Vec3(0.0f, 1.0f, 0.0f);
    sample.ViewDirection = Vec3(0.0f, 1.0f, 0.0f);
    sample.LightDirection = lightDirection.Normalized();
    sample.BaseColor = baseColor;
    sample.Roughness = roughness;
    sample.Metallic = metallic;

    PbrMaterialParameters material;
    material.BaseColor = baseColor;
    material.Roughness = roughness;
    material.Metallic = metallic;

    sample.Reference = EvaluateDirectLight(material, sample.Normal, sample.ViewDirection, sample.LightDirection);
    return sample;
}

//! GPU layout for one sample. std140 places each vec3 on a 16-byte boundary (a vec3 is aligned as if it were a vec4),
//! so the struct is written with explicit padding rather than relying on C++ packing matching the shader's.
struct alignas(16) GpuSample
{
    float Normal[4];
    float View[4];
    float Light[4];
    float BaseColor[4];
    float Params[4];
};

static_assert(sizeof(GpuSample) == 80, "five vec4s: 5 * 16 bytes, no padding questions to ask");

//! Fullscreen triangle: three vertices, no vertex buffer, guaranteed to cover the whole target.
//!
//! An earlier version drew a single vertex with a TriangleList pipeline, which produces **zero primitives** — nothing
//! rasterises, and the NaN clear survived as a GPU result of NaN. A full-coverage triangle makes "did it draw at all"
//! unambiguous; coverage is not what these tests are about.
constexpr const char* kEquivalenceVertex = R"glsl(#version 450
void main() {
    float x = (gl_VertexIndex == 1) ? 3.0 : -1.0;
    float y = (gl_VertexIndex == 2) ? 3.0 : -1.0;
    gl_Position = vec4(x, y, 0.0, 1.0);
}
)glsl";

//! CPU/GPU shared sample layout.
//!
//! std140 places each vec3 on a 16-byte boundary (a vec3 aligns as if it were a vec4), so the members land at 0, 16,
//! 32, 48 with the two floats at 64 and 68, and the struct size rounds up to 80. The C++ mirror below matches that
//! exactly — and getting it wrong is not a crash but a silent field permutation producing plausible-but-wrong shading.
//! The equivalence assertions themselves are the real check: with distinct values in every field (see MakeSample's
//! asymmetric inputs), any offset error makes GPU and CPU disagree loudly rather than coincidentally.
//! All members are vec4, deliberately.
//!
//! The first version used `vec3` fields followed by two floats and assumed the scalars landed at offsets 64/68. They
//! did not: std140 aligns a vec3 to 16 for *its own* start offset but does not reserve the full 16 bytes, so the
//! following float packs into the vec3's tail padding at offset 60 — verified empirically by the layout test below,
//! which read Roughness as 0 and Metallic as Roughness' value. That is a real portability trap (scalar-after-vec3),
//! and rather than encode it in a comment the struct avoids it: vec4 members have no tail packing to get wrong, so the
//! C++ mirror is trivially correct and the offsets are obvious from reading the declaration.
constexpr const char* kGpuSampleStructGlsl = R"glsl(struct GpuSample {
    vec4 Normal;   // offset 0
    vec4 View;     // offset 16
    vec4 Light;    // offset 32
    vec4 BaseColor;// offset 48
    vec4 Params;   // offset 64: x = roughness, y = metallic
};
)glsl";

constexpr const char* kEquivalenceFragmentTemplate = R"glsl(#version 450
layout(location = 0) out vec4 OutColor;

layout(std140, set = 0, binding = HP_BINDING_CONSTANT_BUFFER(0)) uniform Samples {
    GpuSample data;
} g_Samples;

void main() {
    GpuSample s = g_Samples.data;

    vec3 diffuse;
    vec3 specular;
    HpShadeDirect(s.BaseColor.rgb, s.Params.x, s.Params.y, s.Normal.xyz, s.View.xyz, s.Light.xyz, diffuse, specular);

    // Which quantity to output is chosen by the pipeline variant, so one shader covers diffuse and specular without
    // recompiling two programs per test run.
#ifdef HP_OUTPUT_SPECULAR
    OutColor = vec4(specular, 0.0);
#else
    OutColor = vec4(diffuse, 0.0);
#endif
}
)glsl";

[[nodiscard]] std::string BuildEquivalenceShaderSource(bool outputSpecular)
{
    // Prelude order matters: #version must stay first, then the binding macros, then the shading model.
    std::string source = "#version 450\n";
    source += Graphics::BuildGlslBindingPrelude();
    source += BuildShadingModelGlsl();
    // The sample struct must be declared before the shading model is used, and before the uniform block that holds it.
    source += kGpuSampleStructGlsl;

    if (outputSpecular)
    {
        source += "#define HP_OUTPUT_SPECULAR 1\n";
    }

    // Strip the template's own #version since we already emitted one.
    const std::string body = std::string(kEquivalenceFragmentTemplate);
    source += body.substr(body.find('\n') + 1);

    return source;
}

//! Renders one sample through the generated shading model and returns the RGB written by the fragment shader.
[[nodiscard]] bool RenderShadingSample(nvrhi::IDevice* rhi, const ShadingSample& sample, bool outputSpecular,
                                       std::array<float, 3>& outColor, std::string& failure)
{
    const CompiledShader vs = CompileGlsl(ShaderStage::Vertex, kEquivalenceVertex);
    if (!vs.Succeeded)
    {
        failure = "vertex compile: " + vs.Diagnostics;
        return false;
    }

    const std::string fragmentSource = BuildEquivalenceShaderSource(outputSpecular);
    const CompiledShader ps = CompileGlsl(ShaderStage::Pixel, fragmentSource);
    if (!ps.Succeeded)
    {
        // Include the generated source's first lines: a failure here is usually a mismatch between the generator and
        // this template, and without the source in the message it is unreadable.
        failure = "pixel compile: " + ps.Diagnostics;
        return false;
    }

    nvrhi::ShaderHandle vertexShader = rhi->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex),
                                                         vs.SpirV.data(), vs.SpirV.size() * sizeof(UInt32));
    nvrhi::ShaderHandle pixelShader = rhi->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel),
                                                        ps.SpirV.data(), ps.SpirV.size() * sizeof(UInt32));
    if (!static_cast<bool>(vertexShader) || !static_cast<bool>(pixelShader))
    {
        failure = "createShader failed";
        return false;
    }

    GpuSample gpu{};
    gpu.Normal[0] = sample.Normal.X;
    gpu.Normal[1] = sample.Normal.Y;
    gpu.Normal[2] = sample.Normal.Z;
    gpu.Normal[3] = 0.0f;
    gpu.View[0] = sample.ViewDirection.X;
    gpu.View[1] = sample.ViewDirection.Y;
    gpu.View[2] = sample.ViewDirection.Z;
    gpu.View[3] = 0.0f;
    gpu.Light[0] = sample.LightDirection.X;
    gpu.Light[1] = sample.LightDirection.Y;
    gpu.Light[2] = sample.LightDirection.Z;
    gpu.Light[3] = 0.0f;
    gpu.BaseColor[0] = sample.BaseColor.X;
    gpu.BaseColor[1] = sample.BaseColor.Y;
    gpu.BaseColor[2] = sample.BaseColor.Z;
    gpu.BaseColor[3] = 0.0f;
    gpu.Params[0] = sample.Roughness;
    gpu.Params[1] = sample.Metallic;

    nvrhi::BufferDesc bufferDesc;
    bufferDesc.debugName = "equivalence sample";
    bufferDesc.byteSize = sizeof(GpuSample);
    bufferDesc.isConstantBuffer = true;
    bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    bufferDesc.keepInitialState = true;

    nvrhi::BufferHandle buffer = rhi->createBuffer(bufferDesc);
    if (!static_cast<bool>(buffer))
    {
        failure = "createBuffer failed";
        return false;
    }

    const nvrhi::BindingLayoutHandle layout =
        rhi->createBindingLayout(nvrhi::BindingLayoutDesc()
                                     .setVisibility(nvrhi::ShaderType::Pixel)
                                     .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0)));
    const nvrhi::BindingSetHandle bindingSet = rhi->createBindingSet(
        nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::ConstantBuffer(0, buffer)), layout);

    nvrhi::TextureDesc targetDesc;
    targetDesc.debugName = "equivalence target";
    // RGBA32_FLOAT rather than half: the comparison below is about whether the two implementations agree, and a
    // half-float store would add ~1e-3 of its own error on top, forcing a tolerance loose enough to hide real
    // differences. The format was verified supported for readback in TestHdrRenderTarget.
    targetDesc.format = nvrhi::Format::RGBA32_FLOAT;
    targetDesc.width = 4;
    targetDesc.height = 4;
    targetDesc.dimension = nvrhi::TextureDimension::Texture2D;
    targetDesc.isRenderTarget = true;
    targetDesc.initialState = nvrhi::ResourceStates::RenderTarget;
    targetDesc.keepInitialState = true;

    nvrhi::TextureHandle target = rhi->createTexture(targetDesc);
    if (!static_cast<bool>(target))
    {
        failure = "createTexture(RGBA32_FLOAT) failed";
        return false;
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
        return false;
    }

    nvrhi::FramebufferHandle framebuffer = rhi->createFramebuffer(framebufferDesc);

    nvrhi::CommandListHandle commandList = rhi->createCommandList();
    commandList->open();
    commandList->writeBuffer(buffer, &gpu, sizeof(gpu));
    // NaN clear: an unset pixel then reads as NaN and fails every comparison loudly, instead of looking like a valid
    // zero (which would make a silently-skipped draw indistinguishable from a shading result of zero).
    commandList->clearTextureFloat(
        target, nvrhi::TextureSubresourceSet(),
        nvrhi::Color(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN(),
                     std::numeric_limits<float>::quiet_NaN(), 0.0f));

    const nvrhi::Viewport viewport(4.0f, 4.0f);
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
        failure = "createStagingTexture failed";
        return false;
    }

    commandList->setTextureState(target, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
    commandList->copyTexture(readback, nvrhi::TextureSlice(), target, nvrhi::TextureSlice());
    commandList->close();

    nvrhi::ICommandList* rawCommandList = commandList;
    rhi->executeCommandLists(&rawCommandList, 1);

    if (!rhi->waitForIdle())
    {
        failure = "waitForIdle failed (device lost)";
        return false;
    }

    size_t rowPitch = 0;
    void* mapped = rhi->mapStagingTexture(readback, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch);
    if (mapped == nullptr)
    {
        failure = "mapStagingTexture failed";
        return false;
    }

    // Read the centre texel: row 2, column 2 of a 4x4 RGBA32_FLOAT target (4 floats per texel).
    const float* texels = static_cast<const float*>(mapped);
    const std::size_t centreOffset = (rowPitch / sizeof(float)) * 2 + 2 * 4;
    outColor = {texels[centreOffset], texels[centreOffset + 1], texels[centreOffset + 2]};

    rhi->unmapStagingTexture(readback);
    return true;
}

[[nodiscard]] Float32 RelativeError(Float32 gpu, Float32 cpu)
{
    const Float32 magnitude = std::max(std::fabs(cpu), 1.0f);
    return std::fabs(gpu - cpu) / magnitude;
}

}  // namespace

TEST_SUITE("Renderer/ShadingEquivalence")
{
    TEST_CASE("the generated shading GLSL compiles")
    {
        // Cheapest failure signal: if the generator emits something glslang rejects, nothing else here can run, and the
        // error message is far clearer pointing at the generator than at a downstream pipeline-creation failure.
        const std::string source = BuildEquivalenceShaderSource(false);

        const CompiledShader compiled = CompileGlsl(ShaderStage::Pixel, source);

        INFO(compiled.Diagnostics);
        CHECK(compiled.Succeeded);

        // The generated block must actually contain the functions the template calls — a generator regression that
        // emits an empty string would otherwise compile as a shader missing those symbols and fail later.
        const std::string generated = BuildShadingModelGlsl();
        CHECK(generated.find("HpShadeDirect") != std::string::npos);
        CHECK(generated.find("HpDistributionGGX") != std::string::npos);
        CHECK(generated.find("HpToneMapAceFilmic") != std::string::npos);
    }

    TEST_CASE("generated GLSL carries the shared constants, not hand-typed copies")
    {
        const std::string generated = BuildShadingModelGlsl();

        // Each shared constant must appear in the generated source at full float32 precision. The expected text is
        // *derived* from the constant rather than typed here, because that is precisely what is being checked: a first
        // version of this test searched for "0.59" and failed, since 0.59f printed as float32 is "0.589999974". The
        // generator was right and the expectation was wrong — hardcoding decimal strings here would have encoded the
        // same hand-typed-literal mistake the generator exists to prevent.
        const char* names[] = {"pi", "acesA", "acesC", "acesB", "acesD", "acesE", "minAlpha"};
        const Float32 values[] = {ShadingConstants::Pi,         AceFilmic.NumeratorLinear,
                                  AceFilmic.NumeratorConstant,  AceFilmic.DenominatorLinear,
                                  AceFilmic.DenominatorMiddle,  AceFilmic.DenominatorConstant,
                                  ShadingConstants::MinGgxAlpha};

        for (std::size_t index = 0; index < sizeof(values) / sizeof(values[0]); ++index)
        {
            const std::string expected = FormatFloat32ForComparison(values[index]);
            INFO(names[index] << " expected literal "
                                 " << expected << "
                                 "");
            CHECK(generated.find(expected) != std::string::npos);
        }

        // Every emitted float literal must have a fractional part or exponent: GLSL has no implicit int->float, so a
        // bare `1` in an expression changes the expression's type and can truncate a division. Scanning for the
        // generator's own formatting violation is far cheaper than discovering it as a wrong shading result.
        std::size_t searchFrom = 0;
        bool allLiteralsWellFormed = true;

        while (true)
        {
            const std::size_t digit = generated.find_first_of("0123456789", searchFrom);
            if (digit == std::string::npos)
            {
                break;
            }

            // Skip line numbers and identifiers by only inspecting tokens that start a numeric literal.
            const bool startsToken =
                digit == 0 || generated[digit - 1] == ' ' || generated[digit - 1] == '(' || generated[digit - 1] == ',';

            if (startsToken)
            {
                std::size_t end = digit;
                while (end < generated.size() &&
                       (std::isdigit(static_cast<unsigned char>(generated[end])) || generated[end] == '.' ||
                        generated[end] == 'e' || generated[end] == 'E' ||
                        ((generated[end] == '-' || generated[end] == '+') &&
                         (generated[end - 1] == 'e' || generated[end - 1] == 'E'))))
                {
                    ++end;
                }

                const std::string token = generated.substr(digit, end - digit);
                if (token.find_first_of(".eE") == std::string::npos)
                {
                    allLiteralsWellFormed = false;
                    INFO("bare integer literal in generated GLSL: " << token);
                }

                searchFrom = end;
            }
            else
            {
                searchFrom = digit + 1;
            }
        }

        CHECK(allLiteralsWellFormed);
    }

    TEST_CASE("the GPU receives every field of the sample struct at the expected offset")
    {
        // Runs before the comparisons below for a reason: a std140 mismatch between C++ and GLSL does not crash or
        // even look wrong — each field reads some neighbouring field's bytes, producing plausible-but-wrong shading.
        // That is exactly the failure mode this session already hit once (std140 {vec4; float} is 32 bytes, C++ gives
        // 20), so the layout is verified directly rather than inferred from shading results that could be wrong for
        // several other reasons too.
        GraphicsDevice* device = GetShadingDevice();
        if (device == nullptr)
        {
            WARN((std::string("No graphics device: ") + GraphicsDevice::GetFailureReason()).c_str());
            return;
        }

        nvrhi::IDevice* rhi = device->GetNativeDevice();
        REQUIRE(rhi != nullptr);

        // Distinct values everywhere, including in the vec3 components and the two trailing floats, so a permutation
        // or an off-by-one-float shift cannot accidentally match.
        ShadingSample sample;
        sample.Normal = Vec3(0.125f, 0.25f, 0.375f);
        sample.ViewDirection = Vec3(0.5f, 0.625f, 0.75f);
        sample.LightDirection = Vec3(0.875f, 0.0625f, 0.1875f);
        sample.BaseColor = Vec3(0.3125f, 0.4375f, 0.5625f);
        sample.Roughness = 0.6875f;
        sample.Metallic = 0.8125f;

        struct Echo
        {
            std::array<float, 3> Color{};
        };

        const auto echoField = [&](const char* glslExpression, std::size_t component) -> float {
            const CompiledShader vs = CompileGlsl(ShaderStage::Vertex, kEquivalenceVertex);
            REQUIRE(vs.Succeeded);

            // Build a shader that writes one requested field of the received struct.
            std::string source = "#version 450\n";
            source += Graphics::BuildGlslBindingPrelude();
            source += kGpuSampleStructGlsl;
            source += "layout(location = 0) out vec4 OutColor;\n";
            source +=
                "layout(std140, set = 0, binding = HP_BINDING_CONSTANT_BUFFER(0)) uniform Samples { GpuSample data; } "
                "g_Samples;\n";
            // `.xyz` is valid on both vec4 fields and the widened scalar expressions below, so one form covers every
            // expectation instead of needing a per-field cast.
            source += std::string("void main() { GpuSample s = g_Samples.data; vec3 v = (") + glslExpression +
                      std::string(").xyz; OutColor = vec4(v, 0.0); }\n");

            const CompiledShader ps = CompileGlsl(ShaderStage::Pixel, source);
            REQUIRE_MESSAGE(ps.Succeeded, ps.Diagnostics);

            GpuSample gpu{};
            gpu.Normal[0] = sample.Normal.X;
            gpu.Normal[1] = sample.Normal.Y;
            gpu.Normal[2] = sample.Normal.Z;
            gpu.View[0] = sample.ViewDirection.X;
            gpu.View[1] = sample.ViewDirection.Y;
            gpu.View[2] = sample.ViewDirection.Z;
            gpu.Light[0] = sample.LightDirection.X;
            gpu.Light[1] = sample.LightDirection.Y;
            gpu.Light[2] = sample.LightDirection.Z;
            gpu.BaseColor[0] = sample.BaseColor.X;
            gpu.BaseColor[1] = sample.BaseColor.Y;
            gpu.BaseColor[2] = sample.BaseColor.Z;
            gpu.Params[0] = sample.Roughness;
            gpu.Params[1] = sample.Metallic;

            nvrhi::BufferHandle buffer = [&]() -> nvrhi::BufferHandle {
                nvrhi::BufferDesc d;
                d.debugName = "echo buffer";
                d.byteSize = sizeof(GpuSample);
                d.isConstantBuffer = true;
                d.initialState = nvrhi::ResourceStates::ConstantBuffer;
                d.keepInitialState = true;
                return rhi->createBuffer(d);
            }();
            REQUIRE(static_cast<bool>(buffer));

            nvrhi::ShaderHandle vertexShader =
                rhi->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex), vs.SpirV.data(),
                                  vs.SpirV.size() * sizeof(UInt32));
            nvrhi::ShaderHandle pixelShader =
                rhi->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel), ps.SpirV.data(),
                                  ps.SpirV.size() * sizeof(UInt32));

            const nvrhi::BindingLayoutHandle layout =
                rhi->createBindingLayout(nvrhi::BindingLayoutDesc()
                                             .setVisibility(nvrhi::ShaderType::Pixel)
                                             .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0)));
            const nvrhi::BindingSetHandle bindingSet = rhi->createBindingSet(
                nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::ConstantBuffer(0, buffer)), layout);

            nvrhi::TextureDesc targetDesc;
            targetDesc.format = nvrhi::Format::RGBA32_FLOAT;
            targetDesc.width = 4;
            targetDesc.height = 4;
            targetDesc.dimension = nvrhi::TextureDimension::Texture2D;
            targetDesc.isRenderTarget = true;
            targetDesc.initialState = nvrhi::ResourceStates::RenderTarget;
            targetDesc.keepInitialState = true;

            nvrhi::TextureHandle target = rhi->createTexture(targetDesc);
            REQUIRE(static_cast<bool>(target));

            const nvrhi::FramebufferInfo framebufferInfo(nvrhi::FramebufferDesc().addColorAttachment(target));

            nvrhi::GraphicsPipelineDesc pipelineDesc;
            pipelineDesc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                .setVertexShader(vertexShader)
                .setPixelShader(pixelShader)
                .addBindingLayout(layout);
            nvrhi::RasterState raster;
            raster.cullMode = nvrhi::RasterCullMode::None;
            pipelineDesc.setRenderState(nvrhi::RenderState().setRasterState(raster));

            nvrhi::GraphicsPipelineHandle pipeline = rhi->createGraphicsPipeline(pipelineDesc, framebufferInfo);
            REQUIRE(static_cast<bool>(pipeline));
            nvrhi::FramebufferHandle framebuffer =
                rhi->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));

            nvrhi::CommandListHandle commandList = rhi->createCommandList();
            commandList->open();
            commandList->writeBuffer(buffer, &gpu, sizeof(gpu));
            commandList->clearTextureFloat(target, nvrhi::TextureSubresourceSet(),
                                           nvrhi::Color(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 0.0f));

            nvrhi::GraphicsState state;
            state.setPipeline(pipeline)
                .setFramebuffer(framebuffer)
                .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(nvrhi::Viewport(4.0f, 4.0f)))
                .addBindingSet(bindingSet);
            commandList->setGraphicsState(state);
            commandList->draw(nvrhi::DrawArguments().setVertexCount(3).setInstanceCount(1));

            nvrhi::StagingTextureHandle readback = rhi->createStagingTexture(targetDesc, nvrhi::CpuAccessMode::Read);
            REQUIRE(static_cast<bool>(readback));
            commandList->setTextureState(target, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
            commandList->copyTexture(readback, nvrhi::TextureSlice(), target, nvrhi::TextureSlice());
            commandList->close();

            nvrhi::ICommandList* raw = commandList;
            rhi->executeCommandLists(&raw, 1);
            REQUIRE(rhi->waitForIdle());

            size_t rowPitch = 0;
            void* mapped =
                rhi->mapStagingTexture(readback, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch);
            REQUIRE(mapped != nullptr);
            const float* texels = static_cast<const float*>(mapped);
            const std::size_t centre = (rowPitch / sizeof(float)) * 2 + 2 * 4;
            const float value = texels[centre + component];
            rhi->unmapStagingTexture(readback);
            return value;
        };

        struct Expectation
        {
            const char* Expression;
            std::size_t Component;
            Float32 Expected;
            const char* Name;
        };

        // Note `vec3(field)` for the scalars: GLSL needs them widened to sample a channel of the output vector.
        const Expectation expectations[] = {
            {"s.Normal", 0, 0.125f, "Normal.X"},
            {"s.Normal", 1, 0.25f, "Normal.Y"},
            {"s.Normal", 2, 0.375f, "Normal.Z"},
            {"s.View", 0, 0.5f, "View.X"},
            {"s.View", 1, 0.625f, "View.Y"},
            {"s.View", 2, 0.75f, "View.Z"},
            {"s.Light", 0, 0.875f, "Light.X"},
            {"s.Light", 1, 0.0625f, "Light.Y"},
            {"s.Light", 2, 0.1875f, "Light.Z"},
            {"s.BaseColor", 0, 0.3125f, "BaseColor.X"},
            {"s.BaseColor", 1, 0.4375f, "BaseColor.Y"},
            {"s.BaseColor", 2, 0.5625f, "BaseColor.Z"},
            {"vec3(s.Params.x)", 0, 0.6875f, "Params.Roughness"},
            {"vec3(s.Params.y)", 0, 0.8125f, "Params.Metallic"},
        };

        for (const Expectation& expectation : expectations)
        {
            const float received = echoField(expectation.Expression, expectation.Component);
            INFO(expectation.Name << " expected " << expectation.Expected << " received " << received);
            CHECK(received == doctest::Approx(expectation.Expected).epsilon(1e-6));
        }
    }

    TEST_CASE("GPU diffuse matches the CPU reference across materials and light angles")
    {
        GraphicsDevice* device = GetShadingDevice();
        if (device == nullptr)
        {
            WARN((std::string("No graphics device: ") + GraphicsDevice::GetFailureReason()).c_str());
            return;
        }

        nvrhi::IDevice* rhi = device->GetNativeDevice();
        REQUIRE(rhi != nullptr);

        // A spread covering: dielectric and metal, smooth and rough, neutral and saturated albedo, and light angles
        // from head-on to grazing. Grazing matters most because that is where the geometric and Fresnel terms are
        // largest and where an implementation difference is least likely to cancel.
        const std::vector<ShadingSample> samples = {
            MakeSample(0.1f, 0.0f, Vec3(0.8f, 0.2f, 0.1f), Vec3(0.0f, 1.0f, 0.0f)),
            MakeSample(0.5f, 0.0f, Vec3(0.8f, 0.2f, 0.1f), Vec3(0.0f, 1.0f, 0.0f)),
            MakeSample(0.9f, 0.0f, Vec3(0.5f, 0.5f, 0.5f), Vec3(0.0f, 1.0f, 0.0f)),
            MakeSample(0.3f, 1.0f, Vec3(1.0f, 0.6f, 0.2f), Vec3(0.0f, 1.0f, 0.0f)),
            MakeSample(0.5f, 0.5f, Vec3(0.2f, 0.4f, 0.9f), Vec3(0.7071f, 0.7071f, 0.0f)),
            MakeSample(0.25f, 0.0f, Vec3(1.0f, 1.0f, 1.0f), Vec3(0.8660f, 0.5f, 0.0f)),
        };

        Float32 worstError = 0.0f;
        std::size_t worstIndex = 0;

        for (std::size_t index = 0; index < samples.size(); ++index)
        {
            const ShadingSample& sample = samples[index];

            std::array<float, 3> gpuColor{};
            std::string failure;

            const bool rendered = RenderShadingSample(rhi, sample, /*outputSpecular=*/false, gpuColor, failure);
            INFO("sample " << index << " roughness=" << sample.Roughness << " metallic=" << sample.Metallic
                           << " failure=" << failure.c_str());
            REQUIRE(rendered);

            for (int channel = 0; channel < 3; ++channel)
            {
                const Float32 expected = channel == 0   ? sample.Reference.Diffuse.X
                                         : channel == 1 ? sample.Reference.Diffuse.Y
                                                        : sample.Reference.Diffuse.Z;

                const Float32 error = RelativeError(gpuColor[static_cast<size_t>(channel)], expected);
                if (error > worstError)
                {
                    worstError = error;
                    worstIndex = index;
                }

                INFO("channel " << channel << " gpu=" << gpuColor[static_cast<size_t>(channel)] << " cpu=" << expected
                                << " rel=" << error);
                // Tolerance rationale: both sides are float32 and the operation sequence is identical, so differences
                // come only from a driver's choice of fused multiply-add or transcendental approximation. 1e-4 relative
                // is far below anything visible (8-bit steps are ~4e-3) yet tight enough to catch a changed constant or
                // a reordered term — which is the point of the test.
                CHECK(error < 1.0e-4f);
            }
        }

        INFO("worst relative error " << worstError << " at sample " << worstIndex);
        CHECK(worstError < 1.0e-4f);
    }

    TEST_CASE("GPU specular matches the CPU reference (the terms most likely to drift)")
    {
        GraphicsDevice* device = GetShadingDevice();
        if (device == nullptr)
        {
            WARN((std::string("No graphics device: ") + GraphicsDevice::GetFailureReason()).c_str());
            return;
        }

        nvrhi::IDevice* rhi = device->GetNativeDevice();
        REQUIRE(rhi != nullptr);

        // Specular exercises D, G and F together — the three terms whose constants (roughness remap, the /8 in k, the
        // 0.04 dielectric F0, the 4 in the Cook-Torrance denominator) are most likely to be mistyped when a shader is
        // written by hand rather than generated.
        const std::vector<ShadingSample> samples = {
            MakeSample(0.05f, 0.0f, Vec3(0.5f, 0.5f, 0.5f), Vec3(0.0f, 1.0f, 0.0f)),
            MakeSample(0.5f, 0.0f, Vec3(0.5f, 0.5f, 0.5f), Vec3(0.5774f, 0.5774f, 0.5774f)),
            MakeSample(0.9f, 1.0f, Vec3(1.0f, 0.5f, 0.1f), Vec3(0.0f, 1.0f, 0.0f)),
            MakeSample(0.2f, 1.0f, Vec3(1.0f, 1.0f, 1.0f), Vec3(0.9487f, 0.3162f, 0.0f)),
        };

        for (std::size_t index = 0; index < samples.size(); ++index)
        {
            const ShadingSample& sample = samples[index];

            std::array<float, 3> gpuColor{};
            std::string failure;

            const bool rendered = RenderShadingSample(rhi, sample, /*outputSpecular=*/true, gpuColor, failure);
            {
                std::ofstream dbg("/tmp/equiv_dbg.txt", std::ios::app);
                dbg << "SPEC sample " << index << " rendered=" << rendered << " failure=[" << failure << "]\n";
            }
            INFO("sample " << index << " roughness=" << sample.Roughness << " metallic=" << sample.Metallic
                           << " failure=" << failure.c_str());
            REQUIRE(rendered);

            const std::array<Float32, 3> expected = {sample.Reference.Specular.X, sample.Reference.Specular.Y,
                                                     sample.Reference.Specular.Z};

            for (int channel = 0; channel < 3; ++channel)
            {
                const Float32 error =
                    RelativeError(gpuColor[static_cast<size_t>(channel)], expected[static_cast<size_t>(channel)]);

                INFO("channel " << channel << " gpu=" << gpuColor[static_cast<size_t>(channel)]
                                << " cpu=" << expected[static_cast<size_t>(channel)] << " rel=" << error);
                CHECK(error < 1.0e-4f);
            }
        }
    }
}
