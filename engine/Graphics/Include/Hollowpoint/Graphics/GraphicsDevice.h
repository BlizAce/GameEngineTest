#pragma once

#include <string>
#include <vector>

#include "Hollowpoint/Graphics/GraphicsApi.h"

// This header is deliberately free of Vulkan and NVRHI includes: engine code that needs to know a device exists
// (or that wants to compile shaders) should not pay for the Vulkan headers, which are among the slowest in the
// dependency graph. The RHI handle is exposed through Graphics/RhiAccess.h, which says explicitly what it costs.

namespace Hollowpoint::Platform
{
class Window;
}

// Forward declaration, not an include: exposing the RHI pointer needs only the class name, and including NVRHI's
// headers here would make every Graphics consumer compile Vulkan (the slowest header in the graph). Code that wants
// to *use* the returned pointer includes Hollowpoint/Graphics/RhiAccess.h, which says what that costs.
namespace nvrhi
{
class IDevice;
}

namespace Hollowpoint::Graphics
{

// ---------------------------------------------------------------------------
// The graphics device.
//
// Owns a VkInstance, a selected physical device, a VkDevice and the NVRHI device built on top. NVRHI's Vulkan
// backend deliberately does not create the native objects, so this class is where the decisions that affect every
// later frame get made: which GPU, which API version, which extensions, which queues, whether validation layers
// are on.
//
// Lifetime: exactly one GraphicsDevice per process in practice (Vulkan allows more, and this code does not
// forbid them, but the dispatcher storage below is process-global so a second device re-initialises it). It must
// outlive every resource created from it, including anything the renderer holds — which is why it is owned by the
// application object rather than by a subsystem.
// ---------------------------------------------------------------------------

//! A discrete or integrated GPU as Vulkan sees it.
struct AdapterInfo
{
    UInt32 Index = 0;  //!< Position in the enumeration order, used to select one explicitly.
    std::string Name;
    //! Vulkan's VK_PHYSICAL_DEVICE_TYPE_* mapped onto a stable engine enum ordering: Other < IntegratedDiscrete.
    bool IsDiscrete = false;
    UInt32 VendorId = 0;
    UInt32 DeviceId = 0;
    //! Bytes of **device-local** memory — real VRAM on a discrete card, 0-ish for an integrated part whose only
    //! heap is shared system RAM. This is the number a texture-streaming budget must be sized against; counting
    //! host-visible system heaps here inflates it by 5x or more (measured: 10 GB reported as 58 GB on an RTX 3080),
    //! which makes a streamer believe it can hold every mip of every texture at once.
    UInt64 DedicatedVideoMemoryBytes = 0;

    //! Bytes across **all** memory heaps, device-local and shared. Exposed alongside the device-local figure so a
    //! budget can report "using X of Y" and so the two are comparable: for any adapter that exposes both a local and
    //! a host-only heap, DedicatedVideoMemoryBytes < TotalMemoryBytes, which is what makes a mis-filtered
    //! device-local count detectable rather than plausible-looking.
    UInt64 TotalMemoryBytes = 0;

    //! True when the adapter exposes at least one heap that is NOT device-local (shared system RAM). This is a
    //! separate computation from the two totals above, which is what makes it useful: an adapter with this true must
    //! report DedicatedVideoMemoryBytes strictly below TotalMemoryBytes. Cross-checking the two catches a memory
    //! filter that counts every heap — a bug that otherwise produces a plausible-looking number (58 GB of "VRAM" on
    //! a 10 GB card) and only shows up as streaming thrash much later. It also stays correct for single-heap adapters
    //! (llvmpipe), where the two totals are legitimately equal.
    //! Number of memory heaps the driver reports, taken straight from Vulkan rather than derived from any filtering.
    //! This independence is the point: a test can assert that an adapter with more than one heap must report
    //! DedicatedVideoMemoryBytes strictly below TotalMemoryBytes, and catch a filter that counts every heap. A flag
    //! computed inside that same filter cannot do this — mutating the filter moves the flag and the totals together,
    //! so the assertion stays vacuously true (which is exactly how an earlier version of this check fooled itself).
    UInt32 MemoryHeapCount = 0;
    UInt32 VulkanApiVersion = 0;  //!< Packed VkApiVersion; compare with VK_VERSION_* macros at use site.
};

//! How to choose which GPU to run on.
enum class AdapterPreference : UInt8
{
    HighPerformance,  //!< Prefer a discrete GPU. Default: rendering is the point.
    PowerEfficient,   //!< Prefer integrated: useful for a background tool that should not wake the dGPU.
    AnyCompatible,    //!< Take whatever Vulkan picks first (what vkQuake-style tools do).
};

//! A Vulkan version constructor usable without including the Vulkan headers, which is what lets this header avoid
//! them. Defined inline (not in a .cpp) because it is consteval: the value must be computable where it is written,
//! and its use as a default member initialiser below requires the definition to be visible here. Declared before
//! GraphicsDeviceDesc for the same reason — ordering is load-bearing, not stylistic.
[[nodiscard]] consteval UInt32 MakeVulkanVersion(UInt32 major, UInt32 minor, UInt32 patch) noexcept
{
    // Vulkan's own packing (VK_API_VERSION): 10 bits major, 12 bits minor, 8 bits patch. Spelled out rather than
    // calling the VK_MAKE_API_VERSION macro because that macro expands old-style casts at the use site, which our
    // -Wold-style-cast rejects in engine code.
    return (major << 22U) | (minor << 12U) | patch;
}

struct GraphicsDeviceDesc
{
    std::string ApplicationName = "Hollowpoint";

    //! Minimum Vulkan API version required of the selected device, packed as Vulkan packs it (major<<22 |
    //! minor<<12 | patch). 1.2 buys timeline semaphores and buffer device address, both relied on by NVRHI and by
    //! later features; requiring more than a machine has fails with a clear message rather than at the first
    //! feature query.
    UInt32 RequiredApiVersion = MakeVulkanVersion(1, 2, 0);

    AdapterPreference Preference = AdapterPreference::HighPerformance;

    //! Force a specific adapter by index (see EnumerateAdapters), ignoring Preference. Useful for reproducing a bug
    //! reported on an integrated GPU, and for running the test suite on llvmpipe deliberately.
    bool UseSpecificAdapter = false;
    UInt32 AdapterIndex = 0;

    //! Enable the Vulkan validation layer when present. Default true in debug builds: it is the only thing that
    //! explains why a pipeline creation failed. A missing layer is reported as a warning, not an error — shipping
    //! drivers commonly lack it and the engine must still run.
    bool EnableValidation =
#if !defined(NDEBUG)
        true
#else
        false
#endif
        ;

    //! Request the debug utility extension so renderdoc/RenderDoc-style tools can pick up markers and object names.
    bool EnableDebugUtils =
#if !defined(NDEBUG)
        true
#else
        false
#endif
        ;

    //! Instance extensions required by callers, e.g. a window surface extension from Platform. Duplicates are
    //! removed; unsupported ones cause creation to fail (they were requested for a reason).
    std::vector<std::string> ExtraInstanceExtensions;
};

//! Enumerates adapters compatible with the requested API version.
//!
//! Creates a temporary VkInstance to do so. That costs a few tens of milliseconds and is done once at startup (or
//! by an adapter-picker UI), which is why it is a free function rather than part of GraphicsDevice: listing GPUs
//! must not require committing to one.
[[nodiscard]] HP_GRAPHICS_API std::vector<AdapterInfo> EnumerateAdapters(UInt32 requiredApiVersion = 0);

class GraphicsDeviceImpl;

class HP_GRAPHICS_API GraphicsDevice
{
public:
    //! Creates a device. Returns nullptr on failure with the reason logged and available in GetFailureReason().
    //! Failure is expected and must be handled: no Vulkan driver, an old driver lacking the required API version,
    //! or no queue supporting graphics.
    [[nodiscard]] static GraphicsDevice* Create(const GraphicsDeviceDesc& desc);

    ~GraphicsDevice();

    GraphicsDevice(const GraphicsDevice&) = delete;
    GraphicsDevice& operator=(const GraphicsDevice&) = delete;

    [[nodiscard]] const AdapterInfo& GetAdapter() const noexcept;

    //! True when validation layers were actually enabled (they are requested, not guaranteed — a shipping driver
    //! may not have them).
    [[nodiscard]] bool IsValidationEnabled() const noexcept;

    //! Blocks until the GPU has finished all submitted work. Required before reading back pixels or destroying the
    //! device; also what a test does between submitting a frame and checking its output.
    void WaitForIdle();

    //! Total bytes of dedicated video memory, or 0 for an integrated adapter sharing system memory. Used to size
    //! texture streaming budgets.
    [[nodiscard]] UInt64 GetDedicatedVideoMemoryBytes() const noexcept;

    //! Binds a window so the device can present to it. Optional: an offscreen-only device (asset baking, tests)
    //! never calls this and needs no display server.
    //!
    //! Safe to call more than once with the same window. Passing nullptr detaches. Returns false when surface
    //! creation failed (no compositor, or a backend that cannot present).
    //!
    //! **The device must have been created with this window's instance extensions**, i.e. pass
    //! `window->GetRequiredVulkanInstanceExtensions()` into GraphicsDeviceDesc::ExtraInstanceExtensions *before*
    //! Create(). The instance is built inside this class, and VK_KHR_wayland_surface/_xlib are instance extensions: a
    //! device created without them returns false here with the cause logged only as "could not create a Vulkan
    //! surface", and every later presentation call then reports "no window attached" — naming the symptom, not the
    //! cause. The required order is therefore scope → window → read its extensions → create device, which is also what
    //! an application must do; nothing here can detect or repair the wrong order after the fact.
    bool AttachWindow(Platform::Window* window);

    [[nodiscard]] bool HasWindow() const noexcept;

    //! The underlying RHI device, for the renderer and for tests of this layer.
    //!
    //! This is a deliberate escape hatch rather than the start of a thin wrapper: NVRHI is already a portable
    //! abstraction over Vulkan/D3D12, so re-wrapping each of its ~50 entry points would add indirection without
    //! adding safety, and the renderer needs the real thing. The cost is that calling code must include
    //! Hollowpoint/Graphics/RhiAccess.h (which brings in NVRHI and Vulkan-Hpp) — hence this returns a pointer to an
    //! incomplete type, so merely *seeing* this declaration costs nothing and only users pay.
    //!
    //! Owned by this object; valid for the GraphicsDevice's lifetime. Null if creation somehow produced no RHI
    //! device (Create() then returns nullptr, so callers holding a GraphicsDevice always get a non-null pointer).
    [[nodiscard]] nvrhi::IDevice* GetNativeDevice() const noexcept;

    //! True when this device can present, i.e. VK_KHR_swapchain was supported by the selected adapter and enabled at
    //! VkDevice creation. False on compute-only/virtual drivers and is decided once, at creation — a swapchain cannot
    //! be added later, so callers branch on this instead of discovering the failure inside Swapchain::Create.
    //! Offscreen-only uses (asset baking, the test suite) never present and are unaffected by it being false.
    [[nodiscard]] bool CanPresent() const noexcept;

    //! Why creation failed (empty when it succeeded). Intended for a user-visible message: it names the missing
    //! piece ("no Vulkan-capable adapter found", "driver reports Vulkan 1.1, 1.2 required").
    [[nodiscard]] static const char* GetFailureReason();

private:
    GraphicsDevice();

    //! Native handles for the swapchain, described with the friendship below. Returned as void* so this header stays
    //! free of Vulkan includes — the whole reason GraphicsDevice.h is cheap to include.
    [[nodiscard]] void* GetNativeVkDevice() const noexcept;
    [[nodiscard]] void* GetNativeVkQueue() const noexcept;

    //! The surface created by AttachWindow, or null. Owned by this class — Swapchain must never destroy it.
    [[nodiscard]] void* GetNativeVkSurface() const noexcept;

    //! Swapchain needs the raw VkDevice/VkQueue/surface that this class owns but does not expose: NVRHI has no
    //! swapchain support at all (its Tutorial.md states this outright and defers to Donut's DeviceManager), so the
    //! engine implements presentation in raw Vulkan and must reach the native objects. Kept as one friend rather than
    //! public getters because handing out a VkDevice lets any caller destroy it, reorder queue submission, or create
    //! objects that outlive the device — none of which this class could then reason about. Swapchain.h explains why
    //! the same escape hatch (GetNativeDevice) does not cover this case.
    friend class Swapchain;

    // Held by pointer so this header stays free of NVRHI/Vulkan types. Defined in Src/GraphicsDevice.cpp.
    GraphicsDeviceImpl* m_Impl = nullptr;
};

}  // namespace Hollowpoint::Graphics
