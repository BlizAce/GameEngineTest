// End-to-end: a .hpscene file on disk → CollectRenderables → a real GPU draw → pixels compared to the CPU reference.
//
// Why this exists when ShadingEquivalenceTests already compares GPU output to ComputeForwardShadingCpu: those tests
// hand-write their own shading source and feed it hand-built vectors. They prove the BRDF agrees between C++ and GLSL.
// They do NOT run BuildForwardVertexGlsl() with real geometry, so nothing in the engine had ever checked that the
// generated forward shaders — as shipped — transform a cube, deliver matrices at the offsets the upload code uses, and
// produce lighting for a scene described in JSON. That seam (M2's plan feeding M3's pixels) is where a wrong matrix
// stride, a mismatched light-type encoding, or a dropped model matrix would live, and every one of them reads as "the
// scene renders black" rather than naming itself.
//
// The test therefore uses the engine's own shaders unmodified (no echo shader, no rewritten output), real indexed cube
// geometry with depth testing and back-face culling on, a camera built from the scene's CameraComponent, and lights
// collected from LightComponents — then samples one pixel whose world position it computes analytically.
//
// Deliberate limitation, stated rather than hidden: this validates the flat-face path only. The CPU reference does not
// interpolate normals (see ForwardRender.h), so the sample point is placed at the centre of a cube face, where the
// interpolated normal equals the face normal and CPU/GPU agreement is exact rather than approximate. Curved surfaces
// need per-vertex interpolation on the CPU side, which arrives with real mesh loading.

#include "TestSupport.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Core/RefCounted.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/ECS/World.h"
#include "Hollowpoint/Game/ForwardShaderLayout.h"
#include "Hollowpoint/Game/MeshRenderer.h"
#include "Hollowpoint/Game/RegisterGameTypes.h"
#include "Hollowpoint/Game/RenderPrimitives.h"
#include "Hollowpoint/Graphics/Buffer.h"
#include "Hollowpoint/Graphics/GraphicsDevice.h"
#include "Hollowpoint/Graphics/GraphicsTypes.h"
#include "Hollowpoint/Graphics/Pipeline.h"
#include "Hollowpoint/Graphics/RenderTarget.h"
#include "Hollowpoint/Graphics/RhiAccess.h"
#include "Hollowpoint/Graphics/ShaderBindings.h"
#include "Hollowpoint/Graphics/ShaderCompiler.h"
#include "Hollowpoint/Graphics/Texture.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Renderer/Camera.h"
#include "Hollowpoint/Renderer/ForwardRender.h"
#include "Hollowpoint/Renderer/Geometry.h"
#include "Hollowpoint/Renderer/Lights.h"
#include "Hollowpoint/Scene/LocalTransform.h"
#include "Hollowpoint/Scene/SceneIO.h"
#include "Hollowpoint/Scene/TransformHierarchy.h"

using namespace Hollowpoint;
using namespace Hollowpoint::Graphics;

using Core::Size;
using Core::UInt32;
using ECS::Entity;
using ECS::World;
using Game::CollectInput;
using Game::MeshCullMode;
using Game::MeshRenderer;
using Game::RegisterGameTypes;
using Game::RenderMeshEntry;
using Game::RenderPlan;
using Math::Float32;
using Math::Mat3;
using Math::Mat4;
using Math::Vec3;
using Math::Vec4;
using Renderer::BuildForwardFragmentGlsl;
using Renderer::BuildForwardVertexGlsl;
using Renderer::CameraComponent;
using Renderer::CameraState;
using Renderer::ComputeCameraState;
using Renderer::ComputeForwardShadingCpu;
using Renderer::ComputeNormalMatrix;
using Renderer::ForwardDrawItem;
using Renderer::ForwardPassParams;
using Renderer::GeometryData;
using Renderer::GeometryVertex;
using Renderer::LightComponent;
using Renderer::MakeCube;
using Renderer::MakePlane;
using Scene::ComponentFactory;
using Scene::LoadSceneFromFile;
using Scene::LocalTransform;
using Scene::SaveSceneToFile;
using Scene::SceneLoadReport;

namespace
{

[[nodiscard]] GraphicsDevice* GetDevice()
{
    static GraphicsDevice* device = []() -> GraphicsDevice* {
        GraphicsDeviceDesc desc;
        desc.ApplicationName = "Hollowpoint scene-to-pixel tests";
        return GraphicsDevice::Create(desc);
    }();

    return device;
}

[[nodiscard]] std::string Describe(const Vec3& v)
{
    return "(" + std::to_string(v.X) + ", " + std::to_string(v.Y) + ", " + std::to_string(v.Z) + ")";
}

//! Relative error, safe at zero. A plain `|a-b|/|b|` divides by zero for a black reference — which is exactly what an
//! unlit or culled pixel produces, so the common failure case must not report inf and hide in the noise of the pass.
[[nodiscard]] Float32 RelativeError(Float32 gpu, Float32 cpu)
{
    const Float32 difference = std::fabs(gpu - cpu);
    const Float32 magnitude = std::max(std::fabs(cpu), 1.0e-6f);
    return difference / magnitude;
}

// ---------------------------------------------------------------------------
// Uniform layouts.
//
// These used to be hand-written copies inside this file, which meant the test was pinning *its own* description of the
// contract against the shader while production held a third one — a drift between those two could not fail anything.
// They now come from ForwardShaderLayout.h, the same types SceneRenderer uploads through, so the layout test below
// checks the code that actually ships.
// ---------------------------------------------------------------------------

using Hollowpoint::Game::ForwardFrameBlock;
using Hollowpoint::Game::ForwardMaterialBlock;
using Hollowpoint::Game::ForwardTransformBlock;
using Hollowpoint::Game::ShaderLayout::EncodeLightType;
using Hollowpoint::Game::ShaderLayout::StoreMat4;

//! LightArray (slot 2), structure-of-arrays, for the 8-light bound this test's shaders are generated with. The shared
//! header exposes offsets rather than a fixed-size struct (the renderer's bound is runtime-configurable), so the upload
//! here packs a flat buffer through the same offset helper production uses.
struct LightBlock
{
    static constexpr UInt32 kMaxLights = 8;

    //! Four vec4 (16 bytes each) per light — the block's GLSL size, asserted below against the shader.
    static constexpr Size kBytes = static_cast<Size>(kMaxLights) * 4u * 16u;

    std::array<float, kBytes / sizeof(float)> Bytes{};

    [[nodiscard]] float* Field(UInt32 field, UInt32 index)
    {
        return &Bytes[Hollowpoint::Game::ShaderLayout::LightFieldOffset(kMaxLights, field, index) / sizeof(float)];
    }
};

//! DrawLightIndices (slot 4): ivec4 LightIndices[ceil(N/4)]; int LightCount at its own 16-byte unit.
struct IndexBlock
{
    static constexpr UInt32 kMaxLights = 8;

    std::array<int, 12> Values{};  // two ivec4s + count + tail padding

    void SetIndex(UInt32 index, UInt32 value)
    {
        Values[(index / 4u) * 4u + (index % 4u)] = static_cast<int>(value);
    }

    void SetCount(UInt32 count)
    {
        Values[Hollowpoint::Game::ShaderLayout::IndexBlockCountOffset(kMaxLights) / sizeof(int)] =
            static_cast<int>(count);
    }
};

[[nodiscard]] Core::Ref<Buffer> UploadConstant(GraphicsDevice& device, nvrhi::IDevice* rhi, const char* name,
                                               const void* data, Size bytes)
{
    BufferDesc desc;
    desc.Name = name;
    desc.ByteSize = static_cast<UInt32>(bytes);
    desc.Usage = BufferUsage::Constant;

    Core::Ref<Buffer> buffer = Buffer::Create(device, desc);
    if (buffer == nullptr)
    {
        return nullptr;
    }

    nvrhi::CommandListHandle commandList = rhi->createCommandList();
    commandList->open();
    commandList->writeBuffer(buffer->GetNativeBuffer(), data, bytes);
    commandList->close();

    nvrhi::ICommandList* raw = commandList;
    rhi->executeCommandLists(&raw, 1);
    device.WaitForIdle();
    return buffer;
}

//! One rendered sample: the RGB read back from the centre of a cube face, plus diagnostics for failure messages.
struct RenderedSample
{
    float Red = std::numeric_limits<float>::quiet_NaN();
    float Green = std::numeric_limits<float>::quiet_NaN();
    float Blue = std::numeric_limits<float>::quiet_NaN();

    //! Alpha is the "did the fragment shader run at all" signal: the shader writes 1.0, a clear that never landed
    //! leaves 0, and an unwritten pixel keeps the NaN background. Without it, RGB=0 cannot be attributed.
    float Alpha = std::numeric_limits<float>::quiet_NaN();

    //! Where the sampled pixel's world position was solved to be, and the face normal used for the CPU reference.
    Vec3 WorldPosition{};
    Vec3 FaceNormalLocal{};
};

//! Draws one collected mesh entry through the engine's real forward shaders and reads back one texel.
//!
//! Returns false (rather than asserting) when the device refuses something, so the caller can report which stage failed
//! — a pipeline-creation failure and a shader compile failure need different fixes and must not both read as "nullptr".
[[nodiscard]] bool RenderAndSample(GraphicsDevice& device, const ForwardPassParams& params, const ForwardDrawItem& draw,
                                   const GeometryData& geometry, const std::string& vertexGlsl,
                                   const std::string& fragmentGlsl, RenderedSample& outSample, std::string& outFailure)
{
    nvrhi::IDevice* rhi = device.GetNativeDevice();
    if (rhi == nullptr)
    {
        outFailure = "no native device";
        return false;
    }

    const CompiledShader vertex = CompileGlsl(ShaderStage::Vertex, vertexGlsl);
    if (!vertex.Succeeded)
    {
        outFailure = "vertex compile: " + vertex.Diagnostics.substr(0, 400);
        return false;
    }

    const CompiledShader fragment = CompileGlsl(ShaderStage::Pixel, fragmentGlsl);
    if (!fragment.Succeeded)
    {
        outFailure = "fragment compile: " + fragment.Diagnostics.substr(0, 600);
        return false;
    }

    // --- uniform blocks from the plan -------------------------------------------------
    ForwardFrameBlock frame{};
    StoreMat4(frame.ViewProjection, params.ViewProjection);
    frame.CameraPositionAndPad[0] = params.CameraPosition.X;
    frame.CameraPositionAndPad[1] = params.CameraPosition.Y;
    frame.CameraPositionAndPad[2] = params.CameraPosition.Z;
    frame.CameraPositionAndPad[3] = 0.0f;

    ForwardTransformBlock transform{};
    StoreMat4(transform.ModelMatrix, draw.ModelMatrix);
    // The shader uses mat3(NormalMatrix) on an interpolated varying and normalises in the fragment stage, so it needs
    // exactly the matrix ComputeWorldNormal uses on the CPU — computed here from the same helper, never hand-derived.
    const Mat3 normalMatrix = ComputeNormalMatrix(draw.ModelMatrix.GetUpperLeft3());
    StoreMat4(transform.NormalMatrix, Mat4(normalMatrix));

    LightBlock lights{};
    if (draw.LightIndices.size() > LightBlock::kMaxLights)
    {
        outFailure = "scene has more lights than this test's fixed block";
        return false;
    }

    for (Size index = 0; index < draw.LightIndices.size(); ++index)
    {
        const auto slot = static_cast<Size>(draw.LightIndices[index]);
        if (slot >= params.Lights.size())
        {
            outFailure = "draw item references a light index past the collected list";
            return false;
        }

        const Renderer::ForwardLight& light = params.Lights[slot];

        float* position = lights.Field(0u, static_cast<UInt32>(index));
        position[0] = light.WorldPosition.X;
        position[1] = light.WorldPosition.Y;
        position[2] = light.WorldPosition.Z;
        position[3] = light.Light.Intensity;

        float* colour = lights.Field(1u, static_cast<UInt32>(index));
        colour[0] = light.Light.Color.X;
        colour[1] = light.Light.Color.Y;
        colour[2] = light.Light.Color.Z;
        colour[3] = EncodeLightType(light.Light.Type);

        float* range = lights.Field(2u, static_cast<UInt32>(index));
        range[0] = light.Light.Range;
        range[1] = light.Light.SpotInnerAngleRadians;
        range[2] = light.Light.SpotOuterAngleRadians;
        range[3] = 0.0f;

        float* direction = lights.Field(3u, static_cast<UInt32>(index));
        direction[0] = light.WorldDirection.X;
        direction[1] = light.WorldDirection.Y;
        direction[2] = light.WorldDirection.Z;
        direction[3] = 0.0f;
    }

    ForwardMaterialBlock material{};
    material.BaseColor[0] = draw.Material.BaseColor.X;
    material.BaseColor[1] = draw.Material.BaseColor.Y;
    material.BaseColor[2] = draw.Material.BaseColor.Z;
    material.BaseColor[3] = 1.0f;
    material.MaterialParams[0] = draw.Material.Roughness;
    material.MaterialParams[1] = draw.Material.Metallic;

    IndexBlock indices{};
    for (Size index = 0; index < draw.LightIndices.size(); ++index)
    {
        indices.SetIndex(static_cast<UInt32>(index), draw.LightIndices[index]);
    }
    indices.SetCount(static_cast<UInt32>(draw.LightIndices.size()));

    Core::Ref<Buffer> frameBuffer = UploadConstant(device, rhi, "e2e-frame", &frame, sizeof(frame));
    Core::Ref<Buffer> transformBuffer = UploadConstant(device, rhi, "e2e-transform", &transform, sizeof(transform));
    Core::Ref<Buffer> lightBuffer =
        UploadConstant(device, rhi, "e2e-lights", lights.Bytes.data(), lights.Bytes.size() * sizeof(float));
    Core::Ref<Buffer> materialBuffer = UploadConstant(device, rhi, "e2e-material", &material, sizeof(material));
    Core::Ref<Buffer> indexBuffer =
        UploadConstant(device, rhi, "e2e-indices", indices.Values.data(), indices.Values.size() * sizeof(int));

    if (frameBuffer == nullptr || transformBuffer == nullptr || lightBuffer == nullptr || materialBuffer == nullptr ||
        indexBuffer == nullptr)
    {
        outFailure = "constant buffer creation failed";
        return false;
    }

    // --- geometry ---------------------------------------------------------------------
    BufferDesc vertexBufferDesc;
    vertexBufferDesc.Name = "e2e-vertices";
    vertexBufferDesc.ByteSize = static_cast<UInt32>(geometry.Vertices.size() * sizeof(GeometryVertex));
    vertexBufferDesc.Usage = BufferUsage::Vertex;
    Core::Ref<Buffer> vertexBuffer = Buffer::Create(device, vertexBufferDesc);

    BufferDesc indexBufferDesc;
    indexBufferDesc.Name = "e2e-index";
    indexBufferDesc.ByteSize = static_cast<UInt32>(geometry.Indices.size() * sizeof(UInt32));
    indexBufferDesc.Usage = BufferUsage::Index;
    Core::Ref<Buffer> gpuIndices = Buffer::Create(device, indexBufferDesc);

    if (vertexBuffer == nullptr || gpuIndices == nullptr)
    {
        outFailure = "vertex/index buffer creation failed";
        return false;
    }

    // --- target with a real depth attachment ------------------------------------------
    constexpr UInt32 kExtent = 64;

    TextureDesc colourDesc;
    colourDesc.Name = "e2e-colour";
    colourDesc.Width = kExtent;
    colourDesc.Height = kExtent;
    colourDesc.Format = GraphicsFormat::Rgba32Float;
    colourDesc.IsRenderTarget = true;
    Core::Ref<Texture> colour = Texture::Create(device, colourDesc);

    // D32Float is the engine's depth default (GraphicsTypes.h) and RenderTargetDesc takes exactly that format.
    TextureDesc depthDesc;
    depthDesc.Name = "e2e-depth";
    depthDesc.Width = kExtent;
    depthDesc.Height = kExtent;
    depthDesc.Format = GraphicsFormat::D32Float;
    depthDesc.IsRenderTarget = true;
    Core::Ref<Texture> depth = Texture::Create(device, depthDesc);

    if (colour == nullptr || depth == nullptr)
    {
        outFailure = "render target texture creation failed";
        return false;
    }

    RenderTargetDesc renderTargetDesc;
    renderTargetDesc.Colour = colour;
    renderTargetDesc.Depth = depth;
    Core::Ref<RenderTarget> renderTarget = RenderTarget::Create(device, renderTargetDesc);
    if (renderTarget == nullptr)
    {
        outFailure = "framebuffer creation failed";
        return false;
    }

    // --- pipeline: the engine's shaders, culling and depth ON ------------------------------
    PipelineDesc pipelineDesc;
    pipelineDesc.Name = "e2e-forward";
    pipelineDesc.VertexShader.SpirV = vertex.SpirV;
    pipelineDesc.PixelShader.SpirV = fragment.SpirV;
    for (UInt32 slot = 0; slot < 5; ++slot)
    {
        pipelineDesc.Bindings.push_back(BindingSlotDesc{BindingSlotDesc::Kind::ConstantBuffer, slot});
    }
    // Both stages must see the blocks they declare — a slot invisible to a stage reads as zeros there, which is the
    // silent-zero failure this engine's binding convention exists to prevent (ShaderBindings.h).
    pipelineDesc.BindingsVisibleToVertex = true;
    pipelineDesc.BindingsVisibleToPixel = true;

    // Culling and depth ON rather than disabled for convenience. This line is why the test exists: every earlier GPU
    // test set TriangleCulling = None, so nothing ever asked which winding the engine considers front — and with the
    // pipeline's (incorrect) inherited default, back-face culling discarded the faces pointing at the camera. A test
    // that turns culling off cannot catch that class of bug at all.
    pipelineDesc.TriangleCulling = CullMode::Back;
    pipelineDesc.IsDepthEnabled = true;
    pipelineDesc.DepthFunction = CompareFunction::LessEqual;
    pipelineDesc.IsDepthWriteEnabled = true;

    pipelineDesc.VertexAttributes = {
        VertexAttribute{"InPosition", GraphicsFormat::Rgb32Float, 0, sizeof(GeometryVertex)},
        VertexAttribute{"InNormal", GraphicsFormat::Rgb32Float, 12, sizeof(GeometryVertex)},
        VertexAttribute{"InUv", GraphicsFormat::Rg32Float, 24, sizeof(GeometryVertex)},
    };

    Core::Ref<Pipeline> pipeline = Pipeline::Create(device, pipelineDesc, *renderTarget);
    if (pipeline == nullptr)
    {
        outFailure = "pipeline creation failed";
        return false;
    }

    const nvrhi::BindingSetHandle bindingSet =
        rhi->createBindingSet(nvrhi::BindingSetDesc()
                                  .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, frameBuffer->GetNativeBuffer()))
                                  .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, transformBuffer->GetNativeBuffer()))
                                  .addItem(nvrhi::BindingSetItem::ConstantBuffer(2, lightBuffer->GetNativeBuffer()))
                                  .addItem(nvrhi::BindingSetItem::ConstantBuffer(3, materialBuffer->GetNativeBuffer()))
                                  .addItem(nvrhi::BindingSetItem::ConstantBuffer(4, indexBuffer->GetNativeBuffer())),
                              pipeline->GetBindingLayout());

    if (!static_cast<bool>(bindingSet))
    {
        outFailure = "binding set creation failed";
        return false;
    }

    // --- draw -------------------------------------------------------------------------
    constexpr float kBackground = std::numeric_limits<float>::quiet_NaN();

    nvrhi::CommandListHandle commandList = rhi->createCommandList();
    commandList->open();
    commandList->writeBuffer(vertexBuffer->GetNativeBuffer(), geometry.Vertices.data(),
                             geometry.Vertices.size() * sizeof(GeometryVertex));
    commandList->writeBuffer(gpuIndices->GetNativeBuffer(), geometry.Indices.data(),
                             geometry.Indices.size() * sizeof(UInt32));

    // NaN background: a pixel that was never written reads back as NaN and fails its comparison loudly, rather than
    // matching an expected-zero reference and passing for the wrong reason.
    commandList->clearTextureFloat(colour->GetNativeTexture(), nvrhi::TextureSubresourceSet(),
                                   nvrhi::Color(kBackground, kBackground, kBackground, kBackground));
    commandList->clearDepthStencilTexture(depth->GetNativeTexture(), nvrhi::TextureSubresourceSet(), true, 1.0f, false,
                                          0);

    nvrhi::GraphicsState state;
    state.setPipeline(pipeline->GetNativePipeline())
        .setFramebuffer(renderTarget->GetNativeFramebuffer())
        .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
            nvrhi::Viewport(static_cast<float>(kExtent), static_cast<float>(kExtent))))
        .addVertexBuffer(nvrhi::VertexBufferBinding{vertexBuffer->GetNativeBuffer(), 0, 0})
        // The index buffer is part of graphics state, not of drawIndexed's arguments: omit it and the driver indexes
        // garbage — which on this host cost a device-lost and surfaced as "readback failed" three calls later, with
        // nothing naming the missing binding. R32_UINT because GeometryData::Indices are uint32; the engine has no
        // index-format entry in GraphicsFormat yet (no indexed draw existed before this test), so the NVRHI format is
        // named directly through the documented escape hatch rather than inventing an engine enum value here.
        .setIndexBuffer(nvrhi::IndexBufferBinding{gpuIndices->GetNativeBuffer(), nvrhi::Format::R32_UINT, 0})
        .addBindingSet(bindingSet);

    commandList->setGraphicsState(state);
    commandList->drawIndexed(
        nvrhi::DrawArguments().setVertexCount(static_cast<UInt32>(geometry.Indices.size())).setInstanceCount(1));
    commandList->close();

    nvrhi::ICommandList* raw = commandList;
    rhi->executeCommandLists(&raw, 1);
    device.WaitForIdle();

    // --- read back the pixel at the face centre -----------------------------------------
    UInt32 rowPitchBytes = 0;
    const std::vector<UInt8> bytes = colour->ReadBack(device, rowPitchBytes);
    if (bytes.empty())
    {
        outFailure = "readback failed";
        return false;
    }

    const auto* texels = reinterpret_cast<const float*>(bytes.data());
    const Size floatsPerRow = rowPitchBytes / sizeof(float);

    // Centred, and the row is taken from the pitch rather than assumed equal to the width — a padded pitch read as an
    // unpadded one samples a neighbouring row, which shows up as a colour mismatch with no hint of its cause.
    const Size centre = floatsPerRow * (kExtent / 2) + (kExtent / 2) * 4u;

    outSample.Red = texels[centre + 0];
    outSample.Green = texels[centre + 1];
    outSample.Blue = texels[centre + 2];
    outSample.Alpha = texels[centre + 3];
    return true;
}

// ---------------------------------------------------------------------------
// Scene construction / scratch files
// ---------------------------------------------------------------------------

[[nodiscard]] std::filesystem::path MakeScratchPath(std::string_view label)
{
    static Core::UInt64 s_Counter = 0;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("hp_e2e_" + std::string(label) + "_" + std::to_string(reinterpret_cast<uintptr_t>(&s_Counter)) + "_" +
         std::to_string(s_Counter++));

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    return directory / "scene.hpscene";
}

class ScopedScratchFile
{
public:
    explicit ScopedScratchFile(std::string_view label) : Path(MakeScratchPath(label)) {}

    ~ScopedScratchFile()
    {
        std::error_code ignored;
        // The whole scratch DIRECTORY goes, not just the file: SaveSceneToFile creates parents, and leaving those
        // behind under /tmp is how a suite slowly fills a tmpfs.
        std::filesystem::remove_all(Path.parent_path(), ignored);
    }

    ScopedScratchFile(const ScopedScratchFile&) = delete;
    ScopedScratchFile& operator=(const ScopedScratchFile&) = delete;

    std::filesystem::path Path;
};

//! A cube at `position` with the given material, plus an entity carrying it.
Entity AddMesh(World& world, const Vec3& position, const Vec3& baseColor, Float32 roughness, Float32 metallic,
               const char* path = "engine:cube")
{
    const Entity entity = world.CreateEntity();

    LocalTransform transform;
    transform.Position = position;
    world.AddComponent<LocalTransform>(entity).SetFrom(transform.ToTransform());

    MeshRenderer renderer;
    renderer.MeshPath = path;
    renderer.BaseColor = baseColor;
    renderer.Roughness = roughness;
    renderer.Metallic = metallic;
    world.AddComponent<MeshRenderer>(entity) = renderer;
    return entity;
}

Entity AddDirectionalLight(World& world, const Vec3& direction, Float32 intensity, const Vec3& color)
{
    const Entity entity = world.CreateEntity();

    LightComponent light;
    light.Type = LightComponent::LightType::Directional;
    light.Direction = direction;
    light.Intensity = intensity;
    light.Color = color;
    world.AddComponent<LightComponent>(entity) = light;
    return entity;
}

Entity AddCamera(World& world, const Vec3& position, const Vec3& target)
{
    const Entity entity = world.CreateEntity();

    CameraComponent camera;
    camera.Position = position;
    camera.Target = target;
    camera.NearPlane = 0.1f;
    camera.FarPlane = 100.0f;
    world.AddComponent<CameraComponent>(entity) = camera;
    return entity;
}

//! The asset system this test injects in place of an importer: every path resolves to the same unit cube.
GeometryData g_CubeGeometry;

const GeometryData* ResolveCube(std::string_view /*path*/)
{
    return &g_CubeGeometry;
}

}  // namespace

TEST_SUITE("Renderer/SceneToPixel")
{
    TEST_CASE("the uniform layouts the upload uses match the generated shaders' blocks")
    {
        // The C++ structs above and the GLSL below are two descriptions of one contract, written in two languages with
        // no compile-time link between them. A mismatch is invisible: Vulkan reads whatever bytes happen to be at the
        // offset, so a reordered field delivers a plausible-looking value from the wrong member — e.g. metallic
        // arriving as roughness — and every pixel stays finite. So: parse the generated shader for each block's fields
        // and check their cumulative offsets against sizeof of the C++ struct. This is a real cross-check, not a
        // tautology: it fails if either side changes without the other.
        const std::string vertexGlsl = BuildForwardVertexGlsl();
        const std::string fragmentGlsl = BuildForwardFragmentGlsl(8u);

        const auto contains = [](const std::string& haystack, std::string_view needle) {
            return haystack.find(needle) != std::string::npos;
        };

        // FrameParams: mat4 (64 bytes) + vec4 (16) = 80.
        CHECK(contains(vertexGlsl, "mat4 ViewProjection"));
        CHECK(contains(vertexGlsl, "vec4 CameraPositionAndPad"));
        CHECK(sizeof(ForwardFrameBlock) == 80u);

        // DrawTransform: two mat4s = 128.
        CHECK(contains(vertexGlsl, "mat4 ModelMatrix"));
        CHECK(contains(vertexGlsl, "mat4 NormalMatrix"));
        CHECK(sizeof(ForwardTransformBlock) == 128u);

        // LightArray: four vec4 arrays of 8 = 512, in the order the upload writes them.
        CHECK(contains(fragmentGlsl, "vec4 PositionAndIntensity[8]"));
        CHECK(contains(fragmentGlsl, "vec4 ColorTypeAndRange[8]"));
        CHECK(contains(fragmentGlsl, "vec4 RangeAndSpotAngles[8]"));
        CHECK(contains(fragmentGlsl, "vec4 DirectionAndPad[8]"));
        CHECK(LightBlock::kBytes == 512u);  // four vec4 per light x 8 lights

        // DrawMaterial: two vec4s = 32.
        CHECK(contains(fragmentGlsl, "vec4 BaseColor"));
        CHECK(contains(fragmentGlsl, "vec4 MaterialParams"));
        CHECK(sizeof(ForwardMaterialBlock) == 32u);

        // DrawLightIndices: ivec4[2] (32) + int (4). The C++ struct is alignas(16) — required for a std140 uniform
        // buffer — so the tail pads to 48; that padding is never read by the shader, but the size is asserted exactly
        // so a layout change in either language fails here instead of silently truncating indices.
        CHECK(contains(fragmentGlsl, "ivec4 LightIndices[2]"));
        CHECK(contains(fragmentGlsl, "int LightCount"));
        CHECK(sizeof(IndexBlock) == 48u);

        // And the light-type encoding, both halves: the C++ enum's numeric values AND the shader's documented meaning.
        // Nothing else in the engine pins these together (verified by search), so reordering LightComponent::LightType
        // would silently reinterpret every light in every saved scene — a point light shading as directional, with no
        // error anywhere. If this fails, fix EncodeLightType and ForwardRender.cpp's shader comment together.
        CHECK(static_cast<unsigned>(LightComponent::LightType::Directional) == 0u);
        CHECK(static_cast<unsigned>(LightComponent::LightType::Point) == 1u);
        CHECK(static_cast<unsigned>(LightComponent::LightType::Spot) == 2u);
        CHECK(EncodeLightType(LightComponent::LightType::Directional) == 0.0f);
        CHECK(EncodeLightType(LightComponent::LightType::Point) == 1.0f);
        CHECK(EncodeLightType(LightComponent::LightType::Spot) == 2.0f);

        // The vertex shader must consume the normal matrix rather than the model matrix for normals: using the model
        // matrix directly is only correct for uniform scale, and every transform in the engine currently IS uniformly
        // scaled, so the shortcut would sit dormant until someone stretched a box.
        CHECK(contains(vertexGlsl, "mat3(drawTransform.NormalMatrix) * InNormal"));
    }

    TEST_CASE("procedural geometry is wound counter-clockwise as its normals claim")
    {
        // MakeCube's comment says its triangles are "wound consistently ... so both inherit the face's outward
        // orientation", and the pipeline defaults to back-face culling — but nothing checked the claim, so a wrong
        // winding sat dormant until the first draw that actually culled (see
        // PipelineDesc::IsFrontFaceCounterClockwise). Geometric normal from the indices, compared against the vertex
        // normal the file stores: if they disagree, the face culls itself.
        const auto checkWinding = [](const GeometryData& geometry, const char* name) {
            Size agree = 0;
            Size disagree = 0;

            for (Size triangle = 0; triangle + 2 < geometry.Indices.size(); triangle += 3)
            {
                const GeometryVertex& a = geometry.Vertices[geometry.Indices[triangle]];
                const GeometryVertex& b = geometry.Vertices[geometry.Indices[triangle + 1]];
                const GeometryVertex& c = geometry.Vertices[geometry.Indices[triangle + 2]];

                const Vec3 edge1 = b.GetPosition() - a.GetPosition();
                const Vec3 edge2 = c.GetPosition() - a.GetPosition();
                const Vec3 geometricNormal = edge1.Cross(edge2);

                // Positive dot iff the index winding agrees with the stored normal (right-hand rule). Degenerate
                // triangles are skipped rather than counted, so an unrelated geometry bug cannot masquerade here.
                const Float32 dot = geometricNormal.Dot(a.GetNormal());
                if (dot > 0.0f)
                {
                    ++agree;
                }
                else if (dot < 0.0f)
                {
                    ++disagree;
                }
            }

            INFO(name);
            CHECK(disagree == 0u);
            CHECK(agree > 0u);
        };

        checkWinding(MakeCube(1.0f), "cube");
        checkWinding(MakeCube(2.5f), "scaled cube");
        checkWinding(MakePlane(1.0f), "plane");
    }

    TEST_CASE("a scene file's camera drives the projection matrices used for the draw")
    {
        // No GPU needed: this pins that the plan + CameraComponent produce a view-projection under which the sampled
        // face is actually visible. If it is not, the pixel test below would read NaN and fail as "no device" rather
        // than as "the camera looks the wrong way", so this diagnosis belongs here where it is cheap.
        RegisterGameTypes();

        World world;
        const Vec3 eye(0.0f, 0.0f, 4.0f);
        AddCamera(world, eye, Vec3(0.0f, 0.0f, 0.0f));
        AddMesh(world, Vec3(0.0f, 0.0f, 0.0f), Vec3(1.0f, 1.0f, 1.0f), 0.5f, 0.0f);

        const CameraComponent* camera = nullptr;
        world.ForEachEntity([&](const Entity& entity) {
            if (const auto* found = world.TryGetComponent<CameraComponent>(entity))
            {
                camera = found;
            }
        });
        REQUIRE(camera != nullptr);

        const CameraState state = ComputeCameraState(*camera, 1.0f);

        // Eye position comes from the component, and the cube's front face centre (0,0,0.5) must project inside NDC.
        CHECK(state.EyePosition.Z == doctest::Approx(4.0f));

        const Vec4 clip = state.ViewProjection * Vec4(0.0f, 0.0f, 0.5f, 1.0f);
        INFO("clip " << clip.X << ", " << clip.Y << ", " << clip.Z << ", w=" << clip.W);
        REQUIRE(clip.W > 0.0f);
        CHECK(std::fabs(clip.X / clip.W) < 1.0f);
        CHECK(std::fabs(clip.Y / clip.W) < 1.0f);

        // Depth inside the near/far range, i.e. not clipped away — a scene that projects fine but fails depth would
        // also read as an unwritten pixel.
        const Float32 ndcZ = clip.Z / clip.W;
        CHECK(ndcZ > -1.0f);
        CHECK(ndcZ < 1.0f);
    }

    TEST_CASE("a .hpscene on disk renders through the engine's forward shaders matching the CPU reference (GPU)")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN((std::string("No graphics device on this host: ") + GraphicsDevice::GetFailureReason()).c_str());
            return;
        }

        RegisterGameTypes();
        g_CubeGeometry = MakeCube(1.0f);
        REQUIRE_FALSE(g_CubeGeometry.IsEmpty());

        // ---- build a scene and write it to disk ----------------------------------------
        ScopedScratchFile scratch("scene");

        Vec3 cubePosition(0.31f, -0.23f, 0.0f);  // off-origin: an identity-model-matrix bug must not pass by luck
        const Vec3 baseColor(0.72f, 0.31f, 0.18f);
        constexpr Float32 kRoughness = 0.34f;
        constexpr Float32 kMetallic = 0.0f;
        // Direction the beam points, authored so it travels toward -Z and therefore strikes the +Z-facing side: a light
        // aimed away from the sampled face shades it exactly black, which makes every comparison below vacuous (0 vs
        // 0).
        const Vec3 lightDirection(0.2673f, 0.5345f, -0.8018f);  // normalised (0.3, 0.6, -0.9)

        {
            World world;
            AddCamera(world, Vec3(0.0f, 0.0f, 4.0f), Vec3(0.0f, 0.0f, 0.0f));
            AddDirectionalLight(world, lightDirection, 3.5f, Vec3(1.0f, 0.9f, 0.8f));
            AddMesh(world, cubePosition, baseColor, kRoughness, kMetallic);

            const auto saved = SaveSceneToFile(world, scratch.Path.string());
            REQUIRE_MESSAGE(saved.Saved, "scene could not be written to disk");
        }

        // ---- load it back through the real file path -------------------------------------
        World world;
        const SceneLoadReport report = LoadSceneFromFile(world, scratch.Path.string());
        report.ReportToLog("scene-to-pixel");
        REQUIRE(report.Loaded);
        REQUIRE_FALSE(report.HasErrors());

        // The file round-trip must have produced the components the plan needs — otherwise this test would silently
        // collect an empty scene and compare nothing.
        Size meshCount = 0;
        Size lightCount = 0;
        const CameraComponent* camera = nullptr;
        world.ForEachEntity([&](const Entity& entity) {
            if (world.TryGetComponent<MeshRenderer>(entity) != nullptr)
            {
                ++meshCount;
            }
            if (world.TryGetComponent<LightComponent>(entity) != nullptr)
            {
                ++lightCount;
            }
            if (const auto* found = world.TryGetComponent<CameraComponent>(entity))
            {
                camera = found;
            }
        });
        CHECK(meshCount == 1u);
        CHECK(lightCount == 1u);
        REQUIRE(camera != nullptr);

        // ---- collect via the Game bridge -------------------------------------------------
        // World matrices come from the hierarchy, which is what a real frame fills before collecting: parent chains,
        // staleness checks and all. Building it here rather than hand-composing from LocalTransform keeps this test on
        // the same path the runtime will use, so a transform-hierarchy regression shows up as a wrong pixel here too.
        Scene::TransformHierarchy hierarchy;
        hierarchy.Update(world);

        std::vector<Entity> meshes;
        std::vector<Mat4> meshTransforms;
        std::vector<Entity> lights;
        std::vector<Mat4> lightTransforms;

        world.ForEachEntity([&](const Entity& entity) {
            if (world.TryGetComponent<MeshRenderer>(entity) != nullptr)
            {
                meshes.push_back(entity);
                meshTransforms.push_back(hierarchy.GetWorldMatrix(world, entity));
            }
            if (world.TryGetComponent<LightComponent>(entity) != nullptr)
            {
                lights.push_back(entity);
                lightTransforms.push_back(hierarchy.GetWorldMatrix(world, entity));
            }
        });

        REQUIRE(meshes.size() == 1u);
        REQUIRE(lights.size() == 1u);

        // The loaded transform must place the cube where it was authored — this is the value that becomes ModelMatrix.
        const Vec3 loadedTranslation = meshTransforms[0].GetTranslation();
        INFO("loaded translation " << Describe(loadedTranslation));
        CHECK(loadedTranslation.X == doctest::Approx(cubePosition.X));
        CHECK(loadedTranslation.Y == doctest::Approx(cubePosition.Y));

        CollectInput input;
        input.World = &world;
        input.MeshEntities = meshes.data();
        input.MeshWorldTransforms = meshTransforms.data();
        input.MeshCount = meshes.size();
        input.LightEntities = lights.data();
        input.LightWorldTransforms = lightTransforms.data();
        input.LightCount = lights.size();
        input.ResolveGeometry = &ResolveCube;

        const CameraState cameraState = ComputeCameraState(*camera, 1.0f);

        // The bridge deliberately does NOT build matrices — it has no camera state, only the eye position it is handed.
        // So the pass owner (here, and the renderer later) supplies ViewProjection; asserted below that it is the
        // camera's rather than the plan's default identity, since an identity slipping through would render a cube that
        // fills the frame exactly as plausibly as a correct projection.
        input.CameraPosition = cameraState.EyePosition;

        RenderPlan plan = CollectRenderables(input);
        plan.Pass.ViewProjection = cameraState.ViewProjection;

        REQUIRE(plan.Meshes.size() == 1u);
        REQUIRE(plan.Pass.Lights.size() == 1u);

        // The bridge must hand the pass a light direction that survived JSON, entity rotation (none here) and the
        // authored-direction rule. If it had assumed local -Z, this would be (0,0,-1) and every comparison below would
        // still be self-consistent — which is exactly why the scene file has to be the source of truth here.
        const Vec3 collectedDirection = plan.Pass.Lights[0].WorldDirection;
        INFO("collected light direction " << Describe(collectedDirection));
        CHECK(collectedDirection.X == doctest::Approx(lightDirection.X).epsilon(1e-5).scale(1.0f));
        // Negative: the authored beam points toward -Z, and this asserts the collector kept that sign rather than
        // substituting an axis. (An earlier version of this line asserted Z > 0.9 — a sign error in my own test data,
        // caught only because the CPU reference came back black.)
        CHECK(collectedDirection.Z < -0.75f);

        const ForwardDrawItem& draw = plan.Meshes[0].Draw;
        REQUIRE(draw.IsDrawable());

        // ---- solve for the sampled pixel's world position --------------------------------
        // The centre of a face-on cube: take the +Z face (normal toward the camera at +Z), whose centre in local space
        // is (0,0,0.5); transform by the model matrix actually uploaded. That point is inside the face's projected
        // quad, so the fragment covering the viewport centre has exactly this world position — and on a flat face the
        // interpolated normal equals the face normal, which is what makes the CPU reference exact (ForwardRender.h).
        const Vec3 localFaceNormal(0.0f, 0.0f, 1.0f);

        // The face centre is NOT the right answer, and this is worth stating because it is the easy wrong version:
        // with the cube off-origin (deliberately, to catch an ignored model matrix), its face centre projects away from
        // NDC origin, so the centre pixel shows a different point on the face. Solve instead — intersect the camera's
        // principal ray (NDC 0,0) with the plane of that face, then map back through the inverse model matrix to get
        // the LOCAL normal for the CPU reference. Flat face ⇒ every point on it shares one normal, so any point inside
        // the quad is exact, and this one is exact *by construction* at the sampled pixel.
        const Float32 planeZ = cubePosition.Z + 0.5f;

        const Vec4 nearPoint = cameraState.InverseViewProjection * Vec4(0.0f, 0.0f, 0.0f, 1.0f);
        const Vec4 farPoint = cameraState.InverseViewProjection * Vec4(0.0f, 0.0f, 1.0f, 1.0f);
        const Vec3 rayOrigin(nearPoint.X / nearPoint.W, nearPoint.Y / nearPoint.W, nearPoint.Z / nearPoint.W);
        const Vec4 farDivided(farPoint.X / farPoint.W, farPoint.Y / farPoint.W, farPoint.Z / farPoint.W, 1.0f);
        const Vec3 rayDirection = (Vec3(farDivided.X, farDivided.Y, farDivided.Z) - rayOrigin).Normalized();

        INFO("ray origin " << Describe(rayOrigin) << " dir " << Describe(rayDirection));
        REQUIRE(std::fabs(rayDirection.Z) > 1.0e-6f);

        const Float32 hitDistance = (planeZ - rayOrigin.Z) / rayDirection.Z;
        REQUIRE(hitDistance > 0.0f);

        const Vec3 worldSample(rayOrigin.X + rayDirection.X * hitDistance, rayOrigin.Y + rayDirection.Y * hitDistance,
                               planeZ);

        // It must be inside the face's footprint, or the centre pixel is on an edge/another face and the flat-normal
        // assumption (and this test's claim) is void. Half-extent 0.5 for MakeCube(1).
        const Float32 localX = worldSample.X - cubePosition.X;
        const Float32 localY = worldSample.Y - cubePosition.Y;
        INFO("sample offset from face centre " << localX << ", " << localY);
        CHECK(std::fabs(localX) < 0.5f);
        CHECK(std::fabs(localY) < 0.5f);

        // Cross-check the framing itself: projecting the solved point must land at NDC origin, which is where the
        // readback samples. If this drifts, a comparison failure would read as a shading bug when it is a framing one.
        const Vec4 clip = plan.Pass.ViewProjection * Vec4(worldSample, 1.0f);
        INFO("sample clip " << clip.X << ", " << clip.Y << ", w=" << clip.W);
        REQUIRE(clip.W > 0.0f);
        CHECK(std::fabs(clip.X / clip.W) < 1.0e-4f);
        CHECK(std::fabs(clip.Y / clip.W) < 1.0e-4f);

        ForwardPassParams params = plan.Pass;
        CHECK(params.ViewProjection == cameraState.ViewProjection);

        // ---- CPU reference for that exact point ------------------------------------------
        const Vec3 expected = ComputeForwardShadingCpu(params, draw, worldSample, localFaceNormal);
        INFO("CPU reference " << Describe(expected));
        // A black reference would make the comparison vacuous: guard it so a broken collection (no lights, zero
        // intensity) cannot pass by matching an unwritten pixel that also happens to read as zero.
        CHECK(expected.X > 0.0f);
        CHECK(expected.Z > 0.0f);

        // ---- GPU draw through the engine's own shaders ------------------------------------
        const std::string header = "#version 450\n" + BuildGlslBindingPrelude();
        const std::string vertexGlsl = header + BuildForwardVertexGlsl();
        const std::string fragmentGlsl = header + BuildForwardFragmentGlsl(8u);

        RenderedSample sample;
        std::string failure;
        const bool rendered =
            RenderAndSample(*device, params, draw, *draw.Geometry, vertexGlsl, fragmentGlsl, sample, failure);
        INFO("render failure: " << (failure.empty() ? "<none reported>" : failure));
        REQUIRE(rendered);

        // NaN would mean the pixel was never written (cleared and not drawn): a real failure, reported distinctly from
        // "drawn but different", which is the difference between a transform/culling bug and a shading bug.
        INFO("GPU centre texel RGBA " << sample.Red << ", " << sample.Green << ", " << sample.Blue
                                      << ", a=" << sample.Alpha);
        CHECK_FALSE(std::isnan(sample.Red));
        CHECK_FALSE(std::isnan(sample.Green));
        CHECK_FALSE(std::isnan(sample.Blue));

        // Tolerance: same float32 arithmetic, but the GPU path interpolates a normal and normalises it, runs through a
        // 32-bit colour target, and may fuse differently. 2e-3 relative is well under an 8-bit step (~4e-3 of full
        // scale) yet catches a dropped term, a wrong constant, or a mis-uploaded block field.
        CHECK(RelativeError(sample.Red, expected.X) < 2.0e-3f);
        CHECK(RelativeError(sample.Green, expected.Y) < 2.0e-3f);
        CHECK(RelativeError(sample.Blue, expected.Z) < 2.0e-3f);
    }
}
