#include <utility>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Graphics/Buffer.h"

// BufferImpl (the pimpl) plus the NVRHI conversion switches; also brings RhiAccess.h. Module-internal header, see
// its own comment on why conversions live in one place.
#include "RhiConversions.h"

namespace Hollowpoint::Graphics
{

namespace
{

//! Maps engine buffer usage/access onto NVRHI's flags plus the initial resource state.
//!
//! The initial state is not a detail: NVRHI tracks resource state per command list, and this machine has no Vulkan
//! validation layer to report a use from an untracked state — the failure is silent corruption, not an error. Every
//! buffer therefore starts in a state its usage makes legal and keeps it (keepInitialState), so command lists begin
//! tracking from a known point.
[[nodiscard]] nvrhi::BufferDesc MakeNvrhiDesc(const BufferDesc& desc)
{
    nvrhi::BufferDesc setup;
    setup.debugName = desc.Name;
    setup.byteSize = desc.ByteSize;
    setup.isVertexBuffer = HasFlag(desc.Usage, BufferUsage::Vertex);
    setup.isIndexBuffer = HasFlag(desc.Usage, BufferUsage::Index);
    setup.isConstantBuffer = HasFlag(desc.Usage, BufferUsage::Constant);
    setup.canHaveTypedViews = HasFlag(desc.Usage, BufferUsage::ShaderResource);
    setup.cpuAccess = desc.Access == CpuAccess::Read ? nvrhi::CpuAccessMode::Read
                                                     : (desc.Access == CpuAccess::Write ? nvrhi::CpuAccessMode::Write
                                                                                        : nvrhi::CpuAccessMode::None);

    // Device-local buffers start Common (a legal resting state for GPU-only resources); CPU-visible ones start in
    // their access-specific state so the first write/read is tracked from there. There is no GenericRead in NVRHI's
    // state list — a read-staging buffer is written by copies, so CopyDest is its natural starting state.
    setup.initialState =
        desc.Access == CpuAccess::Read ? nvrhi::ResourceStates::CopyDest : nvrhi::ResourceStates::Common;
    setup.keepInitialState = true;

    return setup;
}

}  // namespace

Core::Ref<Buffer> Buffer::Create(GraphicsDevice& device, const BufferDesc& desc)
{
    // Zero-size buffers are a programmer error, not a runtime condition: nothing sensible can be uploaded to or
    // drawn from one, and Vulkan's behaviour with size 0 is implementation-defined.
    HP_ASSERT_ARGUMENT(desc.ByteSize > 0, "Buffer::Create requires ByteSize > 0");

    // CPU access requires the staging flag: without it the buffer is device-local regardless of what cpuAccess says,
    // and every later map or readback fails far from this call site. Rejecting the combination here keeps the error
    // where it can be explained. (Stated as reasoning, not history — this path was never observed failing in anger.)
    const bool wantsCpuAccess = desc.Access != CpuAccess::None;
    HP_ASSERT_ARGUMENT(!wantsCpuAccess || HasFlag(desc.Usage, BufferUsage::Staging),
                       "Buffer::Create: CPU access requires BufferUsage::Staging");

    nvrhi::IDevice* rhi = device.GetNativeDevice();
    if (rhi == nullptr)
    {
        HP_LOG_ERROR("GRAPHICS", "Buffer::Create called with a device that has no RHI object");
        return nullptr;
    }

    const nvrhi::BufferDesc nvrhiDesc = MakeNvrhiDesc(desc);

    nvrhi::BufferHandle handle = rhi->createBuffer(nvrhiDesc);
    if (!static_cast<bool>(handle))
    {
        // A plausible runtime failure (out of VRAM, an unsupported flag combination on some driver): logged, and
        // the caller gets nullptr rather than an assert — a missing vertex buffer should degrade, not abort.
        HP_LOG_ERROR("GRAPHICS", "createBuffer failed for \"{}\" ({} bytes, usage {})", desc.Name, desc.ByteSize,
                     static_cast<unsigned>(desc.Usage));
        return nullptr;
    }

    // Ref's explicit pointer constructor rather than CreateObject: the `new` inside that template is access-checked
    // at its definition context (RefCounted.h), which cannot see a private ctor. Constructing here — inside the
    // class's own static method — keeps construction funelled through Create while keeping the ctor private.
    Core::Ref<Buffer> buffer = Core::Ref<Buffer>(new Buffer());
    buffer->m_Desc = desc;
    buffer->m_Impl = new BufferImpl{std::move(handle)};
    return buffer;
}

Buffer::~Buffer()
{
    // Releasing the NVRHI handle here (rather than in GraphicsDevice's shutdown) is correct because the frame loop
    // drops resources before tearing the device down; Vulkan requires the VkDevice to outlive its buffers, and a
    // buffer outliving its device is an assert-worthy programming error above us, not something to paper over.
    delete m_Impl;
    m_Impl = nullptr;
}

bool Buffer::IsValid() const noexcept
{
    return m_Impl != nullptr && static_cast<bool>(m_Impl->Handle);
}

nvrhi::IBuffer* Buffer::GetNativeBuffer() const noexcept
{
    return IsValid() ? m_Impl->Handle : nullptr;
}

}  // namespace Hollowpoint::Graphics
