#pragma once

#include <string>
#include <vector>

#include "Hollowpoint/Core/RefCounted.h"
#include "Hollowpoint/Graphics/GraphicsApi.h"
#include "Hollowpoint/Graphics/GraphicsTypes.h"

// Forward declaration only; see Buffer.h for the rationale (pointer needs a name, not a header).
namespace nvrhi
{
class ITexture;
}

// ---------------------------------------------------------------------------
// A GPU texture: 2D, single mip chain unless MipLevels says otherwise.
//
// Same thin-wrapper philosophy as Buffer: creation-time correctness is the value added, call-forwarding is not.
// The one operation this class does own — ReadBack — exists because doing it correctly by hand takes four steps in
// exactly the right order (create staging from the same desc; transition to CopySource; copy; map/unmap), skipping
// any of which produces garbage or an unreported validation error on a machine without a validator. Encoding them
// once here means call sites cannot get the sequence wrong, and every numeric shading test shares one readback path
// rather than re-deriving it from comments in TestHdrRenderTarget.cpp.
//
// NVRHI's staging layout follows the texture's format rather than converting float formats to 32-bit: an
// RGBA16_FLOAT target maps as 4 × uint16 per texel (measured in TestHdrRenderTarget, which indexes the mapping as
// uint16 with a stride of four halfs). ReadBack therefore returns bytes verbatim; callers decode per format. The
// row pitch can exceed width * texel size when NVRHI pads rows — use the returned outRowPitchBytes or every row
// past the first is corrupt.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Graphics
{

class GraphicsDevice;
class TextureImpl;

//! Creation parameters for Texture::Create. Only 2D textures are exposed today; cube and 2D-array come with IBL and
//! instancing, and adding them means extending the flag mapping in Src/Texture.cpp deliberately, not by accident.
struct TextureDesc
{
    std::string Name;

    UInt32 Width = 1;
    UInt32 Height = 1;
    UInt32 MipLevels = 1;

    GraphicsFormat Format = GraphicsFormat::Rgba16Float;

    //! Render-target usage (colour or depth — the format decides which). Drives NVRHI's framebuffer and barrier
    //! handling on every backend, so it is a flag here rather than inferred from the format.
    bool IsRenderTarget = true;

    //! Sampled from shaders. Scene targets are usually both rendered to and sampled (blits, post chains); pure
    //! data textures may set this without IsRenderTarget.
    bool IsShaderResource = false;

    //! Engine-level textures are always device-local: NVRHI's TextureDesc has no CPU access field — CPU-visible
    //! staging is a separate object type (createStagingTexture), which ReadBack creates internally from the GPU
    //! texture's own descriptor. There is deliberately no Access knob here to get wrong.
};

//! A GPU texture. See file header for the readback contract.
class HP_GRAPHICS_API Texture : public Core::RefCounted
{
public:
    [[nodiscard]] static Core::Ref<Texture> Create(GraphicsDevice& device, const TextureDesc& desc);

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    [[nodiscard]] const TextureDesc& GetDesc() const noexcept
    {
        return m_Desc;
    }

    [[nodiscard]] bool IsValid() const noexcept;

    //! The underlying NVRHI texture for recording (clears, copies, SRV bindings). Never null on an instance that
    //! exists — Create returns nullptr when creation failed.
    [[nodiscard]] nvrhi::ITexture* GetNativeTexture() const noexcept;

    //! Copies the whole texture to CPU memory and returns it as converted bytes: NVRHI's documented staging layout,
    //! which for float formats is 4 bytes per channel regardless of GPU texel size. Synchronous (it waits for the
    //! copy); correct for tests and tools, never a frame-loop path.
    //!
    //! Returns an empty vector with the reason logged on failure. `outRowPitchBytes` receives the row stride of the
    //! returned block — it can exceed Width * bytes-per-channel when NVRHI pads rows, and ignoring it corrupts
    //! every row past the first (measured behaviour, see TestHdrRenderTarget).
    [[nodiscard]] std::vector<UInt8> ReadBack(GraphicsDevice& device, UInt32& outRowPitchBytes);

protected:
    ~Texture() override;

private:
    Texture() = default;

    TextureDesc m_Desc;

    // Holds the nvrhi::TextureHandle. Defined in Src/Texture.cpp; pimpl rationale as in Buffer.h. Owned.
    TextureImpl* m_Impl = nullptr;
};

}  // namespace Hollowpoint::Graphics
