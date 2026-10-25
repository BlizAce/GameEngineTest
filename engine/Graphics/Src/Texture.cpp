#include "RhiConversions.h"  // TextureImpl + conversion switches; also brings RhiAccess.h

#include <cstring>
#include <utility>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Graphics/GraphicsDevice.h"
#include "Hollowpoint/Graphics/Texture.h"

namespace Hollowpoint::Graphics
{

namespace
{

//! Converts the engine descriptor and picks the creation state. Returns false when the combination has no legal image
//! layout (see the state-selection comment below); `outNvrhiDesc` is then untouched.
[[nodiscard]] bool MakeNvrhiDesc(const TextureDesc& desc, nvrhi::TextureDesc& outNvrhiDesc)
{
    nvrhi::TextureDesc nvrhiDesc;
    nvrhiDesc.debugName = desc.Name;
    nvrhiDesc.format = Detail::ToNvrhi(desc.Format);
    nvrhiDesc.width = desc.Width;
    nvrhiDesc.height = desc.Height;
    nvrhiDesc.mipLevels = desc.MipLevels;
    nvrhiDesc.dimension = nvrhi::TextureDimension::Texture2D;
    nvrhiDesc.isRenderTarget = desc.IsRenderTarget;
    nvrhiDesc.isShaderResource = desc.IsShaderResource;

    // Note there is no cpuAccess on NVRHI's TextureDesc: CPU-visible textures are a separate object type created
    // via createStagingTexture (which ReadBack does from the GPU texture's own desc). Engine-level Textures are
    // therefore always device-local, and the header says so.

    // State at creation mirrors the draw test that established this path (TestDraw.cpp): render targets start in
    // RenderTarget and keep it, so every command list tracks from there. Skipping keepInitialState is how "the
    // target stayed the clear colour" mysteries are born. Depth targets use DepthWrite, which is where depth
    // attachment loads/stores expect them.
    //
    // The third branch exists because of a trap that cost a debugging session. NVRHI's Vulkan backend converts each
    // ResourceStates bit to an image layout, and **ResourceStates::Common maps to vk::ImageLayout::eUndefined** (see
    // g_ResourceStateMap in vulkan-constants.cpp). With keepInitialState set, CommandList::close() emits a barrier back
    // to the initial state — so an initial state of Common is a barrier whose new layout is UNDEFINED. NVRHI asserts
    // exactly that in debug (commitBarriersInternal) and writes an invalid transition into the command buffer
    // otherwise. It stayed latent because nothing created a non-render-target texture until the tone-map blit needed a
    // sampled source, and every existing test left IsRenderTarget at its default of true. Hence: choose a state with a
    // real layout per supported usage, and refuse a texture that has none.
    if (Detail::IsDepthFormat(desc.Format))
    {
        // Reported and refused by the caller (an assertion is a report, never a guard — AGENTS.md §4). Returning false
        // matters because creating a depth texture that is not a render target yields a resource with no legal layout
        // and no way to use it: every later failure would name the command list rather than this call.
        if (!desc.IsRenderTarget)
        {
            HP_LOG_ERROR("GRAPHICS", "Texture::Create: '{}' is a depth format but not a render target", desc.Name);
            return false;
        }
        nvrhiDesc.initialState = nvrhi::ResourceStates::DepthWrite;
    }
    else if (desc.IsRenderTarget)
    {
        nvrhiDesc.initialState = nvrhi::ResourceStates::RenderTarget;
    }
    else if (desc.IsShaderResource)
    {
        // Sampled-only: the layout a shader reads through. Update()/ReadBack() transition out to the transfer layouts
        // and back, which keepInitialState then restores to a legal, defined layout.
        nvrhiDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    }
    else
    {
        // No usage at all: there is no image layout NVRHI can park this in that any operation would accept.
        HP_LOG_ERROR("GRAPHICS",
                     "Texture::Create: '{}' has neither IsRenderTarget nor IsShaderResource — a texture with no usage "
                     "cannot be created",
                     desc.Name);
        return false;
    }

    nvrhiDesc.keepInitialState = true;

    // Note: CreateWrapping (swapchain back buffers) never reaches this function — it wraps an existing native handle
    // and takes no state decisions, which is correct: a swapchain image's layout belongs to the compositor and
    // vkAcquireNextImageKHR, and Vulkan permits only UNDEFINED or PRESENT_SRC_KHR between presentation and acquire.

    outNvrhiDesc = nvrhiDesc;
    return true;
}

}  // namespace

Core::Ref<Texture> Texture::Create(GraphicsDevice& device, const TextureDesc& desc)
{
    HP_ASSERT_ARGUMENT(desc.Width > 0 && desc.Height > 0, "Texture::Create requires non-zero dimensions");
    HP_ASSERT_ARGUMENT(desc.Format != GraphicsFormat::Unknown, "Texture::Create requires a known format");

    nvrhi::IDevice* rhi = device.GetNativeDevice();
    if (rhi == nullptr)
    {
        HP_LOG_ERROR("GRAPHICS", "Texture::Create called with a device that has no RHI object");
        return nullptr;
    }

    nvrhi::TextureDesc nvrhiDesc;
    if (!MakeNvrhiDesc(desc, nvrhiDesc))
    {
        return nullptr;  // reason already logged with the texture's name
    }

    nvrhi::TextureHandle handle = rhi->createTexture(nvrhiDesc);
    if (!static_cast<bool>(handle))
    {
        HP_LOG_ERROR("GRAPHICS", "createTexture failed for \"{}\" ({}x{}, format {})", desc.Name, desc.Width,
                     desc.Height, static_cast<unsigned>(desc.Format));
        return nullptr;
    }

    // Ref's explicit pointer constructor rather than CreateObject — see Buffer::Create for why the ctor is private
    // and that helper cannot reach it.
    Core::Ref<Texture> texture = Core::Ref<Texture>(new Texture());
    texture->m_Desc = desc;
    texture->m_Impl = new TextureImpl{std::move(handle)};
    return texture;
}

Core::Ref<Texture> Texture::CreateWrapping(const TextureDesc& desc, nvrhi::ITexture* native)
{
    // No creation and no ownership transfer: `native` is borrowed (the swapchain owns the underlying image), and the
    // handle stored here is a strong NVRHI reference to the wrapper object, not to the VkImage. Callers must destroy
    // the returned Texture before whatever owns the image — Swapchain documents that ordering at its Destroy().
    assert(native != nullptr && "Texture::CreateWrapping requires a valid native texture");

    Core::Ref<Texture> texture = Core::Ref<Texture>(new Texture());
    texture->m_Desc = desc;
    texture->m_Impl = new TextureImpl{nvrhi::TextureHandle(native)};
    return texture;
}

Texture::~Texture()
{
    delete m_Impl;
    m_Impl = nullptr;
}

bool Texture::IsValid() const noexcept
{
    return m_Impl != nullptr && static_cast<bool>(m_Impl->Handle);
}

nvrhi::ITexture* Texture::GetNativeTexture() const noexcept
{
    return IsValid() ? m_Impl->Handle : nullptr;
}

std::vector<UInt8> Texture::ReadBack(GraphicsDevice& device, UInt32& outRowPitchBytes)
{
    outRowPitchBytes = 0;

    if (!IsValid())
    {
        HP_LOG_ERROR("GRAPHICS", "Texture::ReadBack on an invalid texture");
        return {};
    }

    nvrhi::IDevice* rhi = device.GetNativeDevice();
    if (rhi == nullptr)
    {
        return {};
    }

    // The staging texture is created from the GPU texture's own descriptor with CPU-read access — building a desc
    // by hand here is how format conversions go silently wrong; NVRHI decides the CPU layout, we only ask for it.
    const nvrhi::TextureDesc gpuDesc = m_Impl->Handle->getDesc();

    nvrhi::StagingTextureHandle staging = rhi->createStagingTexture(gpuDesc, nvrhi::CpuAccessMode::Read);
    if (!static_cast<bool>(staging))
    {
        HP_LOG_ERROR("GRAPHICS", "createStagingTexture failed for readback of \"{}\"", m_Desc.Name);
        return {};
    }

    nvrhi::CommandListHandle commandList = rhi->createCommandList();
    if (!static_cast<bool>(commandList))
    {
        HP_LOG_ERROR("GRAPHICS", "createCommandList failed for readback of \"{}\"", m_Desc.Name);
        return {};
    }

    commandList->open();

    // Explicit transition before the copy. NVRHI tracks state per resource and does not insert this implicitly;
    // without it the copy reads an undefined layout — silently, on a host with no validation layer.
    commandList->setTextureState(m_Impl->Handle, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
    commandList->copyTexture(staging, nvrhi::TextureSlice(), m_Impl->Handle, nvrhi::TextureSlice());
    commandList->close();

    nvrhi::ICommandList* rawCommandList = commandList;
    rhi->executeCommandLists(&rawCommandList, 1);

    if (!rhi->waitForIdle())
    {
        HP_LOG_ERROR("GRAPHICS", "waitForIdle failed during readback of \"{}\" (device lost?)", m_Desc.Name);
        return {};
    }

    size_t rowPitch = 0;
    void* mapped = rhi->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch);
    if (mapped == nullptr)
    {
        HP_LOG_ERROR("GRAPHICS", "mapStagingTexture failed for readback of \"{}\"", m_Desc.Name);
        return {};
    }

    // Copy the mapped block out verbatim and hand the row pitch back with it. Rows may be padded, and what lives in
    // the padding is undefined — callers index with outRowPitchBytes (the failure mode this helper exists to
    // prevent is a caller assuming width * bytes-per-texel and corrupting every row past the first).
    const size_t totalBytes = rowPitch * m_Desc.Height;

    std::vector<UInt8> pixels(totalBytes);
    std::memcpy(pixels.data(), mapped, totalBytes);

    rhi->unmapStagingTexture(staging);

    outRowPitchBytes = static_cast<UInt32>(rowPitch);
    return pixels;
}

bool Texture::Update(GraphicsDevice& device, const void* pixels, size_t byteCount, UInt32 rowPitchBytes)
{
    if (!IsValid())
    {
        HP_LOG_ERROR("GRAPHICS", "Texture::Update on an invalid texture");
        return false;
    }
    if (pixels == nullptr)
    {
        HP_LOG_ERROR("GRAPHICS", "Texture::Update called with null pixels for \"{}\"", m_Desc.Name);
        return false;
    }

    nvrhi::IDevice* rhi = device.GetNativeDevice();
    if (rhi == nullptr)
    {
        return false;
    }

    // Only the base mip level: partial-mip and subresource uploads belong with the asset streamer, where they can be
    // batched. Accepting a mip index here would invite callers to assume it regenerates the chain, which nothing does.
    const UInt32 bytesPerPixel = Detail::BytesPerPixel(m_Desc.Format);
    if (bytesPerPixel == 0u)
    {
        HP_LOG_ERROR("GRAPHICS", "Texture::Update: format of \"{}\" has no known texel size", m_Desc.Name);
        return false;
    }

    const UInt32 pitch = rowPitchBytes != 0u ? rowPitchBytes : static_cast<UInt32>(m_Desc.Width) * bytesPerPixel;
    if (pitch < static_cast<UInt32>(m_Desc.Width) * bytesPerPixel)
    {
        // A pitch smaller than one row means the copy would read overlapping rows and produce a smeared texture
        // rather than failing. Reject it: this is the kind of off-by-one that survives to look like a broken importer.
        HP_LOG_ERROR("GRAPHICS", "Texture::Update: rowPitchBytes {} is narrower than one row ({} bytes) for \"{}\"",
                     pitch, static_cast<UInt32>(m_Desc.Width) * bytesPerPixel, m_Desc.Name);
        return false;
    }

    const size_t requiredBytes = static_cast<size_t>(pitch) * m_Desc.Height;
    if (byteCount < requiredBytes)
    {
        HP_LOG_ERROR("GRAPHICS", "Texture::Update: {} bytes supplied, {} needed for \"{}\" ({}x{}, pitch {})",
                     byteCount, requiredBytes, m_Desc.Name, m_Desc.Width, m_Desc.Height, pitch);
        return false;
    }

    // Write side of the staging pair. Created from this texture's own descriptor so the CPU layout NVRHI chooses is by
    // construction the one the copy expects — hand-building a desc is how format mismatches get baked in silently.
    const nvrhi::TextureDesc gpuDesc = m_Impl->Handle->getDesc();

    nvrhi::StagingTextureHandle staging = rhi->createStagingTexture(gpuDesc, nvrhi::CpuAccessMode::Write);
    if (!static_cast<bool>(staging))
    {
        HP_LOG_ERROR("GRAPHICS", "createStagingTexture failed for update of \"{}\"", m_Desc.Name);
        return false;
    }

    size_t stagingRowPitch = 0;
    void* mapped =
        rhi->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Write, &stagingRowPitch);
    if (mapped == nullptr)
    {
        HP_LOG_ERROR("GRAPHICS", "mapStagingTexture failed for update of \"{}\"", m_Desc.Name);
        return false;
    }

    // Row by row: the caller's pitch and NVRHI's staging pitch are independent, and assuming either matches the
    // texture width is the bug this loop exists to prevent. Staging may be padded, and its padding is undefined — so
    // only the meaningful bytes of each row are written, exactly as ReadBack only returns them.
    auto* destination = static_cast<UInt8*>(mapped);
    const auto* source = static_cast<const UInt8*>(pixels);
    const size_t rowBytes = static_cast<size_t>(m_Desc.Width) * bytesPerPixel;
    for (UInt32 row = 0; row < m_Desc.Height; ++row)
    {
        std::memcpy(destination + stagingRowPitch * row, source + static_cast<size_t>(pitch) * row, rowBytes);
    }

    rhi->unmapStagingTexture(staging);

    nvrhi::CommandListHandle commandList = rhi->createCommandList();
    if (!static_cast<bool>(commandList))
    {
        HP_LOG_ERROR("GRAPHICS", "createCommandList failed for update of \"{}\"", m_Desc.Name);
        return false;
    }

    commandList->open();
    // The destination needs an explicit state: NVRHI does not insert barriers implicitly and an undefined layout is
    // silent corruption on a host without validation. (Only the destination — IStagingTexture is an IResource, not an
    // ITexture, so it cannot be transitioned at all; NVRHI manages staging internally, which is also why ReadBack
    // transitions just its GPU side.) The texture is then returned to its creation state, since every draw path here
    // relies on keepInitialState and would otherwise find a texture parked in a transfer layout.
    commandList->setTextureState(m_Impl->Handle, nvrhi::AllSubresources, nvrhi::ResourceStates::CopyDest);
    commandList->copyTexture(m_Impl->Handle, nvrhi::TextureSlice(), staging, nvrhi::TextureSlice());
    commandList->setTextureState(m_Impl->Handle, nvrhi::AllSubresources,
                                 gpuDesc.keepInitialState ? gpuDesc.initialState : nvrhi::ResourceStates::Common);
    commandList->close();

    nvrhi::ICommandList* rawCommandList = commandList;
    rhi->executeCommandLists(&rawCommandList, 1);

    if (!rhi->waitForIdle())
    {
        HP_LOG_ERROR("GRAPHICS", "waitForIdle failed during update of \"{}\" (device lost?)", m_Desc.Name);
        return false;
    }

    return true;
}

}  // namespace Hollowpoint::Graphics
