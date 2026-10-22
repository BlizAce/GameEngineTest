#include "RhiConversions.h"  // RenderTargetImpl; also brings RhiAccess.h

#include <utility>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Graphics/GraphicsDevice.h"
#include "Hollowpoint/Graphics/RenderTarget.h"
#include "Hollowpoint/Graphics/Texture.h"

namespace Hollowpoint::Graphics
{

Core::Ref<RenderTarget> RenderTarget::Create(GraphicsDevice& device, const RenderTargetDesc& desc)
{
    HP_ASSERT_ARGUMENT(desc.Colour != nullptr, "RenderTarget::Create requires a colour attachment");

    nvrhi::IDevice* rhi = device.GetNativeDevice();
    if (rhi == nullptr)
    {
        HP_LOG_ERROR("GRAPHICS", "RenderTarget::Create called with a device that has no RHI object");
        return nullptr;
    }

    if (!desc.Colour->IsValid() || (desc.Depth != nullptr && !desc.Depth->IsValid()))
    {
        HP_LOG_ERROR("GRAPHICS", "RenderTarget \"{}\" created with an invalid attachment", desc.Name);
        return nullptr;
    }

    // Depth must match the colour attachment's dimensions: Vulkan requires every attachment in a framebuffer to be
    // the same size, and a mismatch fails deep inside createFramebuffer with an opaque message. Asserted (a wiring
    // mistake, not user data) and then refused — an assertion is a report, never a guard, so the safe action follows
    // it (see AGENTS.md §4; ASan caught exactly this pattern in Mat4::GetRow).
    if (desc.Depth != nullptr && (desc.Depth->GetDesc().Width != desc.Colour->GetDesc().Width ||
                                  desc.Depth->GetDesc().Height != desc.Colour->GetDesc().Height))
    {
        HP_ASSERT_ARGUMENT(false, "RenderTarget depth and colour attachments must have identical dimensions");
        HP_LOG_ERROR("GRAPHICS",
                     "RenderTarget \"{}\": depth attachment is {}x{} but colour is {}x{} — attachments must match",
                     desc.Name, desc.Depth->GetDesc().Width, desc.Depth->GetDesc().Height, desc.Colour->GetDesc().Width,
                     desc.Colour->GetDesc().Height);
        return nullptr;
    }

    nvrhi::FramebufferDesc framebufferDesc;
    framebufferDesc.addColorAttachment(desc.Colour->GetNativeTexture());
    if (desc.Depth != nullptr)
    {
        framebufferDesc.setDepthAttachment(desc.Depth->GetNativeTexture());
    }

    auto* impl = new RenderTargetImpl();

    // Both halves from one desc: the object and the info can never disagree, which is the mismatch this class was
    // introduced to make structurally impossible.
    impl->Info = nvrhi::FramebufferInfo(framebufferDesc);
    impl->Framebuffer = rhi->createFramebuffer(framebufferDesc);

    // Ref copies of the attachments: a caller dropping its own references cannot strand the framebuffer pointing at
    // freed GPU memory.
    impl->ColourAttachment = desc.Colour;
    if (desc.Depth != nullptr)
    {
        impl->DepthAttachment = desc.Depth;
    }

    if (!static_cast<bool>(impl->Framebuffer))
    {
        HP_LOG_ERROR("GRAPHICS", "createFramebuffer failed for \"{}\"", desc.Name);
        delete impl;
        return nullptr;
    }

    Core::Ref<RenderTarget> target = Core::Ref<RenderTarget>(new RenderTarget());
    target->m_Impl = impl;
    return target;
}

RenderTarget::~RenderTarget()
{
    delete m_Impl;
    m_Impl = nullptr;
}

UInt32 RenderTarget::GetWidth() const noexcept
{
    return m_Impl != nullptr && m_Impl->ColourAttachment ? m_Impl->ColourAttachment->GetDesc().Width : 0u;
}

UInt32 RenderTarget::GetHeight() const noexcept
{
    return m_Impl != nullptr && m_Impl->ColourAttachment ? m_Impl->ColourAttachment->GetDesc().Height : 0u;
}

bool RenderTarget::HasDepth() const noexcept
{
    return m_Impl != nullptr && static_cast<bool>(m_Impl->DepthAttachment);
}

nvrhi::IFramebuffer* RenderTarget::GetNativeFramebuffer() const noexcept
{
    return m_Impl != nullptr ? m_Impl->Framebuffer : nullptr;
}

const nvrhi::FramebufferInfo& RenderTarget::GetFramebufferInfo() const noexcept
{
    // The instance exists only if Create succeeded, which guarantees a non-null impl; the fallback is a static
    // default so a misuse reads as "no attachments" rather than a null dereference.
    static const nvrhi::FramebufferInfo s_Empty{};
    return m_Impl != nullptr ? m_Impl->Info : s_Empty;
}

}  // namespace Hollowpoint::Graphics
