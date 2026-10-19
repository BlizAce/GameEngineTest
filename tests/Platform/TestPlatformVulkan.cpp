// Vulkan-integration surface of the Platform module, in its own executable.
//
// Why this is not part of PlatformTests: querying GLFW's required Vulkan instance extensions makes GLFW
// initialise the Vulkan loader, which loads the GPU driver. On this machine the NVIDIA driver opens a session
// D-Bus connection during that path and retains its message-loader buffers for the process lifetime (~2.1 KB in 3
// allocations; every frame of the stack is inside libdbus-1 / libvulkan / the vendor module, none in engine code).
// No client-callable teardown exists, so LeakSanitizer reports it at exit.
//
// LSan reports per *process*, so one such call would force suppressions onto every windowing test and blunt
// engine-side leak checking for all of them. Isolating the Vulkan-touching assertions here keeps PlatformTests
// running fully unsuppressed while still covering this API. The suppression file names only system libraries, so a
// genuine Hollowpoint leak would still be reported in this binary too.

#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Platform/PlatformInit.h"
#include "Hollowpoint/Platform/Window.h"

using namespace Hollowpoint::Platform;

namespace
{

//! One process-wide scope, forced to the Null backend so no window ever maps and no compositor is involved.
PlatformScope& GetNullScope()
{
    static PlatformScope scope = [] {
        PlatformInitOptions options;
        options.PreferredBackend = WindowingBackend::Null;
        return PlatformScope(options);
    }();

    return scope;
}

//! A window that is never shown. Created invisibly because these tests run on machines where someone is working at
//! the console, and nothing here needs presentation.
class ScopedWindow
{
public:
    ScopedWindow()
    {
        (void)GetNullScope();

        WindowDesc desc;
        desc.Title = "Hollowpoint Vulkan surface probe";
        desc.Visible = false;

        Handle = Window::Create(desc);
        REQUIRE(Handle != nullptr);
    }

    ~ScopedWindow()
    {
        delete Handle;
    }

    ScopedWindow(const ScopedWindow&) = delete;
    ScopedWindow& operator=(const ScopedWindow&) = delete;

    [[nodiscard]] Window* operator->() const noexcept
    {
        return Handle;
    }

private:
    Window* Handle = nullptr;
};

}  // namespace

TEST_SUITE("Platform/VulkanSurface")
{
    TEST_CASE("required Vulkan instance extensions include VK_KHR_surface and no platform-specific one when headless")
    {
        const ScopedWindow window;
        const std::vector<std::string> extensions = window->GetRequiredVulkanInstanceExtensions();

        // VK_KHR_surface is unconditional: nothing presents without it, on any backend including headless. Its
        // absence means GLFW was built without Vulkan support, which would otherwise surface as an opaque
        // vkCreateInstance failure much later in the renderer.
        bool hasSurface = false;
        for (const std::string& extension : extensions)
        {
            if (extension == "VK_KHR_surface")
            {
                hasSurface = true;
            }
        }
        CHECK(hasSurface);

        // Under the headless backend no *platform* surface extension may be requested. Asking a driver for
        // VK_KHR_wayland_surface / _xlib / _xcb / _win32 without the corresponding connection is a classic cause of
        // vkCreateInstance failing inside containers, so the list must stay clean of them.
        if (GetNullScope().IsHeadless())
        {
            for (const std::string& extension : extensions)
            {
                INFO(extension);
                const bool platformSpecific =
                    extension.find("wayland") != std::string::npos || extension.find("xlib") != std::string::npos ||
                    extension.find("xcb") != std::string::npos || extension.find("win32") != std::string::npos ||
                    extension.find("cocoa") != std::string::npos;
                CHECK_FALSE(platformSpecific);
            }
        }
    }

    TEST_CASE("CreateVulkanSurface fails cleanly where no VkInstance exists")
    {
        const ScopedWindow window;

        // A null VkInstance must fail without producing a surface: handing the caller back a garbage handle to pass
        // to vkDestroySurfaceKHR later is a crash that appears far from here.
        void* surface = reinterpret_cast<void*>(0xdeadbeef);
        CHECK_FALSE(window->CreateVulkanSurface(nullptr, &surface));
        CHECK(surface == nullptr);
    }
}
