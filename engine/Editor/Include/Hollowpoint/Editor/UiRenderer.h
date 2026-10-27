#pragma once

#include <string>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/RefCounted.h"
#include "Hollowpoint/Core/Types.h"

// ---------------------------------------------------------------------------
// UiRenderer — draws an ImGui frame into a render target on the engine's own device.
//
// Why this is not just `ImGui_ImplVulkan_RenderDrawData(...)`: the stock Vulkan backend wants to own its command
// buffer, its render pass and its image layout transitions, and it assumes the target is a swapchain image it
// created. The editor's target is neither — it is the engine's swapchain back buffer, already acquired by
// Graphics::Swapchain, already inside an NVRHI command list whose resource states NVRHI has been tracking all
// frame. Handing ImGui a raw VkCommandBuffer and letting it place its own barriers would fight that tracking: two
// authorities over one image's layout is exactly the class of bug where validation passes on one driver and the
// compositor shows garbage on another (the same lesson as the swapchain's acquire/present transitions).
//
// So this file takes the narrow path that works with NVRHI instead of around it:
//   * ImGui builds draw data; we upload its font atlas through NVRHI (Texture::Update), not vkCmdCopyBufferToImage;
//   * geometry goes into an NVRHI vertex/index buffer, written with writeBuffer;
//   * the draw happens inside a Graphics::Pipeline whose shader is a minimal textured-triangle program, created
//     against the engine's own RenderTarget so the format pairing rule Pipeline enforces cannot be violated;
//   * clipping becomes one scissor rect per command (ImGui splits draw callbacks on clip rects already).
//
// That costs ~250 lines and buys: no second command stream, no layout fights, no render-pass assumption, and it
// works identically into the back buffer or an offscreen target (which is what makes a headless UI test possible —
// see tests/Editor).
//
// Coordinate space: ImGui's framebuffer coordinates (pixels from the top-left of the target), which is what
// io.DisplaySize describes. The vertex shader maps them to Vulkan's Y-down NDC without a flip; getting that wrong
// renders the whole UI mirrored vertically, and on a plain-grey background that is hard to notice until a text
// label gives it away.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Graphics
{
class GraphicsDevice;
class RenderTarget;
class Texture;
}  // namespace Hollowpoint::Graphics

struct ImGuiContext;

namespace Hollowpoint::Editor
{

using Core::Ref;
using Core::UInt32;
using Core::UInt64;

//! Creates UiRenderers. One per render target format is enough (the editor has one: the back buffer), and one is
//! created lazily on first use by EditorUi.
class HP_EDITOR_API UiRenderer : public Core::RefCounted
{
public:
    //! Builds pipelines and the dynamic buffers for drawing into targets like `target` (its attachment formats are
    //! what the pipeline is created against). Returns nullptr with the reason logged when shaders fail to compile or
    //! the device refuses a buffer.
    [[nodiscard]] static Ref<UiRenderer> Create(Graphics::GraphicsDevice& device);

    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;

    //! Uploads `target`'s font atlas / user textures if ImGui marked them dirty, then submits its draw list into
    //! `target`. Call after ImGui::Render() and while nothing else is writing the target.
    //!
    //! Returns false when there was no draw data to submit *and* something went wrong (an empty frame with a hidden
    //! window is normal and returns true). On failure the target keeps whatever it had — the UI vanishes, the scene
    //! does not, which is the degradation an editor wants: losing the overlay should never take the viewport with it.
    bool Render(Graphics::RenderTarget& target);

    //! Draw calls submitted since creation. A liveness counter: "the UI drew nothing" and "the UI drew but is off
    //! screen" are different bugs, and this distinguishes them without a debugger.
    [[nodiscard]] UInt64 GetDrawCallCount() const noexcept;

    //! Makes an engine texture usable by widgets (`ImGui::Image`), returning the id to pass.
    /**
     * The editor's viewport is exactly this: the scene renders into an offscreen HDR target at the panel's size and
     * aspect, and the panel samples it — which is the only way a docked layout can show a viewport whose dimensions are
     * decided by the dock node rather than the window. Ids are engine-issued (never 0, so they cannot collide with
     * ImTextureID_Invalid) and stay valid until UnregisterTexture or a pipeline rebuild, after which widgets must ask
     * again — a rebuild means the binding layout changed and every id's descriptor set was destroyed with it.
     *
     * The texture is transitioned to shader-resource state before each UI draw, so a caller that just rendered into it
     * (the viewport case) does not have to know which layout NVRHI left it in.
     */
    [[nodiscard]] Core::UInt64 RegisterTexture(const Core::Ref<Graphics::Texture>& texture);

    //! Drops a registration. Safe on an id this renderer never issued.
    void UnregisterTexture(Core::UInt64 textureId);

protected:
    ~UiRenderer() override;

private:
    UiRenderer();

    struct Impl;
    Impl* m_Impl = nullptr;  //!< Owned. Null only if construction failed.
};

}  // namespace Hollowpoint::Editor
