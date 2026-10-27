#include "imgui.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Editor/UiRenderer.h"
#include "Hollowpoint/Graphics/Buffer.h"
#include "Hollowpoint/Graphics/GraphicsDevice.h"
#include "Hollowpoint/Graphics/Pipeline.h"
#include "Hollowpoint/Graphics/RenderTarget.h"
#include "Hollowpoint/Graphics/RhiAccess.h"
#include "Hollowpoint/Graphics/ShaderBindings.h"
#include "Hollowpoint/Graphics/ShaderCompiler.h"
#include "Hollowpoint/Graphics/Texture.h"

// ---------------------------------------------------------------------------
// UiRenderer implementation — see the header for why this is not imgui_impl_vulkan.
//
// Frame contract: ImGui::Render() has produced draw data; upload whatever textures it marked dirty, copy its
// vertex/index buffers into two NVRHI buffers, and replay its command list as indexed draws with one scissor rect
// per command. Everything goes through the engine's own wrappers (Buffer, Texture, Pipeline), so resource state is
// tracked by NVRHI exactly once — the point of not using the stock backend inside an NVRHI-rendered frame.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Editor
{

using Core::Size;
using Core::UInt64;
using Graphics::BindingSlotDesc;
using Graphics::BlendMode;
using Graphics::Buffer;
using Graphics::BufferDesc;
using Graphics::BufferUsage;
using Graphics::CullMode;
using Graphics::GraphicsDevice;
using Graphics::GraphicsFormat;
using Graphics::Pipeline;
using Graphics::PipelineDesc;
using Graphics::RenderTarget;
using Graphics::ShaderProgram;
using Graphics::Texture;
using Graphics::TextureDesc;
using Graphics::VertexAttribute;

namespace
{

constexpr const char* kLogChannel = "EDITOR";

//! Binding slots the UI shader binds, in engine slot numbering (NVRHI adds the per-type Vulkan offsets — see
//! ShaderBindings.h, whose reason for existing is that raw binding numbers read as zeros forever).
constexpr UInt32 kFontTextureSlot = 0;
constexpr UInt32 kFontSamplerSlot = 0;
constexpr UInt32 kScreenConstantSlot = 0;

//! Screen-extent constant buffer slots, rotated per frame. Three because the frame ring allows three frames in
//! flight: rewriting a single slot would overwrite extents while an earlier frame's UI draws are still reading them
//! — the exact hazard FrameRing documents, one struct wide but identical in kind.
constexpr UInt32 kScreenConstantSlots = 3;

[[nodiscard]] std::string BuildVertexGlsl()
{
    // ImGui vertices arrive in framebuffer pixel coordinates with (0,0) at the top-left. Vulkan's NDC has +Y up while
    // its framebuffer has +Y down, so mapping y=0 → ndc +1 is the flip that makes text read downwards; omitting it
    // mirrors the entire UI vertically, which on flat panels is invisible until a glyph gives it away.
    return "#version 450\n" + Graphics::BuildGlslBindingPrelude() + R"glsl(
layout(location = 0) in vec2 InPosition;
layout(location = 1) in vec2 InUv;
layout(location = 2) in vec4 InColor;

layout(location = 0) out vec2 OutUv;
layout(location = 1) out vec4 OutColor;

// Viewport extent in physical pixels (ImGui's DisplaySize x FramebufferScale).
layout(set = 0, binding = HP_BINDING_CONSTANT_BUFFER(0)) uniform HpUiScreen
{
    vec2 ScreenSize;
} hpUiScreen;

void main()
{
    vec2 ndc = vec2(InPosition.x / hpUiScreen.ScreenSize.x * 2.0 - 1.0,
                    1.0 - InPosition.y / hpUiScreen.ScreenSize.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    OutUv = InUv;
    OutColor = InColor;
}
)glsl";
}

[[nodiscard]] std::string BuildFragmentGlsl()
{
    // Straight-alpha output: PipelineDesc::Blending == Alpha composites `src*a + dst*(1-a)`, which is ImGui's
    // convention (its vertices carry non-premultiplied colour). Premultiplying here as well would double-apply alpha
    // and darken every translucent panel edge.
    return "#version 450\n" + Graphics::BuildGlslBindingPrelude() + R"glsl(
layout(location = 0) in vec2 InUv;
layout(location = 1) in vec4 InColor;

layout(location = 0) out vec4 OutColour;

layout(set = 0, binding = HP_BINDING_TEXTURE(0)) uniform texture2D HpFontAtlas;
layout(set = 0, binding = HP_BINDING_SAMPLER(0)) uniform sampler HpUiSampler;

void main()
{
    vec4 atlas = texture(sampler2D(HpFontAtlas, HpUiSampler), InUv);
    OutColour = InColor * atlas;
}
)glsl";
}

//! ImGui's per-texture id for an ImTextureData is the pointer itself, offset by one so it can never collide with
//! ImTextureID_Invalid (0). One function states that twice: at creation and wherever a command resolves it.
[[nodiscard]] inline ImTextureID TextureIdOf(const ImTextureData* tex) noexcept
{
    return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(tex)) + 1u;
}

}  // namespace

struct UiRenderer::Impl
{
    GraphicsDevice* Device = nullptr;  // not owned; must outlive the renderer (engine-wide lifetime rule)

    //! Compiled against the first target drawn into; rebuilt when a different colour format arrives (the editor's
    //! back-buffer format never changes at runtime, but an offscreen UI test target may differ from it — and a
    //! pipeline whose formats disagree with its framebuffer is undefined behaviour Vulkan need not report).
    Ref<Pipeline> UiPipeline;
    GraphicsFormat PipelineFormat = GraphicsFormat::Unknown;

    nvrhi::SamplerHandle LinearSampler;

    //! 1×1 white atlas bound when a command references a texture this renderer could not create. Visible solid
    //! quads beat invisible ones: "text became grey blocks" names the upload as the suspect; an empty window does not.
    Ref<Texture> FallbackWhite;
    nvrhi::BindingSetHandle FallbackSets[kScreenConstantSlots];

    //! Grown-on-demand dynamic buffers reused across frames.
    Ref<Buffer> Vertices;
    Ref<Buffer> Indices;

    //! One engine texture, plus its binding set per screen-constant slot.
    /**
     * Why an array of sets rather than one: the engine attaches a single binding layout to a pipeline (Pipeline.cpp's
     * createBindingLayout + addBindingLayout), so the screen constant buffer, the texture and the sampler all have to
     * live in the *same* descriptor set — binding them as two sets is a validation error ("sum of firstSet and
     * descriptorSetCount exceeds setLayoutCount") and undefined behaviour without one. That layout is fixed per
     * pipeline, so each combination of (this texture, this frame's constant buffer) needs its own set. With three
     * rotating constant slots and a handful of textures (font atlas, viewport image, white fallback) the product is
     * tiny and built lazily.
     */
    struct TextureEntry
    {
        Ref<Texture> Colour;
        nvrhi::BindingSetHandle Sets[kScreenConstantSlots];
    };
    std::unordered_map<ImTextureID, TextureEntry> Textures;

    //! Engine textures handed to widgets by RegisterTexture (the viewport image). Separate map because their ids are
    //! issued from a counter rather than derived from an ImTextureData pointer: keeping the two namespaces apart means
    //! a heap address can never be mistaken for a registered id, and unregistering one engine texture cannot drop the
    //! font atlas. Counter values (1, 2, 3 …) cannot collide with a `(pointer & ~0xFFF)`-style atlas id on any platform
    //! this engine builds for — userspace addresses are far above the first page.
    std::unordered_map<ImTextureID, TextureEntry> Registered;
    UInt64 NextIssuedTextureId = 1;

    //! Screen-extent constants, rotated per frame (see kScreenConstantSlots). Created once per slot and never
    //! replaced, which is what lets TextureEntry cache its sets: a cached set names one buffer and one texture, and if
    //! either could be swapped underneath it the cache would be a use-after-free generator.
    Ref<Buffer> ScreenConstants[kScreenConstantSlots];
    UInt32 NextConstantSlot = 0;

    //! Screen extent for the frame currently being recorded, written by Render into that frame's command list.
    float PendingScreenWidth = 1.0f;
    float PendingScreenHeight = 1.0f;

    //! CPU staging reused across frames — steady state allocates nothing, the same reasoning Platform/Window.h gives
    //! for draining input into a caller's vector.
    std::vector<ImDrawVert> ScratchVertices;
    std::vector<ImDrawIdx> ScratchIndices;

    UInt64 DrawCalls = 0;

    [[nodiscard]] nvrhi::IDevice* Rhi() const noexcept
    {
        return Device != nullptr ? Device->GetNativeDevice() : nullptr;
    }

    //! Services ImGui's texture create/update/destroy requests for every texture it knows about.
    void RefreshTextures(ImDrawData* drawData);

    [[nodiscard]] Ref<Pipeline> EnsurePipeline(const RenderTarget& target);

    [[nodiscard]] bool EnsureDynamicBuffers(Size vertexBytes, Size indexBytes);

    //! Advances the constant-slot rotation, ensures that slot's buffer exists and records this frame's extent for
    //  Render to upload. Returns false only if the buffer could not be created.
    [[nodiscard]] bool AdvanceScreenConstants(UInt32& outSlot, const RenderTarget& target);

    //! The set combining \p entry's texture/sampler with constant slot \p screenSlot, built on first use.
    [[nodiscard]] nvrhi::BindingSetHandle SetForTexture(TextureEntry& entry, UInt32 screenSlot);

    //! The binding set for one draw command: its texture (or the white fallback) at the given constant slot.
    [[nodiscard]] nvrhi::BindingSetHandle BindingsForCommand(const ImDrawCmd& command, UInt32 screenSlot);
};

void UiRenderer::Impl::RefreshTextures(ImDrawData* drawData)
{
    nvrhi::IDevice* rhi = Rhi();
    if (rhi == nullptr || UiPipeline == nullptr)
    {
        return;
    }

    // The texture list normally lives on the draw data; it can be null when an application manages textures manually,
    // in which case ImGui parked them on the platform IO list instead. Checking both means a future editor feature
    // that hands ImGui its own atlas (icons) works without revisiting this function.
    const ImVector<ImTextureData*>* textures = drawData->Textures;
    if (textures == nullptr)
    {
        textures = &ImGui::GetPlatformIO().Textures;
    }

    for (ImTextureData* tex : *textures)
    {
        if (tex == nullptr)
        {
            continue;
        }

        const ImTextureID id = TextureIdOf(tex);
        const auto existing = Textures.find(id);

        if (tex->Status == ImTextureStatus_WantDestroy || tex->Status == ImTextureStatus_Destroyed)
        {
            // Drop the engine texture and hand ImGui its invalid id back. Skipping the invalidation would let a font
            // atlas destroyed and re-created at the same address be treated as "already uploaded" — last session's
            // glyphs, which survive every test until someone changes DPI and nothing matches any more.
            if (existing != Textures.end())
            {
                Textures.erase(existing);
            }
            if (tex->Status == ImTextureStatus_WantDestroy)
            {
                tex->SetTexID(ImTextureID_Invalid);
                tex->SetStatus(ImTextureStatus_Destroyed);
            }
            continue;
        }

        const bool wantsCreate = tex->Status == ImTextureStatus_WantCreate;
        const bool wantsUpdate = tex->Status == ImTextureStatus_WantUpdates;
        if (!wantsCreate && !wantsUpdate)
        {
            continue;  // OK: nothing this backend must do
        }

        if (tex->Format != ImTextureFormat_RGBA32)
        {
            // Alpha8 is the only other format ImGui emits and nothing in this editor produces it. Reported, marked
            // handled so ImGui stops asking, and drawn with the fallback rather than sampled through a wrong view.
            HP_LOG_ERROR(kLogChannel, "UI texture format {} is unsupported; using the white fallback",
                         static_cast<int>(tex->Format));
            tex->SetStatus(ImTextureStatus_OK);
            continue;
        }

        if (wantsCreate || existing == Textures.end())
        {
            TextureDesc desc;
            desc.Name = "ui-atlas";
            desc.Width = static_cast<UInt32>(std::max(1, tex->Width));
            desc.Height = static_cast<UInt32>(std::max(1, tex->Height));
            desc.Format = GraphicsFormat::Rgba8Unorm;
            desc.IsRenderTarget = false;
            desc.IsShaderResource = true;

            Ref<Texture> texture = Texture::Create(*Device, desc);
            if (texture == nullptr)
            {
                HP_LOG_ERROR(kLogChannel, "could not create a {}x{} UI atlas", tex->Width, tex->Height);
                tex->SetStatus(ImTextureStatus_OK);
                continue;
            }

            TextureEntry entry;
            entry.Colour = std::move(texture);
            // Assignment rather than emplace: on a recreated atlas (same ImGui texture, new pixels — DPI change) the
            // map already holds an entry whose binding set points at the destroyed texture. Overwriting it drops both
            // the old handle and its now-meaningless binding set, which the block below rebuilds.
            Textures[id] = std::move(entry);
        }

        auto stored = Textures.find(id);
        if (stored == Textures.end() || stored->second.Colour == nullptr)
        {
            tex->SetStatus(ImTextureStatus_OK);
            continue;
        }

        // Full-atlas upload rather than the WantUpdates rectangle list: ImGui marks the atlas dirty only when glyphs
        // change (font/DPI), i.e. rarely, and full replacement is the one path that cannot miss a rect.
        const Size byteCount = static_cast<Size>(tex->Width) * static_cast<Size>(tex->Height) * 4u;
        if (tex->Pixels != nullptr && !stored->second.Colour->Update(*Device, tex->Pixels, byteCount))
        {
            HP_LOG_ERROR(kLogChannel, "UI atlas upload failed for a {}x{} texture", tex->Width, tex->Height);
        }

        // No binding set built here: they need a constant slot, which is only known while recording. SetForTexture
        // creates them lazily, and a recreated atlas overwrites the entry — dropping its sets with it, which is exactly
        // what must happen when the texture inside them dies.

        // The id ImGui's draw commands will carry. GetTexID() asserts on an unbacked texture, so this must happen for
        // every texture that reaches OK — including the fallback-forced ones above, which stay invalid and are then
        // resolved to the white fallback by BindingsForCommand.
        tex->SetTexID(id);
        tex->SetStatus(ImTextureStatus_OK);
    }
}

Ref<Pipeline> UiRenderer::Impl::EnsurePipeline(const RenderTarget& target)
{
    const Texture* colour = target.GetColourTexture();
    if (colour == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "UI cannot render into a target with no colour attachment");
        return nullptr;
    }

    const GraphicsFormat format = colour->GetDesc().Format;
    if (UiPipeline != nullptr && PipelineFormat == format)
    {
        return UiPipeline;
    }

    Graphics::CompiledShader vertex = Graphics::CompileGlsl(Graphics::ShaderStage::Vertex, BuildVertexGlsl());
    if (!vertex.Succeeded)
    {
        HP_LOG_ERROR(kLogChannel, "UI vertex shader failed: {}", vertex.Diagnostics);
        return nullptr;
    }

    Graphics::CompiledShader fragment = Graphics::CompileGlsl(Graphics::ShaderStage::Pixel, BuildFragmentGlsl());
    if (!fragment.Succeeded)
    {
        HP_LOG_ERROR(kLogChannel, "UI pixel shader failed: {}", fragment.Diagnostics);
        return nullptr;
    }

    PipelineDesc desc;
    desc.Name = "hp-ui";
    desc.VertexShader = ShaderProgram{std::move(vertex.SpirV), "ui.vert"};
    desc.PixelShader = ShaderProgram{std::move(fragment.SpirV), "ui.frag"};

    // ImGui's ImDrawVert is pos(2f) + uv(2f) + col(4×u8). Position and UV use the engine's vec-attribute formats;
    // colour reuses Rgba8Unorm, which Pipeline maps to VK_FORMAT_R8G8B8A8_UNORM — a format whose vertex component
    // type is UNORM, so the hardware converts the four bytes to a normalised float vec4 for free. Declaring it as
    // four floats instead would read 16 bytes from a 4-byte field and paint garbage; adding a bespoke "unorm8 vec4"
    // enum entry for one consumer is what GraphicsTypes.h warns against when it says members get copied because the
    // API can. Per-attribute stride is ImGui's struct size, offset 0 for the first attribute as Pipeline requires.
    desc.VertexAttributes = {
        VertexAttribute{"InPosition", GraphicsFormat::Rg32Float, 0, sizeof(ImDrawVert)},
        VertexAttribute{"InUv", GraphicsFormat::Rg32Float, offsetof(ImDrawVert, uv), sizeof(ImDrawVert)},
        VertexAttribute{"InColor", GraphicsFormat::Rgba8Unorm, offsetof(ImDrawVert, col), sizeof(ImDrawVert)},
    };

    desc.TriangleCulling = CullMode::None;  // ImGui's clipping can flip winding; culling would delete clipped UI.
    desc.IsDepthEnabled = false;
    desc.IsDepthWriteEnabled = false;
    desc.Blending = BlendMode::Alpha;

    desc.Bindings = {
        BindingSlotDesc{BindingSlotDesc::Kind::ConstantBuffer, kScreenConstantSlot},
        BindingSlotDesc{BindingSlotDesc::Kind::SampledTexture, kFontTextureSlot},
        BindingSlotDesc{BindingSlotDesc::Kind::Sampler, kFontSamplerSlot},
    };
    desc.BindingsVisibleToVertex = true;
    desc.BindingsVisibleToPixel = true;

    Ref<Pipeline> pipeline = Pipeline::Create(*Device, desc, target);
    if (pipeline == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "UI pipeline creation failed");
        return nullptr;
    }

    // A new pipeline means a new binding layout: every cached set was created against the old one and would
    // dereference a destroyed VkDescriptorSetLayout. Clear them all — textures are re-created from ImGui's still-
    // resident pixel data on the next RefreshTextures.
    // Every set below was created against the layout that is about to be replaced. Keeping one of them means
    // vkCmdBindDescriptorSets with a set from a dead VkDescriptorSetLayout — validation catches it, release drivers do
    // not, and the symptom is a device loss several calls later with nothing naming this as the cause.
    Textures.clear();
    for (UInt32 slot = 0; slot < kScreenConstantSlots; ++slot)
    {
        FallbackSets[slot] = nullptr;
    }

    UiPipeline = std::move(pipeline);
    PipelineFormat = format;
    return UiPipeline;
}

bool UiRenderer::Impl::EnsureDynamicBuffers(Size vertexBytes, Size indexBytes)
{
    nvrhi::IDevice* rhi = Rhi();
    if (rhi == nullptr)
    {
        return false;
    }

    // Buffers grow but never shrink: a UI that briefly shows a 10k-line console and then hides it should not
    // reallocate twice. The cap that matters is ImGui's own — draw data is bounded by the screen, so these stay small
    // in practice (tens of KB); growth beyond megabytes would mean a runaway draw list, which is a bug to fix where
    // it happens rather than something this function should absorb.
    if (!static_cast<bool>(Vertices) || Vertices->GetDesc().ByteSize < vertexBytes)
    {
        BufferDesc desc;
        desc.Name = "hp-ui-vertices";
        desc.ByteSize = std::max(vertexBytes, static_cast<Size>(64) * 1024);
        desc.Usage = BufferUsage::Vertex;
        Vertices = Buffer::Create(*Device, desc);
        if (Vertices == nullptr)
        {
            HP_LOG_ERROR(kLogChannel, "could not create the UI vertex buffer");
            return false;
        }
    }

    if (!static_cast<bool>(Indices) || Indices->GetDesc().ByteSize < indexBytes)
    {
        BufferDesc desc;
        desc.Name = "hp-ui-indices";
        desc.ByteSize = std::max(indexBytes, static_cast<Size>(16) * 1024);
        desc.Usage = BufferUsage::Index;
        Indices = Buffer::Create(*Device, desc);
        if (Indices == nullptr)
        {
            HP_LOG_ERROR(kLogChannel, "could not create the UI index buffer");
            return false;
        }
    }

    return true;
}

bool UiRenderer::Impl::AdvanceScreenConstants(UInt32& outSlot, const RenderTarget& target)
{
    nvrhi::IDevice* rhi = Rhi();
    if (rhi == nullptr || UiPipeline == nullptr)
    {
        return false;
    }

    const UInt32 slot = NextConstantSlot;
    NextConstantSlot = (NextConstantSlot + 1u) % kScreenConstantSlots;
    outSlot = slot;

    if (!static_cast<bool>(ScreenConstants[slot]))
    {
        BufferDesc desc;
        desc.Name = "hp-ui-screen";
        // 16 bytes, not 8: the struct is one vec2 (std140 size 8) but rounding to a cacheline-sized allocation keeps
        // every driver's minUniformBufferOffsetAlignment happy without consulting limits per device.
        desc.ByteSize = 16;
        desc.Usage = BufferUsage::Constant;
        ScreenConstants[slot] = Buffer::Create(*Device, desc);
        if (ScreenConstants[slot] == nullptr)
        {
            HP_LOG_ERROR(kLogChannel, "could not create the UI screen constant buffer");
            return false;
        }
    }

    // The value itself is written by Render into its own command list: writeBuffer belongs to ICommandList, not
    // IDevice, and opening a second submission just to upload eight bytes would put an extra sync point in the frame.
    PendingScreenWidth = static_cast<float>(std::max(1u, target.GetWidth()));
    PendingScreenHeight = static_cast<float>(std::max(1u, target.GetHeight()));

    return true;
}

nvrhi::BindingSetHandle UiRenderer::Impl::SetForTexture(TextureEntry& entry, UInt32 screenSlot)
{
    if (screenSlot >= kScreenConstantSlots || entry.Colour == nullptr || UiPipeline == nullptr)
    {
        return nullptr;
    }

    nvrhi::BindingSetHandle& cached = entry.Sets[screenSlot];
    if (static_cast<bool>(cached))
    {
        return cached;
    }

    nvrhi::IDevice* rhi = Rhi();
    if (rhi == nullptr || !static_cast<bool>(ScreenConstants[screenSlot]))
    {
        return nullptr;
    }

    // All three resources in one set, on purpose — see TextureEntry::Sets for why the layout admits no other shape.
    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(
        nvrhi::BindingSetItem::ConstantBuffer(kScreenConstantSlot, ScreenConstants[screenSlot]->GetNativeBuffer()));
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(kFontTextureSlot, entry.Colour->GetNativeTexture()));
    setDesc.addItem(nvrhi::BindingSetItem::Sampler(kFontSamplerSlot, LinearSampler));

    cached = rhi->createBindingSet(setDesc, UiPipeline->GetBindingLayout());
    if (!static_cast<bool>(cached))
    {
        HP_LOG_ERROR(kLogChannel, "could not create a UI binding set");
    }
    return cached;
}

nvrhi::BindingSetHandle UiRenderer::Impl::BindingsForCommand(const ImDrawCmd& command, UInt32 screenSlot)
{
    // A command's texture id is either one of ours (set by RefreshTextures from an ImTextureData pointer), a texture
    // registered from the engine (RegisterTexture — the viewport image), or something this renderer knows nothing
    // about. Unknown ids draw with the white fallback rather than skipping: geometry staying visible but untextured
    // points at the missing registration instead of hiding it, which is a far better bug report than an empty window.
    const ImTextureID id = command.GetTexID();

    if (id != ImTextureID_Invalid)
    {
        auto registered = Registered.find(id);
        if (registered != Registered.end() && registered->second.Colour != nullptr)
        {
            return SetForTexture(registered->second, screenSlot);
        }

        auto stored = Textures.find(id);
        if (stored != Textures.end() && stored->second.Colour != nullptr)
        {
            return SetForTexture(stored->second, screenSlot);
        }
    }

    if (FallbackWhite == nullptr || screenSlot >= kScreenConstantSlots)
    {
        return nullptr;
    }

    nvrhi::BindingSetHandle& cached = FallbackSets[screenSlot];
    if (static_cast<bool>(cached))
    {
        return cached;
    }

    // Cached per screen slot rather than stored as a TextureEntry: the fallback texture is a single 1x1 white image
    // every unmapped command shares, so there is no map to key it by — only the constant slot varies.
    nvrhi::IDevice* rhi = Rhi();
    if (rhi == nullptr || UiPipeline == nullptr || !static_cast<bool>(ScreenConstants[screenSlot]))
    {
        return nullptr;
    }

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(
        nvrhi::BindingSetItem::ConstantBuffer(kScreenConstantSlot, ScreenConstants[screenSlot]->GetNativeBuffer()));
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(kFontTextureSlot, FallbackWhite->GetNativeTexture()));
    setDesc.addItem(nvrhi::BindingSetItem::Sampler(kFontSamplerSlot, LinearSampler));

    cached = rhi->createBindingSet(setDesc, UiPipeline->GetBindingLayout());
    return cached;
}

UInt64 UiRenderer::RegisterTexture(const Ref<Texture>& texture)
{
    if (m_Impl == nullptr || texture == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "RegisterTexture needs a texture");
        return 0;
    }

    const ImTextureID id = m_Impl->NextIssuedTextureId++;

    // No binding set is created here: it must be built against the UI pipeline's binding layout, and widgets register
    // the viewport image before the first draw has compiled one. Sets are built lazily in SetForTexture during Render,
    // which also means a pipeline rebuild for a new target format simply clears them (EnsurePipeline drops every entry)
    // and they are rebuilt on the next frame with no caller having to re-register anything.
    Impl::TextureEntry entry;
    entry.Colour = texture;  // holds a reference: a widget may name this id after the editor dropped its own Ref
    m_Impl->Registered.emplace(id, std::move(entry));
    return static_cast<UInt64>(id);
}

void UiRenderer::UnregisterTexture(UInt64 textureId)
{
    if (m_Impl == nullptr || textureId == 0ull)
    {
        return;
    }
    m_Impl->Registered.erase(static_cast<ImTextureID>(textureId));
}

UiRenderer::UiRenderer() = default;

UiRenderer::~UiRenderer()
{
    delete m_Impl;
}

Ref<UiRenderer> UiRenderer::Create(GraphicsDevice& device)
{
    nvrhi::IDevice* rhi = device.GetNativeDevice();
    if (rhi == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "UI renderer needs a device with an RHI");
        return nullptr;
    }

    auto* impl = new Impl();
    impl->Device = &device;

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.addressU = nvrhi::SamplerAddressMode::Clamp;
    samplerDesc.addressV = nvrhi::SamplerAddressMode::Clamp;
    // Linear filtering: ImGui's atlas packs glyphs with 1–2px padding and positions them at sub-pixel offsets, so
    // nearest resamples text into uneven strokes. The stock backends pick linear for the same reason.
    impl->LinearSampler = rhi->createSampler(samplerDesc);
    if (!static_cast<bool>(impl->LinearSampler))
    {
        HP_LOG_ERROR(kLogChannel, "could not create the UI sampler");
        delete impl;
        return nullptr;
    }

    {
        TextureDesc desc;
        desc.Name = "ui-fallback-white";
        desc.Width = 1;
        desc.Height = 1;
        desc.Format = GraphicsFormat::Rgba8Unorm;
        desc.IsRenderTarget = false;
        desc.IsShaderResource = true;

        impl->FallbackWhite = Texture::Create(device, desc);
        if (impl->FallbackWhite == nullptr)
        {
            HP_LOG_ERROR(kLogChannel, "could not create the UI fallback texture");
            delete impl;
            return nullptr;
        }

        const UInt32 white = 0xFFFFFFFFu;
        if (!impl->FallbackWhite->Update(device, &white, sizeof(white)))
        {
            HP_LOG_ERROR(kLogChannel, "could not upload the UI fallback texture");
            delete impl;
            return nullptr;
        }
    }

    Ref<UiRenderer> renderer(new UiRenderer());
    renderer->m_Impl = impl;
    return renderer;
}

bool UiRenderer::Render(RenderTarget& target)
{
    if (m_Impl == nullptr || m_Impl->Device == nullptr)
    {
        return false;
    }

    if (ImGui::GetCurrentContext() == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "UiRenderer::Render called with no ImGui context");
        return false;
    }

    ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData == nullptr || !drawData->Valid)
    {
        // Nothing was submitted — normal for a hidden window, not an error.
        return true;
    }

    Ref<Pipeline> pipeline = m_Impl->EnsurePipeline(target);
    if (pipeline == nullptr)
    {
        return false;
    }

    nvrhi::IDevice* rhi = m_Impl->Rhi();
    if (rhi == nullptr)
    {
        return false;
    }

    m_Impl->RefreshTextures(drawData);

    // --- gather draw data ---------------------------------------------------------
    // One concatenated vertex/index pair for the whole frame: two buffer writes instead of one per window. Index
    // values inside each list address that list's own vertices, so draws offset them by the list's base via
    // startVertexLocation rather than rewriting every index.
    Size totalVertices = 0;
    Size totalIndices = 0;
    for (ImDrawList* list : drawData->CmdLists)
    {
        totalVertices += static_cast<Size>(list->VtxBuffer.Size);
        totalIndices += static_cast<Size>(list->IdxBuffer.Size);
    }

    if (totalIndices == 0u || totalVertices == 0u)
    {
        return true;  // legitimately empty frame (everything clipped away)
    }

    const Size vertexBytes = totalVertices * static_cast<Size>(sizeof(ImDrawVert));
    const Size indexBytes = totalIndices * static_cast<Size>(sizeof(ImDrawIdx));

    if (!m_Impl->EnsureDynamicBuffers(vertexBytes, indexBytes))
    {
        return false;
    }

    m_Impl->ScratchVertices.resize(static_cast<size_t>(totalVertices));
    m_Impl->ScratchIndices.resize(static_cast<size_t>(totalIndices));

    struct ListBase
    {
        Size VertexBase = 0;
        Size IndexBase = 0;
    };
    std::vector<ListBase> bases;
    bases.reserve(static_cast<size_t>(drawData->CmdLists.Size));

    Size vertexWrite = 0;
    Size indexWrite = 0;
    for (ImDrawList* list : drawData->CmdLists)
    {
        bases.push_back(ListBase{vertexWrite, indexWrite});
        std::copy_n(list->VtxBuffer.Data, static_cast<size_t>(list->VtxBuffer.Size),
                    m_Impl->ScratchVertices.data() + vertexWrite);
        std::copy_n(list->IdxBuffer.Data, static_cast<size_t>(list->IdxBuffer.Size),
                    m_Impl->ScratchIndices.data() + indexWrite);
        vertexWrite += static_cast<Size>(list->VtxBuffer.Size);
        indexWrite += static_cast<Size>(list->IdxBuffer.Size);
    }

    // --- record ---------------------------------------------------------------------
    nvrhi::CommandListHandle commandList = rhi->createCommandList();
    if (!static_cast<bool>(commandList))
    {
        HP_LOG_ERROR(kLogChannel, "UI: could not create a command list");
        return false;
    }

    // The constant slot is chosen before recording starts: the rotation picks which of the three buffers this frame
    // writes, and both the upload below and every binding set used by the draws must name that same slot — a frame in
    // flight reading another frame's extents shows UI sized to the previous window for one frame, then never again.
    UInt32 screenSlot = 0;
    if (!m_Impl->AdvanceScreenConstants(screenSlot, target))
    {
        return false;
    }

    commandList->open();

    struct ScreenConstantsBlock
    {
        float SizeX = 1.0f;
        float SizeY = 1.0f;
    } screenBlock{m_Impl->PendingScreenWidth, m_Impl->PendingScreenHeight};

    commandList->writeBuffer(m_Impl->ScreenConstants[screenSlot]->GetNativeBuffer(), &screenBlock, sizeof(screenBlock));
    commandList->writeBuffer(m_Impl->Vertices->GetNativeBuffer(), m_Impl->ScratchVertices.data(), vertexBytes);
    commandList->writeBuffer(m_Impl->Indices->GetNativeBuffer(), m_Impl->ScratchIndices.data(), indexBytes);

    // ImGui works in virtual (content) pixels; the target is physical. FramebufferScale is the ratio — ignoring it
    // draws the UI at half size on a 2× display and mis-scissors every window edge with it.
    const float scaleX = drawData->FramebufferScale.x > 0.0f ? drawData->FramebufferScale.x : 1.0f;
    const float scaleY = drawData->FramebufferScale.y > 0.0f ? drawData->FramebufferScale.y : 1.0f;

    UInt32 commandsDrawn = 0;
    for (int listIndex = 0; listIndex < drawData->CmdLists.Size; ++listIndex)
    {
        const ImDrawList* list = drawData->CmdLists[listIndex];
        const Size indexBase = bases[static_cast<size_t>(listIndex)].IndexBase;

        for (int commandIndex = 0; commandIndex < list->CmdBuffer.Size; ++commandIndex)
        {
            const ImDrawCmd& command = list->CmdBuffer[commandIndex];
            if (command.UserCallback != nullptr)
            {
                // Custom draw callbacks are not implemented. Skipping keeps the rest of the UI correct, and naming it
                // once makes a missing widget diagnosable rather than an unexplained rectangle.
                HP_LOG_WARNING(kLogChannel, "UI draw callback ignored ({} indices)", command.ElemCount);
                continue;
            }

            if (command.ElemCount == 0u)
            {
                continue;
            }

            // Clip rect → scissor in physical pixels. Clamping is not cosmetic: Vulkan rejects scissors exceeding
            // the image, and ImGui clips against the display, which can round past the last pixel at fractional DPI.
            const float x1 = (command.ClipRect.x - drawData->DisplayPos.x) * scaleX;
            const float y1 = (command.ClipRect.y - drawData->DisplayPos.y) * scaleY;
            const float x2 = (command.ClipRect.z - drawData->DisplayPos.x) * scaleX;
            const float y2 = (command.ClipRect.w - drawData->DisplayPos.y) * scaleY;

            nvrhi::Rect scissor;
            scissor.minX = std::max(0, static_cast<int>(std::floor(x1)));
            scissor.minY = std::max(0, static_cast<int>(std::floor(y1)));
            scissor.maxX = std::min(static_cast<int>(target.GetWidth()), static_cast<int>(std::ceil(x2)));
            scissor.maxY = std::min(static_cast<int>(target.GetHeight()), static_cast<int>(std::ceil(y2)));

            if (scissor.maxX <= scissor.minX || scissor.maxY <= scissor.minY)
            {
                continue;  // fully clipped: normal for off-screen windows, not worth a log line
            }

            nvrhi::BindingSetHandle bindings = m_Impl->BindingsForCommand(command, screenSlot);
            if (!static_cast<bool>(bindings))
            {
                continue;  // no fallback could be built either; skipping keeps the frame alive
            }

            const ListBase& base = bases[static_cast<size_t>(listIndex)];

            nvrhi::GraphicsState state;
            state.setPipeline(pipeline->GetNativePipeline())
                .setFramebuffer(target.GetNativeFramebuffer())
                .setViewport(nvrhi::ViewportState()
                                 .addViewport(nvrhi::Viewport(static_cast<float>(target.GetWidth()),
                                                              static_cast<float>(target.GetHeight())))
                                 .addScissorRect(scissor))
                // The vertex buffer is bound at *this list's* base, not at 0. ImGui numbers vertices per draw list (the
                // first vertex of every window is index 0), and concatenating the lists into one buffer means an
                // unoffset binding makes each window draw the first window's geometry — then its own clip rect hides
                // it. The symptom is exactly one widget visible at the top of the screen with everything else blank,
                // which reads as "the panels never drew" and cost a full debugging pass to find (Session 16).
                .addVertexBuffer(nvrhi::VertexBufferBinding{m_Impl->Vertices->GetNativeBuffer(), 0,
                                                            base.VertexBase * sizeof(ImDrawVert)})
                // ImDrawIdx is 32-bit (hp_imgui compiles it as `unsigned int` — see third_party/CMakeLists.txt for
                // why), matching the R32_UINT index convention SceneRenderer established: GraphicsFormat has no index
                // entries and NVRHI's format is named directly through the documented escape hatch.
                .setIndexBuffer(
                    nvrhi::IndexBufferBinding{m_Impl->Indices->GetNativeBuffer(), nvrhi::Format::R32_UINT, 0})
                // Exactly one set: the engine attaches a single binding layout per pipeline, so constant buffer,
                // texture and sampler are bound together (see TextureEntry::Sets).
                .addBindingSet(bindings);

            commandList->setGraphicsState(state);
            commandList->drawIndexed(nvrhi::DrawArguments()
                                         .setVertexCount(command.ElemCount)
                                         .setInstanceCount(1)
                                         // Indices are absolute in the concatenated index buffer: this command starts
                                         // at its list's base plus its own offset. startVertexLocation stays 0 because
                                         // the vertex buffer binding above already carries the per-list offset — adding
                                         // it twice would shift every window by its own vertex count.
                                         .setStartIndexLocation(static_cast<UInt32>(command.IdxOffset + indexBase))
                                         .setStartVertexLocation(0));
            ++commandsDrawn;
        }
    }

    commandList->close();


    nvrhi::ICommandList* raw = commandList;
    rhi->executeCommandLists(&raw, 1);

    m_Impl->DrawCalls += commandsDrawn;
    return true;
}

UInt64 UiRenderer::GetDrawCallCount() const noexcept
{
    return m_Impl != nullptr ? m_Impl->DrawCalls : 0ull;
}

}  // namespace Hollowpoint::Editor
