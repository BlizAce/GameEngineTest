#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/Game/ForwardShaderLayout.h"
#include "Hollowpoint/Game/SceneRenderer.h"
#include "Hollowpoint/Graphics/Buffer.h"
#include "Hollowpoint/Graphics/GraphicsDevice.h"
#include "Hollowpoint/Graphics/Pipeline.h"
#include "Hollowpoint/Graphics/RenderTarget.h"
#include "Hollowpoint/Graphics/RhiAccess.h"
#include "Hollowpoint/Graphics/ShaderBindings.h"
#include "Hollowpoint/Graphics/ShaderCompiler.h"
#include "Hollowpoint/Graphics/Texture.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Renderer/ForwardRender.h"
#include "Hollowpoint/Renderer/Geometry.h"
#include "Hollowpoint/Renderer/TonemapBlit.h"

// Uniform block layouts, the std140 offset helpers, the mat4 store and the light-type encoding all live in
// ForwardShaderLayout.h — shared with the test that pins them against the generated GLSL. That sharing is the point:
// when the layouts were private to a .cpp and duplicated in a test file, the test pinned its own copies against the
// shader while production held a third description that could drift from both and fail nothing.

namespace Hollowpoint::Game
{

namespace
{

using Core::Size;
using Core::UInt32;
using Core::UInt64;
using Graphics::BindingSlotDesc;
using Graphics::Buffer;
using Graphics::BufferDesc;
using Graphics::BufferUsage;
using Graphics::CompareFunction;
using Graphics::CullMode;
using Graphics::GraphicsDevice;
using Graphics::GraphicsFormat;
using Graphics::Pipeline;
using Graphics::PipelineDesc;
using Graphics::RenderTarget;
using Graphics::ShaderStage;
using Graphics::Texture;
using Graphics::VertexAttribute;
using Renderer::GeometryData;
using Renderer::GeometryVertex;

constexpr const char* kLogChannel = "RENDER";

//! Shader binding slots, named once (the numbers are the generated shaders' block slots — see ForwardRender.h).
constexpr UInt32 kSlotFrameParams = 0;
constexpr UInt32 kSlotTransform = 1;
constexpr UInt32 kSlotLights = 2;
constexpr UInt32 kSlotMaterial = 3;
constexpr UInt32 kSlotLightIndices = 4;

//! `#version` + the binding macros, prepended to body-only shader text. ForwardRender.h documents the required order
//! (#version first, then HP_BINDING_* defined before any block uses them); one assembly point so it cannot drift.
[[nodiscard]] std::string MakeShaderHeader()
{
    return "#version 450\n" + Graphics::BuildGlslBindingPrelude();
}

}  // namespace

//! One mesh's GPU buffers, kept alive across frames so a scene with 200 instances of one cube uploads once rather than
//! 200 times per frame.
struct CachedGeometry
{
    Core::Ref<Buffer> Vertices;
    Core::Ref<Buffer> Indices;
    UInt32 VertexCount = 0;
    UInt32 IndexCount = 0;
};

//! Per-draw constant buffers plus the binding set naming them. One pool entry per draw item in the largest frame seen;
//! contents are rewritten each frame while the buffers and their binding set are reused — a binding set is immutable
//! once created, which is exactly why it belongs to the pool entry rather than to the frame.
struct DrawConstants
{
    Core::Ref<Buffer> Transform;     //!< shader slot 1
    Core::Ref<Buffer> Material;      //!< shader slot 3
    Core::Ref<Buffer> LightIndices;  //!< shader slot 4
    nvrhi::BindingSetHandle Bindings;

    [[nodiscard]] bool HasBuffers() const noexcept
    {
        return Transform != nullptr && Material != nullptr && LightIndices != nullptr;
    }

    [[nodiscard]] bool IsUsable() const noexcept
    {
        return HasBuffers() && static_cast<bool>(Bindings);
    }
};

//! Everything a frame writes that an older in-flight frame may still be reading. One of these per frame slot.
/**
 * This struct is the reason Graphics::FrameRing has a purpose. The ring bounds how many frames may be in flight;
 * without per-slot state that bound protects nothing, because every slot would write the same bytes and frame N+2 would
 * overwrite what frame N's draws are still reading — no error, no validation report on a host with no validation layer,
 * only flicker. Grouping the per-frame data here means "which buffers does this slot own" has exactly one answer.
 */
struct PerFrameState
{
    Core::Ref<Buffer> Frame;     //!< slot 0: view-projection + eye position
    Core::Ref<Buffer> Lights;    //!< slot 2: the frame's light array
    Core::Ref<Buffer> Exposure;  //!< blit exposure

    //! Per-draw buffers for this slot. Not shared between slots even though geometry is: two frames in flight draw
    //! different content, and sharing these would put back exactly the hazard the slots exist to remove.
    std::vector<DrawConstants> DrawPool;
};

//! A forward pipeline for one (colour format, depth presence, cull mode) combination.
/**
 * Keyed by formats rather than by render-target identity because that is precisely Vulkan's pipeline-compatibility
 * rule: a pipeline is valid against any framebuffer whose attachment formats match those it was created with. Keying on
 * target address would be both weaker and leakier — a window resize frees the old back-buffer targets and hands fresh
 * objects (often at recycled addresses) to the next frame, so an address-keyed cache can return a pipeline built
 * against a destroyed target while a real mismatch goes unreported, and every resize adds entries that never die.
 * Keying on formats bounds the cache by the number of formats in use (a handful) and makes reuse automatic across
 * resizes.
 */
struct ForwardVariant
{
    Core::Ref<Pipeline> State;
    GraphicsFormat ColourFormat = GraphicsFormat::Unknown;
    bool HasDepth = false;
    CullMode Culling = CullMode::Back;
};

//! A tonemap pipeline for one (colour format, depth presence) combination. The blit has no depth *state*, but its
//! pipeline is still created against a framebuffer info that includes or omits the depth attachment, so both halves of
//! the key matter.
struct TonemapVariant
{
    Core::Ref<Pipeline> State;
    GraphicsFormat ColourFormat = GraphicsFormat::Unknown;
    bool HasDepth = false;
};

//! Implementation. Nothing here escapes through SceneRenderer.h, which is why NVRHI types are welcome in it.
class SceneRendererImpl
{
public:
    GraphicsDevice* Device = nullptr;  //!< not owned; must outlive this
    std::string Name = "scene-renderer";
    UInt32 MaxLightsPerDraw = 8;

    // SPIR-V is compiled once and retained: pipelines are created lazily per target format, so a new render target (a
    // resized swapchain hands out fresh objects) must not force recompiling shaders.
    std::vector<UInt32> ForwardVertexSpirV;
    std::vector<UInt32> ForwardFragmentSpirV;
    std::vector<UInt32> TonemapVertexSpirV;
    std::vector<UInt32> TonemapFragmentSpirV;

    //! Per-slot state count: one past the largest valid slot argument. Kept beside Slots so the bound and the storage
    //! cannot disagree — every check below compares against this rather than Slots.size().
    UInt32 SlotCount = 1;

    //! One entry per frame slot, sized at Create() from SlotCount. Indexed by the caller's slot argument.
    std::vector<PerFrameState> Slots;

    nvrhi::SamplerHandle NearestSampler;

    std::unordered_map<const GeometryData*, CachedGeometry> GeometryCache;
    std::vector<ForwardVariant> ForwardVariants;
    std::vector<TonemapVariant> TonemapVariants;

    UInt64 Draws = 0;
    UInt64 Submissions = 0;
    bool HasWarnedDepthlessTarget = false;
    bool HasWarnedLightOverflow = false;

    [[nodiscard]] nvrhi::IDevice* Rhi() const noexcept
    {
        return Device != nullptr ? Device->GetNativeDevice() : nullptr;
    }

    // --- one-time setup --------------------------------------------------------

    [[nodiscard]] bool CompileShaders()
    {
        const std::string header = MakeShaderHeader();
        Graphics::ShaderCompileOptions options;

        options.SourceName = Name + "/forward.vert";
        Graphics::CompiledShader vertex =
            Graphics::CompileGlsl(ShaderStage::Vertex, header + Renderer::BuildForwardVertexGlsl(), options);
        if (!vertex.Succeeded)
        {
            HP_LOG_ERROR(kLogChannel, "{}: forward vertex shader failed: {}", Name, vertex.Diagnostics);
            return false;
        }

        options.SourceName = Name + "/forward.frag";
        Graphics::CompiledShader fragment = Graphics::CompileGlsl(
            ShaderStage::Pixel, header + Renderer::BuildForwardFragmentGlsl(MaxLightsPerDraw), options);
        if (!fragment.Succeeded)
        {
            HP_LOG_ERROR(kLogChannel, "{}: forward fragment shader failed: {}", Name, fragment.Diagnostics);
            return false;
        }

        options.SourceName = Name + "/tonemap.vert";
        Graphics::CompiledShader blitVertex =
            Graphics::CompileGlsl(ShaderStage::Vertex, header + Renderer::BuildTonemapBlitVertexGlsl(), options);
        if (!blitVertex.Succeeded)
        {
            HP_LOG_ERROR(kLogChannel, "{}: tonemap vertex shader failed: {}", Name, blitVertex.Diagnostics);
            return false;
        }

        options.SourceName = Name + "/tonemap.frag";
        Graphics::CompiledShader blitFragment =
            Graphics::CompileGlsl(ShaderStage::Pixel, header + Renderer::BuildTonemapBlitFragmentGlsl(), options);
        if (!blitFragment.Succeeded)
        {
            HP_LOG_ERROR(kLogChannel, "{}: tonemap fragment shader failed: {}", Name, blitFragment.Diagnostics);
            return false;
        }

        ForwardVertexSpirV = std::move(vertex.SpirV);
        ForwardFragmentSpirV = std::move(fragment.SpirV);
        TonemapVertexSpirV = std::move(blitVertex.SpirV);
        TonemapFragmentSpirV = std::move(blitFragment.SpirV);
        return true;
    }

    [[nodiscard]] Core::Ref<Buffer> CreateConstant(std::string name, Size bytes)
    {
        BufferDesc desc;
        desc.Name = std::move(name);
        // std140 units — see ForwardShaderLayout.h. A buffer smaller than the block the shader reads lets the shader
        // read past the end, which on most drivers reads zero rather than faulting.
        desc.ByteSize = static_cast<UInt32>(ShaderLayout::AlignUp(bytes, 16u));
        desc.Usage = BufferUsage::Constant;

        Core::Ref<Buffer> buffer = Buffer::Create(*Device, desc);
        if (buffer == nullptr)
        {
            HP_LOG_ERROR(kLogChannel, "{}: could not create constant buffer '{}'", Name, desc.Name);
        }
        return buffer;
    }

    [[nodiscard]] bool CreateSharedResources()
    {
        nvrhi::IDevice* rhi = Rhi();
        if (rhi == nullptr)
        {
            HP_LOG_ERROR(kLogChannel, "{}: device has no RHI object", Name);
            return false;
        }

        Slots.resize(SlotCount);
        for (Size index = 0; index < Slots.size(); ++index)
        {
            const std::string suffix = SlotCount > 1u ? ("[" + std::to_string(index) + "]") : "";
            PerFrameState& slot = Slots[index];
            slot.Frame = CreateConstant("hp-frame" + suffix, sizeof(ForwardFrameBlock));
            slot.Lights = CreateConstant("hp-lights" + suffix, ShaderLayout::LightBlockBytes(MaxLightsPerDraw));
            slot.Exposure = CreateConstant("hp-exposure" + suffix, sizeof(TonemapExposureBlock));
            if (slot.Frame == nullptr || slot.Lights == nullptr || slot.Exposure == nullptr)
            {
                return false;
            }
        }

        // Nearest filtering: the blit's contract is an exact 1:1 copy at matching extents, so anything interpolated
        // would read as a shading bug rather than a filter choice. NVRHI's filters are bools (true = lerp) — there is
        // no FilteringMode enum to guess at — and mip filtering stays off because the scene target has one level.
        nvrhi::SamplerDesc samplerDesc;
        samplerDesc.setAllFilters(false);
        samplerDesc.addressU = nvrhi::SamplerAddressMode::Clamp;
        samplerDesc.addressV = nvrhi::SamplerAddressMode::Clamp;
        NearestSampler = rhi->createSampler(samplerDesc);
        if (!static_cast<bool>(NearestSampler))
        {
            HP_LOG_ERROR(kLogChannel, "{}: could not create the blit sampler", Name);
            return false;
        }

        return true;
    }

    // --- geometry ----------------------------------------------------------------

    [[nodiscard]] const CachedGeometry* AcquireGeometry(const GeometryData& geometry)
    {
        if (geometry.IsEmpty() || geometry.Indices.empty())
        {
            // Not fatal for the frame — the caller skips this entry — but worth one line: an empty mesh reaching here
            // usually means an importer produced nothing, which is otherwise invisible.
            HP_LOG_WARNING(kLogChannel, "{}: geometry has no vertices or indices; nothing to draw", Name);
            return nullptr;
        }

        const auto existing = GeometryCache.find(&geometry);
        if (existing != GeometryCache.end())
        {
            const CachedGeometry& cached = existing->second;
            // Same address with different counts is a reload, and drawing the previous payload would be invisible.
            // Equal counts at an equal address cannot be told apart without hashing the payload every frame, so the
            // public API documents that an asset layer calls ReleaseGeometry when it frees or replaces mesh data.
            if (cached.VertexCount == static_cast<UInt32>(geometry.Vertices.size()) &&
                cached.IndexCount == static_cast<UInt32>(geometry.Indices.size()))
            {
                return &cached;
            }
            GeometryCache.erase(existing);
        }

        BufferDesc vertexDesc;
        vertexDesc.Name = "hp-vertices";
        vertexDesc.ByteSize = static_cast<UInt32>(geometry.Vertices.size() * sizeof(GeometryVertex));
        vertexDesc.Usage = BufferUsage::Vertex;
        Core::Ref<Buffer> vertices = Buffer::Create(*Device, vertexDesc);

        BufferDesc indexDesc;
        indexDesc.Name = "hp-indices";
        indexDesc.ByteSize = static_cast<UInt32>(geometry.Indices.size() * sizeof(UInt32));
        indexDesc.Usage = BufferUsage::Index;
        Core::Ref<Buffer> indices = Buffer::Create(*Device, indexDesc);

        if (vertices == nullptr || indices == nullptr)
        {
            return nullptr;
        }

        nvrhi::IDevice* rhi = Rhi();
        nvrhi::CommandListHandle commandList = rhi->createCommandList();
        if (!static_cast<bool>(commandList))
        {
            HP_LOG_ERROR(kLogChannel, "{}: could not create an upload command list", Name);
            return nullptr;
        }

        commandList->open();
        commandList->writeBuffer(vertices->GetNativeBuffer(), geometry.Vertices.data(),
                                 geometry.Vertices.size() * sizeof(GeometryVertex));
        commandList->writeBuffer(indices->GetNativeBuffer(), geometry.Indices.data(),
                                 geometry.Indices.size() * sizeof(UInt32));
        commandList->close();

        nvrhi::ICommandList* raw = commandList;
        rhi->executeCommandLists(&raw, 1);

        // Blocking is deliberate: this runs at most once per mesh, and "the buffers hold their data by return" keeps
        // every later frame's behaviour independent of upload timing. A real asset streamer batches uploads on its own
        // queue; pretending to be one here would only move the wait somewhere less obvious.
        Device->WaitForIdle();

        CachedGeometry upload;
        upload.Vertices = std::move(vertices);
        upload.Indices = std::move(indices);
        upload.VertexCount = static_cast<UInt32>(geometry.Vertices.size());
        upload.IndexCount = static_cast<UInt32>(geometry.Indices.size());

        return &GeometryCache.emplace(&geometry, std::move(upload)).first->second;
    }

    // --- pipelines ----------------------------------------------------------------

    [[nodiscard]] ForwardVariant* GetForwardVariant(const RenderTarget& target, CullMode culling)
    {
        const GraphicsFormat colourFormat = target.GetColourTexture() != nullptr
                                                ? target.GetColourTexture()->GetDesc().Format
                                                : GraphicsFormat::Unknown;
        const bool hasDepth = target.HasDepth();

        for (ForwardVariant& variant : ForwardVariants)
        {
            if (variant.ColourFormat == colourFormat && variant.HasDepth == hasDepth && variant.Culling == culling)
            {
                return &variant;
            }
        }

        if (!hasDepth && !HasWarnedDepthlessTarget)
        {
            HP_LOG_WARNING(kLogChannel,
                           "{}: rendering to a target with no depth attachment — overlapping geometry resolves by "
                           "rasterisation order rather than depth",
                           Name);
            HasWarnedDepthlessTarget = true;
        }

        PipelineDesc desc;
        desc.Name = Name + "-forward";
        desc.VertexShader.SpirV = ForwardVertexSpirV;
        desc.PixelShader.SpirV = ForwardFragmentSpirV;

        // Five constant-buffer slots, visible to both stages. A slot invisible to a stage reads as zeros there — the
        // silent-zero failure ShaderBindings.h exists to prevent — so visibility is not reasoned about per slot.
        for (UInt32 slot = 0; slot < 5u; ++slot)
        {
            desc.Bindings.push_back(BindingSlotDesc{BindingSlotDesc::Kind::ConstantBuffer, slot});
        }
        desc.BindingsVisibleToVertex = true;
        desc.BindingsVisibleToPixel = true;

        desc.TriangleCulling = culling;
        // Front face stays counter-clockwise (PipelineDesc's documented default). NVRHI's own default asks Vulkan for a
        // clockwise front face, so "back-face culling" discards the camera-facing side — which is how the first mesh
        // ever drawn with this engine rendered its interior.
        desc.IsDepthEnabled = hasDepth;
        desc.IsDepthWriteEnabled = hasDepth;
        desc.DepthFunction = CompareFunction::LessEqual;

        // Attribute names are the binding mechanism (createInputLayout matches by name — GraphicsTypes.h); offsets and
        // stride come from GeometryVertex's pinned 32-byte layout. ElementStride is never left at NVRHI's default of 0,
        // which makes every vertex read element zero (session 12).
        desc.VertexAttributes = {
            VertexAttribute{"InPosition", GraphicsFormat::Rgb32Float, 0, static_cast<UInt32>(sizeof(GeometryVertex))},
            VertexAttribute{"InNormal", GraphicsFormat::Rgb32Float, 12, static_cast<UInt32>(sizeof(GeometryVertex))},
            VertexAttribute{"InUv", GraphicsFormat::Rg32Float, 24, static_cast<UInt32>(sizeof(GeometryVertex))},
        };

        Core::Ref<Pipeline> pipeline = Pipeline::Create(*Device, desc, target);
        if (pipeline == nullptr)
        {
            HP_LOG_ERROR(kLogChannel, "{}: forward pipeline creation failed", Name);
            return nullptr;
        }

        ForwardVariants.push_back(ForwardVariant{std::move(pipeline), colourFormat, hasDepth, culling});
        return &ForwardVariants.back();
    }

    [[nodiscard]] TonemapVariant* GetTonemapVariant(const RenderTarget& target)
    {
        const GraphicsFormat colourFormat = target.GetColourTexture() != nullptr
                                                ? target.GetColourTexture()->GetDesc().Format
                                                : GraphicsFormat::Unknown;
        const bool hasDepth = target.HasDepth();

        for (TonemapVariant& variant : TonemapVariants)
        {
            if (variant.ColourFormat == colourFormat && variant.HasDepth == hasDepth)
            {
                return &variant;
            }
        }

        PipelineDesc desc;
        desc.Name = Name + "-tonemap";
        desc.VertexShader.SpirV = TonemapVertexSpirV;
        desc.PixelShader.SpirV = TonemapFragmentSpirV;

        // No vertex attributes: the triangle is synthesised from gl_VertexIndex, so declaring any would make
        // createInputLayout expect buffers that do not exist.
        desc.Bindings = {
            BindingSlotDesc{BindingSlotDesc::Kind::ConstantBuffer, Renderer::TonemapBlitBindings::ExposureBuffer},
            BindingSlotDesc{BindingSlotDesc::Kind::SampledTexture, Renderer::TonemapBlitBindings::SourceTexture},
            BindingSlotDesc{BindingSlotDesc::Kind::Sampler, Renderer::TonemapBlitBindings::SourceSampler},
        };
        desc.BindingsVisibleToPixel = true;

        // No depth: the blit covers the whole viewport and must work into a back buffer that has no attachment of its
        // own. Enabling a depth test against an undefined buffer is the flakiness trap documented in session 14.
        desc.IsDepthEnabled = false;
        desc.IsDepthWriteEnabled = false;
        desc.TriangleCulling = CullMode::None;

        Core::Ref<Pipeline> pipeline = Pipeline::Create(*Device, desc, target);
        if (pipeline == nullptr)
        {
            HP_LOG_ERROR(kLogChannel, "{}: tonemap pipeline creation failed", Name);
            return nullptr;
        }

        TonemapVariants.push_back(TonemapVariant{std::move(pipeline), colourFormat, hasDepth});
        return &TonemapVariants.back();
    }

    // --- per-frame constant buffers -------------------------------------------------

    //! Ensures this slot's draw pool reaches `index`, with a binding set built against `layout`. A binding set is
    //! immutable and names specific buffers, so it is created once per pool entry and reused; every forward variant
    //! shares the same five-slot description, which is what makes one set valid against any of them (Vulkan requires
    //! the pipeline's set-0 layout to match the bound set — identical descriptor lists make them compatible). Were a
    //! variant ever to declare different slots, this reuse would have to become per-variant; the requirement is stated
    //! rather than assumed silently.
    [[nodiscard]] bool EnsureDrawConstants(PerFrameState& slot, Size index, nvrhi::IBindingLayout* layout)
    {
        while (slot.DrawPool.size() <= index)
        {
            DrawConstants constants;
            constants.Transform = CreateConstant("hp-transform", sizeof(ForwardTransformBlock));
            constants.Material = CreateConstant("hp-material", sizeof(ForwardMaterialBlock));
            constants.LightIndices =
                CreateConstant("hp-light-indices", ShaderLayout::IndexBlockBytes(MaxLightsPerDraw));
            slot.DrawPool.push_back(std::move(constants));
        }

        DrawConstants& entry = slot.DrawPool[index];
        if (!entry.HasBuffers())
        {
            return false;
        }
        if (static_cast<bool>(entry.Bindings))
        {
            return true;
        }

        nvrhi::IDevice* rhi = Rhi();
        entry.Bindings = rhi->createBindingSet(
            nvrhi::BindingSetDesc()
                .addItem(nvrhi::BindingSetItem::ConstantBuffer(kSlotFrameParams, slot.Frame->GetNativeBuffer()))
                .addItem(nvrhi::BindingSetItem::ConstantBuffer(kSlotTransform, entry.Transform->GetNativeBuffer()))
                .addItem(nvrhi::BindingSetItem::ConstantBuffer(kSlotLights, slot.Lights->GetNativeBuffer()))
                .addItem(nvrhi::BindingSetItem::ConstantBuffer(kSlotMaterial, entry.Material->GetNativeBuffer()))
                .addItem(
                    nvrhi::BindingSetItem::ConstantBuffer(kSlotLightIndices, entry.LightIndices->GetNativeBuffer())),
            layout);
        if (!static_cast<bool>(entry.Bindings))
        {
            HP_LOG_ERROR(kLogChannel, "{}: binding set creation failed", Name);
            return false;
        }
        return true;
    }

    void WriteFrameConstants(const PerFrameState& slot, const RenderPlan& plan, nvrhi::ICommandList* commandList)
    {
        ForwardFrameBlock frame{};
        ShaderLayout::StoreMat4(frame.ViewProjection, plan.Pass.ViewProjection);
        frame.CameraPositionAndPad[0] = plan.Pass.CameraPosition.X;
        frame.CameraPositionAndPad[1] = plan.Pass.CameraPosition.Y;
        frame.CameraPositionAndPad[2] = plan.Pass.CameraPosition.Z;
        commandList->writeBuffer(slot.Frame->GetNativeBuffer(), &frame, sizeof(frame));

        // Light array in structure-of-arrays form: four vec4 per light, each field contiguous — the layout the
        // generated block declares, and what makes the upload a straight per-field walk rather than an interleaved
        // pack.
        std::vector<float> lights(ShaderLayout::LightBlockBytes(MaxLightsPerDraw) / sizeof(float), 0.0f);
        const UInt32 written = std::min<UInt32>(static_cast<UInt32>(plan.Pass.Lights.size()), MaxLightsPerDraw);

        for (UInt32 index = 0; index < written; ++index)
        {
            const Renderer::ForwardLight& light = plan.Pass.Lights[index];

            float* position = &lights[ShaderLayout::LightFieldOffset(MaxLightsPerDraw, 0u, index) / sizeof(float)];
            position[0] = light.WorldPosition.X;
            position[1] = light.WorldPosition.Y;
            position[2] = light.WorldPosition.Z;
            position[3] = light.Light.Intensity;

            float* colour = &lights[ShaderLayout::LightFieldOffset(MaxLightsPerDraw, 1u, index) / sizeof(float)];
            colour[0] = light.Light.Color.X;
            colour[1] = light.Light.Color.Y;
            colour[2] = light.Light.Color.Z;
            colour[3] = ShaderLayout::EncodeLightType(light.Light.Type);

            float* range = &lights[ShaderLayout::LightFieldOffset(MaxLightsPerDraw, 2u, index) / sizeof(float)];
            range[0] = light.Light.Range;
            range[1] = light.Light.SpotInnerAngleRadians;
            range[2] = light.Light.SpotOuterAngleRadians;

            float* direction = &lights[ShaderLayout::LightFieldOffset(MaxLightsPerDraw, 3u, index) / sizeof(float)];
            direction[0] = light.WorldDirection.X;
            direction[1] = light.WorldDirection.Y;
            direction[2] = light.WorldDirection.Z;
        }

        if (static_cast<UInt32>(plan.Pass.Lights.size()) > MaxLightsPerDraw && !HasWarnedLightOverflow)
        {
            // Once, not per frame: lights past the bound contribute nothing, and a flood would bury every other message
            // while the frame itself merely looks "a bit dim" — expensive to diagnose from logs.
            HP_LOG_WARNING(kLogChannel, "{}: scene has {} lights but pipelines hold {}; later lights are ignored", Name,
                           plan.Pass.Lights.size(), MaxLightsPerDraw);
            HasWarnedLightOverflow = true;
        }

        commandList->writeBuffer(slot.Lights->GetNativeBuffer(), lights.data(), lights.size() * sizeof(float));
    }

    //! Writes one draw's transform/material/index buffers. A light index outside the frame's list is reported and
    //! skipped: it is a wiring bug (scene edited under the renderer), and silently dropping it changes lighting with no
    //! trace.
    void WriteDrawConstants(const RenderMeshEntry& entry, Size lightCount, const DrawConstants& constants,
                            nvrhi::ICommandList* commandList)
    {
        ForwardTransformBlock transform{};
        ShaderLayout::StoreMat4(transform.ModelMatrix, entry.Draw.ModelMatrix);
        // The shader applies mat3(NormalMatrix) to per-vertex normals and normalises in the fragment stage. Computing
        // it through ComputeNormalMatrix means the CPU reference and the GPU use literally the same matrix;
        // hand-inverting here is how lighting starts disagreeing with the reference — only on non-uniformly-scaled
        // objects, far from where it shows.
        const Math::Mat3 normalMatrix = Renderer::ComputeNormalMatrix(entry.Draw.ModelMatrix.GetUpperLeft3());
        ShaderLayout::StoreMat4(transform.NormalMatrix, Math::Mat4(normalMatrix));
        commandList->writeBuffer(constants.Transform->GetNativeBuffer(), &transform, sizeof(transform));

        ForwardMaterialBlock material{};
        material.BaseColor[0] = entry.Draw.Material.BaseColor.X;
        material.BaseColor[1] = entry.Draw.Material.BaseColor.Y;
        material.BaseColor[2] = entry.Draw.Material.BaseColor.Z;
        // Authored roughness/metallic pass through unclamped: the shader clamps both to [0,1] and the CPU reference
        // clamps inside EvaluateDirectLight, so clamping here would apply the policy a third time in a place of its
        // own.
        material.MaterialParams[0] = entry.Draw.Material.Roughness;
        material.MaterialParams[1] = entry.Draw.Material.Metallic;
        commandList->writeBuffer(constants.Material->GetNativeBuffer(), &material, sizeof(material));

        // Light index block: ivec4 groups, then the count at its own 16-byte unit. Written per draw even though every
        // mesh currently shares one list — Game's collector gives every mesh every active light today, and this layout
        // already supports the per-object selection it will move to.
        std::vector<int> indices(ShaderLayout::IndexBlockBytes(MaxLightsPerDraw) / sizeof(int), 0);
        UInt32 count = 0;
        for (const UInt32 lightIndex : entry.Draw.LightIndices)
        {
            if (count >= MaxLightsPerDraw)
            {
                break;
            }
            if (lightIndex >= static_cast<UInt32>(lightCount))
            {
                HP_LOG_WARNING(kLogChannel, "{}: draw references light index {} outside the frame's {} lights", Name,
                               lightIndex, lightCount);
                continue;
            }
            indices[count] = static_cast<int>(lightIndex);
            ++count;
        }

        const Size countOffset = ShaderLayout::IndexBlockCountOffset(MaxLightsPerDraw) / sizeof(int);
        indices[countOffset] = static_cast<int>(count);

        commandList->writeBuffer(constants.LightIndices->GetNativeBuffer(), indices.data(),
                                 indices.size() * sizeof(int));
    }
};

// --- SceneRenderer ---------------------------------------------------------------

SceneRenderer::SceneRenderer() = default;

SceneRenderer::~SceneRenderer()
{
    delete m_Impl;
    m_Impl = nullptr;
}

Core::Ref<SceneRenderer> SceneRenderer::Create(GraphicsDevice& device, const SceneRendererDesc& desc)
{
    if (device.GetNativeDevice() == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "SceneRenderer::Create: device has no RHI object");
        return nullptr;
    }

    if (desc.MaxLightsPerDraw == 0u)
    {
        // Asserted then refused: a zero light bound generates `vec4 LightArray[0]`, which glslang rejects with a
        // message naming neither this call nor the field it came from.
        HP_ASSERT_ARGUMENT(false, "SceneRendererDesc::MaxLightsPerDraw must be greater than zero");
        return nullptr;
    }

    if (desc.FrameSlots == 0u)
    {
        HP_ASSERT_ARGUMENT(false, "SceneRendererDesc::FrameSlots must be greater than zero");
        return nullptr;
    }

    Core::Ref<SceneRenderer> renderer = Core::Ref<SceneRenderer>(new SceneRenderer());
    auto* impl = new SceneRendererImpl();
    renderer->m_Impl = impl;
    impl->Device = &device;
    impl->Name = desc.Name;
    impl->MaxLightsPerDraw = desc.MaxLightsPerDraw;
    impl->SlotCount = desc.FrameSlots;

    if (!impl->CompileShaders() || !impl->CreateSharedResources())
    {
        return nullptr;  // m_Impl goes with the renderer; every failure path logged its own cause
    }

    HP_LOG_INFO(kLogChannel, "scene renderer ready (max {} lights per draw, {} frame slot(s))", desc.MaxLightsPerDraw,
                desc.FrameSlots);
    return renderer;
}

UInt32 SceneRenderer::GetMaxLightsPerDraw() const noexcept
{
    return m_Impl != nullptr ? m_Impl->MaxLightsPerDraw : 0u;
}

UInt32 SceneRenderer::GetFrameSlotCount() const noexcept
{
    return m_Impl != nullptr ? m_Impl->SlotCount : 0u;
}

UInt32 SceneRenderer::GetForwardVariantCount() const noexcept
{
    return m_Impl != nullptr ? static_cast<UInt32>(m_Impl->ForwardVariants.size()) : 0u;
}

UInt32 SceneRenderer::GetTonemapVariantCount() const noexcept
{
    return m_Impl != nullptr ? static_cast<UInt32>(m_Impl->TonemapVariants.size()) : 0u;
}

UInt32 SceneRenderer::GetDrawPoolSize(UInt32 slot) const noexcept
{
    if (m_Impl == nullptr || slot >= m_Impl->SlotCount)
    {
        return 0u;
    }
    return static_cast<UInt32>(m_Impl->Slots[slot].DrawPool.size());
}

bool SceneRenderer::HasGeometry(const GeometryData& geometry) const noexcept
{
    if (m_Impl == nullptr)
    {
        return false;
    }
    return m_Impl->GeometryCache.count(&geometry) != 0u;
}

void SceneRenderer::ReleaseGeometry(const GeometryData& geometry) noexcept
{
    if (m_Impl != nullptr)
    {
        m_Impl->GeometryCache.erase(&geometry);
    }
}

UInt32 SceneRenderer::GetCachedGeometryCount() const noexcept
{
    return m_Impl != nullptr ? static_cast<UInt32>(m_Impl->GeometryCache.size()) : 0u;
}

UInt64 SceneRenderer::GetDrawCount() const noexcept
{
    return m_Impl != nullptr ? m_Impl->Draws : 0u;
}

UInt64 SceneRenderer::GetSubmissionCount() const noexcept
{
    return m_Impl != nullptr ? m_Impl->Submissions : 0u;
}

bool SceneRenderer::RenderForward(const RenderPlan& plan, const Graphics::RenderTarget& target, UInt32 slot,
                                  const Vec4& backgroundColour)
{
    if (m_Impl == nullptr || m_Impl->Rhi() == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "RenderForward called on an unusable renderer");
        return false;
    }

    SceneRendererImpl& impl = *m_Impl;

    // Asserted then refused rather than clamped: a slot out of range means the caller's frame accounting and this
    // renderer's disagree, and silently picking slot 0 would write over a frame that may still be in flight — the exact
    // hazard slots exist to prevent. An assertion is a report, never a guard (AGENTS.md §4), so the refusal follows it.
    if (slot >= impl.SlotCount)
    {
        HP_ASSERT_ARGUMENT(false, "RenderForward slot index is outside the renderer's frame ring");
        return false;
    }

    PerFrameState& frame = impl.Slots[slot];
    nvrhi::IDevice* rhi = impl.Rhi();

    const Texture* colourTexture = target.GetColourTexture();
    if (colourTexture == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "RenderForward: target has no colour attachment");
        return false;
    }

    // Resolve every drawable entry BEFORE recording. A plan whose geometry will not upload then fails before any GPU
    // work is submitted, rather than submitting a frame whose clear landed while its meshes silently vanished — which
    // reads as a culling or lighting bug rather than as the upload failure it was.
    struct PendingDraw
    {
        const RenderMeshEntry* Entry = nullptr;
        const CachedGeometry* Geometry = nullptr;
        Size PoolIndex = 0;
    };

    std::vector<PendingDraw> pending;
    pending.reserve(plan.Meshes.size());

    for (const RenderMeshEntry& entry : plan.Meshes)
    {
        if (!entry.Draw.IsDrawable())
        {
            continue;  // Game's collector already logged why (empty path, unresolvable asset)
        }

        const CachedGeometry* geometry = impl.AcquireGeometry(*entry.Draw.Geometry);
        if (geometry == nullptr)
        {
            HP_LOG_WARNING(kLogChannel, "mesh {} skipped: geometry upload failed", entry.Owner.ToString());
            continue;
        }

        pending.push_back(PendingDraw{&entry, geometry, static_cast<Size>(pending.size())});
    }

    nvrhi::CommandListHandle commandList = rhi->createCommandList();
    if (!static_cast<bool>(commandList))
    {
        HP_LOG_ERROR(kLogChannel, "RenderForward: could not create a command list");
        return false;
    }

    commandList->open();

    impl.WriteFrameConstants(frame, plan, commandList);

    // The clear belongs to the pass rather than to each caller: attachment memory is undefined at creation (session 14
    // hit LessOrEqual against garbage depth), and a frame that draws nothing must still show backgroundColour rather
    // than whatever the allocator left behind. The header's NaN default means an unwritten texel reads back as NaN and
    // fails a comparison loudly.
    commandList->clearTextureFloat(
        colourTexture->GetNativeTexture(), nvrhi::TextureSubresourceSet(),
        nvrhi::Color(backgroundColour.X, backgroundColour.Y, backgroundColour.Z, backgroundColour.W));

    const Texture* depthTexture = target.GetDepthTexture();
    if (depthTexture != nullptr)
    {
        // Far = 1.0 with LessOrEqual: every fragment of a real draw then passes. Clearing to 0 would reject the whole
        // scene and produce exactly the black frame this clear exists to prevent.
        commandList->clearDepthStencilTexture(depthTexture->GetNativeTexture(), nvrhi::TextureSubresourceSet(), true,
                                              1.0f, false, 0);
    }

    UInt32 drawsThisFrame = 0;
    for (const PendingDraw& draw : pending)
    {
        ForwardVariant* variant = impl.GetForwardVariant(target, draw.Entry->Culling);
        if (variant == nullptr)
        {
            // Logged at creation. Skipping the entry keeps the rest of the frame rendering — one bad pipeline should
            // not blank a whole scene.
            continue;
        }

        if (!impl.EnsureDrawConstants(frame, draw.PoolIndex, variant->State->GetBindingLayout()))
        {
            continue;
        }

        const DrawConstants& constants = frame.DrawPool[draw.PoolIndex];
        impl.WriteDrawConstants(*draw.Entry, plan.Pass.Lights.size(), constants, commandList);

        nvrhi::GraphicsState state;
        state.setPipeline(variant->State->GetNativePipeline())
            .setFramebuffer(target.GetNativeFramebuffer())
            .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                nvrhi::Viewport(static_cast<float>(target.GetWidth()), static_cast<float>(target.GetHeight()))))
            .addVertexBuffer(nvrhi::VertexBufferBinding{draw.Geometry->Vertices->GetNativeBuffer(), 0, 0})
            // The index buffer is part of graphics state, not an argument of drawIndexed: omitting it makes the driver
            // index garbage — which cost a device-lost surfacing as "readback failed" several calls later with nothing
            // naming the missing binding (session 14). R32_UINT because GeometryData::Indices are uint32 and
            // GraphicsFormat has no index-format entry (no indexed draw existed before), so NVRHI's format is named
            // directly through the documented escape hatch.
            .setIndexBuffer(
                nvrhi::IndexBufferBinding{draw.Geometry->Indices->GetNativeBuffer(), nvrhi::Format::R32_UINT, 0})
            .addBindingSet(constants.Bindings);

        commandList->setGraphicsState(state);
        commandList->drawIndexed(nvrhi::DrawArguments().setVertexCount(draw.Geometry->IndexCount).setInstanceCount(1));
        ++drawsThisFrame;
    }

    commandList->close();

    nvrhi::ICommandList* raw = commandList;
    rhi->executeCommandLists(&raw, 1);

    ++impl.Submissions;
    impl.Draws += drawsThisFrame;

    // NVRHI releases resources referenced by finished command lists from its own garbage collector and documents that
    // the call must happen at least once per frame — a renderer that never makes it leaks every buffer it ever
    // submitted. Driven here rather than left to an application loop so no host can forget it.
    rhi->runGarbageCollection();

    return true;
}

bool SceneRenderer::BlitTonemap(const Texture& source, const RenderTarget& destination, Float32 exposure, UInt32 slot)
{
    if (m_Impl == nullptr || m_Impl->Rhi() == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "BlitTonemap called on an unusable renderer");
        return false;
    }

    SceneRendererImpl& impl = *m_Impl;

    if (slot >= impl.SlotCount)
    {
        HP_ASSERT_ARGUMENT(false, "BlitTonemap slot index is outside the renderer's frame ring");
        return false;
    }

    PerFrameState& frame = impl.Slots[slot];
    nvrhi::IDevice* rhi = impl.Rhi();

    if (!source.GetDesc().IsShaderResource)
    {
        // Refused rather than attempted: the binding set names the texture as an SRV, and a texture created without
        // shader-resource usage has no view — NVRHI then fails the binding set with a message that does not name this
        // cause.
        HP_LOG_ERROR(kLogChannel, "BlitTonemap: source '{}' was not created with IsShaderResource",
                     source.GetDesc().Name);
        return false;
    }

    const Texture* destinationColour = destination.GetColourTexture();
    if (destinationColour == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "BlitTonemap: destination has no colour attachment");
        return false;
    }

    TonemapVariant* variant = impl.GetTonemapVariant(destination);
    if (variant == nullptr)
    {
        return false;
    }

    // One descriptor set per call. Deliberately not cached: a set names a specific source view, so caching means keying
    // on the source texture — and an HDR target is recreated whenever the window resizes, which would leave the cache
    // holding (and handing out) a view of a destroyed texture: exactly the staleness the format-keyed pipeline caches
    // above exist to avoid. Traded for one descriptor-set allocation per frame, reclaimed by the GC below; a post chain
    // with a fixed set of targets can cache properly once there is one.
    nvrhi::BindingSetHandle bindingSet = rhi->createBindingSet(
        nvrhi::BindingSetDesc()
            .addItem(nvrhi::BindingSetItem::ConstantBuffer(Renderer::TonemapBlitBindings::ExposureBuffer,
                                                           frame.Exposure->GetNativeBuffer()))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(Renderer::TonemapBlitBindings::SourceTexture,
                                                        source.GetNativeTexture()))
            .addItem(nvrhi::BindingSetItem::Sampler(Renderer::TonemapBlitBindings::SourceSampler, impl.NearestSampler)),
        variant->State->GetBindingLayout());
    if (!static_cast<bool>(bindingSet))
    {
        HP_LOG_ERROR(kLogChannel, "BlitTonemap: binding set creation failed");
        return false;
    }

    const TonemapExposureBlock exposureBlock{exposure};

    nvrhi::CommandListHandle commandList = rhi->createCommandList();
    if (!static_cast<bool>(commandList))
    {
        HP_LOG_ERROR(kLogChannel, "BlitTonemap: could not create a command list");
        return false;
    }

    commandList->open();
    commandList->writeBuffer(frame.Exposure->GetNativeBuffer(), &exposureBlock, sizeof(exposureBlock));

    // Cleared first for the same reason the forward pass clears: an undefined destination makes a blit that drew
    // nothing indistinguishable from one that drew black. Alpha 1 so a composited back buffer stays opaque even where
    // the blit missed.
    commandList->clearTextureFloat(destinationColour->GetNativeTexture(), nvrhi::TextureSubresourceSet(),
                                   nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));

    nvrhi::GraphicsState state;
    state.setPipeline(variant->State->GetNativePipeline())
        .setFramebuffer(destination.GetNativeFramebuffer())
        .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
            nvrhi::Viewport(static_cast<float>(destination.GetWidth()), static_cast<float>(destination.GetHeight()))))
        .addBindingSet(bindingSet);

    commandList->setGraphicsState(state);
    // Three vertices, no vertex buffer: the full-screen triangle is synthesised from gl_VertexIndex.
    commandList->draw(nvrhi::DrawArguments().setVertexCount(3).setInstanceCount(1));
    commandList->close();

    nvrhi::ICommandList* raw = commandList;
    rhi->executeCommandLists(&raw, 1);

    ++impl.Submissions;
    rhi->runGarbageCollection();
    return true;
}

bool SceneRenderer::RenderFrame(const RenderPlan& plan, const RenderTarget& hdrTarget,
                                const RenderTarget& displayTarget, Float32 exposure, UInt32 slot,
                                const Vec4& backgroundColour)
{
    if (!RenderForward(plan, hdrTarget, slot, backgroundColour))
    {
        return false;
    }

    const Texture* hdrColour = hdrTarget.GetColourTexture();
    if (hdrColour == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "RenderFrame: HDR target has no colour attachment to tone map");
        return false;
    }

    return BlitTonemap(*hdrColour, displayTarget, exposure, slot);
}

}  // namespace Hollowpoint::Game
