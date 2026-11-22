#pragma once

#include <string>
#include <vector>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/ECS/Entity.h"
#include "Hollowpoint/ECS/World.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Editor/ContentBrowserModel.h"
#include "Hollowpoint/Editor/EditorConsole.h"
#include "Hollowpoint/Editor/EditorSelection.h"
#include "Hollowpoint/Editor/PropertyView.h"
#include "Hollowpoint/Editor/ViewCamera.h"

// ---------------------------------------------------------------------------
// EditorPanels — the docked UI's state and its drawing code.
//
// Why separate from EditorApplication: the panels are pure functions of (state, context) — they read the world through
// a const pointer and request every engine action through the application — so keeping them in their own translation
// unit makes it impossible for widget code to reach past the interface into the device or the renderer. It also keeps
// the composition frame (EditorApplication::ComposeFrame) short enough to read as the thing it is: feed input, draw
// panels, render UI.
//
// ImGui appears only in the .cpp. That matters for build time more than architecture: imgui.h is not cheap, and a
// header that includes it forces every consumer (including tests of selection/console/content, which need no UI at all)
// to pay.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Editor
{

using Core::Index;
using Core::Size;
using Core::UInt64;
using ECS::Entity;

class EditorApplication;
class EditorConsole;
class EditorIconSet;

//! What the viewport panel observed this frame, and what it is asking the application to do.
/**
 * The split is the panels' central rule made concrete: widgets read input and record *intent*, and only
 * EditorApplication acts on it (moving its camera, picking in its world). That keeps the maths that needs the camera —
 * fly direction, pick ray — next to the camera instead of being duplicated in widget code.
 *
 * Everything here is per-frame scratch filled during DrawViewportPanel and consumed in RenderScenePass before
 * ImGui::Render, with one deliberate exception: ImageWidth/ImageHeight also carry across frames, because the scene
 * target has to be sized from the *previous* frame's measured image area (on frame one ImGui has no layout to measure).
 */
struct HP_EDITOR_API ViewportInteraction
{
    //! True when the pointer was over the viewport image and nothing was on top of it — the gate for every navigation
    //  gesture, so flying never happens while the author is dragging a slider or a menu is open.
    bool PointerOverImage = false;

    //! Top-left of the image on screen, content units: converting pointer position to image-relative pixels needs it,
    //  and picking must use the same rectangle the image was drawn into or clicks land offset.
    float ImageScreenLeft = 0.0f;
    float ImageScreenTop = 0.0f;

    //! Measured display size of the image (content units), from GetContentRegionAvail — never from the target extent, so
    //  a stretched or cropped draw is visible here rather than hidden.
    float ImageWidth = 0.0f;
    float ImageHeight = 0.0f;

    //! Movement intent for this frame. Axes are -1..1 (WASD/QE), pixels are accumulated drag, notches are the wheel.
    ViewAxisInput Fly{};
    Float32 OrbitPixelsX = 0.0f;
    Float32 OrbitPixelsY = 0.0f;
    Float32 PanPixelsX = 0.0f;
    Float32 PanPixelsY = 0.0f;
    Float32 DollyNotches = 0.0f;

    //! True while a right-drag fly or middle-drag orbit owns the pointer (drives the on-image hint line).
    bool Navigating = false;

    //! Set by the F key or `view.frame`: frame the selection, or everything when nothing is selected.
    bool RequestFrameSelection = false;
};

//! One panel's live rectangle, in content units.
/**
 * Why this is data rather than something a test measures from pixels: the pixel check this suite was built on
 * (CountDistinctColours over a fractional region) cannot tell a docked panel from its neighbour, so under a real dock
 * layout it reports "structure present" even when the panel never drew — verified by mutation here, where hiding the
 * outliner entirely left every region assertion green. Geometry is the assertion that can fail; structure on top of it
 * then means something. `layout.report` prints the same numbers, so the console and the tests read one source.
 */
struct HP_EDITOR_API PanelRect
{
    const char* Name = "";
    float X = 0.0f;
    float Y = 0.0f;
    float Width = 0.0f;
    float Height = 0.0f;

    [[nodiscard]] float Area() const noexcept
    {
        return Width > 0.0f && Height > 0.0f ? Width * Height : 0.0f;
    }
};

//! Everything the panels read and mutate that is neither world data nor engine state.
/**
 * Deliberately a plain struct: it is owned by EditorApplication, has no invariants beyond "the ids refer to live
 * resources", and being public lets a test construct one, drive Run() with command lines and assert on the transcript —
 * which is how the console's behaviour is verified without a window.
 */
struct HP_EDITOR_API UiState
{
    //! The command registry. Owned here rather than by the application because the transcript next to it (Log) is only
    //  meaningful alongside the commands that produced it, and because a test can then construct a UiState, register
    //  the UI commands against it and drive the whole console with no device in sight.
    EditorConsole Console;

    EditorSelection Selection;
    PropertyView Properties;
    ContentBrowserModel Content;

    //! Entity and world-structure version the property list was built for; see DrawDetailsPanel.
    Entity PropertiesBuiltFor{};
    UInt64 PropertiesBuiltAtStructure = 0;

    std::string OutlinerFilter;
    std::string ContentFilter;
    std::string ConsoleInput;

    //! Console transcript, oldest first, capped by AddLog.
    std::vector<std::string> Log;

    bool ShowOutliner = true;
    bool ShowDetails = true;
    bool ShowContent = true;
    bool ShowConsole = true;

    //! Viewport gesture state, filled by DrawViewportPanel and consumed by EditorApplication::RenderScenePass.
    ViewportInteraction Viewport;

    //! Set by `layout.reset` / Window > Reset Layout; acted on at the start of the next DrawEditorPanels, which is the
    //  only place with a live dockspace to rebuild. Deferred for the same reason panel actions are deferred elsewhere:
    //  a command can run outside the frame (RunStartupCommand), and ImGui's docking builder needs a submitted node.
    bool RequestLayoutReset = false;

    //! The UI scale in effect (1.0 = metrics as authored). Changed by `ui.scale`; applied by re-running
    //  ApplyEditorTheme, which is safe mid-session because ImGui re-bakes glyphs on demand.
    float UiScale = 1.0f;

    //! Set with `ui.scale` and cleared when the application has restyled. Deferred for the same reason as
    //  RequestLayoutReset: a command can run mid-frame (a panel button), and mutating the style there is not allowed.
    bool RequestStyleApply = false;

    //! Mirror of the viewport's render extent, copied in from PanelContext each frame so `layout.report` (which sees
    //  only UiState) can print it next to the image area. Kept as plain floats rather than reaching for the application:
    //  the commands' dependency on UiState is what keeps them testable with no device.
    float ReportedRenderWidth = 0.0f;
    float ReportedRenderHeight = 0.0f;

    //! Live panel rectangles, refreshed at the end of every DrawEditorPanels call.
    /**
     * A snapshot rather than a live query: ImGui's window list is only valid with its context current between NewFrame
     * and Render, and callers wanting these numbers (`layout.report`, a test after Frame()) are not always in that
     * window. One frame of staleness costs a diagnostic nothing.
     */
    std::vector<PanelRect> PanelGeometry;

    //! Navigation tuning, editable with `view.speed`. Lives here rather than in EditorApplication so the panels can
    //  report it (the viewport's hint line says how fast it will fly) without reaching into the application.
    ViewNavigationSpeeds Navigation;

    //! Name of the content-browser row the author selected, kept by name rather than index because a refresh re-sorts
    //  the list and an index would then highlight a different file than the one clicked.
    std::string SelectedContentName;

    //! Input events fed to ImGui last frame and the window's focus state — the two numbers that answer "why is the UI
    //  not responding?" without a debugger attached to a GUI process.
    Index LastInputEvents = 0;
    bool WindowFocused = true;

    void AddLog(std::string line);

    //! Runs one console line, appending the echo and the result (or the error) to Log. Every route into a command —
    //  the text box, a menu item, a scripted test — goes through here, so "what did this print" has one answer.
    void Run(const std::string& line);
};

//! What the panels need from outside themselves. All borrowed; all valid for the frame.
struct HP_EDITOR_API PanelContext
{
    //! The world being edited. Non-const because the details panel writes component values through PropertyView — that
    //  is the feature. Nothing else in a widget mutates it: entity creation/destruction goes through EditorApplication
    //  so the console and the buttons cannot diverge.
    ECS::World* SceneWorld = nullptr;          //!< not owned
    EditorApplication* Application = nullptr;  //!< not owned
    EditorConsole* Console = nullptr;          //!< not owned

    //! ImGui texture id for the viewport image, or 0 before the first composed frame.
    UInt64 ViewportTextureId = 0;

    //! The editor's icon atlas, or null when it failed to load (or this frame is a headless test with no device).
    /**
     * Borrowed; owned by EditorApplication and valid for the frame. Panels must treat null and unloaded identically —
     * draw the text label alone — because that is the difference between "no icons today" and drawing someone else's
     * glyph, which is the failure a user cannot diagnose.
     */
    const EditorIconSet* Icons = nullptr;  //!< not owned

    //! Whether gameplay scripts are ticking this frame; drives the toolbar's Play/Pause glyph and the status bar.
    bool ScriptsPlaying = false;

    //! Extent the scene was actually rendered at, in pixels. Reported (status bar, `viewport.info`) against the size
    //  the image is drawn at, so a mismatch — the bug class this whole panel exists to make visible — is legible:
    //  "1600x900 → drawn 1382x845" says immediately whether the image is being cropped or stretched.
    float RenderedWidth = 0.0f;
    float RenderedHeight = 0.0f;

    UInt64 FrameCount = 0;
    UInt64 SceneDrawCalls = 0;

    //! The viewport camera's position and target, for the compass overlay and status bar. Read-only here: navigation
    //  goes through the interaction record and EditorApplication, never by a widget moving the camera itself.
    Math::Vec3 CameraPosition{};
    Math::Vec3 CameraTarget{};

    //! True when the navigation hint should be drawn (pointer over the viewport, or a gesture in progress). A permanent
    //  caption would be noise after the first minute; the point is to teach the controls, not decorate the panel.
    bool ViewportHintVisible = false;

    //! Base name of the file being edited, for the status bar. Empty before a scene is loaded or saved.
    std::string ActiveSceneName;

    //! Smoothed frames per second, measured by the application over the last half second rather than from this frame's
    //  delta — an instantaneous 1/dt reads as noise and makes a steady editor look like it is stuttering.
    float FramesPerSecond = 0.0f;

    //! Seconds since last frame, for time-based navigation. Clamped positive by the caller (a zero delta freezes the
    //  camera rather than making motion infinite).
    float DeltaSeconds = 0.0f;

    //! Window content extent, used for the panels' initial layout and for sizing the viewport's image widget. ImGui
    //! writes no ini file, so without an explicit first-frame rectangle every window opens at the same cascade position
    //  and stacks — which looks like "only one panel exists" and is precisely what a screenshot test must not pass on.
    float ContentWidth = 0.0f;
    float ContentHeight = 0.0f;
};

//! The label an outliner row shows: the author's name when set, otherwise a derived one that identifies the entity.
/**
 * Presentation, so it lives with the panels rather than on EntityName (which stores what was typed, empty meaning "not
 * typed"). An outliner whose rows all read "Entity" is unusable past half a dozen entities, which is why UE names every
 * actor; the slot number in the fallback keeps two unnamed entities distinguishable and greppable in a log.
 */
[[nodiscard]] std::string FormatEntityLabel(const ECS::World& world, const Entity& entity);

//! The live rectangle of each named panel, for diagnostics and tests.
/**
 * A panel that was never created is reported with a zero size rather than omitted, so "the outliner did not draw" is a
 * distinguishable answer from "the outliner drew at 0x0" — the two failures need different fixes and collapsing them
 * loses that. Requires a current ImGui context between NewFrame and Render (the window list is only valid there); with
 * no context everything comes back zero, which `layout.report` prints as such rather than pretending the layout settled.
 */
[[nodiscard]] HP_EDITOR_API std::vector<PanelRect> CollectPanelRectangles();

//! Draws the dockspace and every visible panel. Call between ImGui::NewFrame and ImGui::Render.
void DrawEditorPanels(UiState& state, const PanelContext& context);

//! Registers every editor command against `console` — help/quit/select/info plus the scene and content commands that
//  need the application. Idempotent (re-registering replaces), so a test can build a registry from scratch.
void RegisterEditorCommands(EditorConsole& console, UiState& state, EditorApplication& app);

//! The UI-state half of that registration (help, quit, select, info, panels.show). Split out because the commands live
//  where their state lives: these touch only UiState, while scene/entity/content commands need the hosted loop and are
//  registered by EditorApplication.cpp. Called by RegisterEditorCommands; exposed so both halves are testable without a
//  device (build an EditorConsole and a UiState, call this, assert on what `help` lists).
void RegisterUiCommands(EditorConsole& console, UiState& state, EditorApplication& app);

}  // namespace Hollowpoint::Editor
