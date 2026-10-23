// The test M3 exists for: does the forward pass render a lit cube that agrees numerically with the CPU reference?
//
// Method, and why it is built this way:
//   * An ORTHOGRAPHIC camera looks straight down -Z at a unit cube centred on the origin. Under an orthographic view
//     along an axis, every fragment of the front face maps to a known world position with no perspective divide to
//     reason about — so "pixel (8,8) is world (0, 0, +0.5)" is exact arithmetic, not a projection I hope is right.
//     Comparison sweeps every texel whose centre lies on the face, evaluating the CPU reference AT THAT TEXEL CENTRE
//     (see SampleWorldPositionAtTexelCentre): sampling from texels rather than at hand-picked points removes any
//     offset between what the GPU shaded and what the CPU computed. Framebuffer orientation is measured, not assumed
//     — see the Probe suite in TestUniformProbe.cpp for how.
//   * The cube is axis-aligned, so each face has one constant normal and flat shading is exact: the CPU reference's
//     documented contract (flat faces only) covers this scene completely.
//   * The target is cleared to NaN. An unwritten pixel therefore fails loudly instead of reading as a plausible zero
//     — "nothing drew" must never look like "drew black", because those are different bugs with different fixes.
//   * Multiple lights (directional + point + spot) in one frame, each exercised by pixels they do and do not reach.
//
// RGBA32_FLOAT target rather than the engine's usual RGBA16_FLOAT: half-float storage adds ~1e-3 of its own error,
// which would force a tolerance loose enough to hide real shading differences. Half-float fidelity is pinned by its
// own test (TestHdrRenderTarget); this one isolates the shader-vs-CPU question.

#include "TestSupport.h"

#include <array>
#include <cmath>
#include <cstring>
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
#include "Hollowpoint/Math/MathFunctions.h"  // Math::Clamp
#include "Hollowpoint/Renderer/ForwardRender.h"
#include "Hollowpoint/Renderer/Geometry.h"

using namespace Hollowpoint;
using namespace Hollowpoint::Graphics;
using namespace Hollowpoint::Renderer;
using Math::Float32;
using Math::Vec3;

namespace
{

constexpr UInt32 kSize = 16;           // small: every texel is a sample point, and 256 is plenty
constexpr Float32 kHalfExtent = 1.0f;  // orthographic half-extent in world units (cube edge 1 → fills 1/4 of view)

[[nodiscard]] GraphicsDevice* GetDevice()
{
    static GraphicsDevice* device = []() -> GraphicsDevice* {
        GraphicsDeviceDesc desc;
        desc.ApplicationName = "Hollowpoint forward pass tests";
        return GraphicsDevice::Create(desc);
    }();

    return device;
}

[[nodiscard]] std::string Describe(const Vec3& v)
{
    return "(" + std::to_string(v.X) + ", " + std::to_string(v.Y) + ", " + std::to_string(v.Z) + ")";
}

// --- Uniform block layouts ---------------------------------------------------
// Mirror the generated GLSL exactly. std140 puts mat4 as four vec4 columns and arrays of vec4 stay 16-byte aligned;
// these structs exist so the upload is one memcpy per buffer rather than field-by-field pointer arithmetic that can
// drift from the shader silently (all-zero uniforms with no error — session 12).

struct alignas(16) FrameUniforms
{
    float ViewProjection[16];
    float CameraPositionAndPad[4];
};
static_assert(sizeof(FrameUniforms) == 80, "mat4 (64) + vec4 (16): std140 frame block");

struct alignas(16) DrawTransformUniforms
{
    float ModelMatrix[16];
    float NormalMatrix[16];
};
static_assert(sizeof(DrawTransformUniforms) == 128, "two mat4 columns: std140 draw-transform block");

constexpr UInt32 kMaxLights = 8;

struct alignas(16) LightArrayUniforms
{
    float PositionAndIntensity[kMaxLights][4];
    float ColorTypeAndRange[kMaxLights][4];
    float RangeAndSpotAngles[kMaxLights][4];
    float DirectionAndPad[kMaxLights][4];
};

struct alignas(16) DrawMaterialUniforms
{
    float BaseColor[4];
    float MaterialParams[4];
};

struct alignas(16) DrawLightIndicesUniforms
{
    int LightIndices[(kMaxLights + 3) / 4][4];
    int LightCount;
};

//! Upload one buffer, write its bytes, and keep it alive for the frame.
[[nodiscard]] Core::Ref<Buffer> UploadBuffer(GraphicsDevice& device, std::string_view name, const void* data,
                                             size_t byteSize)
{
    BufferDesc desc;
    desc.Name = std::string(name);
    desc.ByteSize = static_cast<UInt32>(byteSize);
    desc.Usage = BufferUsage::Constant;

    Core::Ref<Buffer> buffer = Buffer::Create(device, desc);
    if (buffer == nullptr)
    {
        return nullptr;
    }

    nvrhi::IDevice* rhi = device.GetNativeDevice();
    nvrhi::CommandListHandle commandList = rhi->createCommandList();
    commandList->open();
    commandList->writeBuffer(buffer->GetNativeBuffer(), data, byteSize);
    commandList->close();
    nvrhi::ICommandList* raw = commandList;
    rhi->executeCommandLists(&raw, 1);
    device.WaitForIdle();

    return buffer;
}

//! World → NDC under the test camera: orthographic, looking down -Z from +kHalfExtent·Z... expressed directly so the
//! mapping is visible in one place rather than assembled from matrix helpers. Screen y needs the framebuffer flip:
//! Vulkan's framebuffer origin is top-left while NDC y grows upward (the same asymmetry that forced culling off in
//! the first draw test). Here it is handled exactly, and the assertions depend on it being right.
struct TestCamera
{
    Math::Mat4 ViewProjection;

    [[nodiscard]] static TestCamera MakeLookingDownMinusZ()
    {
        const Math::Mat4 view =
            Math::Mat4::LookAtRH(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f));
        const Math::Mat4 projection = Math::Mat4::OrthographicRH(2.0f * kHalfExtent, 2.0f * kHalfExtent, 0.1f, 20.0f);

        TestCamera camera;
        camera.ViewProjection = projection * view;
        return camera;
    }

    [[nodiscard]] Vec3 Project(const Vec3& world) const
    {
        const Math::Vec4 clip = ViewProjection * Math::Vec4(world.X, world.Y, world.Z, 1.0f);
        // Orthographic along -Z keeps w at 1; dividing anyway documents where the assumption lives rather than
        // silently ignoring a projection change later.
        const Float32 w = clip.W != 0.0f ? clip.W : 1.0f;
        return Vec3(clip.X / w, clip.Y / w, clip.Z / w);
    }

    //! Texel index for an NDC point. y is FLIPPED: measured on this GPU (Probe suite, "Vulkan framebuffer Y
    /// orientation") that NDC +Y lands on the first readback rows — Vulkan's y_f = (1 - y_ndc)/2 * height, top-left
    /// origin. An earlier version of this mapping omitted the flip; its centre sample passed by symmetry while every
    /// off-centre sample silently read the mirrored position, appearing as ~8% lighting errors that looked exactly
    /// like a shading bug. The probe now pins the orientation, so this line cannot drift unnoticed.
    [[nodiscard]] static std::array<UInt32, 2> ToTexel(const Vec3& ndc)
    {
        const Float32 x = (ndc.X * 0.5f + 0.5f) * static_cast<Float32>(kSize);
        const Float32 y = (0.5f - ndc.Y * 0.5f) * static_cast<Float32>(kSize);
        return {static_cast<UInt32>(Math::Clamp(x, 0.0f, static_cast<Float32>(kSize - 1))),
                static_cast<UInt32>(Math::Clamp(y, 0.0f, static_cast<Float32>(kSize - 1)))};
    }

    //! The world position (on the cube's +Z face plane) at a texel's CENTRE.
    ///
    /// Sampling runs outward from texels rather than inward from chosen world points: rounding an arbitrary world
    /// position to a texel means the GPU actually shaded that texel's centre — a slightly different point — and on a
    /// curved lighting field (spot cone edges, point-light falloff) the offset is a real difference the CPU reference
    /// cannot see. Deriving the sample point from the texel centre makes both sides evaluate exactly the same
    /// position, so any remaining disagreement is genuinely a shading difference rather than a sampling artefact.
    [[nodiscard]] Vec3 SampleWorldPositionAtTexelCentre(UInt32 texelX, UInt32 texelY) const
    {
        const Float32 ndcX = (static_cast<Float32>(texelX) + 0.5f) / static_cast<Float32>(kSize) * 2.0f - 1.0f;
        // Inverse of the flipped mapping above: readback row 0 corresponds to NDC +1.
        const Float32 ndcY = 1.0f - (static_cast<Float32>(texelY) + 0.5f) / static_cast<Float32>(kSize) * 2.0f;

        // Undoing the orthographic projection: the view is a pure translation along Z, so X/Y scale linearly with
        // the half-extent and NDC = world / kHalfExtent on both axes.
        return Vec3(ndcX * kHalfExtent, ndcY * kHalfExtent, 0.5f);  // z = the cube's front face plane
    }
};

//! The scene: a unit cube at origin, three lights (one per type), camera down -Z.
struct ForwardScene
{
    GeometryData Cube = MakeCube();
    ForwardPassParams Params;
    ForwardDrawItem Draw;
    TestCamera Camera = TestCamera::MakeLookingDownMinusZ();

    ForwardScene()
    {
        Params.CameraPosition = Vec3(0.0f, 0.0f, 5.0f);

        // A directional light from up-front-right: lights the +Y, +X and +Z faces at different strengths.
        ForwardLight directional;
        directional.Light.Type = LightComponent::LightType::Directional;
        directional.Light.Color = Vec3(1.0f, 0.9f, 0.8f);
        directional.Light.Intensity = 2.0f;
        directional.WorldDirection = Vec3(-0.3f, -0.5f, -0.8f).Normalized();
        Params.Lights.push_back(directional);

        // A point light off to the left: its attenuation must show up as a brightness difference between the cube's
        // left and right sides — an exact value test of inverse-square lives in TestLights already; here the job is
        // proving the GPU applies the same falloff the CPU computed.
        ForwardLight point;
        point.Light.Type = LightComponent::LightType::Point;
        point.WorldPosition = Vec3(-1.5f, 0.5f, 1.5f);
        point.Light.Color = Vec3(0.2f, 0.6f, 1.0f);
        point.Light.Intensity = 4.0f;
        point.Light.Range = 6.0f;
        Params.Lights.push_back(point);

        // A spot pointing down the -Z axis from behind the camera: lights only near screen centre, so pixels away
        // from centre must equal the two-light result — an exact zero-outside-cone test on real hardware.
        ForwardLight spot;
        spot.Light.Type = LightComponent::LightType::Spot;
        spot.WorldPosition = Vec3(0.0f, 0.0f, 2.5f);
        spot.WorldDirection = Vec3(0.0f, 0.0f, -1.0f);
        spot.Light.Color = Vec3(1.0f, 0.3f, 0.1f);
        spot.Light.Intensity = 3.0f;
        spot.Light.Range = 0.0f;  // unbounded: isolates cone behaviour from distance falloff
        spot.Light.SpotInnerAngleRadians = 0.25f;
        spot.Light.SpotOuterAngleRadians = 0.4f;
        Params.Lights.push_back(spot);

        Draw.Geometry = &Cube;
        Draw.ModelMatrix = Math::Mat4::Identity();
        Draw.Material.BaseColor = Vec3(0.7f, 0.3f, 0.15f);
        Draw.Material.Roughness = 0.45f;
        Draw.Material.Metallic = 0.1f;
        Draw.LightIndices = {0u, 1u, 2u};
    }

    //! CPU reference at a world point on the +Z face (the only one this test samples).
    [[nodiscard]] Vec3 ReferenceAt(const Vec3& worldOnFrontFace) const
    {
        return ComputeForwardShadingCpu(Params, Draw, worldOnFrontFace, Vec3(0.0f, 0.0f, 1.0f));
    }
};

//! Pack the scene into the uniform structs and render one frame; returns RGBA-float pixels (row-major, kSize stride).
[[nodiscard]] bool RenderForwardFrame(GraphicsDevice& device, const ForwardScene& scene, std::vector<Vec3>& outPixels,
                                      std::string& failure)
{
    nvrhi::IDevice* rhi = device.GetNativeDevice();
    if (rhi == nullptr)
    {
        failure = "no RHI device";
        return false;
    }

    // --- Shaders -------------------------------------------------------------
    const std::string header = "#version 450\n" + BuildGlslBindingPrelude();

    const CompiledShader vertex = CompileGlsl(ShaderStage::Vertex, header + BuildForwardVertexGlsl());
    if (!vertex.Succeeded)
    {
        failure = "vertex compile: " + vertex.Diagnostics;
        return false;
    }

    const CompiledShader fragment = CompileGlsl(ShaderStage::Pixel, header + BuildForwardFragmentGlsl(kMaxLights));
    if (!fragment.Succeeded)
    {
        failure = "fragment compile: " + fragment.Diagnostics;
        return false;
    }

    // --- Uniform data --------------------------------------------------------
    FrameUniforms frame{};
    std::memcpy(frame.ViewProjection, scene.Camera.ViewProjection.Columns, sizeof(frame.ViewProjection));
    frame.CameraPositionAndPad[0] = scene.Params.CameraPosition.X;
    frame.CameraPositionAndPad[1] = scene.Params.CameraPosition.Y;
    frame.CameraPositionAndPad[2] = scene.Params.CameraPosition.Z;
    frame.CameraPositionAndPad[3] = 0.0f;

    DrawTransformUniforms transform{};
    std::memcpy(transform.ModelMatrix, scene.Draw.ModelMatrix.Columns, sizeof(transform.ModelMatrix));

    // Normal matrix comes from the engine's own ComputeNormalMatrix — the same function the CPU reference reaches
    // through ComputeWorldNormal. An earlier version of this line inverted by hand (Inverse().Transposed()) while its
    // comment claimed to use "the same code path": false, and exactly the duplication that lets a CPU check and a
    // rendered image disagree. It stayed green when the engine's normal math moved to a cofactor form only because
    // this scene uses an identity model matrix — which is the trap: the disagreement would have surfaced on the first
    // rotated object, far from here. Going through the function moves both sides together whatever the transform.
    //
    // Coverage stated honestly: this scene's identity model matrix means the GPU never exercises a non-identity
    // normal matrix, and it will stay that way until the test can decide which cube face each texel shows (a rotated
    // cube exposes two faces, so per-texel CPU comparison needs projected-polygon hit testing — worth doing with the
    // curved-surface/interpolated-normal work rather than approximated here). What covers the normal matrix instead is
    // that both sides call one function, and that function's own tests (cofactor-vs-explicit-inverse-transpose over
    // rotation, anisotropic scale and shear; degeneracy at every magnitude). Correctness transfers by construction,
    // which is the point of not having two implementations.
    {
        const Math::Mat3 normalMatrix = ComputeNormalMatrix(scene.Draw.ModelMatrix.GetUpperLeft3());
        for (int column = 0; column < 3; ++column)
        {
            for (int row = 0; row < 3; ++row)
            {
                transform.NormalMatrix[column * 4 + row] = normalMatrix[column][row];
            }
        }
    }

    LightArrayUniforms lights{};
    for (size_t index = 0; index < scene.Params.Lights.size() && index < kMaxLights; ++index)
    {
        const ForwardLight& entry = scene.Params.Lights[index];
        auto store = [](float (&slot)[4], const Vec3& value, float w) {
            slot[0] = value.X;
            slot[1] = value.Y;
            slot[2] = value.Z;
            slot[3] = w;
        };

        store(lights.PositionAndIntensity[index], entry.WorldPosition, entry.Light.Intensity);

        float typeValue = 1.0f;
        if (entry.Light.Type == LightComponent::LightType::Directional)
        {
            typeValue = 0.0f;
        }
        else if (entry.Light.Type == LightComponent::LightType::Spot)
        {
            typeValue = 2.0f;
        }
        store(lights.ColorTypeAndRange[index], entry.Light.Color, typeValue);

        lights.RangeAndSpotAngles[index][0] = entry.Light.Range;
        lights.RangeAndSpotAngles[index][1] = entry.Light.SpotInnerAngleRadians;
        lights.RangeAndSpotAngles[index][2] = entry.Light.SpotOuterAngleRadians;
        lights.RangeAndSpotAngles[index][3] = 0.0f;

        store(lights.DirectionAndPad[index], entry.WorldDirection, 0.0f);
    }

    DrawMaterialUniforms material{};
    material.BaseColor[0] = scene.Draw.Material.BaseColor.X;
    material.BaseColor[1] = scene.Draw.Material.BaseColor.Y;
    material.BaseColor[2] = scene.Draw.Material.BaseColor.Z;
    material.BaseColor[3] = 1.0f;
    material.MaterialParams[0] = scene.Draw.Material.Roughness;
    material.MaterialParams[1] = scene.Draw.Material.Metallic;

    DrawLightIndicesUniforms indices{};
    for (size_t i = 0; i < scene.Draw.LightIndices.size() && i < kMaxLights; ++i)
    {
        indices.LightIndices[i / 4][i % 4] = static_cast<int>(scene.Draw.LightIndices[i]);
    }
    indices.LightCount = static_cast<int>(scene.Draw.LightIndices.size());

    // --- Resources through the engine wrappers --------------------------------
    Core::Ref<Buffer> frameBuffer = UploadBuffer(device, "forward frame", &frame, sizeof(frame));
    Core::Ref<Buffer> transformBuffer = UploadBuffer(device, "forward transform", &transform, sizeof(transform));
    Core::Ref<Buffer> lightBuffer = UploadBuffer(device, "forward lights", &lights, sizeof(lights));
    Core::Ref<Buffer> materialBuffer = UploadBuffer(device, "forward material", &material, sizeof(material));
    Core::Ref<Buffer> indexBuffer = UploadBuffer(device, "forward light indices", &indices, sizeof(indices));

    const GeometryData& geometry = *scene.Draw.Geometry;
    BufferDesc vertexDesc;
    vertexDesc.Name = "forward vertices";
    vertexDesc.ByteSize = static_cast<UInt32>(geometry.Vertices.size() * sizeof(GeometryVertex));
    vertexDesc.Usage = BufferUsage::Vertex;
    Core::Ref<Buffer> vertexBuffer = Buffer::Create(device, vertexDesc);

    BufferDesc indexBufDesc;
    indexBufDesc.Name = "forward geometry indices";
    indexBufDesc.ByteSize = static_cast<UInt32>(geometry.Indices.size() * sizeof(Math::UInt32));
    indexBufDesc.Usage = BufferUsage::Index;
    Core::Ref<Buffer> geometryIndexBuffer = Buffer::Create(device, indexBufDesc);

    if (!frameBuffer || !transformBuffer || !lightBuffer || !materialBuffer || !indexBuffer || !vertexBuffer ||
        !geometryIndexBuffer)
    {
        failure = "buffer creation failed";
        return false;
    }

    TextureDesc targetDesc;
    targetDesc.Name = "forward HDR target";
    targetDesc.Width = kSize;
    targetDesc.Height = kSize;
    // RGBA32_FLOAT: see file header — half-float's own error would force a tolerance that hides real differences.
    targetDesc.Format = GraphicsFormat::Rgba32Float;
    Core::Ref<Texture> target = Texture::Create(device, targetDesc);

    // Depth is load-bearing here, not decoration: culling is off (to stay independent of the framebuffer-Y winding
    // inversion), so without a depth buffer whichever triangle rasterises last wins each pixel. For this cube that
    // is the -Z face — normal pointing away from all three lights, so every fragment computes exactly zero and the
    // failure reads as "the shader is broken" when the test forgot to resolve visibility. First write of this test
    // made that mistake; the zero it produced is why depth is asserted below rather than assumed.
    TextureDesc depthDesc;
    depthDesc.Name = "forward pass depth";
    depthDesc.Width = kSize;
    depthDesc.Height = kSize;
    depthDesc.Format = GraphicsFormat::D32Float;
    Core::Ref<Texture> depth = Texture::Create(device, depthDesc);

    RenderTargetDesc renderTargetDesc;
    renderTargetDesc.Name = "forward pass target";
    renderTargetDesc.Colour = target;
    renderTargetDesc.Depth = depth;
    Core::Ref<RenderTarget> renderTarget = RenderTarget::Create(device, renderTargetDesc);

    PipelineDesc pipelineDesc;
    pipelineDesc.Name = "forward pass";
    pipelineDesc.VertexShader.SpirV = vertex.SpirV;
    pipelineDesc.VertexShader.Name = "forward vs";
    pipelineDesc.PixelShader.SpirV = fragment.SpirV;
    pipelineDesc.PixelShader.Name = "forward ps";
    // The five constant blocks both stages declare (slots 0-4) must be listed on the pipeline: without a binding
    // layout the Vulkan pipeline has none, and bindDescriptorSets dereferences a null handle at draw time — SIGSEGV
    // inside the driver with nothing naming the cause. PipelineDesc::Bindings documents the requirement.
    pipelineDesc.Bindings = {
        BindingSlotDesc{BindingSlotDesc::Kind::ConstantBuffer, 0},
        BindingSlotDesc{BindingSlotDesc::Kind::ConstantBuffer, 1},
        BindingSlotDesc{BindingSlotDesc::Kind::ConstantBuffer, 2},
        BindingSlotDesc{BindingSlotDesc::Kind::ConstantBuffer, 3},
        BindingSlotDesc{BindingSlotDesc::Kind::ConstantBuffer, 4},
    };
    // FrameParams is read by both stages; the rest are pixel-only, but a layout's visibility covers the union —
    // declaring vertex visibility is required for slot 0 to be visible there at all.
    pipelineDesc.BindingsVisibleToVertex = true;
    pipelineDesc.BindingsVisibleToPixel = true;

    // Offsets and stride match GeometryVertex's pinned layout (position 0, normal 12, uv 24, stride 32 — the same
    // numbers its static_asserts enforce), so the input layout cannot silently disagree with the vertex data.
    pipelineDesc.VertexAttributes = {
        // InPosition is `in vec3` in the generated shader: the format must be three floats (an Rg32 desc here is a
        // silent type mismatch — the attribute matcher accepts it and the shader reads shifted data).
        VertexAttribute{"InPosition", GraphicsFormat::Rgb32Float, 0, sizeof(GeometryVertex)},
        VertexAttribute{"InNormal", GraphicsFormat::Rgb32Float, 12, sizeof(GeometryVertex)},
        VertexAttribute{"InUv", GraphicsFormat::Rg32Float, 24, sizeof(GeometryVertex)},
    };

    // Culling off, and depth ON with the cube alone: a single object cannot self-occlude ambiguously at this size,
    // and leaving culling on would make the result depend on the framebuffer-Y winding inversion (see TestDraw).
    pipelineDesc.TriangleCulling = CullMode::None;

    Core::Ref<Pipeline> pipeline = Pipeline::Create(device, pipelineDesc, *renderTarget);
    if (pipeline == nullptr)
    {
        failure = "pipeline creation failed";
        return false;
    }

    // --- Bindings --------------------------------------------------------------
    // The layout comes from the pipeline (built once from PipelineDesc::Bindings); the per-draw binding set names
    // the concrete buffers against it. Using the pipeline's layout rather than creating a second one keeps the two
    // from drifting when a shader gains a block and someone updates only one side.
    const nvrhi::BindingSetHandle bindingSet =
        rhi->createBindingSet(nvrhi::BindingSetDesc()
                                  .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, frameBuffer->GetNativeBuffer()))
                                  .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, transformBuffer->GetNativeBuffer()))
                                  .addItem(nvrhi::BindingSetItem::ConstantBuffer(2, lightBuffer->GetNativeBuffer()))
                                  .addItem(nvrhi::BindingSetItem::ConstantBuffer(3, materialBuffer->GetNativeBuffer()))
                                  .addItem(nvrhi::BindingSetItem::ConstantBuffer(4, indexBuffer->GetNativeBuffer())),
                              pipeline->GetBindingLayout());

    // --- Record ----------------------------------------------------------------
    nvrhi::CommandListHandle commandList = rhi->createCommandList();
    commandList->open();

    commandList->writeBuffer(vertexBuffer->GetNativeBuffer(), geometry.Vertices.data(),
                             geometry.Vertices.size() * sizeof(GeometryVertex));
    commandList->writeBuffer(geometryIndexBuffer->GetNativeBuffer(), geometry.Indices.data(),
                             geometry.Indices.size() * sizeof(Math::UInt32));

    // Depth cleared to far (1.0): attachment contents are undefined at creation, and LessOrEqual against undefined
    // is driver luck — green on one machine, random on another.
    commandList->clearDepthStencilTexture(depth->GetNativeTexture(), nvrhi::AllSubresources, true, 1.0f, false, 0);

    // NaN clear: an unwritten texel fails comparisons loudly instead of masquerading as a valid black pixel.
    // Caveat, kept honest: some GPU ALUs flush NaN to zero during blending or filtering — this target uses no
    // blending and no sampling, so the NaN survives to readback here (verified by the checks passing). If a future
    // pass adds blend or a sampler between clear and readback, switch this sentinel to an out-of-range value like
    // -1e30 instead of trusting NaN propagation.
    commandList->clearTextureFloat(
        target->GetNativeTexture(), nvrhi::TextureSubresourceSet(),
        nvrhi::Color(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN(),
                     std::numeric_limits<float>::quiet_NaN(), 0.0f));

    const nvrhi::Viewport viewport(static_cast<float>(kSize), static_cast<float>(kSize));

    nvrhi::GraphicsState state;
    state.setPipeline(pipeline->GetNativePipeline())
        .setFramebuffer(renderTarget->GetNativeFramebuffer())
        .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(viewport))
        .addVertexBuffer(nvrhi::VertexBufferBinding{vertexBuffer->GetNativeBuffer(), 0, 0})
        // Index buffer binding is part of the graphics state; omitting it makes drawIndexed dereference a null
        // buffer (measured: SIGSEGV right here on the first frame).
        .setIndexBuffer(nvrhi::IndexBufferBinding{geometryIndexBuffer->GetNativeBuffer(), nvrhi::Format::R32_UINT, 0})
        .addBindingSet(bindingSet);

    commandList->setGraphicsState(state);
    commandList->drawIndexed(
        nvrhi::DrawArguments().setVertexCount(static_cast<uint32_t>(geometry.Indices.size())).setInstanceCount(1));
    commandList->close();

    nvrhi::ICommandList* raw = commandList;
    rhi->executeCommandLists(&raw, 1);
    device.WaitForIdle();

    // --- Read back -------------------------------------------------------------
    UInt32 rowPitchBytes = 0;
    const std::vector<UInt8> bytes = target->ReadBack(device, rowPitchBytes);
    if (bytes.empty())
    {
        failure = "readback returned nothing";
        return false;
    }

    outPixels.clear();
    outPixels.reserve(kSize * kSize);
    const auto pixelsPerRow = rowPitchBytes / static_cast<UInt32>(sizeof(float));
    for (UInt32 y = 0; y < kSize; ++y)
    {
        for (UInt32 x = 0; x < kSize; ++x)
        {
            const float* texel = reinterpret_cast<const float*>(bytes.data()) + y * pixelsPerRow + x * 4u;
            outPixels.emplace_back(texel[0], texel[1], texel[2]);
        }
    }

    return true;
}

}  // namespace

TEST_SUITE("Renderer/ForwardRenderGPU")
{
    TEST_CASE("the forward pass renders a lit cube matching the CPU reference at sampled points")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN((std::string("No graphics device on this host: ") + GraphicsDevice::GetFailureReason()).c_str());
            return;
        }

        ForwardScene scene;
        std::vector<Vec3> pixels;
        std::string failure;
        REQUIRE_MESSAGE(RenderForwardFrame(*device, scene, pixels, failure), failure);
        REQUIRE(pixels.size() == kSize * kSize);

        // Diagnostic first: a corner far outside the cube must still hold the NaN clear. If it reads 0 instead, the
        // problem is the clear or the readback path — not the shading comparison below. (Printed rather than only
        // asserted so a single run tells which half of the pipeline broke.)
        const Vec3 corner = pixels[0];
        INFO("corner pixel (outside cube): " << Describe(corner) << "  NaN=" << std::isnan(corner.X));

        // Sampled surface is the cube's +Z (front) face: the CPU reference below passes localGeometricNormal = +Z,
        // which matches only if depth resolved visibility to that face. The NaN clear plus the "not black" check at
        // the end of this case together prove it: back-face wins would read exactly zero and fail loudly.
        // Compare every texel whose centre falls inside the cube's front face. Sweeping the whole small target beats
        // hand-picking points: it cannot miss a region, and at 16x16 with the unit cube covering roughly a quarter of
        // the image it costs ~64 comparisons per run.
        UInt32 compared = 0;
        std::string firstFailure;

        for (UInt32 texelY = 0; texelY < kSize; ++texelY)
        {
            for (UInt32 texelX = 0; texelX < kSize; ++texelX)
            {
                const Vec3 world = scene.Camera.SampleWorldPositionAtTexelCentre(texelX, texelY);

                // Inside the face footprint only: outside it the pixel holds the NaN clear, and the CPU reference has
                // nothing to say about empty space.
                if (std::abs(world.X) > 0.5f || std::abs(world.Y) > 0.5f)
                {
                    continue;
                }

                const Vec3 expected = scene.ReferenceAt(world);
                const Vec3 actual = pixels[texelY * kSize + texelX];

                // A NaN here means the shader never wrote this pixel — reported per texel, since "nothing drew" and
                // "drew wrong" need different fixes.
                REQUIRE_MESSAGE(!std::isnan(actual.X),
                                "texel (" << texelX << "," << texelY << ") was never written by the shader");

                ++compared;

                // Tolerance: both sides evaluate the same point with the same formulas; what remains is float32
                // rounding plus driver-internal precision (GPUs may contract multiply-adds, and some evaluate
                // transcendentals at reduced precision). 5% relative with a small absolute floor catches every error
                // class this test targets — a doubled cosine is a 4x miss, a dropped light removes a third of the
                // value — without failing on arithmetic noise. Going tighter needs a fixed-precision reference, which
                // is more machinery than M3 justifies.
                const Float32 tolerance = 0.05f * std::max(1.0f, std::abs(expected.X)) + 1.0e-3f;

                const bool matches = std::abs(actual.X - expected.X) <= tolerance &&
                                     std::abs(actual.Y - expected.Y) <= tolerance &&
                                     std::abs(actual.Z - expected.Z) <= tolerance;
                if (!matches && firstFailure.empty())
                {
                    firstFailure = "texel (" + std::to_string(texelX) + "," + std::to_string(texelY) + ") world " +
                                   Describe(world) + "\n  gpu " + Describe(actual) + "\n  cpu " + Describe(expected);
                }
            }
        }

        INFO("first mismatch:\n" << firstFailure);
        CHECK_MESSAGE(firstFailure.empty(), "CPU/GPU shading disagreement over the cube's front face");

        // Guard against a vacuous pass: the sweep must have covered a real block of texels.
        CHECK(compared >= 16u);

        // The lit face must not be black: catches a pipeline that compiled, drew and wrote zeros.
        const Vec3 centre = pixels[(kSize / 2) * kSize + (kSize / 2)];
        CHECK(centre.X > 0.01f);
    }

    TEST_CASE("removing the spot light changes only pixels outside its cone")
    {
        GraphicsDevice* device = GetDevice();
        if (device == nullptr)
        {
            WARN("No graphics device; cone-behaviour test needs one");
            return;
        }

        // Render with all three lights, then with the spot removed. Pixels inside the cone must brighten when it is
        // present; pixels outside must be *identical* (bit-for-bit after float rounding) — the GPU's zero-outside-
        // cone behaviour asserted on hardware rather than assumed from the CPU function's unit test.
        ForwardScene withSpot;
        std::vector<Vec3> pixelsWithSpot;
        std::string failure;
        REQUIRE(RenderForwardFrame(*device, withSpot, pixelsWithSpot, failure));

        ForwardScene withoutSpot = withSpot;
        // Deactivating a light means BOTH halves: the CPU reference skips inactive lights (its documented contract),
        // and the uploaded array entry is zeroed, because the shader reads whatever data is there — it has no notion
        // of an "active" flag. Doing only the first makes CPU and GPU disagree; doing only the second leaves stale
        // state on the component. A future renderer-owned light collector must preserve both, which is why this test
        // asserts the pair rather than trusting a comment in Lights.h.
        withoutSpot.Params.Lights[2].Light.Active = false;
        withoutSpot.Params.Lights[2].Light.Intensity = 0.0f;

        std::vector<Vec3> pixelsWithoutSpot;
        REQUIRE(RenderForwardFrame(*device, withoutSpot, pixelsWithoutSpot, failure));

        // Centre of the face: inside the spot cone (axis-aligned camera, spot on axis) → must be brighter with it.
        const UInt32 centreIndex = (kSize / 2) * kSize + (kSize / 2);
        CHECK(pixelsWithSpot[centreIndex].X > pixelsWithoutSpot[centreIndex].X);

        // A point on the face far from the axis at the cone's edge: outside the outer angle, so both renders agree.
        // World (0.45, 0.45): the spot at (0,0,2.5) sees it ~37 degrees off-axis — beyond its 22.9 degree outer cone.
        const Vec3 cornerWorld(0.45f, 0.45f, 0.5f);
        const std::array<UInt32, 2> cornerTexel = withSpot.Camera.ToTexel(withSpot.Camera.Project(cornerWorld));
        const UInt32 cornerIndex = cornerTexel[1] * kSize + cornerTexel[0];

        const Float32 difference = std::abs(pixelsWithSpot[cornerIndex].X - pixelsWithoutSpot[cornerIndex].X);
        CHECK_MESSAGE(difference <= 1.0e-4f, "spot leaked outside its cone: " << pixelsWithSpot[cornerIndex].X << " vs "
                                                                              << pixelsWithoutSpot[cornerIndex].X);
    }
}
