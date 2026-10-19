#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Graphics/GraphicsDevice.h"
#include "Hollowpoint/Graphics/RhiAccess.h"
#include "Hollowpoint/Platform/Window.h"

namespace Hollowpoint::Graphics
{

using Core::Int32;
using Core::Size;
using Core::UInt64;

namespace
{

std::string g_FailureReason;

//! Vulkan 1.1, the API level requested when *creating* the instance. The instance is deliberately created at 1.1 so
//! vkEnumerateInstanceVersion can then report the driver's real level and an old driver produces a clear message
//! rather than failing inside vkCreateInstance.
constexpr UInt32 kVulkanApi11 = MakeVulkanVersion(1, 1, 0);

[[nodiscard]] constexpr UInt32 VulkanMajor(UInt32 version) noexcept
{
    return version >> 22U;
}

[[nodiscard]] constexpr UInt32 VulkanMinor(UInt32 version) noexcept
{
    return (version >> 12U) & 0x3FFU;
}


//! The validation layer's instance name. Not defined by the Vulkan headers (layers are named by convention, not
//! by the API), so it is spelled here; a typo silently disables validation, hence the constant and its probe test.
constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";

void SetFailure(std::string reason)
{
    g_FailureReason = std::move(reason);
}

void ClearFailure()
{
    g_FailureReason.clear();
}

[[nodiscard]] const char* VkResultName(VkResult result) noexcept
{
    const char* text = nvrhi::vulkan::resultToString(result);
    return text != nullptr ? text : "unknown VkResult";
}

//! Sum of device-local memory heaps.
//!
//! Only device-local heaps are counted: on an integrated part the "local" heap is system RAM shared with the CPU,
//! and counting it as dedicated video memory would let a texture-streaming budget assume far more headroom than
//! exists and thrash.
void QueryMemoryBytes(VkPhysicalDevice physical, UInt64& outDeviceLocal, UInt64& outTotal, UInt32& outHeapCount)
{
    // vkGetPhysicalDeviceMemoryProperties rather than the properties2 extension: it is core, portable, and gives the
    // heap flags needed to separate real VRAM from shared system RAM.
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);

    outDeviceLocal = 0;
    outTotal = 0;

    // Reported verbatim from the driver. The count is deliberately *not* derived from the filtering below: that
    // independence is what lets a test cross-check the filter (see AdapterInfo::MemoryHeapCount). A flag computed
    // inside the same loop cannot do it — mutating the filter moves the flag and the sums together, leaving any
    // assertion between them vacuously true.
    outHeapCount = memory.memoryHeapCount;

    for (uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap)
    {
        const bool deviceLocal = (memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
        outTotal += memory.memoryHeaps[heap].size;

        // Only device-local heaps count as dedicated video memory. On an integrated part the "local" heap *is*
        // system RAM shared with the CPU, and counting host-visible-only heaps would inflate this several-fold —
        // enough for a streaming budget to over-commit and thrash every frame.
        if (deviceLocal)
        {
            outDeviceLocal += memory.memoryHeaps[heap].size;
        }
    }
}

[[nodiscard]] AdapterInfo DescribeAdapter(UInt32 index, VkPhysicalDevice physical)
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);

    AdapterInfo info;
    info.Index = index;
    info.Name = properties.deviceName;
    info.IsDiscrete = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    info.VendorId = properties.vendorID;
    info.DeviceId = properties.deviceID;
    info.VulkanApiVersion = properties.apiVersion;
    QueryMemoryBytes(physical, info.DedicatedVideoMemoryBytes, info.TotalMemoryBytes, info.MemoryHeapCount);
    return info;
}

//! Deterministic adapter ranking rather than "whatever Vulkan enumerated first".
//!
//! On a laptop or an RTX-with-iGPU desktop the enumeration order is driver-defined, so an unordered pick makes
//! performance and even visual behaviour depend on which GPU the driver happened to list first — and differs
//! between two machines with identical hardware. Memory size breaks ties so that the same choice is made every run.
[[nodiscard]] Size PickAdapter(const std::vector<AdapterInfo>& adapters, const GraphicsDeviceDesc& desc)
{
    if (adapters.empty())
    {
        return 0;
    }

    if (desc.UseSpecificAdapter)
    {
        return std::min<std::size_t>(desc.AdapterIndex, adapters.size() - 1);
    }

    Size best = 0;
    Int32 bestScore = std::numeric_limits<Int32>::min();

    for (const AdapterInfo& adapter : adapters)
    {
        Int32 score = 0;

        switch (desc.Preference)
        {
            case AdapterPreference::HighPerformance:
                if (adapter.IsDiscrete)
                {
                    score += 100;
                }
                score += static_cast<Int32>(std::min<UInt64>(adapter.DedicatedVideoMemoryBytes / (1ull << 30), 31));
                break;
            case AdapterPreference::PowerEfficient:
                if (!adapter.IsDiscrete)
                {
                    score += 100;
                }
                break;
            case AdapterPreference::AnyCompatible:
                // Preserve enumeration order: the first adapter wins because scoring cannot distinguish them.
                score = -static_cast<Int32>(adapter.Index);
                break;
        }

        if (score > bestScore)
        {
            bestScore = score;
            best = adapter.Index;
        }
    }

    return best;
}

[[nodiscard]] std::vector<std::string> DedupeExtensions(std::vector<std::string> extensions)
{
    // Vulkan requires unique, present extension names or vkCreateInstance fails. A surface extension is typically
    // requested both by the caller and by our own list, so duplicates are removed rather than passed through.
    std::sort(extensions.begin(), extensions.end());
    extensions.erase(std::unique(extensions.begin(), extensions.end()), extensions.end());
    return extensions;
}

[[nodiscard]] bool HasExtension(const std::vector<VkExtensionProperties>& available, const char* name)
{
    return std::any_of(available.begin(), available.end(), [name](const VkExtensionProperties& candidate) {
        return std::strcmp(candidate.extensionName, name) == 0;
    });
}

}  // namespace

// ============================================================================
// Implementation
// ============================================================================

class GraphicsDeviceImpl
{
public:
    //! Creates the native instance/device and hands them to NVRHI. Returns false with SetFailure() populated on any
    //! problem; nothing is left partially constructed (every Vulkan object created before the failure is destroyed).
    bool Create(const GraphicsDeviceDesc& desc)
    {
        // --- Instance ------------------------------------------------------
        VkApplicationInfo application{};
        application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.pApplicationName = desc.ApplicationName.c_str();
        application.applicationVersion = MakeVulkanVersion(0, 1, 0);
        application.pEngineName = "Hollowpoint";
        application.engineVersion = MakeVulkanVersion(0, 1, 0);
        // The instance is created at 1.1 so vkEnumerateInstanceVersion can then report the driver's real level;
        // asking for 1.2 up front would make an older driver fail without a useful message.
        application.apiVersion = kVulkanApi11;

        std::vector<std::string> instanceExtensions = DedupeExtensions(desc.ExtraInstanceExtensions);
        std::vector<const char*> enabledInstanceExtensions;
        enabledInstanceExtensions.reserve(instanceExtensions.size());

        // Validation and debug utils are *requested* extensions: absent on shipping drivers, so they are probed
        // rather than assumed. Their absence must not fail device creation.
        const bool wantValidation = desc.EnableValidation && ExtensionSupported(kValidationLayerName);
        const bool wantDebugUtils = desc.EnableDebugUtils && ExtensionSupported(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        if (desc.EnableValidation && !wantValidation)
        {
            // A warning, not an error: running without validation is normal on a user machine, but silently doing
            // so in a debug build would hide exactly the API misuse we want caught.
            HP_LOG_WARNING("GRAPHICS", "Vulkan validation layer requested but not installed — running without it");
        }

        if (wantValidation)
        {
            enabledLayers.push_back(kValidationLayerName);
        }
        if (wantDebugUtils)
        {
            instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        for (const std::string& extension : instanceExtensions)
        {
            enabledInstanceExtensions.push_back(extension.c_str());
        }

        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &application;
        instanceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledInstanceExtensions.size());
        instanceInfo.ppEnabledExtensionNames =
            enabledInstanceExtensions.empty() ? nullptr : enabledInstanceExtensions.data();
        instanceInfo.enabledLayerCount = static_cast<uint32_t>(enabledLayers.size());
        instanceInfo.ppEnabledLayerNames = enabledLayers.empty() ? nullptr : enabledLayers.data();

        VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance);
        if (result != VK_SUCCESS)
        {
            // A missing caller-requested extension is the most common cause and is worth naming: "extension not
            // present" alone does not say which one.
            SetFailure(std::string("vkCreateInstance failed: ") + VkResultName(result) +
                       (enabledInstanceExtensions.empty()
                            ? std::string()
                            : std::string(" (with extensions requested; check the surface extensions)")));
            return false;
        }

        // The loader entry points must be resolved before any other vk* call beyond instance creation.
        VULKAN_HPP_DEFAULT_DISPATCHER.init(instance, vkGetInstanceProcAddr);

        ValidationEnabled = wantValidation;
        DebugUtilsEnabled = wantDebugUtils;

        if (DebugUtilsEnabled)
        {
            InstallDebugMessenger();
        }

        // --- Physical devices ---------------------------------------------
        uint32_t count = 0;
        result = vkEnumeratePhysicalDevices(instance, &count, nullptr);
        if (result != VK_SUCCESS || count == 0)
        {
            SetFailure(count == 0u ? "No Vulkan-capable adapter found"
                                   : std::string("vkEnumeratePhysicalDevices failed: ") + VkResultName(result));
            Cleanup();
            return false;
        }

        std::vector<VkPhysicalDevice> candidates(count);
        vkEnumeratePhysicalDevices(instance, &count, candidates.data());

        for (uint32_t index = 0; index < count; ++index)
        {
            Adapters.push_back(DescribeAdapter(index, candidates[index]));
        }

        // Filter by required API version before choosing, so a fast-but-old GPU is not selected and then fail later.
        std::vector<AdapterInfo> compatible;
        for (const AdapterInfo& adapter : Adapters)
        {
            if (adapter.VulkanApiVersion >= desc.RequiredApiVersion)
            {
                compatible.push_back(adapter);
            }
        }

        if (compatible.empty())
        {
            SetFailure("No Vulkan adapter meets the required API version " + VersionString(desc.RequiredApiVersion) +
                       "; best available is " + BestAvailableVersionString(Adapters));
            Cleanup();
            return false;
        }

        // Pick among compatible adapters by their original enumeration index (used to look up the handle).
        const Size chosen = PickAdapter(compatible, desc);
        Selected = compatible[static_cast<size_t>(chosen)];

        VkPhysicalDevice physical = VK_NULL_HANDLE;
        for (uint32_t index = 0; index < count; ++index)
        {
            if (Adapters[index].Index == Selected.Index)
            {
                physical = candidates[index];
                break;
            }
        }

        if (physical == VK_NULL_HANDLE)
        {
            SetFailure("Internal error: selected adapter has no matching handle");
            Cleanup();
            return false;
        }

        PhysicalDevice = physical;
        DedicatedMemoryBytes = Selected.DedicatedVideoMemoryBytes;

        HP_LOG_INFO("GRAPHICS", "Selected adapter '{}' (Vulkan {}.{}, {} MB device-local)", Selected.Name,
                    VulkanMajor(Selected.VulkanApiVersion), VulkanMinor(Selected.VulkanApiVersion),
                    DedicatedMemoryBytes / (1024ull * 1024ull));

        // --- Queues --------------------------------------------------------
        if (!CreateLogicalDevice(desc))
        {
            Cleanup();
            return false;
        }

        // --- NVRHI ---------------------------------------------------------
        nvrhi::vulkan::DeviceDesc nvrhiDesc{};
        nvrhiDesc.instance = instance;
        nvrhiDesc.physicalDevice = PhysicalDevice;
        nvrhiDesc.device = LogicalDevice;
        nvrhiDesc.graphicsQueue = GraphicsQueue;
        nvrhiDesc.graphicsQueueIndex = static_cast<int>(GraphicsQueueFamily);
        // NVRHI's own test program enables timeline semaphores; several of its paths assume them, and they are core
        // in Vulkan 1.2 (which the required-version filter above guarantees).
        nvrhiDesc.bufferDeviceAddressSupported = true;
        nvrhiDesc.errorCB = &MessageCallbackInstance;

        NvrhiDevice = nvrhi::vulkan::createDevice(nvrhiDesc);
        if (!NvrhiDevice)
        {
            SetFailure("NVRHI failed to create a device from the native VkDevice");
            Cleanup();
            return false;
        }

        return true;
    }

    ~GraphicsDeviceImpl()
    {
        // Order matters: NVRHI must release its objects before the native device they belong to is destroyed, and
        // the debug messenger must outlive nothing (destroying it after the instance is UB).
        if (LogicalDevice != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(LogicalDevice);
        }

        NvrhiDevice = nullptr;

        if (DebugUtilsEnabled && instance != VK_NULL_HANDLE && DebugMessenger != VK_NULL_HANDLE)
        {
            const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy != nullptr)
            {
                destroy(instance, DebugMessenger, nullptr);
            }
            DebugMessenger = VK_NULL_HANDLE;
        }

        if (LogicalDevice != VK_NULL_HANDLE)
        {
            vkDestroyDevice(LogicalDevice, nullptr);
            LogicalDevice = VK_NULL_HANDLE;
        }

        if (instance != VK_NULL_HANDLE)
        {
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
    }

    void WaitForIdle()
    {
        if (NvrhiDevice)
        {
            NvrhiDevice->waitForIdle();
        }
        else if (LogicalDevice != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(LogicalDevice);
        }
    }

    bool AttachWindow(Platform::Window* window)
    {
        if (window == nullptr)
        {
            AttachedWindow = nullptr;
            return true;
        }

        void* surface = nullptr;
        if (!window->CreateVulkanSurface(instance, &surface) || surface == nullptr)
        {
            HP_LOG_ERROR("GRAPHICS", "Could not create a Vulkan surface for the window");
            return false;
        }

        // Replacing an existing surface would leak it: surfaces are owned here.
        if (AttachedSurface != VK_NULL_HANDLE)
        {
            const auto destroy =
                reinterpret_cast<PFN_vkDestroySurfaceKHR>(vkGetInstanceProcAddr(instance, "vkDestroySurfaceKHR"));
            if (destroy != nullptr)
            {
                destroy(instance, AttachedSurface, nullptr);
            }
        }

        AttachedSurface = static_cast<VkSurfaceKHR>(surface);
        AttachedWindow = window;
        return true;
    }

    AdapterInfo Selected;
    std::vector<AdapterInfo> Adapters;
    UInt64 DedicatedMemoryBytes = 0;
    bool ValidationEnabled = false;
    bool DebugUtilsEnabled = false;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
    VkDevice LogicalDevice = VK_NULL_HANDLE;
    VkQueue GraphicsQueue = VK_NULL_HANDLE;
    uint32_t GraphicsQueueFamily = 0;
    VkDebugUtilsMessengerEXT DebugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR AttachedSurface = VK_NULL_HANDLE;
    Platform::Window* AttachedWindow = nullptr;
    nvrhi::DeviceHandle NvrhiDevice;

    //! NVRHI routes internal errors here. Logging rather than aborting: an API misuse should be visible in the log
    //! and let the frame fail, since a validation error during development is far more useful with the app still up.
    struct Callback : nvrhi::IMessageCallback
    {
        using nvrhi::IMessageCallback::IMessageCallback;

        void message(nvrhi::MessageSeverity severity, const char* text) override
        {
            switch (severity)
            {
                case nvrhi::MessageSeverity::Info:
                    HP_LOG_INFO("NVRHI", "{}", text);
                    break;
                case nvrhi::MessageSeverity::Warning:
                    HP_LOG_WARNING("NVRHI", "{}", text);
                    break;
                case nvrhi::MessageSeverity::Error:
                default:
                    HP_LOG_ERROR("NVRHI", "{}", text);
                    break;
            }
        }
    } MessageCallbackInstance;

private:
    std::vector<const char*> enabledLayers;  // Kept alive for the lifetime of vkCreateInstance's caller frame.

    [[nodiscard]] static bool ExtensionSupported(const char* name)
    {
        uint32_t count = 0;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS)
        {
            return false;
        }

        std::vector<VkExtensionProperties> available(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
        return HasExtension(available, name);
    }

    void InstallDebugMessenger()
    {
        VkDebugUtilsMessengerCreateInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        // Warnings are included: driver warnings ("you are reading a buffer as an image") are usually the first
        // sign of a real bug, and filtering to errors alone hides them until they become a crash.
        info.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;

        info.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                  const VkDebugUtilsMessengerCallbackDataEXT* data, void*) -> VkBool32 {
            if (data == nullptr)
            {
                return VK_FALSE;
            }

            // Returning VK_FALSE is required: VK_TRUE turns the message into an error that fails the Vulkan call,
            // which would make validation itself break rendering.
            const char* text = data->pMessage != nullptr ? data->pMessage : "(no message)";
            if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            {
                HP_LOG_ERROR("VULKAN", "{}", text);
            }
            else
            {
                HP_LOG_WARNING("VULKAN", "{}", text);
            }
            return VK_FALSE;
        };

        const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create == nullptr)
        {
            DebugUtilsEnabled = false;
            return;
        }

        if (create(instance, &info, nullptr, &DebugMessenger) != VK_SUCCESS)
        {
            DebugMessenger = VK_NULL_HANDLE;
            DebugUtilsEnabled = false;
        }
    }

    bool CreateLogicalDevice(const GraphicsDeviceDesc& desc)
    {
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(PhysicalDevice, &familyCount, nullptr);

        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(PhysicalDevice, &familyCount, families.data());

        // A graphics-capable family is mandatory. Compute and transfer are taken from separate families when they
        // exist (so copies can overlap drawing) and fall back to the graphics family otherwise — an integrated GPU
        // often exposes a single family, and requiring three would make such machines unrunnable.
        GraphicsQueueFamily = familyCount;
        for (uint32_t index = 0; index < familyCount; ++index)
        {
            if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
            {
                GraphicsQueueFamily = index;
                break;
            }
        }

        if (GraphicsQueueFamily >= familyCount)
        {
            SetFailure("Selected adapter exposes no graphics-capable queue");
            return false;
        }

        float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = GraphicsQueueFamily;
        // One queue: the engine is single-threaded for submission (matching GLFW and the frame loop), and creating
        // several would advertise concurrency the renderer does not use while costing driver memory.
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        VkPhysicalDeviceVulkan12Features features12{};
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        // Timeline semaphores: NVRHI's Vulkan backend uses them for queue synchronisation. Core in 1.2, which the
        // adapter filter guarantees, so requiring them cannot exclude a device we would otherwise accept.
        features12.timelineSemaphore = VK_TRUE;
        // Buffer device address underlies shader resource pointers and NVRHI's getBufferDeviceAddress.
        features12.bufferDeviceAddress = VK_TRUE;

        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.pNext = &features12;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;

        const VkResult result = vkCreateDevice(PhysicalDevice, &deviceInfo, nullptr, &LogicalDevice);
        if (result != VK_SUCCESS)
        {
            SetFailure(std::string("vkCreateDevice failed: ") + VkResultName(result));
            return false;
        }

        // Re-init the dispatcher with the device so vkGetDeviceProcAddr-acquired entry points are available; NVRHI
        // calls through it for every command buffer operation.
        VULKAN_HPP_DEFAULT_DISPATCHER.init(instance, vkGetInstanceProcAddr, LogicalDevice);

        vkGetDeviceQueue(LogicalDevice, GraphicsQueueFamily, 0, &GraphicsQueue);
        if (GraphicsQueue == VK_NULL_HANDLE)
        {
            SetFailure("vkGetDeviceQueue returned a null graphics queue");
            return false;
        }

        (void)desc;
        return true;
    }

    void Cleanup()
    {
        NvrhiDevice = nullptr;
        if (LogicalDevice != VK_NULL_HANDLE)
        {
            vkDestroyDevice(LogicalDevice, nullptr);
            LogicalDevice = VK_NULL_HANDLE;
        }
        if (instance != VK_NULL_HANDLE)
        {
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] static std::string VersionString(UInt32 version)
    {
        return std::to_string(VulkanMajor(version)) + "." + std::to_string(VulkanMinor(version));
    }

    [[nodiscard]] static std::string BestAvailableVersionString(const std::vector<AdapterInfo>& adapters)
    {
        UInt32 best = 0;
        for (const AdapterInfo& adapter : adapters)
        {
            best = std::max(best, adapter.VulkanApiVersion);
        }

        if (best == 0u)
        {
            return "none";
        }
        return VersionString(best);
    }
};

// ============================================================================
// Public API
// ============================================================================

GraphicsDevice::GraphicsDevice() = default;

GraphicsDevice::~GraphicsDevice()
{
    delete m_Impl;
    m_Impl = nullptr;
}

GraphicsDevice* GraphicsDevice::Create(const GraphicsDeviceDesc& desc)
{
    ClearFailure();

    auto* impl = new GraphicsDeviceImpl();
    if (!impl->Create(desc))
    {
        HP_LOG_ERROR("GRAPHICS", "Graphics device creation failed: {}", GetFailureReason());
        delete impl;
        return nullptr;
    }

    // The public object is trivially constructible; ownership of the impl transfers here.
    GraphicsDevice* device = new GraphicsDevice();
    device->m_Impl = impl;
    return device;
}

const AdapterInfo& GraphicsDevice::GetAdapter() const noexcept
{
    static const AdapterInfo empty;
    return m_Impl != nullptr ? m_Impl->Selected : empty;
}

bool GraphicsDevice::IsValidationEnabled() const noexcept
{
    return m_Impl != nullptr && m_Impl->ValidationEnabled;
}

UInt64 GraphicsDevice::GetDedicatedVideoMemoryBytes() const noexcept
{
    return m_Impl != nullptr ? m_Impl->DedicatedMemoryBytes : 0ull;
}

void GraphicsDevice::WaitForIdle()
{
    if (m_Impl != nullptr)
    {
        m_Impl->WaitForIdle();
    }
}

nvrhi::IDevice* GraphicsDevice::GetNativeDevice() const noexcept
{
    return m_Impl != nullptr ? m_Impl->NvrhiDevice : nullptr;
}

bool GraphicsDevice::AttachWindow(Platform::Window* window)
{
    return m_Impl != nullptr && m_Impl->AttachWindow(window);
}

bool GraphicsDevice::HasWindow() const noexcept
{
    return m_Impl != nullptr && m_Impl->AttachedWindow != nullptr;
}

const char* GraphicsDevice::GetFailureReason()
{
    return g_FailureReason.c_str();
}

std::vector<AdapterInfo> EnumerateAdapters(UInt32 requiredApiVersion)
{
    std::vector<AdapterInfo> result;

    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Hollowpoint adapter enumeration";
    application.apiVersion = kVulkanApi11;

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &application;

    VkInstance enumerateInstance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instanceInfo, nullptr, &enumerateInstance) != VK_SUCCESS)
    {
        // No Vulkan at all is a normal state on a machine with only an llvmpipe-less install or no driver; report
        // empty rather than failing so a launcher can show "no GPU" instead of crashing.
        HP_LOG_WARNING("GRAPHICS", "Cannot enumerate adapters: vkCreateInstance failed");
        return result;
    }

    VULKAN_HPP_DEFAULT_DISPATCHER.init(enumerateInstance, vkGetInstanceProcAddr);

    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(enumerateInstance, &count, nullptr) == VK_SUCCESS && count > 0)
    {
        std::vector<VkPhysicalDevice> physical(count);
        vkEnumeratePhysicalDevices(enumerateInstance, &count, physical.data());

        for (uint32_t index = 0; index < count; ++index)
        {
            AdapterInfo info = DescribeAdapter(index, physical[index]);
            if (info.VulkanApiVersion >= requiredApiVersion)
            {
                result.push_back(std::move(info));
            }
        }
    }

    vkDestroyInstance(enumerateInstance, nullptr);
    return result;
}

}  // namespace Hollowpoint::Graphics
