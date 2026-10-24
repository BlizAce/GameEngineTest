#pragma once

#include <string>
#include <vector>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/RefCounted.h"
#include "Hollowpoint/Graphics/GraphicsApi.h"
#include "Hollowpoint/Graphics/GraphicsTypes.h"  // GraphicsFormat (the surface format is the compositor's, not ours)

// ---------------------------------------------------------------------------
// The swapchain: the frames this device can present to a window.
//
// Why this is hand-written Vulkan rather than an RHI call: NVRHI does not implement presentation at all. Grepping
// its tree for "swapchain" returns nothing, and its Tutorial.md says so outright — "NVRHI does not create the swap
// chain" — pointing at Donut's DeviceManager_VK as the reference implementation. So this class is the engine's own
// raw-Vulkan presentation path, kept behind an engine type so that nothing above Graphics has to know that.
//
// What it owns:
//   * the VkSwapchainKHR itself (created against the surface GraphicsDevice::AttachWindow already owns — see below),
//   * one wrapped back buffer per swapchain image, each a Texture + RenderTarget pair usable by Pipeline/RenderTarget
//     code exactly like an offscreen target,
//   * one depth attachment shared by those targets, recreated with the extent,
//   * per-image synchronisation semaphores (see AcquireResult/Present for why they are indexed by image).
//
// What it deliberately does NOT own: the VkSurfaceKHR. GraphicsDevice::AttachWindow creates and destroys it, and two
// owners of one surface is a double-destroy waiting to happen. A Swapchain therefore requires an attached window,
// and dies before the device rather than freeing the surface itself.
//
// Synchronisation model (and its one visible cost): NVRHI submits command lists without being able to wait on or
// signal foreign VkSemaphores, so the dependencies are expressed through queue ordering instead — an empty submit
// that waits imageAvailable is enqueued before NVRHI's draws, and an empty submit signalling renderFinished after
// them. Submissions to one Vulkan queue complete in submission order, so the NVRHI work cannot start before the
// acquire completed and present cannot wait on work that has not been queued.
//
// Those two submits are fenced and waited on, because both reusing the transition command buffer and re-signalling the
// shared imageAvailable semaphore require the previous one to have finished (Vulkan's valid-usage rules; without it
// the driver's state corrupts and vkDeviceWaitIdle hangs intermittently — reproduced, see notes/EXPERIMENTS.md). The
// wait therefore bounds how far the CPU may run ahead of the GPU to roughly one frame. Under VSync/FIFO that is free,
// since the next acquire paces the loop anyway; with PresentMode::Immediate it is a real throttle. Timeline
// semaphores would remove both submits and the wait — an optimisation, not a correctness requirement.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Graphics
{

class GraphicsDevice;
class RenderTarget;
class Texture;

//! Presentation mode preference for Swapchain::Desc. Maps to Vulkan present modes at creation; the engine's vocabulary
//! deliberately does not name VK_PRESENT_MODE_* so callers do not need Vulkan headers to ask for vsync.
enum class PresentMode : UInt8
{
    //! Wait for vertical blank, tearing-free, one frame of latency. Always supported by every driver — this is the
    //! fallback when nothing else is available.
    VSync,

    //! Triple-buffered: render ahead without blocking, always showing the newest complete frame. Falls back to VSync
    //! when unsupported (some Wayland compositors expose only FIFO).
    Mailbox,

    //! Show immediately, tearing possible. Development/diagnostic use; never a shipping default.
    Immediate,
};

//! Result of Acquire(), which is the one place out-of-date conditions surface during normal rendering.
enum class AcquireStatus : UInt8
{
    Success,    //!< An image was acquired and may be rendered to.
    Timeout,    //!< No image within the timeout (compositor busy, or window minimised). Skip this frame.
    OutOfDate,  //!< Extent/format no longer matches the window: resize happened. Rebuild the swapchain, do NOT present.
    Unsupported,  //!< No presentation path (device created without VK_KHR_swapchain, or no window attached).
};

//! One presentation frame's worth of swapchain state: which image to render into and what to wait on.
struct AcquiredImage
{
    AcquireStatus Status = AcquireStatus::Unsupported;

    //! Index into the swapchain's back buffers, valid only when Status == Success.
    UInt32 ImageIndex = 0;
};

struct SwapchainDesc
{
    //! Preferred pixel size when the platform leaves extent undefined — which is the normal case on Wayland, where the
    //! compositor does not dictate a size and reports 0xFFFFFFFF instead. Ignored on X11/Win32, where the window's
    //! current size wins because a mismatch there is VK_ERROR_SURFACE_LOST territory.
    UInt32 Width = 1280;
    UInt32 Height = 720;

    PresentMode Mode = PresentMode::VSync;

    //! Debug name for the swapchain and its images; shows in RenderDoc and validation messages.
    std::string Name = "swapchain";
};

//! A window's presentation target: owns a VkSwapchainKHR and exposes its images as render targets.
class HP_GRAPHICS_API Swapchain : public Core::RefCounted
{
public:
    //! Creates a swapchain for the device's attached window. Returns nullptr on failure with the reason logged;
    //! expected failures are "device cannot present" (no VK_KHR_swapchain), "no window attached", and a surface the
    //! compositor has not finished sizing yet.
    [[nodiscard]] static Core::Ref<Swapchain> Create(GraphicsDevice& device, const SwapchainDesc& desc);

    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    //! Number of back buffers the swapchain actually has (the driver may hand out more than requested). At least 2.
    [[nodiscard]] UInt32 GetBackBufferCount() const noexcept;

    //! The render target for image `index`, already paired with this swapchain's shared depth attachment. Format is
    //! whatever the compositor chose (BK8G8R8A8_SRGB on most Linux compositors), NOT necessarily Rgba16Float — which is
    //! why an HDR forward pass renders to its own target and this receives a tonemapped blit rather than radiance.
    [[nodiscard]] RenderTarget* GetRenderTarget(UInt32 index) const noexcept;

    [[nodiscard]] UInt32 GetWidth() const noexcept;
    [[nodiscard]] UInt32 GetHeight() const noexcept;

    //! The surface format chosen, for logging and for tests that assert the blit target matches what is presented.
    [[nodiscard]] GraphicsFormat GetColourFormat() const noexcept;

    //! The same choice as the raw `VkFormat` value, widened to an integer so this header stays free of Vulkan includes.
    //! Exposed because the engine's mapping from driver format to GraphicsFormat is a judgement call made against a
    //! list the compositor picks, and verifying it needs the input side of that decision — without it, a test can only
    //! compare the mapping against itself. Returns 0 when there is no swapchain.
    [[nodiscard]] UInt32 GetNativeVkFormat() const noexcept;

    //! The mode actually in effect, which may be VSync even when Mailbox was asked for (see Create).
    [[nodiscard]] PresentMode GetPresentMode() const noexcept;

    //! Takes ownership of the next swapchain image. Call once per frame before recording draws into GetRenderTarget.
    //! A non-Success result means skip the frame (Timeout) or rebuild (OutOfDate); ImageIndex is meaningless then.
    [[nodiscard]] AcquiredImage Acquire();

    //! Submits the frame rendered into `image` and releases it back to the compositor. Must pair one-to-one with a
    //! successful Acquire of the same index; presenting an un-acquired or already-presented image is undefined in
    //! Vulkan (and on this driver, a device-lost rather than a clean error).
    //!
    //! Returns false when the presentation became out of date (a resize landed mid-frame): caller rebuilds.
    bool Present(UInt32 imageIndex);

    //! True when the swapchain's extent no longer matches what the window wants, i.e. Create should be called again.
    //! Provided so a frame loop can check cheaply instead of treating every OutOfDate as an error; it does not itself
    //! rebuild anything.
    [[nodiscard]] bool NeedsRebuild(UInt32 requestedWidth, UInt32 requestedHeight) const noexcept;

    //! Destroys and recreates the swapchain at a new size (e.g. after a window resize), re-wrapping all back buffers.
    //! Blocks until the device is idle: Vulkan requires that no presented image is still in flight when the old
    //! swapchain is destroyed. Returns false with the old swapchain left intact-but-usable if recreation fails.
    bool Resize(UInt32 width, UInt32 height);

private:
    Swapchain();

    struct Impl;
    Impl* m_Impl = nullptr;
};

}  // namespace Hollowpoint::Graphics
