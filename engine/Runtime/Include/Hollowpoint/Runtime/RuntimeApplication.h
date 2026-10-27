#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/RefCounted.h"
#include "Hollowpoint/Core/Time.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/ECS/World.h"
#include "Hollowpoint/Game/RenderPrimitives.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Platform/InputEvent.h"
#include "Hollowpoint/Runtime/ApplicationHost.h"
#include "Hollowpoint/Runtime/RuntimeConfig.h"
#include "Hollowpoint/Scene/TransformHierarchy.h"

// ---------------------------------------------------------------------------
// RuntimeApplication — the engine's application shell and frame loop.
//
// This is what makes Hollowpoint run. Everything below it has existed for a while: a device that can create resources,
// a renderer whose shaders match a CPU reference, a swapchain that presents to a real compositor, a frame ring that
// bounds in-flight frames, a scene loader, a scripting VM with entity handles. None of it had an owner —
// HP_BUILD_RUNTIME built nothing, and the only code that ever submitted a frame lived inside a test file. The
// consequence was visible in the headers: FrameRing documented itself as having "tests and no production caller", i.e.
// the thing preventing frame N+2 from overwriting in-flight data was correct machinery nobody used. This class is the
// user of all of it, and therefore the point where those individually-tested pieces have to agree for real.
//
// Frame order (fixed, because every step below depends on the one before producing its input):
//
//   1. Poll input            — window events, including the resize that steps 6 and 7 act on.
//   2. Tick time             — one clock reading per frame, before any system reads a delta (TimeSystem's contract).
//   3. Update scripts        — gameplay may move entities, so it runs BEFORE transforms resolve; after would render
//   last
//                              frame's positions.
//   4. Resolve transforms    — parent chains composed once, feeding both collection and the camera.
//   5. Collect renderables   — scene → RenderPlan (Game's bridge), plus the camera state for this window's aspect.
//   6. Acquire + render      — forward pass into an HDR target, tone map into the acquired back buffer.
//   7. Present + end frame   — present, then FrameRing::EndFrame, which arms the completion query. EndFrame is last
//                              because completion covers everything queued at that moment: ending earlier reports a
//                              frame finished while its draws were still in the queue (documented on
//                              FrameRing::EndFrame).
//
// What it deliberately does NOT own:
//   * An asset database. Meshes come from BuiltInMeshes (procedural primitives by path) behind Game's GeometryResolver
//     seam; a glTF/texture loader plugs in at that one function pointer without touching this class. That is the
//     remaining gap between "the engine runs" and "the engine loads your game", and it is documented as such rather
//     than papered over.
//   * Physics and audio. Neither module exists yet (see docs/ARCHITECTURE.md), so there is no step 3.5 to insert; where
//     they go in this order is stated here so adding them is not a guess.
//   * An editor. The runtime is embeddable-by-design: RunFrame() is public and Start()/Stop() are separable, which is
//   what
//     lets the editor host this loop rather than duplicate it.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Graphics
{
class FrameRing;
class GraphicsDevice;
class RenderTarget;
class Swapchain;
class Texture;
}  // namespace Hollowpoint::Graphics

namespace Hollowpoint::Platform
{
class PlatformScope;
class Window;
}  // namespace Hollowpoint::Platform

namespace Hollowpoint::Scripting
{
class ScriptSystem;
class ScriptVM;
}  // namespace Hollowpoint::Scripting

namespace Hollowpoint::Game
{
class SceneRenderer;
}  // namespace Hollowpoint::Game

namespace Hollowpoint::Runtime
{

using Core::Ref;
using Core::Size;
using Core::UInt32;
using Core::UInt64;
using Core::UInt8;

//! Why Run() stopped. Reported rather than assumed by the caller, because "the user closed the window", "the frame
//! count limit was reached" and "startup failed" need different exit codes and different messages in a log.
enum class StopReason : UInt8
{
    Running,        //!< Only observable while inside the loop.
    WindowClosed,   //!< The user asked to close, or the platform destroyed the window.
    FrameLimit,     //!< MaxFrameCount was reached — an automated run finishing normally.
    DeviceLost,     //!< The GPU went away; continuing would produce nothing but errors.
    StartupFailed,  //!< A required subsystem (device, renderer) could not be created.
};

//! Human-readable name for logs and exit messages. Never null.
[[nodiscard]] const char* StopReasonName(StopReason reason) noexcept;

//! Owns the window, device, swapchain, renderer, world and script VM, and runs frames against them.
/**
 * Lifetime: construct, Start(), Run() or a series of RunFrame() calls, then let it go. Destruction order is the reverse
 * of creation and matters (the renderer holds pipelines referencing render targets, which hold textures, which need the
 * device alive), so members are declared in teardown order in the .cpp and nothing external may outlive this object
 * holding a Ref to one of them.
 *
 * Threading: single-threaded, on whichever thread calls Start(). GLFW requires that window creation, polling and
 * destruction all happen on the thread that initialised it (checked by Platform), and NVRHI's command lists are not
 * thread-safe either. A render thread would be an explicit later change with a queue between this loop and submission.
 */
class HP_RUNTIME_API RuntimeApplication
{
public:
    explicit RuntimeApplication(RuntimeConfig config = {});
    ~RuntimeApplication();

    RuntimeApplication(const RuntimeApplication&) = delete;
    RuntimeApplication& operator=(const RuntimeApplication&) = delete;

    //! Creates window, device, swapchain, renderer and (if enabled) scripts. Returns false with the reason logged when
    //! a required subsystem is unavailable — expected failures being "no display", "no Vulkan driver" and "could not
    //! present". Idempotent: calling it twice returns the first result rather than creating a second window.
    bool Start();

    //! True once Start() succeeded. Rendering calls refuse without it.
    [[nodiscard]] bool IsRunning() const noexcept;

    //! Runs one frame: everything in the order documented at the top of this file. Returns false when the application
    //! should stop (see GetStopReason for why). Public so an editor can drive the loop itself, and so a test can render
    //! exactly N frames and assert on what happened without a thread or a timeout.
    bool RunFrame();

    //! Loops RunFrame() until it returns false or MaxFrameCount is reached. Returns why it stopped.
    StopReason Run();

    //! Requests that Run() return after the current frame. Safe to call from a script (via hp.quit, if one is added) or
    //! from an embedding host's own UI action.
    void RequestStop(StopReason reason = StopReason::WindowClosed) noexcept;

    [[nodiscard]] StopReason GetStopReason() const noexcept;

    // --- accessors for an embedding host (an editor) and for tests ------------------

    [[nodiscard]] ECS::World& GetWorld() noexcept;
    [[nodiscard]] const ECS::World& GetWorld() const noexcept;

    [[nodiscard]] Graphics::GraphicsDevice* GetDevice() const noexcept;
    [[nodiscard]] Platform::Window* GetWindow() const noexcept;
    [[nodiscard]] Game::SceneRenderer* GetRenderer() const noexcept;

    //! Frames completed since Start(). Distinguishes "the loop ran and drew nothing" from "the loop never ran", which
    //! is the difference between a scene bug and a wiring bug when someone reports a black window.
    [[nodiscard]] UInt64 GetFrameCount() const noexcept;

    //! The render plan produced by the most recent UpdateAndCollect, valid until the next frame updates it — i.e.
    //! exactly the window an `IApplicationHost::RenderScenePass` implementation runs in. A host that takes the frame
    //! over must render *this* plan rather than re-collecting from the world: collection resolved the camera override
    //! and the transform pass, and re-doing either would diverge from what a standalone run shows. Until hosts existed
    //! the plan had no external consumer and stayed private; exposing it (read-only) is what makes "the host owns
    //! presentation" implementable without duplicating collection.
    [[nodiscard]] const Game::RenderPlan& GetLastRenderPlan() const noexcept;

    //! Current swapchain extent in framebuffer pixels, or 0/0 before the swapchain exists. A host allocating its own
    //! HDR target sizes to this (the same value the runtime's internal target uses), so a takeover frame matches a
    //! native one pixel for pixel.
    void GetSwapchainExtent(UInt32& outWidth, UInt32& outHeight) const noexcept;

    //! Indexed draws submitted across all frames, forwarded from the renderer. Zero with a non-zero frame count means
    //! the scene collected nothing — the single most common cause of a blank window.
    [[nodiscard]] UInt64 GetDrawCount() const noexcept;

    //! Loads and instantiates a .hpscene into the world. Returns false when the file cannot be read or parsed, with the
    //! report logged; on failure the world is left as it was (the loader builds into its own entity list). Called by
    //! Start() for Config.ScenePath and available at runtime for a load-scene command.
    bool LoadScene(const std::string& path);

    //! Runs one Lua file from Config.ScriptDirectory, binding any table of the same name it defines. Returns false when
    //! the file is missing or errors — reported, never fatal, since scripts are content.
    bool RunScriptFile(const std::string& filename);

    //! Captures what the current frame displays and writes it to `path` as a PNG. Returns false with the reason logged.
    /**
     * Implemented by tone-mapping the HDR target into a private RGBA8 texture and reading that back — NOT by reading
     * the swapchain image that was just presented. A presented image belongs to the compositor: its layout is
     * PRESENT_SRC_KHR until the next acquire, and transitioning it to a copy source to read it is undefined behaviour
     * that happens to work on some drivers and produce black rectangles on others. Re-running the same blit into our
     * own target yields pixel-identical output (same shader, same exposure, same extents) with no compositor involved —
     * and works with no window at all.
     *
     * Blocks on the GPU by design; call once a frame is complete, not every frame.
     */
    bool CaptureScreenshot(const std::string& path);

    [[nodiscard]] const RuntimeConfig& GetConfig() const noexcept;

    // -----------------------------------------------------------------------
    // Editor embedding — see ApplicationHost.h for why each hook exists.
    // -----------------------------------------------------------------------

    //! Attaches an embedding host (the editor). Call before Start(); the host is then consulted every frame at the
    //! points documented on RuntimeApplication's frame steps. Passing nullptr detaches and the loop reverts to plain
    //! standalone behaviour. Not owned: the host must outlive this application — which it does by construction, since
    //! the editor owns both.
    void SetEditorHost(IApplicationHost* host) noexcept
    {
        m_Host = host;
    }

    [[nodiscard]] IApplicationHost* GetEditorHost() const noexcept
    {
        return m_Host;
    }

    //! Events drained this frame that the host did not consume, in poll order. Empty without a host (everything
    //! survives) or after one consumed them all. This is what a future action map / gameplay input layer will eat;
    //! until then it exists so tests can assert "the host saw exactly these events and dropped exactly those".
    [[nodiscard]] const std::vector<Platform::InputEvent>& GetUnconsumedEvents() const noexcept
    {
        return m_HostEvents;
    }

private:
    //! The private tone-map target CaptureScreenshot renders into, created on first use at the current swapchain
    //! extent.
    [[nodiscard]] bool EnsureCaptureTarget();
    void DestroyCaptureTarget() noexcept;

    [[nodiscard]] bool StartWindowAndDevice();
    [[nodiscard]] bool CreateSwapchain(UInt32 width, UInt32 height);
    [[nodiscard]] bool EnsureHdrTarget(UInt32 width, UInt32 height);
    void DestroyHdrTarget() noexcept;

    //! Rebuilds the swapchain and HDR target after a resize. Returns false when recreation failed, in which case the
    //! loop stops rather than spinning on a chain that cannot present.
    [[nodiscard]] bool HandleResize(UInt32 framebufferWidth, UInt32 framebufferHeight);

    //! Steps 3-5 of the frame: scripts, transforms, and the render plan for this window's aspect ratio.
    void UpdateAndCollect();

    //! Creates the VM, registers the engine API and loads `scripts/init.lua` if present. Returns false when Lua itself
    //! could not start; a missing init script is not a failure.
    [[nodiscard]] bool InitialiseScripts();

    //! Binds every distinct ScriptComponent::ScriptName in the world. Called after scene load and after running a
    //! script file, because a component holds only a name — binding is what makes it tick (see ScriptSystem's header).
    void BindScriptsInWorld();

    //! Records one frame that was entered but produced no presented image (`reason` names why for the log), and reports
    //! whether the loop may continue. Returns false once too many have been skipped in a row, having set the stop
    //! reason.
    /**
     * Why this exists: frames that skip (no extent yet, acquire timeout, a rebuild pending) deliberately do not count
     * toward MaxFrameCount — only presented frames should satisfy a run's frame budget, or `--frames 5` against a
     * minimised window would exit having drawn nothing and report success. But an uncounted skip with no other bound is
     * an infinite loop in exactly the automated invocation used to check the engine headlessly. So skips are counted
     * separately and capped: a transient condition resolves, a permanent one stops the run loudly instead of hanging
     * it.
     */
    bool NoteSkippedFrame(const char* reason);

    // Config first: everything below reads it during Start().
    RuntimeConfig m_Config;

    //! The embedding host (the editor), or null for standalone operation. Never owned — see SetEditorHost. Placed
    //! before the subsystems it observes so nothing can consult a half-constructed loop.
    IApplicationHost* m_Host = nullptr;  // not owned

    // Owned, in CONSTRUCTION order. Teardown is the reverse, which is what keeps the renderer's pipelines from
    // outliving the render targets they were created against and those from outliving the device.
    Platform::PlatformScope* m_Platform =
        nullptr;  //!< not owned by RefCounted: a scope, held raw by design (see its header)
    Platform::Window* m_Window = nullptr;
    Graphics::GraphicsDevice* m_Device = nullptr;
    Ref<Graphics::Swapchain> m_Swapchain;
    Ref<Graphics::FrameRing> m_FrameRing;
    Ref<Game::SceneRenderer> m_Renderer;

    // HDR scene target, recreated on resize. Sampled by the tone-map blit, so it is a separate texture from any back
    // buffer.
    Ref<Graphics::Texture> m_HdrColour;
    Ref<Graphics::Texture> m_HdrDepth;
    Ref<Graphics::RenderTarget> m_HdrTarget;

    Core::TimeSystem m_Time;
    ECS::World m_World;
    Scene::TransformHierarchy m_Transforms;

    // Scripting, created only when Config.EnableScripting. Held by unique_ptr so this header needs no Lua or sol2
    // includes — the same reason ScriptSystem.h forward-declares ScriptVM.
    std::unique_ptr<Scripting::ScriptVM> m_VM;
    std::unique_ptr<Scripting::ScriptSystem> m_Scripts;

    // Per-frame state, produced by UpdateAndCollect and consumed by the render step. Kept as members so a steady-state
    // frame allocates nothing: RenderPlan's vectors keep their capacity between frames, which for a scene with hundreds
    // of draw items is the difference between "input and collection are free" and "the collector hits the allocator
    // every frame" (the same reasoning Platform/Window.h gives for its drain-into-a-caller-vector event API).
    Game::RenderPlan m_Plan;

    //! Entity lists backing one collection pass, rebuilt per frame from the world. Separate from the plan because
    //! CollectInput takes raw arrays whose lifetime the caller owns.
    std::vector<ECS::Entity> m_MeshEntities;
    std::vector<Math::Mat4> m_MeshTransforms;
    std::vector<ECS::Entity> m_LightEntities;
    std::vector<Math::Mat4> m_LightTransforms;

    //! Scratch buffer for drained window events, reused across frames for the same reason as above.
    std::vector<Platform::InputEvent> m_Events;

    UInt64 m_Frames = 0;
    StopReason m_StopReason = StopReason::Running;
    bool m_Started = false;

    //! Set when a resize arrives mid-frame, i.e. between acquire and present. Vulkan says the presentation will come
    //! back out of date; remembering it lets the loop rebuild once rather than treating the resulting error as fatal.
    bool m_ResizePending = false;
    UInt32 m_PendingWidth = 0;
    UInt32 m_PendingHeight = 0;

    //! Events drained this frame that no host consumed, kept until something else eats input (see RunFrame step 1).
    //! Today nothing iterates it beyond tests; a future action map reads it instead of m_Events.
    std::vector<Platform::InputEvent> m_HostEvents;

    //! The private tone-map target CaptureScreenshot renders into, recreated when the extent changes. Held separately
    //! from the HDR target because it is 8-bit display-referred output: allocating it up front would cost a texture per
    //! run for an operation that happens at most once.
    Ref<Graphics::Texture> m_CaptureColour;
    Ref<Graphics::RenderTarget> m_CaptureTarget;

    //! Consecutive frames entered without presenting, reset by every presented frame. See NoteSkippedFrame.
    UInt64 m_ConsecutiveSkippedFrames = 0;

    //! Last size the swapchain was created at, so NeedsRebuild has something to compare against without asking the
    //! window (which reports content size, not framebuffer pixels — a different number on HiDPI).
    UInt32 m_SwapchainWidth = 0;
    UInt32 m_SwapchainHeight = 0;
};

}  // namespace Hollowpoint::Runtime
