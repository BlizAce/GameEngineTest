#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/RefCounted.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/ECS/Entity.h"
#include "Hollowpoint/Editor/EditorPanels.h"
#include "Hollowpoint/Graphics/GraphicsApi.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Renderer/Camera.h"
#include "Hollowpoint/Runtime/ApplicationHost.h"
#include "Hollowpoint/Runtime/RuntimeApplication.h"

// ---------------------------------------------------------------------------
// EditorApplication — the editor: it hosts RuntimeApplication and owns the UI.
//
// This is what makes "the editor embeds the engine" true rather than aspirational. The class implements
// Runtime::IApplicationHost, so the loop that runs a shipped game is the loop running here — same frame order, same
// renderer, same swapchain code — and the editor adds only what an authoring tool needs: input routed to widgets, a
// camera override for the viewport, scripts paused in edit mode, and a composed frame where the scene occupies a docked
// panel rather than the whole window.
//
// Frame ownership, stated once because it is what a later change is most likely to break:
//   RuntimeApplication::RunFrame does input → time → scripts (paused here) → transforms → collect → acquire → [host] →
//   present. Returning true from RenderScenePass means the runtime draws nothing itself and this class owns every pixel
//   of the back buffer — including clearing it, because Vulkan leaves an attachment's contents undefined unless
//   something writes them. The runtime's own pass performs the equivalent clear; Session 15 finding 4 is why the
//   swapchain image can be cleared at all.
//
// Why the scene renders into its own HDR target rather than the back buffer: the viewport is one docked panel whose
// extent differs from the window's, and perspective depends on which aspect you render at. Stretching a window-aspect
// render into a panel gives a squashed view that looks like a projection bug — it is the first thing anyone notices in
// a home-made editor, and it is a sizing decision made two layers away from the projection.
//
// Lifetime: construct with a config, Start(), Run() or step Frame() manually, then destroy. The hosted application is
// owned here and the host pointer it receives points back at this object; member order below makes that ordering true
// rather than likely (m_Application is declared before m_ImGuiContext/m_Ui so it is destroyed after them... see the
// note at its declaration for why the detach in the destructor is what actually closes the hole).
//
// Threading: single-threaded, like everything that touches GLFW or NVRHI.
// ---------------------------------------------------------------------------

struct ImGuiContext;

namespace Hollowpoint::Editor
{

using Core::Index;
using Core::Ref;
using Core::Size;
using Core::UInt32;
using Core::UInt64;
using Core::UInt8;
using ECS::Entity;
using ECS::World;
using Math::Float32;

//! Editor configuration — a value aggregate for the same reason RuntimeConfig is: tests set these programmatically and
//! a
//  project file or command line writes the same fields rather than a parallel representation.
struct EditorConfig
{
    std::string ApplicationName = "Hollowpoint Editor";

    UInt32 Width = 1600;
    UInt32 Height = 900;

    //! Project root: the content browser's root, and what `content.list`/`content.open` are relative to. Empty means
    //! the
    //  working directory — right for a developer running from a checkout, wrong for a packaged launcher, hence a field.
    std::string ProjectDirectory;

    //! Scene opened at startup. Empty starts an empty world, which is what "no scene" should mean and lets the editor
    //  launch on a machine with no content at all.
    std::string ScenePath;

    bool StartVisible = true;

    //! Vulkan validation layers, forwarded to the hosted runtime's config. Default follows the build type exactly as
    //  RuntimeConfig does — a debug editor run is where a bad pipeline state deserves the only explanation it will get.
    bool EnableValidation =
#if !defined(NDEBUG)
        true
#else
        false
#endif
        ;

    //! Ticks Lua gameplay scripts every frame (UE's "Play"). Default off: an editor is in edit mode, where scripts move
    //  things the author is placing. The VM still exists so `script.run` works on demand — this only gates the tick.
    bool PlayModeScripts = false;

    //! Frames to run before exiting. The automated smoke-test path: proves the editor starts, composes a frame and
    //! shuts
    //  down with no interaction. Zero runs until the window closes.
    UInt64 MaxFrameCount = 0;

    //! Writes a PNG of the composed frame on the way out (after MaxFrameCount or a window close). Empty for none.
    std::string ScreenshotPath;

    //! Clear colour for pixels no panel or geometry covers, linear RGB. Mid grey rather than black so an unwritten
    //! region
    //  is obvious in a screenshot instead of reading as "the void" — the reasoning RuntimeConfig gives for its own.
    float BackgroundRed = 0.07f;
    float BackgroundGreen = 0.08f;
    float BackgroundBlue = 0.10f;

    //! Exposure applied between the viewport's HDR pass and its display transform, matching the runtime's field so a
    //  scene looks identical in the editor and in a standalone run at default settings.
    float Exposure = 1.0f;
};

//! Why Run() returned. The runtime has an equivalent for the loop it owns; this one adds editor causes and keeps "the
//  editor failed to start" distinguishable from "the renderer failed", which need different exit codes.
enum class EditorStopReason : UInt8
{
    Running,
    WindowClosed,
    FrameLimit,
    StartupFailed,
};

[[nodiscard]] HP_EDITOR_API const char* EditorStopReasonName(EditorStopReason reason) noexcept;

//! The editor application.
class HP_EDITOR_API EditorApplication : public Runtime::IApplicationHost
{
public:
    explicit EditorApplication(EditorConfig config = {});
    ~EditorApplication() override;

    EditorApplication(const EditorApplication&) = delete;
    EditorApplication& operator=(const EditorApplication&) = delete;

    //! Creates the hosted runtime (window, device, swapchain, renderer), the ImGui context, the UI renderer and the
    //  panels. Returns false with the reason logged when any of those cannot be created.
    bool Start();

    //! Runs frames until something stops it. Returns why.
    EditorStopReason Run();

    //! One frame. Public so a test can drive exactly N frames and assert on state without a thread or a timeout — the
    //  same reason RuntimeApplication::RunFrame is public.
    bool Frame();

    void RequestStop(EditorStopReason reason = EditorStopReason::WindowClosed) noexcept;

    [[nodiscard]] EditorStopReason GetStopReason() const noexcept
    {
        return m_StopReason;
    }

    [[nodiscard]] bool IsRunning() const noexcept
    {
        return m_Started;
    }

    // --- editing operations -----------------------------------------------------------
    // Each of these has exactly one implementation, called by both the panel widget and the console command that offers
    // the same action. Two implementations of one action is how "the button saves, the command doesn't" happens.

    [[nodiscard]] World& GetWorld() noexcept;

    [[nodiscard]] Entity GetSelectedEntity() const noexcept;
    [[nodiscard]] EditorSelection& GetSelection() noexcept;

    //! Selects a live entity by world slot. Returns false when the slot is dead or out of range — reported rather than
    //  ignored, because "the command did nothing" is indistinguishable from a broken command.
    bool SelectBySlot(Index slot);

    //! Creates an entity with a transform and an EntityName (plus a MeshRenderer when `meshPath` is non-empty) and
    //  selects it. Returns a null handle on failure.
    [[nodiscard]] Entity CreateEntity(const std::string& name, const std::string& meshPath);

    //! Destroys every selected entity and clears the selection.
    void DestroySelected();

    bool LoadProjectScene(const std::string& path);
    bool SaveProjectScene(const std::string& path);
    [[nodiscard]] const std::string& GetScenePath() const noexcept
    {
        return m_ActiveScenePath;
    }

    //! Opens a project file by kind: .hpscene loads as a scene, .lua runs as a script, anything else reports "no
    //  importer" and returns false. Reported rather than silent — a content browser that ignores half its files is
    //  worse than one that says what it cannot do yet (notes/EDITOR.md §1, the asset pipeline).
    bool OpenProjectFile(const std::filesystem::path& path);

    //! Runs a Lua file through the hosted loop's VM. Absolute paths pass through; relative names resolve against the
    //  runtime's script directory, which is the rule RuntimeApplication already documents — `script.run` must not
    //  invent a second, different resolution rule for the same file.
    bool RunScript(const std::string& path);

    //! Writes a PNG of what the editor displays: background, scene-in-panel and every widget, exactly as composited.
    /**
     * Not RuntimeApplication::CaptureScreenshot, which re-runs the tone map into its own target and therefore captures
     * only the scene pass — for an editor that is the one thing a screenshot has to show beyond the game view, and a
     * "screenshot of the editor" that contains no panels has passed every pixel assertion while proving nothing about
     * whether the UI drew. This re-renders the current ImGui draw data into an offscreen target of the back buffer's
     * extent (the data is valid until the next NewFrame) and reads that back.
     *
     * Fails, rather than writing a blank image, when no frame has been composed yet: an empty PNG from a mistyped
     * `--screenshot` reads like a rendering bug instead of the ordering mistake it is.
     */
    bool CaptureComposedFrame(const std::string& path);

    //! The composed frame as 8-bit RGBA, row-major, no padding — the same pixels `CaptureComposedFrame` writes to a
    //! PNG.
    /**
     * Exists because verifying a rendered UI needs the pixels, and the engine has no PNG *decoder* (only stb's writer
     * is compiled in). Going through a file would mean either adding a decoder dependency or comparing bytes of an
     * encoded stream, which changes when the encoder does. Reading the capture target back directly is exact, cheaper,
     * and usable from a test with no filesystem involved — which is what makes "did the editor actually draw its
     * panels" an assertion rather than a screenshot someone eyeballs.
     *
     * Same gating as the file version: fails when no frame has been composed yet.
     */
    bool CaptureComposedRgba8(std::vector<unsigned char>& outRgba, UInt32& outWidth, UInt32& outHeight);

    void SetViewportCamera(const Math::Vec3& eye, const Math::Vec3& target) noexcept;

    //! Selects whatever is under a viewport-image pixel (origin at the image's top-left, content units). Returns the
    //! hit
    //  entity or a null handle when nothing was hit.
    [[nodiscard]] Entity PickAtViewportPixel(float x, float y);

    [[nodiscard]] UInt64 GetFrameCount() const noexcept;

    //! Runs one console command line after startup, before the loop begins. Used by `--command` so an automated run can
    //  compose a frame containing specific actions; the reply/error lands in the console transcript like any other
    //  route.
    bool RunStartupCommand(const std::string& line);

    [[nodiscard]] ImGuiContext* GetUiContext() const noexcept
    {
        return m_ImGuiContext;
    }

    // --- IApplicationHost --------------------------------------------------------------

    bool ConsumeInputEvent(const Platform::InputEvent& event) override;
    bool GetCameraOverride(Renderer::CameraState& outCamera) const override;
    bool AreScriptsPaused() const override;
    bool RenderScenePass(const Runtime::FrameContext& context) override;
    void OnResize(UInt32 framebufferWidth, UInt32 framebufferHeight) override;

private:
    [[nodiscard]] bool EnsureViewportTargets(UInt32 width, UInt32 height);
    void DestroyViewportTargets() noexcept;

    //! Offscreen RGBA8 target for CaptureComposedFrame, sized to the window's back buffer.
    [[nodiscard]] bool EnsureCaptureTargets(UInt32 width, UInt32 height);

    //! Re-renders the last ImGui frame (plus background) into the capture target. Shared by both capture entry points
    //! so
    //  a PNG and an in-memory readback can never disagree about what "the composed frame" means.
    [[nodiscard]] bool RenderIntoCaptureTarget();

    //! The viewport's camera as a CameraComponent rather than loose fields: ComputeCameraState takes that type, and
    //  storing the same shape avoids a conversion whose defaults could drift from the component's.
    [[nodiscard]] Renderer::CameraState BuildViewportCamera(Float32 aspectRatio) const;

    EditorConfig m_Config;

    //! Owned hosted loop.
    std::unique_ptr<Runtime::RuntimeApplication> m_Application;

    //! True once SetEditorHost(this) ran, so the destructor detaches before the rest of teardown. The ordering hazard
    //! is
    //  concrete: RuntimeApplication's destructor tears down renderer and swapchain, and any path in that teardown that
    //  reached a host hook would be reading a half-destroyed editor. Explicit detach makes it impossible rather than
    //  merely unlikely.
    bool m_AttachedHost = false;

    ImGuiContext* m_ImGuiContext = nullptr;  //!< Owned; released with ImGui::DestroyContext here.

    Ref<class UiRenderer> m_Ui;

    std::unique_ptr<UiState> m_State;

    // Viewport targets: scene → HDR (Rgba16Float + depth) → tone-mapped Rgba8 that widgets sample.
    Ref<Graphics::Texture> m_ViewportHdrColour;
    Ref<Graphics::Texture> m_ViewportDepth;
    Ref<Graphics::RenderTarget> m_ViewportHdrTarget;
    Ref<Graphics::Texture> m_ViewportDisplayColour;
    Ref<Graphics::RenderTarget> m_ViewportDisplayTarget;

    //! ImGui texture id for the display target, or 0 before the first composed frame.
    UInt64 m_ViewportTextureId = 0;

    UInt32 m_ViewportWidth = 0;
    UInt32 m_ViewportHeight = 0;

    //! The viewport's fly camera. Default looks at the origin from a three-quarter angle, which frames the built-in
    //! demo
    //  scene on first launch — an editor that opens inside a cube wall reads as broken on every screenshot test.
    Renderer::CameraComponent m_Camera;

    std::string m_ActiveScenePath;

    //! Focus tracked from Focus events for FeedImGuiFrameState; the window starts focused because Start() maps it.
    bool m_WindowFocused = true;

    //! Events seen since the last NewFrame. The runtime hands them over one at a time and ImGui wants a frame's worth
    //  before NewFrame, so they queue here and are drained in RenderScenePass — which is also what makes "events this
    //  frame" a number `editor.input` can report.
    std::vector<Platform::InputEvent> m_PendingUiEvents;

    EditorStopReason m_StopReason = EditorStopReason::Running;
    bool m_Started = false;

    //! Capture target for `screenshot` / `--screenshot`: the composed frame, not the scene pass.
    Ref<Graphics::Texture> m_CaptureColour;
    Ref<Graphics::RenderTarget> m_CaptureTarget;
    UInt32 m_CaptureWidth = 0;
    UInt32 m_CaptureHeight = 0;

    //! Extent of the last back buffer drawn into, so a capture matches what was on screen rather than the requested
    //  window size — which differs from it on HiDPI displays and during resize.
    UInt32 m_LastBackBufferWidth = 0;
    UInt32 m_LastBackBufferHeight = 0;

    //! True once RenderScenePass has composed at least one frame. Gates capture: ImGui's draw data before the first
    //  NewFrame/Render pair is empty, and capturing then would write a background-coloured PNG that looks like a render
    //  failure.
    bool m_HasComposedFrame = false;

    //! Set once when the UI renderer fails, so a persistent failure logs one line rather than flooding the log — and is
    //  still discoverable via `editor.info`.
    bool m_UiFailureReported = false;
};

}  // namespace Hollowpoint::Editor
