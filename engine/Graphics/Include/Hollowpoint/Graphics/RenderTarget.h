#pragma once

#include <string>

#include "Hollowpoint/Core/RefCounted.h"
#include "Hollowpoint/Graphics/GraphicsApi.h"
#include "Hollowpoint/Graphics/Texture.h"  // RenderTargetDesc holds Ref<Texture>; its dtor needs the complete type

// Forward declarations only; see Buffer.h for the rationale (a pointer needs a name, not a header).
namespace nvrhi
{
class IFramebuffer;
struct FramebufferInfo;
}  // namespace nvrhi

// ---------------------------------------------------------------------------
// A framebuffer: one colour attachment and an optional depth attachment.
//
// Deliberately narrow: every engine pass renders to a single colour target (forward pass, blits; shadow maps are
// pure-depth). Multi-render-target stays out until a pass actually needs it — supporting MRT "because Vulkan can"
// makes every pipeline's attachment list load-bearing before anything uses it.
//
// The wrapper exists because NVRHI splits framebuffer creation in two: the *info* (attachment formats, required at
// pipeline creation) and the *object* (the attachments themselves). A pipeline must be created against exactly the
// info its framebuffer will provide; constructing the two by hand in separate places is how they drift into a
// validation error that this machine cannot report. This class builds both from the same textures, so the pairing
// is structural rather than remembered.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Graphics
{

class GraphicsDevice;
class RenderTargetImpl;
class Texture;

//! Creation parameters: the attachments. Colour is required; depth is optional. The created object holds Refs to
//! both, so they outlive it even if callers drop their own references.
struct RenderTargetDesc
{
    std::string Name;

    //! Colour attachment; must have been created with IsRenderTarget = true. A Ref rather than a raw pointer on
    //! purpose: Core::Ref's pointer constructor *adopts* (no AddRef), so handing borrowed pointers to it would
    //! double-free. Holding Refs here means the desc itself keeps attachments alive — callers pass what they hold,
    //! and the RenderTarget copies them from the desc.
    Core::Ref<Texture> Colour;

    //! Optional D32Float depth attachment, also a render-target texture. Empty disables depth for this target.
    Core::Ref<Texture> Depth;
};

//! A renderable framebuffer. See file header for why info + object are created together.
class HP_GRAPHICS_API RenderTarget : public Core::RefCounted
{
public:
    [[nodiscard]] static Core::Ref<RenderTarget> Create(GraphicsDevice& device, const RenderTargetDesc& desc);

    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;

    [[nodiscard]] UInt32 GetWidth() const noexcept;
    [[nodiscard]] UInt32 GetHeight() const noexcept;

    [[nodiscard]] bool HasDepth() const noexcept;

    //! The NVRHI framebuffer for GraphicsState. Never null on an instance that exists.
    [[nodiscard]] nvrhi::IFramebuffer* GetNativeFramebuffer() const noexcept;

protected:
    // Pipeline needs the attachment info this target was built with to create a matching pipeline state — and only
    // Pipeline does. Exposing it publicly would let callers assemble pipelines against hand-made infos, exactly the
    // drift this class exists to make impossible (see file header), so it stays protected behind one friend.
    [[nodiscard]] const nvrhi::FramebufferInfo& GetFramebufferInfo() const noexcept;

    friend class Pipeline;

    ~RenderTarget() override;

private:
    RenderTarget() = default;  // Create() only.

    // Holds the nvrhi::FramebufferHandle, the FramebufferInfo, and Refs to the attached textures (keeping them
    // alive through the framebuffer's lifetime). Defined in Src/RenderTarget.cpp; pimpl rationale as in Buffer.h.
    RenderTargetImpl* m_Impl = nullptr;  //!< Owned. Null when invalid.
};

}  // namespace Hollowpoint::Graphics
