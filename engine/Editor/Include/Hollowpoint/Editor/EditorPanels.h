#pragma once

#include <string>
#include <vector>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/ECS/Entity.h"
#include "Hollowpoint/ECS/World.h"
#include "Hollowpoint/Editor/ContentBrowserModel.h"
#include "Hollowpoint/Editor/EditorConsole.h"
#include "Hollowpoint/Editor/EditorSelection.h"
#include "Hollowpoint/Editor/PropertyView.h"

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
    float ViewportWidth = 0.0f;
    float ViewportHeight = 0.0f;

    UInt64 FrameCount = 0;
    UInt64 SceneDrawCalls = 0;

    //! Window content extent, used for the panels' initial layout. ImGui has no saved state (no ini file), so without
    //! an
    //  explicit first-frame rectangle every window opens at the same cascade position and stacks — which looks like
    //  "only one panel exists" and is precisely what a screenshot test must not pass on.
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
