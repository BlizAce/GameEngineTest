// Tests for the swapchain: creation against a real window, back-buffer wrapping, acquire/present pairing, and resize.
//
// These are the engine's first presentation tests, and they run against a genuinely mapped window on whatever display
// server this host has (Wayland or X11). That is deliberate: NVRHI does not implement presentation at all (see
// Swapchain.h), so this code is raw Vulkan and there is nothing to mock — a swapchain either survives real compositor
// interaction or it does not work. On a host with no display every case here WARNs and returns, because "no monitor"
// is a property of the machine, not a defect in the engine.
//
// What can and cannot be verified, stated plainly: nothing can read back a presented image (the compositor owns it once
// presented), so these tests assert API-level correctness — creation with sane parameters, images usable as colour
// attachments, acquire/present pairing across more frames than there are images, resize producing fresh valid targets,
// and the device still working afterwards. Pixel correctness of what gets presented is covered by SceneToPixelTests
// against an offscreen target; this suite's job is that presentation does not corrupt or lose the device.

#include "TestSupport.h"

#include <algorithm>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Core/RefCounted.h"
#include "Hollowpoint/Graphics/GraphicsDevice.h"
#include "Hollowpoint/Graphics/Pipeline.h"
#include "Hollowpoint/Graphics/RenderTarget.h"
#include "Hollowpoint/Graphics/RhiAccess.h"
#include "Hollowpoint/Graphics/ShaderCompiler.h"
#include "Hollowpoint/Graphics/Swapchain.h"
#include "Hollowpoint/Platform/PlatformInit.h"
#include "Hollowpoint/Platform/Window.h"

using namespace Hollowpoint::Graphics;
using Hollowpoint::Core::Ref;
using Hollowpoint::Core::UInt32;
using Hollowpoint::Platform::PlatformInitOptions;
using Hollowpoint::Platform::PlatformScope;
using Hollowpoint::Platform::Window;
using Hollowpoint::Platform::WindowDesc;

namespace
{

constexpr UInt32 kWidth = 320;
constexpr UInt32 kHeight = 240;

//! Process-wide windowing scope on a REAL display backend, or nullptr when the host has none.
//!
//! Unlike the other Platform tests this cannot use the Null backend: a null-backend window creates no VkSurfaceKHR, and
//! a swapchain needs a real surface. So no PreferredBackend is requested — GLFW takes the platform default — and
//! IsHeadless() is how callers learn there is nothing to present to.
[[nodiscard]] PlatformScope* GetDisplayScope()
{
    static PlatformScope* scope = []() -> PlatformScope* {
        static PlatformScope instance = [] {
            PlatformInitOptions options;
            options.AllowHeadlessFallback = true;  // no display -> Null backend -> tests skip rather than fail to start
            return PlatformScope(options);
        }();

        return instance.IsInitialized() ? &instance : nullptr;
    }();

    return scope;
}

[[nodiscard]] GraphicsDevice* GetDeviceForWindow(Window& window)
{
    // The instance extensions MUST come from the window before the device is created, and this is the single most
    // important line in the file. GraphicsDevice creates its own VkInstance, and a surface extension
    // (VK_KHR_wayland_surface / _xlib) has to be enabled on that instance for glfwCreateWindowSurface to work — so
    // AttachWindow silently fails (returns false) on a device built without them. Nothing else reports it: the failure
    // surfaces at swapchain creation as "no window attached", which names the wrong thing entirely.
    //
    // This was found because every display-dependent case in this file initially skipped as vacuously-green; the tell
    // was 5 assertions for 6 test cases. The correct order is: platform scope -> window -> read its required
    // extensions -> create the device, which is also the order an application has to use.
    static GraphicsDevice* device = [&window]() -> GraphicsDevice* {
        GraphicsDeviceDesc desc;
        desc.ApplicationName = "Hollowpoint swapchain tests";
        desc.ExtraInstanceExtensions = window.GetRequiredVulkanInstanceExtensions();
        return GraphicsDevice::Create(desc);
    }();

    return device;
}

//! A device that has never had a window attached, for testing the "no surface" failure path.
/**
 * Deliberately a different object from the fixture's device: presentation needs the instance extensions read from a
 * window (see GetDeviceForWindow), and this case must exercise a device with no surface at all. When the host has a
 * display the extensions are supplied so CanPresent() is true and the test reaches the real "no window attached"
 * branch; when it has none, there is nothing to attach anyway and callers skip.
 */
[[nodiscard]] GraphicsDevice* GetUnattachedDevice()
{
    static GraphicsDevice* device = []() -> GraphicsDevice* {
        GraphicsDeviceDesc desc;
        desc.ApplicationName = "Hollowpoint swapchain tests (unattached)";

        PlatformScope* scope = GetDisplayScope();
        if (scope != nullptr && !scope->IsHeadless())
        {
            // Any window will do to learn the extensions — it is not attached to this device.
            WindowDesc probeDesc;
            probeDesc.Title = "Hollowpoint extension probe";
            probeDesc.Width = 64;
            probeDesc.Height = 64;
            probeDesc.Visible = false;

            if (Window* probe = Window::Create(probeDesc))
            {
                desc.ExtraInstanceExtensions = probe->GetRequiredVulkanInstanceExtensions();
                delete probe;
            }
        }

        return GraphicsDevice::Create(desc);
    }();

    return device;
}

//! A mapped window with its surface attached to the graphics device — everything a swapchain requires. Leaves nothing
//! behind when this host cannot provide one.
class ScopedPresentableWindow
{
public:
    ScopedPresentableWindow()
    {
        PlatformScope* scope = GetDisplayScope();
        if (scope == nullptr)
        {
            Reason = "windowing could not initialise";
            return;
        }
        if (scope->IsHeadless())
        {
            Reason = "no display server on this host";
            return;
        }

        WindowDesc windowDesc;
        windowDesc.Title = "Hollowpoint swapchain test";
        windowDesc.Width = kWidth;
        windowDesc.Height = kHeight;
        // Mapped deliberately. An unmapped window on Wayland never gets a compositor-allocated surface at the requested
        // size, which is exactly the undefined-extent case Build() handles — testing with a visible window keeps this
        // suite about presentation rather than about window-manager timing.
        windowDesc.Visible = true;

        Handle = Window::Create(windowDesc);
        if (Handle == nullptr)
        {
            Reason = "could not create a window";
            return;
        }

        Device = GetDeviceForWindow(*Handle);
        if (Device == nullptr)
        {
            Reason = std::string("no graphics device: ") + GraphicsDevice::GetFailureReason();
            return;
        }

        if (!Device->AttachWindow(Handle))
        {
            Reason = "could not attach the window to the graphics device";
            delete Handle;
            Handle = nullptr;
            return;
        }

        Ready = true;
    }

    ~ScopedPresentableWindow()
    {
        // Detach before the window dies: GraphicsDevice owns the surface created from it, and destroying the GLFW
        // window first would leave that surface dangling.
        if (Device != nullptr)
        {
            Device->AttachWindow(nullptr);
        }
        delete Handle;
    }

    ScopedPresentableWindow(const ScopedPresentableWindow&) = delete;
    ScopedPresentableWindow& operator=(const ScopedPresentableWindow&) = delete;

    [[nodiscard]] bool IsReady() const noexcept
    {
        return Ready;
    }

    [[nodiscard]] Window* GetHandle() const noexcept
    {
        return Handle;
    }

    //! The device the window is attached to. Only valid when IsReady(); tests use this rather than fetching a device
    //! themselves because presentation requires the one created with this window's surface extensions.
    [[nodiscard]] GraphicsDevice& GetDevice() const noexcept
    {
        return *Device;
    }

    std::string Reason;

private:
    Window* Handle = nullptr;
    GraphicsDevice* Device = nullptr;
    bool Ready = false;
};

}  // namespace

#define SKIP_WITHOUT_DISPLAY(context)                                                    \
    do                                                                                   \
    {                                                                                    \
        if (!(context).IsReady())                                                        \
        {                                                                                \
            WARN((std::string("Skipping swapchain test: ") + (context).Reason).c_str()); \
            return;                                                                      \
        }                                                                                \
    }                                                                                    \
    while (false)

namespace
{

//! A trivial pass-through pipeline description, used only to prove a back buffer is a legitimate colour attachment.
/**
 * Building a Pipeline against a RenderTarget is the strongest check available that a wrapped swapchain image is real:
 * Pipeline::Create pairs the pipeline's attachment formats with the framebuffer's and fails when they disagree (that
 * pairing is structural by design — see RenderTarget.h). A wrong format on the wrapped image, or an image NVRHI does
 * not understand, therefore fails here rather than producing a draw into nothing.
 */
[[nodiscard]] Ref<Pipeline> BuildProbePipeline(GraphicsDevice& device, RenderTarget& target)
{
    const std::string vertexSource =
        "#version 450\n"
        "layout(location = 0) in vec2 InPosition;\n"
        "void main() { gl_Position = vec4(InPosition, 0.0, 1.0); }\n";
    const std::string fragmentSource =
        "#version 450\n"
        "layout(location = 0) out vec4 OutColor;\n"
        "void main() { OutColor = vec4(0.25, 0.5, 0.75, 1.0); }\n";

    const CompiledShader vertex = CompileGlsl(ShaderStage::Vertex, vertexSource);
    if (!vertex.Succeeded)
    {
        return nullptr;
    }
    const CompiledShader fragment = CompileGlsl(ShaderStage::Pixel, fragmentSource);
    if (!fragment.Succeeded)
    {
        return nullptr;
    }

    PipelineDesc desc;
    desc.Name = "swapchain probe";
    desc.VertexShader.SpirV = vertex.SpirV;
    desc.PixelShader.SpirV = fragment.SpirV;
    desc.VertexAttributes = {VertexAttribute{"InPosition", GraphicsFormat::Rg32Float, 0, 8}};
    // Culling off: this pipeline exists to validate the attachment, and a full-screen triangle's winding is not what
    // is under test here (front-face winding has its own coverage in SceneToPixelTests).
    desc.TriangleCulling = CullMode::None;

    return Pipeline::Create(device, desc, target);
}

}  // namespace

//! Test-only bridge to GraphicsDevice's private native-handle accessors (see the friend declaration in
//! GraphicsDevice.h, which is compiled in only when HP_BUILD_TESTS is defined). Exists so presentation tests can check
//! the engine's view of a device against what Vulkan reports without making those handles public API.
TEST_SUITE("Graphics/Swapchain")
{
    TEST_CASE("a device without a window reports it cannot create a swapchain")
    {
        GraphicsDevice* device = GetUnattachedDevice();
        REQUIRE(device != nullptr);

        if (!device->CanPresent())
        {
            WARN("this host's device has no VK_KHR_swapchain");
            return;
        }

        // The failure path every caller hits first, and the one that must be clean: creating against a null
        // VkSurfaceKHR is an undefined-handle crash rather than an error return. Needs no display, so it runs anywhere.
        REQUIRE_FALSE(device->HasWindow());

        Ref<Swapchain> swapchain;
        {
            Hollowpoint::Test::ScopedLogCapture logs;
            SwapchainDesc desc;
            swapchain = Swapchain::Create(*device, desc);
            CHECK(logs.AnyMessageContains("no window attached"));
        }
        CHECK(swapchain == nullptr);
    }

    TEST_CASE("a swapchain is created with back buffers usable as colour attachments (display required)")
    {
        ScopedPresentableWindow window;
        SKIP_WITHOUT_DISPLAY(window);

        SwapchainDesc desc;
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.Name = "test-swapchain";

        GraphicsDevice& device = window.GetDevice();
        Ref<Swapchain> swapchain = Swapchain::Create(device, desc);
        REQUIRE_MESSAGE(swapchain != nullptr, "swapchain creation failed");

        // Two images minimum is an API requirement (present one while rendering into another), so failing here means
        // the presentation path is unusable rather than a cosmetic count mismatch.
        const UInt32 backBuffers = swapchain->GetBackBufferCount();
        INFO("back buffers: " << backBuffers);
        CHECK(backBuffers >= 2u);

        CHECK(swapchain->GetWidth() > 0u);
        CHECK(swapchain->GetHeight() > 0u);

        // An unrepresentable surface format is refused at Create, so arriving here with a known format is the
        // assertion: presenting through Unknown would draw with the wrong channel order or double gamma, silently.
        CHECK(swapchain->GetColourFormat() != GraphicsFormat::Unknown);

        // Every back buffer must be a real attachment, not just image 0. A pipeline created against target 0 and drawn
        // into target 2 is ordinary frame-loop behaviour, so each index gets its own pipeline build.
        for (UInt32 index = 0; index < backBuffers; ++index)
        {
            RenderTarget* target = swapchain->GetRenderTarget(index);
            INFO("image " << index);
            REQUIRE(target != nullptr);

            CHECK(target->GetWidth() == swapchain->GetWidth());
            CHECK(target->HasDepth());

            // The load-bearing check: NVRHI accepts this wrapped image as a colour attachment.
            Ref<Pipeline> pipeline = BuildProbePipeline(device, *target);
            CHECK_MESSAGE(pipeline != nullptr, "could not build a pipeline against swapchain image " << index);
        }

        // Out-of-range is a caller bug and must be refused rather than reading past the vector.
        CHECK(swapchain->GetRenderTarget(backBuffers) == nullptr);
    }

    TEST_CASE("the reported colour format is the surface's actual format (display required)")
    {
        ScopedPresentableWindow window;
        SKIP_WITHOUT_DISPLAY(window);

        SwapchainDesc desc;
        desc.Width = kWidth;
        desc.Height = kHeight;

        Ref<Swapchain> swapchain = Swapchain::Create(window.GetDevice(), desc);
        REQUIRE(swapchain != nullptr);

        // A surface for the format query. Created here rather than borrowed because GraphicsDevice owns its own and
        // does not expose it; querying formats on any valid surface for this window returns the same list, which is
        // what makes this independent of the code under test.
        nvrhi::IDevice* deviceForSurface = window.GetDevice().GetNativeDevice();
        const VkInstance instance =
            reinterpret_cast<VkInstance>(deviceForSurface->getNativeObject(nvrhi::ObjectTypes::VK_Instance).pointer);
        REQUIRE(instance != VK_NULL_HANDLE);

        void* rawSurface = nullptr;
        REQUIRE(window.GetHandle()->CreateVulkanSurface(instance, &rawSurface));
        REQUIRE(rawSurface != nullptr);
        const VkSurfaceKHR surface = static_cast<VkSurfaceKHR>(rawSurface);

        // Why this test exists on its own: checking that a pipeline builds against a back buffer proves only that the
        // framebuffer and the pipeline agree, and both are derived from the same wrapped texture — so a wrong format on
        // the wrap is invisible to it (verified: mapping VK_FORMAT_B8G8R8A8_UNORM to Rgba8Unorm passed every other case
        // here). The only non-circular check is against what Vulkan itself says the surface is, mapped through an
        // independent table written from the spec rather than from the implementation under test.
        nvrhi::IDevice* rhi = window.GetDevice().GetNativeDevice();
        REQUIRE(rhi != nullptr);

        const VkPhysicalDevice physical =
            reinterpret_cast<VkPhysicalDevice>(rhi->getNativeObject(nvrhi::ObjectTypes::VK_PhysicalDevice).pointer);

        // Which VkFormat the swapchain says it chose, and which formats this host's surface actually offers. Querying
        // the physical device here is independent of the swapchain code under test: it goes straight to the driver.
        const VkFormat chosen = static_cast<VkFormat>(swapchain->GetNativeVkFormat());
        INFO("chosen VkFormat: " << static_cast<unsigned>(chosen));
        REQUIRE(chosen != VK_FORMAT_UNDEFINED);

        uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data());
        REQUIRE_FALSE(formats.empty());

        // The chosen format must be one the surface offers — checked against VkFormat values directly, so a wrong
        // engine-side mapping cannot make this pass.
        bool chosenIsOffered = false;
        for (const VkSurfaceFormatKHR& format : formats)
        {
            if (format.format == chosen)
            {
                chosenIsOffered = true;
            }
        }
        CHECK(chosenIsOffered);

        // Expected engine format for each offered surface format, written from the enum definitions rather than by
        // calling the engine's converter (which is what is under test).
        const auto expectedFor = [](VkFormat format) {
            switch (format)
            {
                case VK_FORMAT_B8G8R8A8_UNORM:
                    return GraphicsFormat::Bgra8Unorm;
                case VK_FORMAT_B8G8R8A8_SRGB:
                    return GraphicsFormat::Rgba8Srgb;
                case VK_FORMAT_R8G8B8A8_UNORM:
                    return GraphicsFormat::Rgba8Unorm;
                default:
                    return GraphicsFormat::Unknown;
            }
        };

        // The engine prefers BGRA8 UNORM with the sRGB-nonlinear colourspace; assert that is what it chose when the
        // surface offers it, and otherwise that its choice is one of the offered formats.
        const bool surfaceOffersBgra = std::any_of(formats.begin(), formats.end(), [](const VkSurfaceFormatKHR& f) {
            return f.format == VK_FORMAT_B8G8R8A8_UNORM;
        });

        const GraphicsFormat reported = swapchain->GetColourFormat();
        INFO("reported engine format: " << static_cast<unsigned>(reported));

        if (surfaceOffersBgra)
        {
            CHECK(reported == GraphicsFormat::Bgra8Unorm);
        }

        // Whichever it picked must correspond to a format the surface actually offers — the property that would break
        // if the wrap claimed a format the compositor will not deliver.
        bool reportedIsOffered = false;
        for (const VkSurfaceFormatKHR& format : formats)
        {
            if (expectedFor(format.format) == reported)
            {
                reportedIsOffered = true;
            }
        }
        CHECK(reportedIsOffered);

        // And the back-buffer texture must report the same size and format as the swapchain itself: a wrapped image
        // with a mismatched descriptor is what a readback or blit would silently mis-decode.
        RenderTarget* target = swapchain->GetRenderTarget(0);
        REQUIRE(target != nullptr);
        CHECK(target->GetWidth() == swapchain->GetWidth());
        CHECK(target->GetHeight() == swapchain->GetHeight());

        vkDestroySurfaceKHR(instance, surface, nullptr);
    }

    TEST_CASE("acquire/present pairs up across more frames than there are images (display required)")
    {
        ScopedPresentableWindow window;
        SKIP_WITHOUT_DISPLAY(window);

        SwapchainDesc desc;
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.Name = "test-swapcycle";

        GraphicsDevice& device = window.GetDevice();
        Ref<Swapchain> swapchain = Swapchain::Create(device, desc);
        REQUIRE(swapchain != nullptr);

        // Presenting something never acquired is undefined in Vulkan — a device-lost on this driver rather than an
        // error return — so the guard is exercised first, while nothing is outstanding for it to confuse.
        CHECK_FALSE(swapchain->Present(0));

        // More frames than images, deliberately: reuse of already-presented images is where a wrong layout transition
        // or a semaphore signalled twice shows up, and a one-frame test never reaches it.
        const UInt32 frames = swapchain->GetBackBufferCount() * 3u;
        UInt32 presented = 0;
        UInt32 timeouts = 0;

        for (UInt32 frame = 0; frame < frames; ++frame)
        {
            const AcquiredImage acquired = swapchain->Acquire();
            INFO("frame " << frame);

            if (acquired.Status == AcquireStatus::Timeout)
            {
                // Legitimate when the compositor is busy or the window is occluded; skipping the frame is correct.
                // Counted so an all-timeout run cannot pass by presenting nothing.
                ++timeouts;
                continue;
            }

            REQUIRE(acquired.Status == AcquireStatus::Success);
            REQUIRE(acquired.ImageIndex < swapchain->GetBackBufferCount());
            if (swapchain->Present(acquired.ImageIndex))
            {
                ++presented;
            }
        }

        INFO("presented " << presented << ", timed out " << timeouts << " of " << frames);
        CHECK(presented > 0u);

        // The real point: the device still answers after all of that. Swapchain bugs typically corrupt rather than
        // return an error, so "still able to create command lists and build a pipeline" is what separates a working
        // presentation path from one that has quietly killed the queue.
        device.WaitForIdle();

        nvrhi::IDevice* rhi = device.GetNativeDevice();
        REQUIRE(rhi != nullptr);
        CHECK(static_cast<bool>(rhi->createCommandList()));

        if (RenderTarget* target = swapchain->GetRenderTarget(0))
        {
            CHECK(BuildProbePipeline(device, *target) != nullptr);
        }
    }

    // What the rest of this suite cannot do: prove a frame rendered through the engine's own back-buffer wrappers
    // arrives as the pixels the format mapping claims. Everything else checks API-level correctness (creation succeeds,
    // targets are usable attachments), which would still hold if ToEngineFormat picked a channel order that made every
    // presented frame a colour swap. Reading actual bytes back is the only check that closes that loop.
    TEST_CASE("a colour cleared through a back buffer reads back with the mapped channel order (display required)")
    {
        ScopedPresentableWindow window;
        SKIP_WITHOUT_DISPLAY(window);

        GraphicsDevice& device = window.GetDevice();

        SwapchainDesc desc;
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.Name = "test-swapchain-readback";

        Ref<Swapchain> swapchain = Swapchain::Create(device, desc);
        REQUIRE(swapchain != nullptr);

        const GraphicsFormat format = swapchain->GetColourFormat();
        INFO("surface format: " << static_cast<int>(format));
        // 8-bit UNORM formats only: the byte-level expectation below decodes the clear colour by hand, which is exact
        // for 4x8 and meaningless for float formats. This host reports a BGRA/RGBA 8-bit surface.
        REQUIRE((format == GraphicsFormat::Bgra8Unorm || format == GraphicsFormat::Rgba8Unorm));

        RenderTarget* target = swapchain->GetRenderTarget(0);
        REQUIRE(target != nullptr);
        Texture* backBuffer = swapchain->GetTexture(0);
        REQUIRE(backBuffer != nullptr);

        // Clear through NVRHI using the engine's own wrapper, so what is verified is the wrapper + format pairing and
        // not a separately-created texture. An unmistakable colour with distinct channels: a channel-order bug cannot
        // turn (160, 32, 96) into itself.
        nvrhi::IDevice* rhi = device.GetNativeDevice();
        REQUIRE(rhi != nullptr);

        constexpr float kRed = 160.0f / 255.0f;
        constexpr float kGreen = 32.0f / 255.0f;
        constexpr float kBlue = 96.0f / 255.0f;

        nvrhi::CommandListHandle commandList = rhi->createCommandList();
        commandList->open();
        commandList->setTextureState(backBuffer->GetNativeTexture(), nvrhi::AllSubresources,
                                     nvrhi::ResourceStates::RenderTarget);
        commandList->clearTextureFloat(backBuffer->GetNativeTexture(), nvrhi::TextureSubresourceSet(),
                                       nvrhi::Color(kRed, kGreen, kBlue, 1.0f));
        commandList->close();

        nvrhi::ICommandList* raw = commandList;
        rhi->executeCommandLists(&raw, 1);

        // ReadBack transitions to copy-source and copies — which is why the swapchain requests TRANSFER_SRC on its
        // images (see Build()); without it this call fails rather than returning wrong data.
        UInt32 rowPitch = 0;
        const std::vector<UInt8> pixels = backBuffer->ReadBack(device, rowPitch);
        REQUIRE_FALSE(pixels.empty());
        REQUIRE(rowPitch >= swapchain->GetWidth() * 4u);

        // Decode by the format the swapchain reported, then require the bytes to match. If ToEngineFormat and the
        // NVRHI mapping disagreed about BGRA vs RGBA, the read-back bytes would be swapped against this expectation.
        // Scale the normalised clear colour to bytes and round, matching what an 8-bit UNORM target stores. Written
        // explicitly rather than compared as floats because the whole point is a byte-level channel-order check.
        const auto byteOf = [](float channel) { return static_cast<UInt8>(channel * 255.0f + 0.5f); };
        const UInt32 firstByte = 0;

        // Every texel must carry the clear colour, not just the first: a partially-written or mis-strided buffer shows
        // up as later rows differing, which is exactly what an ignored row pitch produces.
        UInt32 mismatchingTexels = 0;
        for (UInt32 y = 0; y < swapchain->GetHeight(); ++y)
        {
            const UInt8* row = pixels.data() + static_cast<size_t>(y) * rowPitch;
            for (UInt32 x = 0; x < swapchain->GetWidth(); ++x)
            {
                const UInt8* texel = row + static_cast<size_t>(x) * 4u;
                const bool bgraMatches = texel[0] == byteOf(kBlue) && texel[1] == byteOf(kGreen) &&
                                         texel[2] == byteOf(kRed) && texel[3] == 255;
                const bool rgbaMatches = texel[0] == byteOf(kRed) && texel[1] == byteOf(kGreen) &&
                                         texel[2] == byteOf(kBlue) && texel[3] == 255;

                // Exactly one of the two orders may match, and it must be the one the reported format says.
                const bool expectedMatches = format == GraphicsFormat::Bgra8Unorm ? bgraMatches : rgbaMatches;
                if (!expectedMatches)
                {
                    ++mismatchingTexels;
                }
            }
        }

        INFO("first texel bytes: " << static_cast<int>(pixels[firstByte]) << ", "
                                   << static_cast<int>(pixels[firstByte + 1]) << ", "
                                   << static_cast<int>(pixels[firstByte + 2]) << ", "
                                   << static_cast<int>(pixels[firstByte + 3]));
        CHECK_MESSAGE(mismatchingTexels == 0u, mismatchingTexels << " texels did not match the cleared colour");

        // Presenting after a readback must still work: ReadBack leaves the image in copy-source layout via NVRHI's
        // tracker while the swapchain tracks its own transitions, and a presented frame that showed stale or discarded
        // contents here would mean those two disagree in a way that matters to real rendering.
        const AcquiredImage acquired = swapchain->Acquire();
        if (acquired.Status == AcquireStatus::Success)
        {
            CHECK(swapchain->Present(acquired.ImageIndex));
        }

        device.WaitForIdle();
    }

    TEST_CASE("presenting the same image twice is refused (display required)")
    {
        ScopedPresentableWindow window;
        SKIP_WITHOUT_DISPLAY(window);

        SwapchainDesc desc;
        desc.Width = kWidth;
        desc.Height = kHeight;

        Ref<Swapchain> swapchain = Swapchain::Create(window.GetDevice(), desc);
        REQUIRE(swapchain != nullptr);

        const AcquiredImage acquired = swapchain->Acquire();
        if (acquired.Status != AcquireStatus::Success)
        {
            WARN("compositor gave no image; cannot exercise double-present");
            return;
        }

        // First present succeeds, second is refused. Without the per-image acquired flag this is undefined behaviour
        // and would take the device down frames later, far from the cause.
        CHECK(swapchain->Present(acquired.ImageIndex));
        CHECK_FALSE(swapchain->Present(acquired.ImageIndex));

        window.GetDevice().WaitForIdle();
    }

    TEST_CASE("resize produces fresh valid back buffers (display required)")
    {
        ScopedPresentableWindow window;
        SKIP_WITHOUT_DISPLAY(window);

        SwapchainDesc desc;
        desc.Width = kWidth;
        desc.Height = kHeight;

        GraphicsDevice& device = window.GetDevice();
        Ref<Swapchain> swapchain = Swapchain::Create(device, desc);
        REQUIRE(swapchain != nullptr);

        const UInt32 originalWidth = swapchain->GetWidth();
        const UInt32 originalHeight = swapchain->GetHeight();
        REQUIRE(originalWidth > 0u);

        // Same size must be a no-op reporting success: a frame loop that calls Resize unconditionally after comparing
        // sizes would otherwise destroy and rebuild the swapchain every frame.
        CHECK(swapchain->Resize(originalWidth, originalHeight));
        CHECK_FALSE(swapchain->NeedsRebuild(originalWidth, originalHeight));

        const UInt32 newWidth = originalWidth + 64u;
        const UInt32 newHeight = originalHeight + 48u;
        CHECK(swapchain->NeedsRebuild(newWidth, newHeight));
        REQUIRE(swapchain->Resize(newWidth, newHeight));

        CHECK(swapchain->GetWidth() > 0u);
        CHECK(swapchain->GetHeight() > 0u);
        INFO("resized to " << swapchain->GetWidth() << "x" << swapchain->GetHeight());

        // The old NVRHI wrappers referenced VkImages freed with the old swapchain — which is why Destroy() clears them
        // before vkDestroySwapchainKHR. A stale wrapper would appear here as an attachment that no longer builds a
        // pipeline, or worse work by luck until it did not.
        const UInt32 backBuffers = swapchain->GetBackBufferCount();
        CHECK(backBuffers >= 2u);
        for (UInt32 index = 0; index < backBuffers; ++index)
        {
            RenderTarget* target = swapchain->GetRenderTarget(index);
            INFO("image " << index);
            REQUIRE(target != nullptr);
            CHECK(target->GetWidth() == swapchain->GetWidth());
            CHECK(BuildProbePipeline(device, *target) != nullptr);
        }

        // And the rebuilt swapchain still presents: a resize that leaves a dead one behind is exactly what a user
        // dragging a window corner would hit.
        const AcquiredImage acquired = swapchain->Acquire();
        if (acquired.Status == AcquireStatus::Success)
        {
            CHECK(swapchain->Present(acquired.ImageIndex));
        }

        device.WaitForIdle();
    }

    TEST_CASE("a zero-size request never yields a degenerate swapchain")
    {
        ScopedPresentableWindow window;
        SKIP_WITHOUT_DISPLAY(window);

        GraphicsDevice& device = window.GetDevice();

        SwapchainDesc desc;
        desc.Width = 0;
        desc.Height = 0;

        // A mapped window makes the compositor report a real extent, so this may legitimately succeed using it. What
        // must never happen is a swapchain with zero width or height: vkCreateSwapchainKHR with a zero extent fails,
        // and a half-initialised object handed back would fail later inside a framebuffer creation.
        Ref<Swapchain> swapchain = Swapchain::Create(device, desc);
        if (swapchain != nullptr)
        {
            CHECK(swapchain->GetWidth() > 0u);
            CHECK(swapchain->GetHeight() > 0u);
        }
    }

    // The fallback decision, tested headlessly. Doing this through Swapchain::Create cannot work: this host's surface
    // offers all four Vulkan present modes, so every request is honoured and the downgrade branch — the one that was
    // broken — never executes. Any test written against a real compositor here would assert only that support exists.
    TEST_CASE("present mode resolution falls back to VSync when the request is unsupported")
    {
        // VSync asked, VSync available always (FIFO is required of every driver).
        constexpr PresentModeSupport none{};
        constexpr PresentModeSupport mailboxOnly{.Mailbox = true};
        constexpr PresentModeSupport immediateOnly{.Immediate = true};
        constexpr PresentModeSupport both{.Mailbox = true, .Immediate = true};

        CHECK(ResolvePresentMode(PresentMode::VSync, none) == PresentMode::VSync);
        CHECK(ResolvePresentMode(PresentMode::VSync, both) == PresentMode::VSync);

        // The branches that matter: unsupported requests degrade, supported ones pass through.
        CHECK(ResolvePresentMode(PresentMode::Mailbox, mailboxOnly) == PresentMode::Mailbox);
        CHECK(ResolvePresentMode(PresentMode::Mailbox, immediateOnly) == PresentMode::VSync);
        CHECK(ResolvePresentMode(PresentMode::Mailbox, none) == PresentMode::VSync);

        CHECK(ResolvePresentMode(PresentMode::Immediate, immediateOnly) == PresentMode::Immediate);
        CHECK(ResolvePresentMode(PresentMode::Immediate, mailboxOnly) == PresentMode::VSync);
        CHECK(ResolvePresentMode(PresentMode::Immediate, none) == PresentMode::VSync);
        CHECK(ResolvePresentMode(PresentMode::Immediate, both) == PresentMode::Immediate);

        // Immediate and Mailbox must not be confused with each other: a resolver that returned the *other* unsupported
        // mode rather than VSync would satisfy "not the requested one" while producing tearing where low latency was
        // asked for, or vice versa.
        CHECK(ResolvePresentMode(PresentMode::Mailbox, immediateOnly) != PresentMode::Immediate);
        CHECK(ResolvePresentMode(PresentMode::Immediate, mailboxOnly) != PresentMode::Mailbox);
    }

    // That creation honours the resolved mode rather than re-deriving it. Stated limitation, because it matters for
    // reading a green result here: on a host whose surface offers every present mode — this one exposes four — resolved
    // and requested are always equal, so NOTHING can distinguish "passed the resolved mode" from "passed the requested
    // mode" by observation, and this case proves only that GetPresentMode() agrees with ResolvePresentMode(). Verified:
    // mutating `resolved` to be derived from desc.Mode instead of ActualMode leaves all 112 assertions passing on this
    // machine. On a FIFO-only compositor the difference is observable — passing an unsupported mode through fails
    // vkCreateSwapchainKHR, and the REQUIRE below fires rather than skipping quietly. The resolver's own branches are
    // covered unconditionally by the headless case above; what is hardware-dependent is only whether creation uses it.
    TEST_CASE("creation honours the resolved present mode (display required)")
    {
        ScopedPresentableWindow window;
        SKIP_WITHOUT_DISPLAY(window);

        GraphicsDevice& device = window.GetDevice();

        nvrhi::IDevice* rhi = device.GetNativeDevice();
        REQUIRE(rhi != nullptr);
        const VkPhysicalDevice physical =
            reinterpret_cast<VkPhysicalDevice>(rhi->getNativeObject(nvrhi::ObjectTypes::VK_PhysicalDevice).pointer);

        const VkInstance instance =
            reinterpret_cast<VkInstance>(rhi->getNativeObject(nvrhi::ObjectTypes::VK_Instance).pointer);
        void* rawSurface = nullptr;
        REQUIRE(window.GetHandle()->CreateVulkanSurface(instance, &rawSurface));
        const VkSurfaceKHR surface = static_cast<VkSurfaceKHR>(rawSurface);

        uint32_t modeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &modeCount, nullptr);
        std::vector<VkPresentModeKHR> offered(modeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &modeCount, offered.data());
        vkDestroySurfaceKHR(instance, surface, nullptr);
        REQUIRE_FALSE(offered.empty());

        // Support computed the way Build() does it, from the driver's list.
        PresentModeSupport support;
        support.Mailbox = std::find(offered.begin(), offered.end(), VK_PRESENT_MODE_MAILBOX_KHR) != offered.end();
        support.Immediate = std::find(offered.begin(), offered.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != offered.end();

        // Ask for the mode least likely to be honoured and check the swapchain reports exactly what the resolver says —
        // not what was requested. On a host with everything this is a pass-through; on a FIFO-only compositor it is the
        // downgrade, and either way GetPresentMode() must agree with ResolvePresentMode().
        static constexpr PresentMode kRequests[] = {PresentMode::VSync, PresentMode::Mailbox, PresentMode::Immediate};
        for (const PresentMode requested : kRequests)
        {
            SwapchainDesc desc;
            desc.Width = kWidth;
            desc.Height = kHeight;
            desc.Mode = requested;

            Ref<Swapchain> swapchain = Swapchain::Create(device, desc);
            INFO("requested mode " << static_cast<int>(requested));
            REQUIRE_MESSAGE(swapchain != nullptr, "an unsupported present mode must degrade rather than fail creation");

            CHECK(swapchain->GetPresentMode() == ResolvePresentMode(requested, support));
            // Whatever mode it settled on, the swapchain has to be usable — a fallback that produced a broken object
            // would be no better than failing.
            CHECK(swapchain->GetBackBufferCount() >= 2u);
            CHECK(swapchain->GetRenderTarget(0) != nullptr);
        }
    }

    TEST_CASE("attaching to a device built without surface extensions says why (display required)")
    {
        PlatformScope* scope = GetDisplayScope();
        if (scope == nullptr)
        {
            WARN("windowing could not initialise");
            return;
        }
        if (scope->IsHeadless())
        {
            WARN("no display server on this host");
            return;
        }

        WindowDesc windowDesc;
        windowDesc.Title = "Hollowpoint missing-extension probe";
        windowDesc.Width = 64;
        windowDesc.Height = 64;
        windowDesc.Visible = false;
        Window* window = Window::Create(windowDesc);
        if (window == nullptr)
        {
            WARN("could not create a window");
            return;
        }

        // The mistake this guards: creating the device WITHOUT the window's required instance extensions. The
        // VkInstance is built inside GraphicsDevice and cannot gain VK_KHR_wayland_surface later, so AttachWindow must
        // fail — and it has to say which extension was missing, because the alternative symptom ("no window attached"
        // from every later presentation call) points at a window that is plainly there. This cost an entire session of
        // vacuously-green swapchain tests; the message is the fix's durable part.
        GraphicsDeviceDesc desc;
        desc.ApplicationName = "Hollowpoint missing-extension probe";  // deliberately no ExtraInstanceExtensions

        GraphicsDevice* device = GraphicsDevice::Create(desc);
        if (device == nullptr)
        {
            WARN((std::string("no graphics device: ") + GraphicsDevice::GetFailureReason()).c_str());
            delete window;
            return;
        }

        {
            Hollowpoint::Test::ScopedLogCapture logs;
            CHECK_FALSE(device->AttachWindow(window));
            std::string captured;
            for (const std::string& line : logs.GetLines())
            {
                captured += line + "\n";
            }
            INFO(captured);
            CHECK(logs.AnyMessageContains("VK_KHR_surface"));
        }

        CHECK_FALSE(device->HasWindow());
        delete device;
        delete window;
    }
}
