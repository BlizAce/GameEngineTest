#pragma once

#include <string>

#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/Graphics/Swapchain.h"

// ---------------------------------------------------------------------------
// Runtime configuration: everything the application shell needs that a user might want to change.
//
// Why a plain struct with defaults rather than a parsed-file type: the same values are needed by tests (which must set
// them programmatically, without writing a config file), by command-line overrides, and eventually by a project
// settings file. Keeping it a value aggregate means all three write to one place. Parsing lives in
// RuntimeConfigFromJsonText so the struct never grows a dependency on a document type.
//
// Frame slots are here rather than hardcoded because they must agree between two objects that are constructed apart:
// the Graphics::FrameRing and Game::SceneRenderer. A mismatch is not a crash — it is frame N+2 overwriting data frame
// N's draws are still reading, which presents as occasional flicker with nothing in a log. RuntimeApplication therefore
// asserts the renderer was created with at least as many slots as the ring has (see its Start), and this value being
// one number keeps them equal by construction.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Runtime
{

using Core::UInt32;
using Core::UInt64;

//! How long to run before exiting on its own. Zero means "run until the window closes" — the normal case.
constexpr double kRunUntilNoFrames = 0.0;

struct RuntimeConfig
{
    std::string ApplicationName = "Hollowpoint";

    //! Window content size (not framebuffer pixels: on a 2× display the framebuffer is twice this).
    UInt32 Width = 1280;
    UInt32 Height = 720;

    bool Resizable = true;

    //! Starts hidden. Used by automated runs and by an agent-driven editor that should not steal the screen.
    bool StartVisible = true;

    //! Vulkan validation layers. Default follows the build type (on in debug), matching GraphicsDeviceDesc — a shipping
    //! build must not depend on layers being installed, and a debug build without them loses the only explanation of a
    //! failed pipeline creation this machine can give.
    bool EnableValidation =
#if !defined(NDEBUG)
        true
#else
        false
#endif
        ;

    //! Presentation mode preference; degrades to what the compositor offers (see Graphics/SelectPresentMode).
    Graphics::PresentMode PresentMode = Graphics::PresentMode::VSync;

    //! Frames allowed in flight at once. 3 is the engine default: CPU runs two ahead of the GPU while vsync paces the
    //! loop. Setting 1 serialises CPU and GPU — correct, slow, and useful for bisecting a race.
    UInt32 FrameSlots = 3;

    //! Light-array bound passed to the scene renderer (see Game::SceneRendererDesc).
    UInt32 MaxLightsPerDraw = 8;

    //! Scene file to load at startup. Empty starts with an empty world, which is valid: a tool or test may build
    //! content programmatically or wait for a script to do it.
    std::string ScenePath;

    //! Lua files loaded after the scene, in order, each into the shared VM. Missing files are reported and skipped
    //! rather than fatal — a project under construction has scripts referenced before they are written, and refusing to
    //! start is the wrong response to a typo in a config file.
    std::string ScriptDirectory = "scripts";

    //! Exposure applied between the HDR pass and the display transform, in linear stops (see
    //! ColorPipeline::ApplyExposure). 1.0 is neutral; this is the value an auto-exposure pass would later drive.
    float Exposure = 1.0f;

    //! Background colour for pixels no geometry covers, as linear RGBA. The scene's own clear colour: a mid grey rather
    //! than black so an unwritten region is obvious in a screenshot instead of reading as "the void".
    float BackgroundRed = 0.18f;
    float BackgroundGreen = 0.20f;
    float BackgroundBlue = 0.24f;

    //! Stop after this many frames. 0 runs until the window closes. Non-zero exists because an automated run needs a
    //! bounded loop to screenshot and assert against, and "kill it after N seconds" is not a contract.
    UInt64 MaxFrameCount = 0;

    //! Seconds to block waiting for input when the window is unfocused or iconified, instead of rendering into nothing
    //! at full CPU. Zero polls continuously.
    double IdleWaitSeconds = 0.0;

    //! Loads scripts on startup. False is what a render-only smoke test wants: no Lua VM, no script errors to
    //! distinguish from engine errors.
    bool EnableScripting = true;

    //! Where to write a PNG of the frame before exiting, or empty for no capture. Set by `--screenshot <path>`.
    //!
    //! Captured on the way out rather than every frame because the operation stalls the pipeline by design: once per
    //! run is a diagnostic, N times per second is a benchmark nobody asked for. It is also what makes an unattended run
    //! reviewable —
    //! `--screenshot out.png --frames 3` produces an artifact a human can open without a display attached.
    std::string ScreenshotPath;
};

}  // namespace Hollowpoint::Runtime
