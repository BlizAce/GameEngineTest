#pragma once

#include <string>

#include "Hollowpoint/Core/RefCounted.h"
#include "Hollowpoint/Graphics/GraphicsApi.h"
#include "Hollowpoint/Graphics/GraphicsTypes.h"

// Forward declaration only — the GraphicsDevice::GetNativeDevice precedent: a pointer needs the class *name*, not
// its header. Code that records commands includes Hollowpoint/Graphics/RhiAccess.h and pays for Vulkan there.
namespace nvrhi
{
class IBuffer;
}

// ---------------------------------------------------------------------------
// A GPU buffer: vertices, indices, uniform blocks, staging data.
//
// Thin by design — NVRHI already *is* the portable abstraction over Vulkan/D3D12, and re-wrapping its every call
// would add indirection without adding safety. What this wrapper owns is exactly the parts where being wrong is
// silent, each learned the expensive way (notes/EXPERIMENTS.md):
//
//   * usage/access combinations a backend rejects or misbehaves on — asserted at creation;
//   * explicit initial resource states — NVRHI tracks state per command list and an untracked use is a validation
//     error, and this machine has no validation layer, so nothing would ever report it;
//   * a debug name attached at creation: an unnamed GPU resource is invisible in RenderDoc.
//
// Command recording deliberately stays NVRHI's (writeBuffer, setBufferState, bindings): the renderer uses the
// documented escape hatch and includes RhiAccess.h. Wrapping record methods here would re-expose the same
// vocabulary through ~50 pass-throughs for zero safety gain.
//
// Lifetime: reference counted; the owning GraphicsDevice must outlive every buffer (Vulkan requires the VkDevice
// alive while its resources exist). Releasing the last Ref defers the actual free to NVRHI's per-frame garbage
// collection, which a frame loop must drive.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Graphics
{

class BufferImpl;
class GraphicsDevice;  // Forward declaration only; see GraphicsDevice.h.

//! Creation parameters for Buffer::Create. Values only; nothing is retained by reference.
struct BufferDesc
{
    std::string Name;  //!< RenderDoc/PIX name and NVRHI error-message prefix. Empty works but hinders debugging.

    UInt64 ByteSize = 0;  //!< Must be > 0 — a zero-size buffer fails creation with a logged reason.

    //! How the buffer is used; drives RHI flags and legal operations. See BufferUsage.
    BufferUsage Usage = BufferUsage::Vertex;

    //! CPU visibility. Read or Write require Usage | Staging; device-local (the default) is never CPU-visible.
    CpuAccess Access = CpuAccess::None;
};

//! A GPU buffer. Not safe for concurrent recording from multiple threads without external synchronisation:
//! NVRHI's state tracking is per command list, matching Vulkan's rules.
class HP_GRAPHICS_API Buffer : public Core::RefCounted
{
public:
    //! Creates a buffer on `device`. Returns nullptr on failure with the reason logged. Programmer errors
    //! (zero size, CPU access without the staging flag) assert before any API call.
    [[nodiscard]] static Core::Ref<Buffer> Create(GraphicsDevice& device, const BufferDesc& desc);

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    [[nodiscard]] const BufferDesc& GetDesc() const noexcept
    {
        return m_Desc;
    }

    [[nodiscard]] bool IsValid() const noexcept;

    //! The underlying NVRHI buffer for recording (writeBuffer, state transitions, bindings). Owned by this object
    //! for its lifetime; null only when creation failed — and Create returns nullptr then, so callers holding a
    //! Buffer never see null.
    [[nodiscard]] nvrhi::IBuffer* GetNativeBuffer() const noexcept;

protected:
    // Virtual via RefCounted, so destruction runs through the derived type and releases the NVRHI handle while the
    // owning device is still alive — the frame loop drops resources before shutting the device down.
    ~Buffer() override;

private:
    Buffer() = default;  // Create() only.

    BufferDesc m_Desc;

    // Holds the nvrhi::BufferHandle. Defined in Src/Buffer.cpp so this header stays free of NVRHI and Vulkan — the
    // same pimpl choice GraphicsDevice makes, for the same reason. Null when invalid; owned by this object.
    BufferImpl* m_Impl = nullptr;
};

}  // namespace Hollowpoint::Graphics
