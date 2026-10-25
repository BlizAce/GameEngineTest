#include "RhiConversions.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Graphics/GraphicsDevice.h"
#include "Hollowpoint/Graphics/RenderTarget.h"
#include "Hollowpoint/Graphics/RhiAccess.h"
#include "Hollowpoint/Graphics/Swapchain.h"
#include "Hollowpoint/Graphics/Texture.h"

namespace Hollowpoint::Graphics
{

namespace
{

[[nodiscard]] const char* VkResultName(VkResult result) noexcept
{
    switch (result)
    {
        case VK_SUCCESS:
            return "VK_SUCCESS";
        case VK_TIMEOUT:
            return "VK_TIMEOUT";
        case VK_NOT_READY:
            return "VK_NOT_READY";
        case VK_SUBOPTIMAL_KHR:
            return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR:
            return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_SURFACE_LOST_KHR:
            return "VK_ERROR_SURFACE_LOST_KHR";
        default:
            return "VkResult error";
    }
}

[[nodiscard]] GraphicsFormat ToEngineFormat(VkFormat format) noexcept
{
    switch (format)
    {
        case VK_FORMAT_B8G8R8A8_UNORM:
            return GraphicsFormat::Bgra8Unorm;
        case VK_FORMAT_B8G8R8A8_SRGB:
            return GraphicsFormat::Rgba8Srgb;
        case VK_FORMAT_R8G8B8A8_UNORM:
            return GraphicsFormat::Rgba8Unorm;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return GraphicsFormat::Rgba16Float;
        default:
            return GraphicsFormat::Unknown;
    }
}

[[nodiscard]] VkPresentModeKHR ToVkPresentMode(PresentMode mode) noexcept
{
    switch (mode)
    {
        case PresentMode::VSync:
            return VK_PRESENT_MODE_FIFO_KHR;
        case PresentMode::Mailbox:
            return VK_PRESENT_MODE_MAILBOX_KHR;
        case PresentMode::Immediate:
            return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

//! Prefer 8-bit UNORM BGRA with the sRGB-nonlinear colourspace (the common Linux compositor surface), then plain
//! BGRA, then whatever is offered first. An SRGB *format* is avoided deliberately: the engine's blit already writes
//! display-encoded values, so an sRGB-decoded image would apply gamma twice and darken the whole frame.
[[nodiscard]] bool ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats, VkSurfaceFormatKHR& out) noexcept
{
    if (formats.empty())
    {
        return false;
    }

    for (const VkSurfaceFormatKHR& candidate : formats)
    {
        if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            out = candidate;
            return true;
        }
    }
    for (const VkSurfaceFormatKHR& candidate : formats)
    {
        if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM)
        {
            out = candidate;
            return true;
        }
    }

    out = formats.front();
    return true;
}

}  // namespace

PresentMode ResolvePresentMode(PresentMode requested, const PresentModeSupport& support) noexcept
{
    // VSync needs no check: VK_PRESENT_MODE_FIFO_KHR is required of every driver by the Vulkan spec, so it is both the
    // fallback and always reachable. Anything unrecognised in `requested` lands there too rather than being passed on.
    switch (requested)
    {
        case PresentMode::Mailbox:
            return support.Mailbox ? PresentMode::Mailbox : PresentMode::VSync;
        case PresentMode::Immediate:
            return support.Immediate ? PresentMode::Immediate : PresentMode::VSync;
        case PresentMode::VSync:
            break;
    }
    return PresentMode::VSync;
}


struct Swapchain::Impl
{
    GraphicsDevice* Device = nullptr;
    VkDevice VkDevices = VK_NULL_HANDLE;
    VkQueue Queue = VK_NULL_HANDLE;
    VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
    VkSurfaceKHR Surface = VK_NULL_HANDLE;  //!< owned by GraphicsDevice — never destroyed here
    uint32_t QueueFamilyIndex = 0;

    VkSwapchainKHR Handle = VK_NULL_HANDLE;
    GraphicsFormat EngineFormat = GraphicsFormat::Unknown;
    VkFormat NativeFormat = VK_FORMAT_UNDEFINED;
    PresentMode ActualMode = PresentMode::VSync;
    UInt32 Width = 0;
    UInt32 Height = 0;

    //! Raw swapchain images, in presentation order. Owned by the VkSwapchainKHR.
    std::vector<VkImage> Images;
    //! NVRHI wrappers (borrowed images: managed == false, so they never destroy the VkImage).
    std::vector<nvrhi::TextureHandle> WrappedImages;
    std::vector<Core::Ref<Texture>> Textures;
    std::vector<Core::Ref<RenderTarget>> Targets;
    Core::Ref<Texture> Depth;

    std::vector<VkSemaphore> RenderFinished;
    VkSemaphore ImageAvailable = VK_NULL_HANDLE;

    //! Per-image "acquired and not yet presented". Double-presenting is undefined in Vulkan and a device-lost on this
    //! driver rather than an error return, so it is refused here.
    std::vector<bool> Acquired;

    //! Command buffer and fence for layout transitions, reused for the swapchain's lifetime.
    /**
     * The fence is not optional; two valid-usage rules demand it:
     *
     *  * vkResetCommandBuffer/vkBeginCommandBuffer on a command buffer that is still executing is undefined
     *    behaviour — the rule requires the previous execution to have completed. This buffer is submitted twice per
     *    frame (acquire wait, present transition) and was previously re-recorded with nothing enforcing that. The
     *    symptom was not an error return but corrupted driver state: vkDeviceWaitIdle hanging inside
     *    libnvidia-glcore roughly one run in eight, reproduced and backtraced (see notes/EXPERIMENTS.md).
     *  * vkAcquireNextImageKHR requires its semaphore to have no uncompleted signal or wait pending. The empty submit
     *    issued after an acquire is what consumes that signal, so the following acquire must not start until that
     *    submit has finished — otherwise a still-busy binary semaphore gets signalled again. That is why
     *    WaitForTransition() runs at the top of Acquire(), before vkAcquireNextImageKHR rather than only before this
     *    buffer is reused.
     *
     * A ring of buffers+fences was the other candidate and is not correct on its own: it would satisfy the first rule
     * while leaving a previous acquire's wait pending against the shared ImageAvailable semaphore.
     *
     * Cost, stated plainly: this queue completes submissions in order, so waiting here stops the CPU running more
     * than one frame ahead of the GPU. Under VSync/FIFO — the only mode guaranteed everywhere — pacing is imposed at
     * the next acquire regardless, so the practical loss is nil; with PresentMode::Immediate it is a real throttle.
     * Timeline semaphores would remove the wait entirely and are the intended optimisation, not a correctness need.
     */
    VkCommandPool Pool = VK_NULL_HANDLE;
    VkCommandBuffer BarrierBuffer = VK_NULL_HANDLE;
    VkFence BarrierFence = VK_NULL_HANDLE;

    //! True between a SubmitBarrier and the WaitForTransition that reaps it. WaitForTransition must not block on a
    //! fence no submit will signal — both Acquire() and SubmitBarrier wait, so without this flag the second wait of a
    //! frame would sit out the full timeout against an already-reaped fence.
    bool TransitionPending = false;

    //! How long a transition may take before it is treated as lost. Healthy transitions finish in microseconds, so
    //! this only ever fires on a wedged driver — where its value is turning a hang into an error return. Same
    //! reasoning as Acquire()'s finite timeout.
    static constexpr UInt64 kTransitionWaitNanoseconds = 5000000000ull;

    ~Impl()
    {
        Destroy();
    }

    void Destroy()
    {
        if (VkDevices == VK_NULL_HANDLE)
        {
            return;
        }

        // Nothing may be in flight when a swapchain dies: vkDestroySwapchainKHR with submissions outstanding is
        // undefined, and the semaphores freed below are ones a pending submit still references — destroying them under
        // the driver hangs it intermittently rather than erroring. Resize() already waits for this same rule; the
        // destruction path needs it too, because "the caller remembered to idle first" is not something this class can
        // assume (and getting it wrong produces a hang at shutdown with no error to read).
        vkDeviceWaitIdle(VkDevices);

        // Order matters: every wrapper holding a swapchain VkImage must die before vkDestroySwapchainKHR frees them.
        Targets.clear();
        Textures.clear();
        Depth = nullptr;
        WrappedImages.clear();
        Images.clear();

        if (Handle != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(VkDevices, Handle, nullptr);
            Handle = VK_NULL_HANDLE;
        }

        TransitionPending = false;  // vkDeviceWaitIdle above already guaranteed completion

        if (BarrierFence != VK_NULL_HANDLE)
        {
            vkDestroyFence(VkDevices, BarrierFence, nullptr);
            BarrierFence = VK_NULL_HANDLE;
        }

        if (Pool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(VkDevices, Pool, nullptr);
            Pool = VK_NULL_HANDLE;
            BarrierBuffer = VK_NULL_HANDLE;  // freed with the pool
        }

        for (VkSemaphore& semaphore : RenderFinished)
        {
            if (semaphore != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(VkDevices, semaphore, nullptr);
            }
        }
        RenderFinished.clear();

        if (ImageAvailable != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(VkDevices, ImageAvailable, nullptr);
            ImageAvailable = VK_NULL_HANDLE;
        }

        Acquired.clear();
    }

    //! Creates the transition command buffer and its fence once. Called on first use so Build() stays readable and a
    //! swapchain that never presents (created then discarded) allocates nothing.
    [[nodiscard]] bool EnsureCommandResources()
    {
        if (Pool != VK_NULL_HANDLE && BarrierFence != VK_NULL_HANDLE)
        {
            return true;
        }

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        // Command buffers may only be submitted to a queue whose family matches the pool's, so this must be the
        // graphics family rather than a fresh one.
        poolInfo.queueFamilyIndex = QueueFamilyIndex;

        if (vkCreateCommandPool(VkDevices, &poolInfo, nullptr, &Pool) != VK_SUCCESS)
        {
            return false;
        }

        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = Pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;

        if (vkAllocateCommandBuffers(VkDevices, &allocate, &BarrierBuffer) != VK_SUCCESS)
        {
            return false;
        }

        // Unsignalled at creation, which is what vkQueueSubmit requires of its fence (VUID-vkQueueSubmit-fence-00063).
        // This used to be VK_FENCE_CREATE_SIGNALED_BIT, intended to spare a "has this ever been submitted" flag — but
        // TransitionPending below is exactly that flag and already guards the first wait, so the signalled start only
        // made the first submit of every run illegal. Invisible for months because no validation layer was installed on
        // this host (see LayerSupported), and drivers accepted it.
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

        return vkCreateFence(VkDevices, &fenceInfo, nullptr, &BarrierFence) == VK_SUCCESS;
    }

    //! Blocks until the last submitted transition has completed. Returns false if the driver does not answer within
    //! kTransitionWaitNanoseconds — reported, not absorbed, because every later call depends on that work having
    //! finished and a silent continue would corrupt state while hiding the cause.
    [[nodiscard]] bool WaitForTransition(const char* why)
    {
        if (!TransitionPending)
        {
            return true;  // nothing outstanding: covers the first call and every repeat after a reap
        }

        const VkResult waited = vkWaitForFences(VkDevices, 1, &BarrierFence, VK_TRUE, kTransitionWaitNanoseconds);
        if (waited != VK_SUCCESS)
        {
            HP_LOG_ERROR("GRAPHICS", "swapchain: waiting for the {} transition timed out ({})", why,
                         VkResultName(waited));
            return false;
        }

        vkResetFences(VkDevices, 1, &BarrierFence);
        TransitionPending = false;
        return true;
    }

    //! Records `barriers` into the reusable command buffer and submits it, optionally waiting on / signalling the
    //! given semaphores. Returns false (with the reason logged) if any step fails.
    bool SubmitBarrier(const std::vector<VkImageMemoryBarrier>& barriers, VkPipelineStageFlags srcStage,
                       VkPipelineStageFlags dstStage, VkSemaphore waitSemaphore, VkPipelineStageFlags waitStage,
                       VkSemaphore signalSemaphore, const char* what)
    {
        if (!EnsureCommandResources())
        {
            HP_LOG_ERROR("GRAPHICS", "swapchain: could not create the transition command resources");
            return false;
        }

        // Rule 1 of the fence's doc comment: this buffer may not be re-recorded while its previous execution is
        // running. Signalled-at-creation plus a wait here means that holds on every call including the first.
        if (!WaitForTransition("previous"))
        {
            return false;
        }

        vkResetCommandBuffer(BarrierBuffer, 0);

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(BarrierBuffer, &begin) != VK_SUCCESS)
        {
            HP_LOG_ERROR("GRAPHICS", "swapchain: could not begin the {} command buffer", what);
            return false;
        }

        vkCmdPipelineBarrier(BarrierBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr,
                             static_cast<uint32_t>(barriers.size()), barriers.data());

        if (vkEndCommandBuffer(BarrierBuffer) != VK_SUCCESS)
        {
            HP_LOG_ERROR("GRAPHICS", "swapchain: could not record the {} command buffer", what);
            return false;
        }

        const VkPipelineStageFlags waitStages[] = {waitStage};
        const VkSemaphore waits[] = {waitSemaphore};
        const VkSemaphore signals[] = {signalSemaphore};

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &BarrierBuffer;
        if (waitSemaphore != VK_NULL_HANDLE)
        {
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = waits;
            submit.pWaitDstStageMask = waitStages;
        }
        if (signalSemaphore != VK_NULL_HANDLE)
        {
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = signals;
        }

        // The fence is signalled by this submit and waited on by the next one (and by the next Acquire), which is what
        // makes "the previous transition has finished" a fact rather than an assumption.
        const VkResult result = vkQueueSubmit(Queue, 1, &submit, BarrierFence);
        if (result != VK_SUCCESS)
        {
            HP_LOG_ERROR("GRAPHICS", "swapchain: {} submit failed: {}", what, VkResultName(result));
            return false;
        }

        TransitionPending = true;
        return true;
    }

    //! Move every back buffer from UNDEFINED to colour-attachment-optimal, once. A fresh swapchain's images are in
    //! VK_IMAGE_LAYOUT_UNDEFINED; declaring that as the source layout is Vulkan's way of saying "contents are
    //! discarded", which is correct (we overwrite them) and cheaper than a load. Doing it up front keeps the caller's
    //! command lists free of swapchain bookkeeping.
    bool TransitionAllToRenderable()
    {
        std::vector<VkImageMemoryBarrier> barriers;
        barriers.reserve(Images.size());

        for (const VkImage image : Images)
        {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = 0;  // must be zero when oldLayout is UNDEFINED
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;
            barriers.push_back(barrier);
        }

        return SubmitBarrier(barriers, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_NULL_HANDLE, 0, VK_NULL_HANDLE, "initial layout");
    }

    bool Build(const SwapchainDesc& desc, UInt32 preferredWidth, UInt32 preferredHeight)
    {
        nvrhi::IDevice* rhi = Device->GetNativeDevice();

        VkSurfaceCapabilitiesKHR caps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(PhysicalDevice, Surface, &caps) != VK_SUCCESS)
        {
            HP_LOG_ERROR("GRAPHICS", "{}: cannot query surface capabilities", desc.Name);
            return false;
        }

        uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(PhysicalDevice, Surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(PhysicalDevice, Surface, &formatCount, formats.data());

        VkSurfaceFormatKHR chosen{};
        if (!ChooseSurfaceFormat(formats, chosen))
        {
            HP_LOG_ERROR("GRAPHICS", "{}: surface offers no pixel formats", desc.Name);
            return false;
        }

        EngineFormat = ToEngineFormat(chosen.format);
        NativeFormat = chosen.format;
        if (EngineFormat == GraphicsFormat::Unknown)
        {
            HP_LOG_ERROR("GRAPHICS", "{}: chosen surface format {} is not in the engine's format list", desc.Name,
                         static_cast<unsigned>(chosen.format));
            return false;
        }

        uint32_t presentCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(PhysicalDevice, Surface, &presentCount, nullptr);
        std::vector<VkPresentModeKHR> modes(presentCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(PhysicalDevice, Surface, &presentCount, modes.data());

        // FIFO is guaranteed by spec; a preference only applies if offered. Wayland compositors frequently expose FIFO
        // alone, so mailbox silently becoming VSync is normal — logged because it changes latency, not because it is
        // an error. The decision itself lives in ResolvePresentMode so it can be tested on a host that offers every
        // mode, where creation alone cannot show a fallback happening.
        PresentModeSupport support;
        support.Mailbox = std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end();
        support.Immediate = std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end();

        ActualMode = ResolvePresentMode(desc.Mode, support);
        if (ActualMode != desc.Mode)
        {
            HP_LOG_INFO("GRAPHICS", "{}: requested present mode unavailable, using VSync", desc.Name);
        }

        // What goes to vkCreateSwapchainKHR is the RESOLVED mode, never the requested one: Vulkan requires presentMode
        // to be among those the surface reports. Passing `wanted` here (as an earlier revision did) made the fallback
        // above dead code — computed, logged, and ignored — so a request the compositor does not support would fail
        // creation instead of degrading. Found by reading rather than by failure: every run to date asked for VSync,
        // which is always offered, making requested and resolved equal in practice.
        const VkPresentModeKHR resolved = ToVkPresentMode(ActualMode);

        // X11/Win32 report the window's size and it must be used; Wayland reports 0xFFFFFFFF ("undefined") and the
        // application chooses — measured on this host, so the fallback is not speculation. Clamping to
        // min/maxImageExtent is required either way: an out-of-range extent fails the create rather than validating.
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == 0xFFFFFFFFu || extent.height == 0xFFFFFFFFu)
        {
            extent.width = std::clamp(preferredWidth, caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(preferredHeight, caps.minImageExtent.height, caps.maxImageExtent.height);
        }

        if (extent.width == 0 || extent.height == 0)
        {
            // A minimised or unmapped window reports zero legitimately. Silent: a frame loop that rebuilds per frame
            // would turn this into an error flood, and there is nothing for the caller to fix until the window returns.
            return false;
        }

        UInt32 imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        {
            imageCount = caps.maxImageCount;
        }

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = Surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = chosen.format;
        createInfo.imageColorSpace = chosen.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        // TRANSFER_SRC is not needed to present; it is here because reading a presented frame back is the only way to
        // verify the presentation path end-to-end (a test can render a known colour into a back buffer and compare the
        // pixels), and because screenshot support — editor, bug reports, automated visual regression — needs the same
        // capability. On tile-based GPUs an extra usage bit can cost memory tiling flexibility; on the desktop drivers
        // this engine targets it is free, and "we could not take a screenshot of what we presented" is the alternative.
        //
        // TRANSFER_DST for the same reason: the tone-map blit clears the back buffer before drawing (an undefined
        // destination makes "drew nothing" indistinguishable from "drew black"), and vkCmdClearColorImage needs the bit
        // — validation rejected the first real frame with `missing VK_IMAGE_USAGE_TRANSFER_DST_BIT` on exactly this
        // image. A swapchain that omitted it would work until the clear, then fail mid-frame at the point where the
        // layout barrier to TRANSFER_DST_OPTIMAL is illegal too.
        createInfo.imageUsage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        // The compositor's transform rather than IDENTITY: on a rotated display, asking for IDENTITY and rendering
        // unrotated produces a sideways image — passing it through lets the compositor compensate.
        createInfo.preTransform = caps.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = resolved;
        createInfo.clipped = VK_TRUE;

        VkSwapchainKHR handle = VK_NULL_HANDLE;
        const VkResult result = vkCreateSwapchainKHR(VkDevices, &createInfo, nullptr, &handle);
        if (result != VK_SUCCESS)
        {
            HP_LOG_ERROR("GRAPHICS", "{}: vkCreateSwapchainKHR failed: {}", desc.Name, VkResultName(result));
            return false;
        }

        uint32_t imageCountOut = 0;
        vkGetSwapchainImagesKHR(VkDevices, handle, &imageCountOut, nullptr);
        if (imageCountOut == 0)
        {
            vkDestroySwapchainKHR(VkDevices, handle, nullptr);
            HP_LOG_ERROR("GRAPHICS", "{}: swapchain reports zero images", desc.Name);
            return false;
        }

        Images.resize(imageCountOut);
        vkGetSwapchainImagesKHR(VkDevices, handle, &imageCountOut, Images.data());

        Handle = handle;
        Width = extent.width;
        Height = extent.height;

        // --- wrap each image for NVRHI and for the engine ------------------------------------------------
        nvrhi::TextureDesc textureDesc;
        textureDesc.setWidth(Width);
        textureDesc.setHeight(Height);
        textureDesc.setFormat(Detail::ToNvrhi(EngineFormat));
        textureDesc.setArraySize(1);
        textureDesc.setIsRenderTarget(true);
        textureDesc.debugName = desc.Name + " image";
        // These wrappers must declare a tracked initial state, and it must be RenderTarget (colour-attachment-optimal).
        //
        // Leaving initialState at Unknown made every draw into a back buffer log "Unknown prior state of texture ...
        // swapchain image" — one error per frame from NVRHI's barrier tracker, which is the RHI telling the truth: with
        // keepInitialState false and no beginTrackingTextureState call, it has no idea what layout the image is in and
        // cannot place a correct transition. Silent on this host (no validation layer); a correctness problem
        // everywhere else.
        //
        // RenderTarget is not a guess, it is an invariant this file maintains: TransitionAllToRenderable puts every
        // image there once, Present moves it to PRESENT_SRC_KHR, and Acquire brings it back before handing it out — so
        // at the moment a caller records into an acquired image, colour-attachment-optimal is exactly where it is.
        // Declaring that lets NVRHI seed its tracker (no error), skip a transition it would otherwise have to guess at,
        // and restore the same state at command-list close, which is again a no-op. Our own foreign submits keep the
        // invariant; NVRHI never sees the present-side transition and never needs to.
        textureDesc.initialState = nvrhi::ResourceStates::RenderTarget;
        textureDesc.keepInitialState = true;

        WrappedImages.clear();
        Textures.clear();
        Targets.clear();

        for (uint32_t index = 0; index < imageCountOut; ++index)
        {
            // Borrows the VkImage (NVRHI sets managed=false), so releasing these handles never frees a swapchain
            // image — but they must still be released before the swapchain itself goes away.
            nvrhi::TextureHandle wrapped = rhi->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image,
                                                                             nvrhi::Object(Images[index]), textureDesc);
            if (!static_cast<bool>(wrapped))
            {
                HP_LOG_ERROR("GRAPHICS", "{}: could not wrap swapchain image {}", desc.Name, index);
                return false;
            }
            WrappedImages.push_back(std::move(wrapped));

            TextureDesc engineDesc;
            engineDesc.Name = desc.Name + " image " + std::to_string(index);
            engineDesc.Width = Width;
            engineDesc.Height = Height;
            engineDesc.Format = EngineFormat;
            engineDesc.IsRenderTarget = true;

            Core::Ref<Texture> texture = Texture::CreateWrapping(engineDesc, WrappedImages.back());
            if (texture == nullptr)
            {
                return false;
            }
            Textures.push_back(std::move(texture));
        }

        // One depth attachment shared by every back buffer: only one frame renders at a time on this single queue, so
        // N per-image depths would be N textures doing the work of one.
        TextureDesc depthDesc;
        depthDesc.Name = desc.Name + " depth";
        depthDesc.Width = Width;
        depthDesc.Height = Height;
        depthDesc.Format = GraphicsFormat::D32Float;
        depthDesc.IsRenderTarget = true;

        Depth = Texture::Create(*Device, depthDesc);
        if (Depth == nullptr)
        {
            HP_LOG_ERROR("GRAPHICS", "{}: could not create the swapchain depth attachment", desc.Name);
            return false;
        }

        for (const Core::Ref<Texture>& texture : Textures)
        {
            RenderTargetDesc targetDesc;
            targetDesc.Name = desc.Name + " target";
            targetDesc.Colour = texture;
            targetDesc.Depth = Depth;

            Core::Ref<RenderTarget> target = RenderTarget::Create(*Device, targetDesc);
            if (target == nullptr)
            {
                HP_LOG_ERROR("GRAPHICS", "{}: could not build a framebuffer for a swapchain image", desc.Name);
                return false;
            }
            Targets.push_back(std::move(target));
        }

        // --- synchronisation ---------------------------------------------------------------------------
        RenderFinished.assign(Images.size(), VK_NULL_HANDLE);
        for (VkSemaphore& semaphore : RenderFinished)
        {
            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (vkCreateSemaphore(VkDevices, &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS)
            {
                HP_LOG_ERROR("GRAPHICS", "{}: could not create a presentation semaphore", desc.Name);
                return false;
            }
        }

        if (ImageAvailable == VK_NULL_HANDLE)
        {
            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (vkCreateSemaphore(VkDevices, &semaphoreInfo, nullptr, &ImageAvailable) != VK_SUCCESS)
            {
                HP_LOG_ERROR("GRAPHICS", "{}: could not create the acquire semaphore", desc.Name);
                return false;
            }
        }

        Acquired.assign(Images.size(), false);

        if (!TransitionAllToRenderable())
        {
            HP_LOG_ERROR("GRAPHICS", "{}: initial layout transition failed", desc.Name);
            return false;
        }

        HP_LOG_INFO("GRAPHICS", "{} created: {}x{}, {} images, format {}, mode {}", desc.Name, Width, Height,
                    Images.size(), static_cast<unsigned>(EngineFormat), static_cast<unsigned>(ActualMode));
        return true;
    }
};

Core::Ref<Swapchain> Swapchain::Create(GraphicsDevice& device, const SwapchainDesc& desc)
{
    if (!device.CanPresent())
    {
        HP_LOG_ERROR("GRAPHICS", "Swapchain::Create: this device has no VK_KHR_swapchain and cannot present");
        return nullptr;
    }

    auto* impl = new Impl();
    impl->Device = &device;
    impl->VkDevices = static_cast<VkDevice>(device.GetNativeVkDevice());
    impl->Queue = static_cast<VkQueue>(device.GetNativeVkQueue());
    impl->Surface = static_cast<VkSurfaceKHR>(device.GetNativeVkSurface());

    if (impl->VkDevices == VK_NULL_HANDLE || impl->Queue == VK_NULL_HANDLE)
    {
        delete impl;
        HP_LOG_ERROR("GRAPHICS", "Swapchain::Create: device exposes no native handles");
        return nullptr;
    }

    if (impl->Surface == VK_NULL_HANDLE)
    {
        delete impl;
        HP_LOG_ERROR("GRAPHICS", "Swapchain::Create: no window attached — call GraphicsDevice::AttachWindow first");
        return nullptr;
    }

    nvrhi::IDevice* rhi = device.GetNativeDevice();
    impl->PhysicalDevice =
        reinterpret_cast<VkPhysicalDevice>(rhi->getNativeObject(nvrhi::ObjectTypes::VK_PhysicalDevice).pointer);

    // Which family the engine's single queue belongs to: command pools must be created for that family. Asked of the
    // device, which chose it while enumerating queue properties. The previous code probed every family with
    // vkGetDeviceQueue and compared handles — undefined behaviour, since only one family was requested at VkDevice
    // creation (validation reports VUID-vkGetDeviceQueue-queueFamilyIndex-00384 for each of the others).
    impl->QueueFamilyIndex = device.GetVkQueueFamilyIndex();

    Core::Ref<Swapchain> swapchain = Core::Ref<Swapchain>(new Swapchain());
    swapchain->m_Impl = impl;

    if (!impl->Build(desc, desc.Width, desc.Height))
    {
        delete impl;
        swapchain->m_Impl = nullptr;
        return nullptr;
    }

    return swapchain;
}

Swapchain::Swapchain() = default;

Swapchain::~Swapchain()
{
    if (m_Impl != nullptr)
    {
        // Vulkan requires no presented image still be in flight when the swapchain is destroyed.
        if (m_Impl->VkDevices != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(m_Impl->VkDevices);
        }
        delete m_Impl;
        m_Impl = nullptr;
    }
}

UInt32 Swapchain::GetBackBufferCount() const noexcept
{
    return m_Impl != nullptr ? static_cast<UInt32>(m_Impl->Images.size()) : 0u;
}

RenderTarget* Swapchain::GetRenderTarget(UInt32 index) const noexcept
{
    if (m_Impl == nullptr || index >= m_Impl->Targets.size())
    {
        return nullptr;
    }
    return m_Impl->Targets[index].Get();
}

Texture* Swapchain::GetTexture(UInt32 index) const noexcept
{
    if (m_Impl == nullptr || index >= m_Impl->Textures.size())
    {
        return nullptr;
    }
    return m_Impl->Textures[index].Get();
}

UInt32 Swapchain::GetWidth() const noexcept
{
    return m_Impl != nullptr ? m_Impl->Width : 0u;
}

UInt32 Swapchain::GetHeight() const noexcept
{
    return m_Impl != nullptr ? m_Impl->Height : 0u;
}

PresentMode Swapchain::GetPresentMode() const noexcept
{
    return m_Impl != nullptr ? m_Impl->ActualMode : PresentMode::VSync;
}

UInt32 Swapchain::GetNativeVkFormat() const noexcept
{
    return m_Impl != nullptr ? static_cast<UInt32>(m_Impl->NativeFormat) : 0u;
}

GraphicsFormat Swapchain::GetColourFormat() const noexcept
{
    return m_Impl != nullptr ? m_Impl->EngineFormat : GraphicsFormat::Unknown;
}

AcquiredImage Swapchain::Acquire()
{
    AcquiredImage result;

    if (m_Impl == nullptr || m_Impl->Handle == VK_NULL_HANDLE)
    {
        result.Status = AcquireStatus::Unsupported;
        return result;
    }

    // Rule 2 of the fence's doc comment: vkAcquireNextImageKHR must not be handed a semaphore that still has an
    // uncompleted wait pending, and ImageAvailable is reused every frame. The submit that consumes the previous
    // acquire's signal is therefore waited on here, before this call can signal it again — doing it inside
    // SubmitBarrier would be too late, since that runs after the acquire.
    if (!m_Impl->WaitForTransition("acquire"))
    {
        result.Status = AcquireStatus::Unsupported;
        return result;
    }

    uint32_t index = 0;
    // A finite timeout on purpose: with UINT64_MAX a compositor that cannot hand back an image (window hidden) stalls
    // the entire application. One second turns that into repeated Timeouts the frame loop can skip instead of a hang.
    const VkResult acquired = vkAcquireNextImageKHR(m_Impl->VkDevices, m_Impl->Handle, 1000000000ull,
                                                    m_Impl->ImageAvailable, VK_NULL_HANDLE, &index);

    if (acquired == VK_TIMEOUT)
    {
        result.Status = AcquireStatus::Timeout;
        return result;
    }
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR || acquired == VK_ERROR_SURFACE_LOST_KHR)
    {
        result.Status = AcquireStatus::OutOfDate;
        return result;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
    {
        HP_LOG_ERROR("GRAPHICS", "swapchain acquire failed: {}", VkResultName(acquired));
        result.Status = AcquireStatus::Unsupported;
        return result;
    }

    if (index >= m_Impl->Acquired.size())
    {
        // Out-of-range index from the driver. Asserting and refusing beats indexing Acquired[] out of bounds below,
        // which would corrupt memory rather than fail.
        HP_ASSERT_ARGUMENT(false, "vkAcquireNextImageKHR returned an out-of-range image index");
        result.Status = AcquireStatus::Unsupported;
        return result;
    }

    if (m_Impl->Acquired[index])
    {
        // Two acquires of the same image means the previous one was never presented — a bug in the caller's loop, and
        // one that deadlocks the swapchain eventually, so it is reported rather than absorbed.
        HP_LOG_ERROR("GRAPHICS", "swapchain image {} acquired twice without presenting", index);
    }

    m_Impl->Acquired[index] = true;

    // Wait for the acquire AND bring this image back to a renderable layout, in one submit.
    //
    // The layout half is not optional and was missing until the first real frame loop ran: Present() leaves an image in
    // PRESENT_SRC_KHR, and nothing put it back — so the second time an image came round, rendering wrote into an
    // attachment whose layout the spec does not permit a colour write in. Undefined behaviour: on this driver it
    // rendered (which is exactly why a test that acquires once and presents once passes), and validation layers would
    // call it.
    //
    // oldLayout = UNDEFINED rather than PRESENT_SRC_KHR, deliberately. It is Vulkan's "discard the contents, I will
    // overwrite them" idiom, and it is legal precisely because acquire already guarantees the compositor finished
    // reading this image. Declaring it lets a driver skip the resolve/copy it would otherwise perform to preserve
    // pixels nobody reads — naming PRESENT_SRC_KHR would be the more descriptive spelling and strictly slower on
    // tile-based GPUs.
    //
    // The consequence, stated because it is not obvious and will matter later: back-buffer contents are NOT preserved
    // across an acquire. Nothing may read last frame's presented image through a back buffer (a temporal effect —
    // motion blur, TAA, a debug "compare to previous" — must sample its own history texture, which it should be writing
    // anyway). Every current consumer clears before drawing, so nothing depends on contents today; this is the
    // constraint that would have to change if one did.
    //
    // Expressed as a barrier inside the acquire's own submit rather than a second one: queue order already serialises
    // it against the caller's next submission, so this costs no extra sync.
    VkImageMemoryBarrier layoutBarrier{};
    layoutBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    layoutBarrier.srcAccessMask = 0;  // must be zero when oldLayout is UNDEFINED
    layoutBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    layoutBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    layoutBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    layoutBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    layoutBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    layoutBarrier.image = m_Impl->Images[index];
    layoutBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    layoutBarrier.subresourceRange.levelCount = 1;
    layoutBarrier.subresourceRange.layerCount = 1;

    // Make subsequent work wait for the acquire. NVRHI cannot wait on a foreign semaphore, so the dependency is
    // expressed by queue ordering: this submit is enqueued before whatever the caller submits next, and one Vulkan
    // queue completes submissions in order. See the header for the full model.
    // Destination stage is COLOR_ATTACHMENT_OUTPUT, not TOP_OF_PIPE. The barrier's dstAccessMask asks for
    // colour-write access, and validation rejects an access flag the destination stage cannot produce
    // (VUID-vkCmdPipelineBarrier-pImageMemoryBarriers-02820) — TOP_OF_PIPE "waits on nothing", which here would also be
    // wrong: the point is that the colour pass does not start writing until the acquire has finished. The wait stage on
    // the ImageAvailable semaphore is the same stage for the same reason.
    if (!m_Impl->SubmitBarrier({layoutBarrier}, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, m_Impl->ImageAvailable,
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_NULL_HANDLE, "acquire wait"))
    {
        m_Impl->Acquired[index] = false;
        result.Status = AcquireStatus::OutOfDate;
        return result;
    }

    result.Status = AcquireStatus::Success;
    result.ImageIndex = index;
    return result;
}

bool Swapchain::Present(UInt32 imageIndex)
{
    if (m_Impl == nullptr || m_Impl->Handle == VK_NULL_HANDLE)
    {
        return false;
    }

    if (imageIndex >= m_Impl->Acquired.size() || !m_Impl->Acquired[imageIndex])
    {
        HP_LOG_ERROR("GRAPHICS", "Present called for image {} which is not acquired", imageIndex);
        return false;
    }

    m_Impl->Acquired[imageIndex] = false;

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    // COLOR_ATTACHMENT_WRITE / COLOR_ATTACHMENT_OUTPUT: the rasteriser's writes must complete before the compositor
    // reads. A weaker source stage "waits on nothing" here and shows partially drawn frames.
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = 0;  // the presentation engine synchronises its own read
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_Impl->Images[imageIndex];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    const VkSemaphore signal = m_Impl->RenderFinished[imageIndex];
    if (!m_Impl->SubmitBarrier({barrier}, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_NULL_HANDLE, 0, signal, "present transition"))
    {
        return false;
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &signal;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &m_Impl->Handle;
    presentInfo.pImageIndices = &imageIndex;

    const VkResult presented = vkQueuePresentKHR(m_Impl->Queue, &presentInfo);
    if (presented == VK_SUCCESS)
    {
        return true;
    }

    if (presented == VK_SUBOPTIMAL_KHR || presented == VK_ERROR_OUT_OF_DATE_KHR ||
        presented == VK_ERROR_SURFACE_LOST_KHR)
    {
        // Expected during a resize; info-level so a loop that keeps presenting does not become an error flood.
        HP_LOG_INFO("GRAPHICS", "presentation became out of date ({})", VkResultName(presented));
        return false;
    }

    HP_LOG_ERROR("GRAPHICS", "vkQueuePresentKHR failed: {}", VkResultName(presented));
    return false;
}

bool Swapchain::NeedsRebuild(UInt32 requestedWidth, UInt32 requestedHeight) const noexcept
{
    if (m_Impl == nullptr || m_Impl->Handle == VK_NULL_HANDLE)
    {
        return true;
    }
    return m_Impl->Width != requestedWidth || m_Impl->Height != requestedHeight;
}

bool Swapchain::Resize(UInt32 width, UInt32 height)
{
    if (m_Impl == nullptr || width == 0 || height == 0)
    {
        return false;
    }

    if (!NeedsRebuild(width, height))
    {
        return true;
    }

    // Vulkan requires nothing in flight before the old swapchain is destroyed. Skipping this is the classic validation
    // error here — and it is a use-after-free of presented images rather than a clean failure.
    vkDeviceWaitIdle(m_Impl->VkDevices);

    const SwapchainDesc desc;  // extent comes from the arguments; modes/formats are re-chosen from the surface
    m_Impl->Destroy();

    if (!m_Impl->Build(desc, width, height))
    {
        HP_LOG_ERROR("GRAPHICS", "swapchain resize to {}x{} failed", width, height);
        return false;
    }
    return true;
}

}  // namespace Hollowpoint::Graphics
