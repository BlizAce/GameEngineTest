#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Editor/ContentBrowserModel.h"
#include "Hollowpoint/Editor/EditorApplication.h"
#include "Hollowpoint/Editor/EditorConsole.h"
#include "Hollowpoint/Editor/EditorIcons.h"
#include "Hollowpoint/Editor/EditorPanels.h"
#include "Hollowpoint/Editor/EditorSelection.h"
#include "Hollowpoint/Editor/EditorStyle.h"
#include "Hollowpoint/Editor/EntityName.h"
#include "Hollowpoint/Editor/PropertyView.h"
#include "Hollowpoint/Editor/ViewCamera.h"
#include "Hollowpoint/Game/MeshRenderer.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Reflection/Descriptors.h"
#include "Hollowpoint/Reflection/Registry.h"
#include "Hollowpoint/Renderer/Camera.h"
#include "Hollowpoint/Renderer/Lights.h"
#include "Hollowpoint/Scene/LocalTransform.h"

#include <imgui.h>
// DockBuilder (DockBuilderSplitNode / DockBuilderDockWindow) is only in imgui_internal.h: ImGui calls it "eventually
// public, too early to expose", and this is the one file that needs it. The alternative — five free-floating windows
// laid out on top of a dockspace — is exactly what made the previous layout scatter instead of tile, so the dependency
// is worth taking; it is confined to BuildDefaultDockLayout below and cannot leak into an engine header.
#include <imgui_internal.h>

// ---------------------------------------------------------------------------
// The editor's docked panels. See EditorPanels.h for why this is a separate translation unit.
//
// Every panel is a function of (state, context) and touches nothing else — in particular no panel creates GPU
// resources or calls the device. That is what keeps the composition frame short and makes it obvious where each side
// effect belongs: widgets request an action through EditorApplication, and only EditorApplication acts on the engine.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Editor
{

using Core::Index;
using Core::Int64;
using Core::Size;
using ECS::Entity;
using Math::Vec3;
using Reflection::ValueKind;

namespace
{

[[nodiscard]] std::string ToLower(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

[[nodiscard]] bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle)
{
    if (needle.empty())
    {
        return true;
    }
    return ToLower(haystack).find(ToLower(needle)) != std::string::npos;
}

//! Copies a std::string into an ImGui text buffer. Returns false when the string does not fit, which for the filters
//  means "the rest was cut" — better than the alternative (truncating silently at 255 characters and having a user
//  wonder why their search finds nothing).
bool FillBuffer(std::string_view text, char* buffer, Size capacity)
{
    if (text.size() + 1 > capacity)
    {
        return false;
    }
    std::snprintf(buffer, capacity, "%s", text.data());
    return true;
}

[[nodiscard]] std::string JoinNames(const std::vector<std::string>& names, std::string_view separator)
{
    std::string out;
    for (const std::string& name : names)
    {
        if (!out.empty())
        {
            out += separator;
        }
        out += name;
    }
    return out;
}

[[nodiscard]] bool ParseFloat(std::string_view text, float& outValue) noexcept
{
    try
    {
        std::string s(text);
        Size consumed = 0;
        const float parsed = std::stof(s, &consumed);
        // Trailing garbage is a typo rather than a number with a suffix: `view.speed 5x` must fail rather than set 5 and
        // leave the author believing their value took.
        if (consumed != s.size())
        {
            return false;
        }
        outValue = parsed;
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Icon widgets
//
// One place converts (icon name, size) into a draw call, because every other file in this module must not care whether
// the atlas loaded. A missing icon draws nothing rather than a placeholder square: UE's chrome is dense with glyphs and
// a placeholder on every one of them would be noisier than the label beside it. Where an icon carries meaning a label
// cannot (an eye for visibility), the caller draws a text fallback too — that decision belongs to the caller, which is
// why it is not baked into these helpers.
// ---------------------------------------------------------------------------

//! True when `icons` can actually be drawn with. Null and unloaded are the same answer for a widget.
[[nodiscard]] bool IconsAvailable(const EditorIconSet* icons)
{
    return icons != nullptr && icons->IsLoaded();
}

//! Draws one glyph at `size`, advancing the cursor by exactly that width when it drew.
/**
 * Returns false when nothing was drawn, so a caller can fall back to text. The advance matters: an icon followed by
 * SameLine() with no item is laid out at the previous x, which stacks a label on top of the glyph — visible only as
 * "the panel looks squashed" in a screenshot.
 */
bool DrawIcon(const EditorIconSet* icons, const char* name, float size)
{
    IconUvRect uv;
    if (!IconsAvailable(icons) || !icons->Find(name, uv))
    {
        return false;
    }

    ImGui::Image(ImTextureRef{static_cast<ImTextureID>(icons->GetTextureId())}, ImVec2(size, size),
                 ImVec2(uv.U0, uv.V0), ImVec2(uv.U1, uv.V1));
    return true;
}

//! An icon button: a button-sized frame with the glyph centred in it.
/**
 * Built as Button + a draw-list overlay rather than ImageButton-with-the-atlas for two reasons. ImageButton tints its
 * image with the button colour on hover, which would recolour every glyph when UE's icons keep their own colour and
 * only the background changes; and passing no texture at all (the no-atlas case) would fall through to this renderer's
 * white-texture fallback and paint a solid square over the button. A Button submits no textured command beyond the font
 * atlas, so there is nothing to mis-sample.
 *
 * Frame padding is pushed to half the icon size so an empty label yields an exactly size x size frame: rows of these
 * align with the text row beside them, which is what separates "a toolbar" from "buttons with pictures in".
 */
bool IconButton(const EditorIconSet* icons, const char* name, const char* id, const char* tooltip, float size,
                bool enabled = true)
{
    if (!enabled)
    {
        ImGui::BeginDisabled();
    }

    // The label is forced invisible: ImGui::Button draws its label, and passing a human-readable one here produced a
    // text button with the glyph painted over it — light pixels on light text, i.e. an unreadable smear that also made
    // the icons look like they were not drawing at all. Prefixing internally means no call site can reintroduce that.
    const std::string label = std::string("##") + id;

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(size * 0.5f, size * 0.5f));
    const bool clicked = ImGui::Button(label.c_str());
    ImGui::PopStyleVar();
    if (tooltip != nullptr && tooltip[0] != '\0')
    {
        ImGui::SetItemTooltip("%s", tooltip);
    }

    if (!enabled)
    {
        ImGui::EndDisabled();
    }

    IconUvRect uv;
    if (!IconsAvailable(icons) || !icons->Find(name, uv))
    {
        return clicked;  // no atlas: the button's label is the fallback, so nothing more to draw
    }

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 frameSize = ImGui::GetItemRectSize();
    const float offset = (frameSize.x - size) * 0.5f;
    // Dimmed with the frame when disabled: an icon that stays bright next to a greyed button reads as still clickable.
    const ImU32 tint = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    ImGui::GetWindowDrawList()->AddImage(ImTextureRef{static_cast<ImTextureID>(icons->GetTextureId())},
                                         ImVec2(min.x + offset, min.y + offset),
                                         ImVec2(min.x + offset + size, min.y + offset + size), ImVec2(uv.U0, uv.V0),
                                         ImVec2(uv.U1, uv.V1), tint);

    // Lay the next item on the same row. Doing it here rather than at each call site is what keeps a toolbar from
    // silently becoming a vertical stack: without it every button drops to a new line, and inside a 29 px strip that
    // means only the first is ever visible — which looks exactly like "the icons are missing".
    ImGui::SameLine();
    return clicked;
}

// ---------------------------------------------------------------------------
// Dock layout
// ---------------------------------------------------------------------------

//! Stable id for the editor's dockspace. Never 0: ImGui treats a dockspace created with id 0 differently from one it
//  assigns itself, and passing GetID("...") here would make the layout depend on window-list order.
constexpr ImGuiID kDockSpaceId = 0x4850444BU;  // 'HPDK'

//! Window titles. One place, because DockBuilderDockWindow matches by name and a typo silently produces an undocked
//  floating panel — the kind of failure that shows up as "the outliner vanished" in a screenshot.
constexpr const char* kOutlinerTitle = "World Outliner";
constexpr const char* kDetailsTitle = "Details";
constexpr const char* kContentTitle = "Content Browser";
constexpr const char* kConsoleTitle = "Output Log";
constexpr const char* kViewportTitle = "Viewport";

//! Default split fractions of the docked layout, expressed as fractions of the space *remaining at that moment* —
//  which is what DockBuilderSplitNode divides, because each split re-parents the previous subtree. Getting these wrong
//  is invisible in code and obvious on screen (a 24% left panel becomes 16% once three splits compose), so the numbers
//  below were tuned against a measured 1600x900 capture rather than derived: see notes/EDITOR_UI_REVIEW.md §5 for the
//  measured panel boundaries.
constexpr float kLeftSplit = 0.20f;
constexpr float kRightSplitOfRemaining = 0.28f;
constexpr float kBottomSplitOfRemaining = 0.30f;
constexpr float kContentShareOfBottom = 0.34f;

//! Height of the bottom status strip, in content units at 1x scale.
/**
 * One definition because two things must agree on it: the dockspace shrinks by exactly this much so no panel is covered,
 * and the bar draws itself at exactly this height. Two separate expressions drift the moment someone changes padding,
 * and the failure is a strip of console hidden behind an opaque bar — invisible in code review, obvious on screen.
 */
[[nodiscard]] float StatusBarHeight()
{
    return ImGui::GetFrameHeight() + 2.0f;
}

//! Height of the main toolbar strip, in content units at 1x scale.
/**
 * Same reason as StatusBarHeight: SubmitDockspace must subtract exactly what the toolbar occupies, or the first panel
 * row is drawn underneath an opaque strip and its clicks are swallowed — a bug that looks like "the outliner does not
 * respond" rather than "two numbers disagree". Must match DrawMainToolBar's own height expression; ToolbarHeightFor is
 * the single definition both use.
 */
[[nodiscard]] float ToolbarHeight()
{
    return ImGui::GetFrameHeight() + 6.0f;
}

//! Where the dock area and toolbar begin: directly below the main menu bar, measured from the menu bar itself.
/**
 * Deriving this from the menu bar window's real bottom edge — instead of from viewport->WorkPos.y plus a guessed menu
 * height — is what makes it correct regardless of whether this ImGui build has already folded the menu bar into WorkPos
 * (it differs between docking/no-decoration configurations, and both readings were tried here: one drew the toolbar over
 * the menu labels, the other left a 23px gap and covered the first panel row). The expression is self-consistent either
 * way because it never mixes measured window positions with assumed heights.
 */
struct ChromeStrips
{
    float BelowMenuBar = 0.0f;  //!< y where the toolbar starts
    float DockTopInset = 0.0f;  //!< how far below WorkPos.y the dock area must start (menu bar + toolbar)
};

[[nodiscard]] ChromeStrips MeasureChrome(float toolbarHeight)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiWindow* menuBar = ImGui::FindWindowByName("##MainMenuBar");

    ChromeStrips strips;
    strips.BelowMenuBar = menuBar != nullptr ? menuBar->Pos.y + menuBar->Size.y : viewport->WorkPos.y;
    strips.DockTopInset = (strips.BelowMenuBar - viewport->WorkPos.y) + toolbarHeight;
    return strips;
}

//! Icon edge for the current UI scale: glyphs track the text they sit beside so one slider scales both.
[[nodiscard]] float ToolbarIconSize(const UiState& state)
{
    return IconSizeForScale(state.UiScale, 16.0f);
}

//! Splits `node` along `dir`, returning both children by reference.
/**
 * Both ids matter: windows dock into *leaf* nodes, so a layout that docks into the node it split gets no diagnostic and
 * simply leaves the panel floating. Keeping the pair explicit is what makes the tree below readable as a tree.
 */
void Split(ImGuiID node, ImGuiDir dir, float ratioAtDir, ImGuiID& outAtDir, ImGuiID& outOpposite)
{
    ImGui::DockBuilderSplitNode(node, dir, ratioAtDir, &outAtDir, &outOpposite);
}

//! Builds the default docked tree: outliner left, details right, content + console along the bottom, viewport central.
/**
 * Called on the first frame and whenever `layout.reset` runs. Rebuilding means re-docking by window title, which is why
 * every panel has a fixed title above. DockBuilderAddNode with the DockSpace flag removes any existing tree at that id
 * first (ImGui does that internally), so this doubles as the reset path — there is no separate teardown to get wrong.
 *
 * Order matters: each split carves an edge off what remains, so the centre shrinks towards the middle. The old layout
 * never did this at all — five independent windows were floated over a bare dockspace, which is why dragging one panel's
 * edge moved nothing and the arrangement could be scattered with no way back.
 */
void BuildDefaultDockLayout()
{
    const ImGuiID root = ImGui::DockBuilderAddNode(kDockSpaceId, ImGuiDockNodeFlags_DockSpace);
    // A size is required before splitting (DockNodeTreeSplit asserts on an unsized node); the *value* only sets the
    // ratios' rounding basis — the real extents come from the host window on the same frame.
    ImGui::DockBuilderSetNodeSize(root, ImVec2(1600.0f, 900.0f));

    ImGuiID leftNode = 0;
    ImGuiID centreNode = 0;
    Split(root, ImGuiDir_Left, kLeftSplit, leftNode, centreNode);

    ImGuiID rightNode = 0;
    Split(centreNode, ImGuiDir_Right, kRightSplitOfRemaining, rightNode, centreNode);

    ImGuiID bottomNode = 0;
    Split(centreNode, ImGuiDir_Down, kBottomSplitOfRemaining, bottomNode, centreNode);

    // Content browser and console share the bottom strip side by side: files on the left, output along the rest — the
    // reading order an author follows when importing something and watching for the result.
    ImGuiID contentNode = 0;
    ImGuiID consoleNode = 0;
    Split(bottomNode, ImGuiDir_Left, kContentShareOfBottom, contentNode, consoleNode);

    ImGui::DockBuilderDockWindow(kOutlinerTitle, leftNode);
    ImGui::DockBuilderDockWindow(kDetailsTitle, rightNode);
    ImGui::DockBuilderDockWindow(kContentTitle, contentNode);
    ImGui::DockBuilderDockWindow(kConsoleTitle, consoleNode);
    ImGui::DockBuilderDockWindow(kViewportTitle, centreNode);

    ImGui::DockBuilderFinish(kDockSpaceId);
}

//! True when the dockspace has no tree worth keeping — i.e. nothing was ever built, or a rebuild was requested.
/**
 * Checked against ImGui's own node state rather than a "first frame" bool on the application: the layout is only
 * meaningful once the dock node exists with children, and an author who drags every panel out of it should get a working
 * editor back from Reset Layout rather than a half-tree. A missing or childless node means exactly one thing — there is
 * nothing to preserve.
 */
[[nodiscard]] bool DockLayoutNeedsBuild()
{
    const ImGuiDockNode* node = ImGui::DockBuilderGetNode(kDockSpaceId);
    return node == nullptr || (node->ChildNodes[0] == nullptr && node->ChildNodes[1] == nullptr
                               && node->Windows.Size == 0);
}

//! Submits the full-window dockspace host and (when asked) rebuilds the default tree into it.
/**
 * Host window flags mirror DockSpaceOverViewport, which this replaces: NoDocking on the host (a dockspace dockable into
 * itself is a tangle), zero padding so panels meet the window edge with no gap, and no background so the dockspace's own
 * fill is what shows between panels.
 */
void SubmitDockspace(bool rebuildLayout, float bottomInset, float topInset)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // The dock area starts below the menu bar + toolbar and ends above the status bar; both insets are subtracted from
    // work height so panels never sit under an opaque strip.
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + topInset));
    // The status bar gets its own strip rather than being painted over the dock: an overlay would cover the console's
    // last line and swallow clicks meant for whatever panel happened to sit under it.
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, std::max(64.0f, viewport->WorkSize.y - bottomInset - topInset)));
    ImGui::SetNextWindowViewport(viewport->ID);

    const ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("DockspaceHost", nullptr, hostFlags);

    if (rebuildLayout || DockLayoutNeedsBuild())
    {
        BuildDefaultDockLayout();
    }

    // AutoHideTabBar keeps a single-window node header-free (a lone panel shows its name in a slim bar rather than a
    // tab strip). The central node is excluded from that by ImGui itself, so the viewport always has a tab.
    ImGui::DockSpace(kDockSpaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_AutoHideTabBar);

    ImGui::End();
    ImGui::PopStyleVar(3);
}

}  // namespace

// ---------------------------------------------------------------------------
// Command registration (UI-state commands; scene/entity/content commands live in EditorApplication.cpp)
// ---------------------------------------------------------------------------

void RegisterUiCommands(EditorConsole& console, UiState& state, EditorApplication& app)
{
    console.RegisterFixed(
        "help", "List every command, or show usage for one.", ArgumentCount::AtLeast, 0,
        [&console](const std::vector<std::string>& arguments, std::string& outReply, std::string& outError) {
            if (arguments.empty())
            {
                outReply = "commands: " + JoinNames(console.Complete(""), ", ");
                return true;
            }

            const ConsoleCommand* command = console.Find(arguments[0]);
            if (command == nullptr)
            {
                outError = "no command named \"" + arguments[0] + "\"";
                return false;
            }
            outReply = command->Usage + "\n" + command->Summary;
            return true;
        });

    console.RegisterFixed("editor.quit", "Close the editor.", ArgumentCount::Exact, 0,
                          [&app](const std::vector<std::string>&, std::string&, std::string&) {
                              app.RequestStop(EditorStopReason::WindowClosed);
                              return true;
                          });

    console.RegisterFixed(
        "editor.select", "Select an entity by slot.", ArgumentCount::Exact, 1,
        [&app](const std::vector<std::string>& arguments, std::string& outReply, std::string& outError) {
            // stoull throws on garbage; catching keeps a typo in a console line from taking the editor
            // down with it, which is the difference between a tool and a crash report.
            unsigned long long slot = 0;
            try
            {
                slot = std::stoull(arguments[0]);
            }
            catch (const std::exception&)
            {
                outError = "\"" + arguments[0] + "\" is not an entity slot number";
                return false;
            }

            if (!app.SelectBySlot(static_cast<Index>(slot)))
            {
                outError = "no live entity at slot " + arguments[0];
                return false;
            }
            outReply = "selected slot " + arguments[0];
            return true;
        });

    console.RegisterFixed("editor.clear", "Clear the selection.", ArgumentCount::Exact, 0,
                          [&state](const std::vector<std::string>&, std::string& outReply, std::string&) {
                              const Size was = state.Selection.GetCount();
                              state.Selection.Clear();
                              outReply = "cleared " + std::to_string(was) + " selection(s)";
                              return true;
                          });

    console.RegisterFixed("editor.info", "Report frame, selection and panel state.", ArgumentCount::Exact, 0,
                          [&state, &app](const std::vector<std::string>&, std::string& outReply, std::string&) {
                              char buffer[512];
                              const Entity primary = state.Selection.GetPrimary();
                              std::snprintf(buffer, sizeof(buffer),
                                            "frames=%llu  selected=%zu (primary slot %s)  panels: outliner=%d "
                                            "details=%d content=%d console=%d",
                                            static_cast<unsigned long long>(app.GetFrameCount()),
                                            static_cast<size_t>(state.Selection.GetCount()),
                                            primary.IsNull() ? "-" : std::to_string(primary.Slot).c_str(),
                                            state.ShowOutliner, state.ShowDetails, state.ShowContent,
                                            state.ShowConsole);
                              outReply = buffer;
                              return true;
                          });

    console.RegisterFixed(
        "editor.input", "Report how many input events reached the UI last frame.", ArgumentCount::Exact, 0,
        [&state](const std::vector<std::string>&, std::string& outReply, std::string&) {
            // Exists because "the editor ignores my keyboard" has three causes (no events arriving,
            // events arriving and being consumed elsewhere, ImGui not routing them) and this names
            // which one it is without attaching a debugger to a GUI process.
            outReply = "last frame: " + std::to_string(state.LastInputEvents) +
                       " event(s) fed to ImGui; focus=" + std::string(state.WindowFocused ? "yes" : "no");
            return true;
        });

    console.RegisterFixed(
        "panels.show",
        "Show or hide a panel: panels.show <outliner|details|content|console> "
        "<0|1>.",
        ArgumentCount::Exact, 2,
        [&state](const std::vector<std::string>& arguments, std::string& outReply, std::string& outError) {
            const bool visible = arguments[1] != "0" && ToLower(arguments[1]) != "false";
            const std::string which = ToLower(arguments[0]);

            if (which == "outliner")
            {
                state.ShowOutliner = visible;
            }
            else if (which == "details")
            {
                state.ShowDetails = visible;
            }
            else if (which == "content")
            {
                state.ShowContent = visible;
            }
            else if (which == "console")
            {
                state.ShowConsole = visible;
            }
            else
            {
                outError = "unknown panel \"" + arguments[0] + "\" (outliner, details, content, console)";
                return false;
            }

            outReply = which + " " + (visible ? "shown" : "hidden");
            return true;
        });

    // Layout and appearance commands exist because a docked layout you cannot get back to is a worse tool than a fixed
    // one: the first thing an author does after scattering panels is look for Reset Layout, and there was nothing.
    console.RegisterFixed("layout.reset", "Restore the default panel layout, showing any hidden panel.",
                          ArgumentCount::Exact, 0,
                          [&state](const std::vector<std::string>&, std::string& outReply, std::string&) {
                              // Visibility is part of the layout: a "reset" that left panels hidden through the Window
                              // menu would restore the tree while the author still could not see half of it. Resetting to
                              // all-shown (rather than to whatever was visible before) is the only defined state worth
                              // returning to, and matches every editor's Reset Layout.
                              state.ShowOutliner = true;
                              state.ShowDetails = true;
                              state.ShowContent = true;
                              state.ShowConsole = true;

                              // Deferred to the next DrawEditorPanels: the docking builder needs a submitted dockspace,
                              // and this command can run before any frame exists (--command layout.reset).
                              state.RequestLayoutReset = true;
                              outReply = "layout reset queued for the next frame";
                              return true;
                          });

    console.RegisterFixed(
        "layout.report", "Report each panel's docked rectangle and the viewport's render vs display size.",
        ArgumentCount::Exact, 0,
        [&state](const std::vector<std::string>&, std::string& outReply, std::string&) {
            // Panel geometry first: the numbers a person asks for when something looks wrong ("is the console actually
            // there, or is it zero-height behind the status bar?"). Printed from CollectPanelRectangles so the console and
            // the test suite read exactly the same source — a diagnostic that disagrees with the tests is worse than none.
            std::string report;
            for (const PanelRect& rect : CollectPanelRectangles())
            {
                char line[128];
                std::snprintf(line, sizeof(line), "%s: %.0fx%.0f at %.0f,%.0f\n", rect.Name,
                              static_cast<double>(rect.Width), static_cast<double>(rect.Height),
                              static_cast<double>(rect.X), static_cast<double>(rect.Y));
                report += line;
            }

            // Then the viewport's render extent next to its displayed extent: a mismatch between those two is what
            // "the viewport looks cropped" means in numbers, and printing them side by side makes it legible without
            // measuring a screenshot by hand.
            char buffer[512];
            const ViewportInteraction& viewport = state.Viewport;
            std::snprintf(buffer, sizeof(buffer),
                          "viewport drawn %.0fx%.0f | image area %.0fx%.0f | scale %.2f | layout %s",
                          static_cast<double>(state.ReportedRenderWidth), static_cast<double>(state.ReportedRenderHeight),
                          static_cast<double>(viewport.ImageWidth), static_cast<double>(viewport.ImageHeight),
                          static_cast<double>(state.UiScale), state.RequestLayoutReset ? "reset pending" : "default");
            outReply = report + buffer;
            return true;
        });

    console.RegisterFixed(
        "ui.scale", "Set the UI scale (0.6 to 3.0): ui.scale <factor>.", ArgumentCount::Exact, 1,
        [&state](const std::vector<std::string>& arguments, std::string& outReply, std::string& outError) {
            float scale = 1.0f;
            if (!ParseFloat(arguments[0], scale))
            {
                outError = "\"" + arguments[0] + "\" is not a number";
                return false;
            }
            state.UiScale = std::clamp(scale, 0.6f, 3.0f);
            // Applied by the application on the next frame rather than here: restyling touches the ImGui style, which is
            // safe between frames but not from inside a command that might itself be running mid-frame (a panel button).
            state.RequestStyleApply = true;
            outReply = "ui scale " + std::to_string(state.UiScale);
            return true;
        });

    console.RegisterFixed(
        "view.speed", "Set fly speed in metres per second: view.speed <mps>.", ArgumentCount::Exact, 1,
        [&state](const std::vector<std::string>& arguments, std::string& outReply, std::string& outError) {
            float metres = 0.0f;
            if (!ParseFloat(arguments[0], metres) || metres <= 0.0f)
            {
                outError = "\"" + arguments[0] + "\" is not a positive speed";
                return false;
            }
            state.Navigation.FlyMetresPerSecond = metres;
            outReply = "fly speed " + std::to_string(metres) + " m/s";
            return true;
        });

    console.RegisterFixed("view.frame", "Frame the selection (or everything) in the viewport.", ArgumentCount::Exact, 0,
                          [&state](const std::vector<std::string>&, std::string& outReply, std::string&) {
                              // Same deferral rule as layout.reset: the camera move needs the viewport's real extent,
                              // which only exists inside a frame.
                              state.Viewport.RequestFrameSelection = true;
                              outReply = "framing " + std::string(state.Selection.IsEmpty() ? "the whole scene"
                                                                                            : "the selection");
                              return true;
                          });

    console.RegisterFixed(
        "editor.scripts.play", "Resume the gameplay script tick (scripts only — no PIE snapshot).",
        ArgumentCount::Exact, 0,
        [&app](const std::vector<std::string>&, std::string& outReply, std::string&) {
            app.SetScriptsPlaying(true);
            outReply = "scripts running";
            return true;
        });

    console.RegisterFixed(
        "editor.scripts.pause", "Pause the gameplay script tick.", ArgumentCount::Exact, 0,
        [&app](const std::vector<std::string>&, std::string& outReply, std::string&) {
            app.SetScriptsPlaying(false);
            outReply = "scripts paused";
            return true;
        });

    console.RegisterFixed(
        "editor.scripts.stop", "Stop the gameplay script tick. Mutations made while running are kept.",
        ArgumentCount::Exact, 0,
        [&app](const std::vector<std::string>&, std::string& outReply, std::string&) {
            app.SetScriptsPlaying(false);
            outReply = "scripts stopped";
            return true;
        });
}

// ---------------------------------------------------------------------------
// UiState
// ---------------------------------------------------------------------------

void UiState::AddLog(std::string line)
{
    Log.push_back(std::move(line));

    // Bounded: an editor left open for a day with a script printing every frame must not grow its transcript without
    // limit. Dropping the oldest lines keeps the useful end (the recent ones) and costs nothing.
    constexpr Size kMaxLogLines = 512;
    if (Log.size() > kMaxLogLines)
    {
        Log.erase(Log.begin(), Log.begin() + static_cast<Index>(Log.size() - kMaxLogLines));
    }
}

void UiState::Run(const std::string& line)
{
    if (line.empty())
    {
        return;
    }

    AddLog("> " + line);

    std::string reply;
    std::string error;
    if (Console.ExecuteLine(line, reply, error))
    {
        if (!reply.empty())
        {
            AddLog(reply);
        }
    }
    else
    {
        AddLog("! " + error);
    }
}

// ---------------------------------------------------------------------------
// Panels
// ---------------------------------------------------------------------------

namespace
{

// ---------------------------------------------------------------------------
// Row layout helper
// ---------------------------------------------------------------------------

//! Opens a label-left / control-right row: the label takes exactly `labelWidth` and the control fills what is left.
/**
 * ImGui's default stacks a widget's label above it at natural width, which turns an inspector into a ragged column and
 * wastes most of a 380-pixel panel. PushItemWidth(-1) after a fixed-width SameLine makes every row in every component
 * align — the single change that makes a reflected panel read as an inspector rather than a debug list, and the layout
 * UE users expect (label left, value right, full-width drag target).
 *
 * Call EndFieldRow() after the widget. Both are here rather than inline so the Push/Pop pairing cannot be broken by an
 * early `continue` in the field loop — the bug class that produces a corrupted style stack several panels later.
 */
void BeginFieldRow(std::string_view label, float labelWidth)
{
    ImGui::PushItemWidth(-1.0f);
    ImGui::TextUnformatted(label.data(), label.data() + label.size());
    ImGui::SameLine(labelWidth);
}

void EndFieldRow()
{
    ImGui::PopItemWidth();
}

[[nodiscard]] std::vector<std::pair<Int64, std::string>> EnumEntries(const PropertyField& field)
{
    std::vector<std::pair<Int64, std::string>> entries;
    if (field.EnumName.empty())
    {
        return entries;
    }

    const Reflection::EnumDescriptor* enumeration = Reflection::Registry::Get().FindEnum(field.EnumName);
    if (enumeration == nullptr)
    {
        return entries;
    }

    for (Size index = 0; index < enumeration->GetEntryCount(); ++index)
    {
        const Reflection::EnumEntry& entry = enumeration->GetEntry(index);
        entries.emplace_back(entry.Value, entry.Name != nullptr ? entry.Name : "?");
    }
    return entries;
}

//! A numeric drag with a speed and format suited to the field's name.
/**
 * Why name-matched rather than attribute-driven: FieldAttributes carries Min/Max/Step, but nothing in the engine
 * populates them yet (grep for HasStep finds no writers), so reflection alone cannot say that Roughness wants 0.01 and
 * Position.x wants 0.05. Matching on the leaf name is a heuristic, stated as one, and it beats the previous behaviour —
 * one speed for everything, which made a world position crawl and a roughness spin past its range. When attributes get
 * populated they win over this table; that precedence is what makes the heuristic deletable.
 */
[[nodiscard]] float DragSpeedFor(std::string_view path)
{
    const std::string lower = ToLower(path);

    if (lower.find("rotation") != std::string::npos || lower.find("angle") != std::string::npos)
    {
        return 0.5f;  // degrees feel right for rotation; a radian-scaled 0.01 makes aiming impossible
    }
    if (lower.find("position") != std::string::npos || lower.find("target") != std::string::npos
        || lower.find("direction") != std::string::npos)
    {
        return 0.05f;
    }
    if (lower.find("scale") != std::string::npos)
    {
        return 0.02f;
    }
    if (lower.find("nearplane") != std::string::npos || lower.find("farplane") != std::string::npos)
    {
        return 0.1f;
    }
    return 0.01f;  // material and light parameters: roughness, intensity, exposure
}

[[nodiscard]] const char* DragFormatFor(ValueKind kind)
{
    // Integers drag as integers (a "1.000000" for a count reads as broken) and reals get three decimals, which is where
    // a roughness value is legible without being noisy.
    switch (kind)
    {
        case ValueKind::Int8:
        case ValueKind::Int16:
        case ValueKind::Int32:
        case ValueKind::Int64:
        case ValueKind::UInt8:
        case ValueKind::UInt16:
        case ValueKind::UInt32:
        case ValueKind::UInt64:
            return "%d";
        default:
            return "%.3f";
    }
}

//! Formats a byte count as the content browser shows it (KB/MB with one decimal under a megabyte, whole MB above).
/**
 * The old panel printed "0 KB" for every file under 1024 bytes — which is every .lua and most .hpscene files in a real
 * project — so the Size column was uniformly useless. Rounding rules are stated because a size column that changes its
 * own unit per row needs them to be consistent.
 */
[[nodiscard]] std::string FormatByteSize(Core::UInt64 bytes)
{
    char buffer[48];
    if (bytes < 1024ull)
    {
        std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
    }
    else if (bytes < 1024ull * 1024ull)
    {
        std::snprintf(buffer, sizeof(buffer), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    }
    else
    {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return buffer;
}

//! Carries the UiState into ImGui's InputText completion callback, which has no other way to reach it.
/**
 * Lifetime: constructed on the stack immediately before InputText and dead right after — ImGui invokes the callback
 * synchronously inside that call, so the pointer cannot outlive its owner. This is also why the completion state cannot
 * be a captured lambda: the callback signature is a C function pointer.
 */
struct ConsoleCompletionContext
{
    UiState& State;
};

//! ImGui completion callback: replaces the word under the cursor with a command match, or lists the candidates.
/**
 * Tab-completion used to be polled next to the text box and read state.ConsoleInput, which only updates when the box
 * commits — so it completed the *previous* line and could not work. The callback receives the live edit buffer, which is
 * the only correct source, and owns its own cursor bookkeeping (CompletionCursor) because completing twice in a row must
 * cycle candidates rather than re-inserting the first one every time.
 */
int ConsoleCompletionCallback(ImGuiInputTextCallbackData* data)
{
    if (data->EventFlag != ImGuiInputTextFlags_CallbackCompletion)
    {
        return 0;
    }

    auto& context = *static_cast<ConsoleCompletionContext*>(data->UserData);

    // Complete the word at the cursor, not the whole line: `entity.set Na` should complete the property name against
    // nothing (no registry of those), but `entity.se` completes the command. Only the first token has command names, so
    // anything past it is left alone rather than mangled.
    const std::string text(data->Buf, static_cast<size_t>(data->CursorPos));
    const Size lastSpace = text.find_last_of(' ');
    const bool completingFirstWord = lastSpace == std::string::npos;
    if (!completingFirstWord)
    {
        return 0;
    }

    const std::string prefix = text.substr(lastSpace == std::string::npos ? 0 : lastSpace + 1);
    const std::vector<std::string> matches = context.State.Console.Complete(prefix);
    if (matches.empty())
    {
        return 0;
    }

    // One match: complete it and leave a trailing space so the next Tab starts the second argument. Several: insert the
    // longest common prefix, which is what makes `ed` + Tab give `editor.` without guessing.
    std::string replacement = matches[0];
    if (matches.size() > 1)
    {
        replacement = matches[0];
        for (const std::string& match : matches)
        {
            Size common = 0;
            while (common < replacement.size() && common < match.size() && replacement[common] == match[common])
            {
                ++common;
            }
            replacement.resize(common);
        }
    }

    if (replacement.size() > prefix.size())
    {
        data->InsertChars(static_cast<int>(data->CursorPos), replacement.c_str() + prefix.size());
    }
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Panels
//
// Every panel Begin()s with its fixed title and *no* position or size: docked windows take their rectangle from the
// dock node, which is what makes the splitters work. The previous version positioned each one with
// ImGuiCond_FirstUseEver over a bare dockspace — five independent windows that happened to be laid out like a docked
// editor without being one, so no splitter resized its neighbour and no reset could restore anything.
// ---------------------------------------------------------------------------

//! The outliner glyph for an entity, from the components it actually has.
/**
 * Type-safe lookups, not name matching: IComponentStorage::GetTypeName() returns whatever registration handed it, and
 * for these engine components that is a mangled C++ name (N11Hollowpoint4Game12MeshRendererE), so an earlier version
 * of this function that compared against "MeshRenderer" matched nothing and every row drew the generic glyph — silently,
 * with no error to follow. World::HasComponent<T> goes through the same storage map by type and cannot drift from a
 * rename.
 *
 * Ordered most specific first. Guessing a type from the entity's *name* would mislabel anything the author renamed, and
 * a confidently wrong icon is worse than a generic one because it looks authoritative. Script beats MeshRenderer
 * because a scripted actor is what an author thinks of as "the script".
 */
[[nodiscard]] const char* OutlinerIconFor(const ECS::World& world, const ECS::Entity& entity)
{
    if (world.HasComponent<Renderer::CameraComponent>(entity))
    {
        return "actor_camera";
    }
    if (world.HasComponent<Renderer::LightComponent>(entity))
    {
        return "actor_light";
    }
    // No script glyph: Hollowpoint::Scripting is an optional module (engine/Editor/CMakeLists.txt links it only when the
    // target exists), so naming ScriptComponent here would break a -DHP_ENABLE_SCRIPTING=OFF build for one icon. The
    // atlas already has actor_script; wiring it needs a scripting-agnostic way to ask "does this entity have a script".
    if (world.HasComponent<Game::MeshRenderer>(entity))
    {
        return "actor_cube";
    }
    return "actor_empty";
}

void DrawOutlinerPanel(UiState& state, const PanelContext& context)
{
    if (!state.ShowOutliner)
    {
        return;
    }

    ImGui::Begin(kOutlinerTitle, &state.ShowOutliner);

    // Toolbar first: the actions are what an author reaches for constantly, and hiding them below the list (as before)
    // meant scrolling to create an entity in a long scene.
    if (ImGui::Button("Entity"))
    {
        static_cast<void>(context.Application->CreateEntity("Entity", ""));
    }
    ImGui::SameLine();
    if (ImGui::Button("Cube"))
    {
        static_cast<void>(context.Application->CreateEntity("Cube", "engine:cube"));
    }
    ImGui::SameLine();

    const bool canDelete = !state.Selection.IsEmpty();
    if (!canDelete)
    {
        ImGui::BeginDisabled();
    }
    // Delete goes through the application, like every other mutation, so the button and `entity.destroy` cannot
    // diverge — and so the selection is cleared by the same code either way.
    if (ImGui::Button("Delete") && canDelete)
    {
        context.Application->DestroySelected();
    }
    if (!canDelete)
    {
        ImGui::EndDisabled();
    }
    ImGui::SetItemTooltip("Deletes the selection with no undo (notes/EDITOR.md §5).");

    char filter[256];
    FillBuffer(state.OutlinerFilter, filter, sizeof(filter));
    if (ImGui::InputTextWithHint("##filter", "Filter", filter, sizeof(filter)))
    {
        state.OutlinerFilter = filter;
    }
    ImGui::SetItemTooltip("Case-insensitive substring match on entity names.");

    // Rows are gathered first, then drawn. Iterating the world while widgets are live would let a widget callback run
    // code that mutates the world mid-iteration (a rename commits text, which is user-visible code in a scripting
    // build) — and World's iteration contract says structural changes belong outside it.
    struct Row
    {
        Entity Handle;
        std::string Label;
        const char* Icon = "actor_empty";  //!< atlas glyph for this row's type
    };
    std::vector<Row> rows;

    // Icon decisions go through typed component lookups; nothing is cached per frame because the world's storage map is
    // already indexed by type (see OutlinerIconFor for why name matching cannot work here).
    context.SceneWorld->ForEachEntity([&](const Entity& entity) {
        if (!context.SceneWorld->IsAlive(entity))
        {
            return;
        }
        std::string label = FormatEntityLabel(*context.SceneWorld, entity);
        if (ContainsIgnoreCase(label, state.OutlinerFilter))
        {
            rows.push_back(Row{entity, std::move(label), OutlinerIconFor(*context.SceneWorld, entity)});
        }
    });

    // A single Name column: the slot number the old panel showed is an implementation detail no author reads, and it
    // took a third of the panel's width. Slots remain available in `entity.inspect` and the status bar.
    if (ImGui::BeginTable("entities", 1,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY
                              | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name");
        ImGui::TableHeadersRow();

        for (const Row& row : rows)
        {
            ImGui::TableNextRow();

            char label[512];
            std::snprintf(label, sizeof(label), "%s##ent%llu", row.Label.c_str(),
                          static_cast<unsigned long long>(row.Handle.Slot));

            ImGui::TableNextColumn();
            // Icon then name, with the selectable spanning both: clicking anywhere on the row selects. The glyph is
            // drawn as part of the row (not in its own column) so a narrow panel still shows the whole label.
            const float iconSize = IconSizeForScale(state.UiScale, 14.0f);
            if (DrawIcon(context.Icons, row.Icon, iconSize))
            {
                ImGui::SameLine();
            }
            // Click selects, Ctrl+click toggles — the behaviour every outliner has, and both routes go through
            // EditorSelection so the viewport, the console and this list can never disagree about what is selected.
            if (ImGui::Selectable(label, state.Selection.IsSelected(row.Handle), ImGuiSelectableFlags_SpanAllColumns))
            {
                if (ImGui::GetIO().KeyCtrl)
                {
                    state.Selection.Toggle(row.Handle);
                }
                else
                {
                    state.Selection.Select(row.Handle);
                }
            }
        }

        ImGui::EndTable();
    }

    // Footer: counts, because "is the filter hiding my entity or did I delete it" should not require guessing.
    const Size total = context.SceneWorld->GetAliveEntityCount();
    ImGui::TextDisabled("%zu of %zu  ·  %zu selected", static_cast<size_t>(rows.size()), static_cast<size_t>(total),
                        static_cast<size_t>(state.Selection.GetCount()));

    ImGui::End();
}

void DrawDetailsPanel(UiState& state, const PanelContext& context)
{
    if (!state.ShowDetails)
    {
        return;
    }

    ImGui::Begin(kDetailsTitle, &state.ShowDetails);

    const Entity primary = state.Selection.GetPrimary();
    if (primary.IsNull() || !context.SceneWorld->IsAlive(primary))
    {
        ImGui::TextDisabled("Nothing selected.");
        ImGui::TextWrapped("Click an object in the viewport or a row in the outliner. Right-drag to fly, middle-drag to "
                           "orbit, wheel to zoom, F to frame the selection.");
        ImGui::End();
        return;
    }

    // Rebuilt only when the selection changed or the world's structure moved (a component added or removed) — those are
    // the two ways a property list can go stale, and both are cheap to check every frame.
    const UInt64 structure = context.SceneWorld->GetStructureVersion();
    if (!(state.PropertiesBuiltFor == primary) || state.PropertiesBuiltAtStructure != structure)
    {
        state.Properties.Build(*context.SceneWorld, primary);
        state.PropertiesBuiltFor = primary;
        state.PropertiesBuiltAtStructure = structure;
    }

    ImGui::TextUnformatted(FormatEntityLabel(*context.SceneWorld, primary).c_str());
    ImGui::Separator();

    // Label column width: wide enough for the longest path this entity actually has, clamped so a pathological name
    // cannot squeeze every control to nothing. Computed per entity rather than fixed because components vary wildly and
    // a constant either wastes space or truncates.
    float labelWidth = 120.0f;
    for (const PropertyField& field : state.Properties.GetFields())
    {
        labelWidth = std::max(labelWidth, ImGui::CalcTextSize(field.Path.c_str()).x + 12.0f);
    }
    labelWidth = std::min(labelWidth, ImGui::GetContentRegionAvail().x * 0.55f);

    for (const std::string& componentName : state.Properties.GetComponentNames())
    {
        if (!ImGui::CollapsingHeader(componentName.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            continue;
        }

        for (const PropertyField& field : state.Properties.GetFieldsForComponent(componentName))
        {
            // PushID on the path: two components can carry same-named fields (Position on a transform and on a camera),
            // and ImGui ids are per-window — without this they collide and editing one moves the other.
            ImGui::PushID(field.Path.c_str());

            if (!field.IsEditable())
            {
                ImGui::TextDisabled("%s", state.Properties.FormatValue(field).c_str());
                ImGui::PopID();
                continue;
            }

            switch (static_cast<ValueKind>(field.Kind))
            {
                case ValueKind::Boolean:
                {
                    bool value = false;
                    static_cast<void>(state.Properties.ReadBool(field, value));
                    BeginFieldRow(field.Path, labelWidth);
                    if (ImGui::Checkbox("##v", &value))
                    {
                        static_cast<void>(state.Properties.WriteBool(field, value));
                    }
                    EndFieldRow();
                    break;
                }

                case ValueKind::String:
                {
                    std::string text;
                    static_cast<void>(state.Properties.ReadString(field, text));
                    char buffer[512];
                    FillBuffer(text, buffer, sizeof(buffer));
                    BeginFieldRow(field.Path, labelWidth);
                    // The field name is stripped from the row's storage id so the visible label is the path alone;
                    // "##v" keeps the widget id distinct from the TextUnformatted that precedes it.
                    if (ImGui::InputText("##v", buffer, sizeof(buffer)))
                    {
                        static_cast<void>(state.Properties.WriteString(field, buffer));
                    }
                    EndFieldRow();
                    break;
                }

                case ValueKind::Enum:
                {
                    // A combo, not an integer spinner. The registry carries the enumerator names (the serialiser uses
                    // them), so showing `0` where `Back` is available was a lost call rather than a missing capability.
                    const std::vector<std::pair<Int64, std::string>> entries = EnumEntries(field);

                    Int64 raw = 0;
                    static_cast<void>(state.Properties.ReadEnum(field, raw));

                    int currentIndex = -1;
                    for (Size index = 0; index < entries.size(); ++index)
                    {
                        if (entries[index].first == raw)
                        {
                            currentIndex = static_cast<int>(index);
                            break;
                        }
                    }

                    BeginFieldRow(field.Path, labelWidth);
                    const char* preview = currentIndex >= 0 ? entries[static_cast<Size>(currentIndex)].second.c_str()
                                                            : "(value not in enum)";
                    if (ImGui::BeginCombo("##v", preview))
                    {
                        for (Size index = 0; index < entries.size(); ++index)
                        {
                            bool selected = static_cast<int>(index) == currentIndex;
                            if (ImGui::Selectable(entries[index].second.c_str(), &selected))
                            {
                                static_cast<void>(state.Properties.WriteEnum(field, entries[index].first));
                            }
                        }
                        ImGui::EndCombo();
                    }
                    EndFieldRow();

                    if (entries.empty())
                    {
                        // No enumerable names (a type registered without RegisterEnum): fall back to the integer so the
                        // value is at least reachable rather than showing an empty combo.
                        int index = static_cast<int>(raw);
                        BeginFieldRow(" ", labelWidth);
                        if (ImGui::InputInt("##v", &index))
                        {
                            static_cast<void>(state.Properties.WriteEnum(field, static_cast<Int64>(index)));
                        }
                        EndFieldRow();
                    }
                    break;
                }

                case ValueKind::Float32:
                case ValueKind::Float64:
                case ValueKind::Int8:
                case ValueKind::Int16:
                case ValueKind::Int32:
                case ValueKind::Int64:
                case ValueKind::UInt8:
                case ValueKind::UInt16:
                case ValueKind::UInt32:
                case ValueKind::UInt64:
                {
                    // ImGui has no 64-bit drag widget, so a reflected double is narrowed to float for editing and widened
                    // back on write — a real limitation of the widget, not of the storage behind it.
                    double existing = 0.0;
                    static_cast<void>(state.Properties.ReadReal(field, existing));
                    float value = static_cast<float>(existing);

                    BeginFieldRow(field.Path, labelWidth);
                    if (ImGui::DragFloat("##v", &value, DragSpeedFor(field.Path), 0.0f, 0.0f,
                                         DragFormatFor(static_cast<ValueKind>(field.Kind))))
                    {
                        static_cast<void>(state.Properties.WriteReal(field, value));
                    }
                    EndFieldRow();
                    break;
                }

                default:
                {
                    // Any kind reflection gains later: shown formatted rather than dropped. A read-only row is honest
                    // about what the panel does not edit yet; silently omitting it is not.
                    BeginFieldRow(field.Path, labelWidth);
                    ImGui::TextDisabled("%s", state.Properties.FormatValue(field).c_str());
                    EndFieldRow();
                    break;
                }
            }

            ImGui::PopID();
        }
    }

    ImGui::End();
}

namespace
{

//! Content Browser glyph for an entry, chosen from its extension.
/**
 * Extension, not IsEngineAsset: the atlas distinguishes scene / script / mesh / texture, and those are the categories
 * someone scans a folder for. Anything unrecognised gets the generic document rather than nothing, so rows stay aligned
 * and an unfamiliar file still reads as "a file here" instead of a gap.
 */
[[nodiscard]] const char* ContentIconFor(const ContentEntry& entry)
{
    if (entry.IsDirectory)
    {
        return "group_folder";
    }

    const std::size_t dot = entry.Name.rfind('.');
    if (dot == std::string::npos)
    {
        return "asset_generic";
    }

    std::string extension = entry.Name.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (extension == "hpscene")
    {
        return "asset_scene";
    }
    if (extension == "lua")
    {
        return "asset_script";
    }
    if (extension == "glb" || extension == "gltf" || extension == "obj" || extension == "fbx")
    {
        return "asset_mesh";
    }
    if (extension == "png" || extension == "jpg" || extension == "jpeg" || extension == "tga" || extension == "hdr"
        || extension == "exr" || extension == "xcf")
    {
        return "asset_texture";
    }
    return "asset_generic";
}

}  // namespace

void DrawContentPanel(UiState& state, const PanelContext& context)
{
    if (!state.ShowContent)
    {
        return;
    }

    ImGui::Begin(kContentTitle, &state.ShowContent);

    // Toolbar row: navigate up, refresh, and a search box — the three things a content browser is actually used for,
    // all reachable without hunting. The raw root path that used to be the panel's first line is now a tooltip on the
    // breadcrumb-style label, because it is information you check occasionally, not something to burn a row on.
    const bool canGoUp = state.Content.CanNavigateUp();
    if (!canGoUp)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::ArrowButton("up", ImGuiDir_Up) && canGoUp)
    {
        std::string error;
        if (!state.Content.NavigateUp(&error))
        {
            state.AddLog("! " + error);
        }
    }
    if (!canGoUp)
    {
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Already at the project root.");
    }
    else
    {
        ImGui::SetItemTooltip("Up one folder.");
    }

    ImGui::SameLine();
    if (ImGui::ArrowButton("refresh", ImGuiDir_Right))
    {
        std::string error;
        if (!state.Content.Refresh(std::filesystem::path{}, &error))
        {
            state.AddLog("! " + error);
        }
    }
    ImGui::SetItemTooltip("Re-read this folder.");

    ImGui::SameLine();
    const std::string currentFolder = state.Content.GetCurrentPath().filename().string();
    ImGui::TextUnformatted(currentFolder.empty() ? "/" : currentFolder.c_str());
    ImGui::SetItemTooltip("%s", state.Content.GetCurrentPath().string().c_str());

    char filter[256];
    FillBuffer(state.ContentFilter, filter, sizeof(filter));
    if (ImGui::InputTextWithHint("##search", "Search", filter, sizeof(filter)))
    {
        state.ContentFilter = filter;
    }

    const std::vector<Size> visible = state.Content.FilterByName(state.ContentFilter);

    if (ImGui::BeginTable("files", 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable
                              | ImGuiTableFlags_BordersInnerV))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();

        for (const Size index : visible)
        {
            const ContentEntry& entry = state.Content.GetEntries()[index];
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            const bool selected = entry.Name == state.SelectedContentName;
            const std::string rowLabel = entry.Name + "##file" + std::to_string(index);
            // Icon then name, like the outliner: scanning a folder is a shape-matching task, and an all-text column
            // forces reading every extension to know what a file is.
            if (DrawIcon(context.Icons, ContentIconFor(entry), IconSizeForScale(state.UiScale, 14.0f)))
            {
                ImGui::SameLine();
            }
            if (ImGui::Selectable(rowLabel.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
            {
                // Single click selects by *name*: a refresh re-sorts the list, so an index would highlight a different
                // file than the one clicked. Opening stays on double-click so an accidental click never loads a scene
                // over unsaved work.
                state.SelectedContentName = entry.Name;
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                std::string message;
                if (state.Content.OpenEntry(entry, message))
                {
                    context.Application->OpenProjectFile(message);
                }
                else
                {
                    state.AddLog("! " + message);
                }
            }

            ImGui::TableNextColumn();
            if (entry.IsDirectory)
            {
                ImGui::TextDisabled("/");
            }
            else
            {
                ImGui::TextDisabled("%s", FormatByteSize(entry.SizeBytes).c_str());
            }
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

void DrawConsolePanel(UiState& state, const PanelContext& context)
{
    if (!state.ShowConsole)
    {
        return;
    }

    ImGui::Begin(kConsoleTitle, &state.ShowConsole);

    const float lineHeight = ImGui::GetTextLineHeight();
    if (ImGui::BeginChild("log", ImVec2(0.0f, -lineHeight * 3.0f), ImGuiChildFlags_Borders))
    {
        for (const std::string& line : state.Log)
        {
            if (!line.empty() && line[0] == '!')
            {
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.55f, 1.0f), "%s", line.c_str());
            }
            else if (!line.empty() && line[0] == '>')
            {
                ImGui::TextColored(ImVec4(0.62f, 0.80f, 1.0f, 1.0f), "%s", line.c_str());
            }
            else
            {
                ImGui::TextUnformatted(line.c_str());
            }
        }

        // Follows output only when the reader is already at the bottom; scrolling up to read history and having the
        // view yanked down by the next log line is the single most annoying thing about most in-app consoles.
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();

    // Completion runs through InputText's completion callback rather than a separate IsKeyPressed check. The previous
    // version polled Tab while reading state.ConsoleInput — which is only written when the box *commits*, so it
    // completed whatever was typed before the current edit and could not work as written. The callback gets the live
    // buffer, which is the only correct source.
    ConsoleCompletionContext completion{state};

    char input[512];
    FillBuffer(state.ConsoleInput, input, sizeof(input));
    if (ImGui::InputTextWithHint(
            "##command", "command   Tab completes, `help` lists all", input, sizeof(input),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll
                | ImGuiInputTextFlags_CallbackCompletion,
            &ConsoleCompletionCallback, &completion))
    {
        state.Run(input);
        state.ConsoleInput.clear();
    }

    ImGui::SameLine();
    if (ImGui::Button("Run"))
    {
        // Runs whatever is in the buffer right now rather than only on Enter, so the mouse path and the keyboard path
        // cannot disagree about what "execute this line" means.
        state.Run(input);
        state.ConsoleInput.clear();
    }
    ImGui::SetItemTooltip("Execute the text above (same as Enter).");

    static_cast<void>(context);
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Viewport panel
// ---------------------------------------------------------------------------

namespace
{

//! Draws a world-space axis as a labelled line in the viewport's corner compass.
/**
 * The compass is the only orientation cue a 3D viewport has when nothing in the scene happens to be axis-aligned, and
 * it costs three projected lines. Projection is a plain dot product against the view basis — the same basis the
 * renderer built the view matrix from (GetViewBasis), which is why the indicator cannot drift out of agreement with what
 * you are looking at.
 */
void DrawCompassAxis(ImDrawList& drawList, const ImVec2& origin, const Vec3& axisWorld,
                     const ViewBasis& basis, float radius, ImU32 colour, const char* label)
{
    // Screen x = axis · right, screen y = -(axis · up): the negation is because screen y grows downwards while world
    // up grows upwards. Forgetting it mirrors the compass vertically, which reads as a projection bug rather than a sign
    // error and is maddening to trace.
    const ImVec2 direction(axisWorld.Dot(basis.Right), -axisWorld.Dot(basis.Up));
    const ImVec2 tip(origin.x + direction.x * radius, origin.y + direction.y * radius);

    drawList.AddLine(origin, tip, colour, 2.0f);
    drawList.AddCircleFilled(tip, 3.0f, colour);

    // The label sits just beyond the tip so it never overlaps the line it names.
    const ImVec2 labelPos(tip.x + direction.x * 9.0f - 4.0f, tip.y + direction.y * 9.0f - 6.0f);
    drawList.AddText(labelPos, IM_COL32(230, 230, 232, 255), label);
}

//! Overlays the viewport's compass and navigation hint using the panel's own draw list.
/**
 * Drawn after the Image item rather than in a reserved row below it: UE paints its overlays on top of the scene, and
 * reserving layout rows is what made the old viewport compute its size twice (once for the image, once for the text)
 * and get both wrong.
 */
void DrawViewportOverlay(const PanelContext& context, const ImVec2& imageMin, const ImVec2& imageSize)
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // A translucent plate behind the compass keeps it legible over a bright sky, which a bare line drawing never is.
    const ImVec2 compassCentre(imageMin.x + imageSize.x - 52.0f, imageMin.y + 46.0f);
    drawList->AddCircleFilled(compassCentre, 30.0f, IM_COL32(12, 13, 15, 140));

    const ViewBasis basis = GetViewBasis(context.CameraPosition, context.CameraTarget);
    DrawCompassAxis(*drawList, compassCentre, Vec3(1.0f, 0.0f, 0.0f), basis, 20.0f, IM_COL32(224, 92, 92, 255), "X");
    DrawCompassAxis(*drawList, compassCentre, Vec3(0.0f, 1.0f, 0.0f), basis, 20.0f, IM_COL32(120, 214, 120, 255), "Y");
    DrawCompassAxis(*drawList, compassCentre, Vec3(0.0f, 0.0f, 1.0f), basis, 20.0f, IM_COL32(108, 156, 244, 255), "Z");

    // Navigation hint, top-left, only while navigating or when the pointer is over the viewport — a permanent caption
    // would be noise after the first minute and the point is to teach the controls, not decorate the panel.
    ImGui::SetCursorScreenPos(ImVec2(imageMin.x + 10.0f, imageMin.y + 8.0f));
    if (context.ViewportHintVisible)
    {
        ImGui::TextColored(ImVec4(0.75f, 0.78f, 0.83f, 0.92f),
                           "RMB fly · MMB orbit · Shift sprint · wheel zoom · LMB select · F frame");
    }
}

}  // namespace

void DrawViewportPanel(UiState& state, const PanelContext& context)
{
    ImGui::Begin(kViewportTitle);

    // The image takes the panel's entire remaining content area. This is the fix for the crop: previously the scene was
    // rendered at an *estimated* window-minus-panels extent and then drawn at that fixed pixel size, so it overflowed the
    // panel by ~130 px, got clipped, and sprouted two scrollbars — while being projected at an aspect ratio nobody could
    // see. Measuring here means the render target and the drawn rect agree by construction.
    ImVec2 available = ImGui::GetContentRegionAvail();
    if (available.x < 1.0f)
    {
        available.x = 1.0f;
    }
    if (available.y < 1.0f)
    {
        available.y = 1.0f;
    }

    const ImVec2 imageMin = ImGui::GetCursorScreenPos();

    // No scrollbars, ever: the image is sized to fit, so a scrollbar here would only appear if the sizing logic broke —
    // and when it did before, it hid half the scene behind itself.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    if (context.ViewportTextureId != 0ull)
    {
        ImGui::Image(ImTextureRef{static_cast<ImTextureID>(context.ViewportTextureId)}, available);
    }
    else
    {
        // Still draw the placeholder as a filled rect so the panel has an identifiable extent on frame one rather than a
        // line of text at the origin.
        ImGui::Dummy(available);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(imageMin, ImVec2(imageMin.x + available.x, imageMin.y + available.y),
                                IM_COL32(16, 17, 19, 255));
        drawList->AddText(ImVec2(imageMin.x + 12.0f, imageMin.y + 12.0f), IM_COL32(160, 166, 176, 255),
                          "Viewport initialising...");
    }

    ImGui::PopStyleVar();

    // Geometry and hover state, recorded for the application (camera navigation) and for next frame's target sizing.
    ViewportInteraction& viewport = state.Viewport;
    viewport.ImageScreenLeft = imageMin.x;
    viewport.ImageScreenTop = imageMin.y;
    viewport.ImageWidth = available.x;
    viewport.ImageHeight = available.y;

    // Hover is IsItemHovered on the image item: it is already false when a popup, tooltip or another window covers the
    // point, which is what stops right-drag-flying while the cursor happens to be over an open menu. The extra
    // WantCaptureMouse test covers the other half — a widget in *another* panel that owns an in-progress drag (a slider
    // in the inspector) must keep its mouse even when the pointer travels back over the viewport.
    // AllowWhenBlockedByActiveItem is needed because a navigation gesture starts by pressing a button we handle
    // ourselves, and without it ImGui would report "not hovered" from the second frame of the drag onwards.
    viewport.PointerOverImage =
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && !ImGui::GetIO().WantCaptureMouse;

    DrawViewportOverlay(context, imageMin, available);

    // --- input: read here (after NewFrame, where ImGui's edge detection is defined), apply next frame in the app ----
    const ImGuiIO& io = ImGui::GetIO();

    if (viewport.PointerOverImage)
    {
        // Left click selects. It goes straight through the application's own picking entry point — the same one the
        // `pick.at` command uses — so a click and a typed coordinate can never disagree, and so the pick happens this
        // frame rather than one later (selection latency is far more noticeable than camera latency).
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && context.ViewportTextureId != 0ull)
        {
            const ImVec2 mouse = ImGui::GetMousePos();
            static_cast<void>(context.Application->PickAtViewportPixel(mouse.x - viewport.ImageScreenLeft,
                                                                       mouse.y - viewport.ImageScreenTop));
        }

        // Navigation gestures accumulate; EditorApplication applies them at the top of the next frame. One frame of
        // latency on camera motion is imperceptible and buys the panels their central rule: widgets never touch the
        // engine's state, they record what the author asked for.
        const bool flying = ImGui::IsMouseDown(ImGuiMouseButton_Right);
        const bool orbiting = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
        viewport.Navigating = flying || orbiting;

        if (flying)
        {
            ViewAxisInput& fly = viewport.Fly;
            if (ImGui::IsKeyDown(ImGuiKey_W) || ImGui::IsKeyDown(ImGuiKey_UpArrow))
            {
                fly.Forward += 1.0f;
            }
            if (ImGui::IsKeyDown(ImGuiKey_S) || ImGui::IsKeyDown(ImGuiKey_DownArrow))
            {
                fly.Forward -= 1.0f;
            }
            if (ImGui::IsKeyDown(ImGuiKey_D))
            {
                fly.Right += 1.0f;
            }
            if (ImGui::IsKeyDown(ImGuiKey_A))
            {
                fly.Right -= 1.0f;
            }
            if (ImGui::IsKeyDown(ImGuiKey_Q))
            {
                fly.Up -= 1.0f;
            }
            if (ImGui::IsKeyDown(ImGuiKey_E))
            {
                fly.Up += 1.0f;
            }
            fly.Sprint = ImGui::IsKeyDown(ImGuiMod_Shift);
        }

        if (orbiting)
        {
            // Shift+middle pans, plain middle orbits — the same split UE uses, so muscle memory transfers without a
            // settings page.
            const ImVec2 delta = io.MouseDelta;
            if (ImGui::IsKeyDown(ImGuiMod_Shift))
            {
                viewport.PanPixelsX += delta.x;
                viewport.PanPixelsY += delta.y;
            }
            else
            {
                viewport.OrbitPixelsX += delta.x;
                viewport.OrbitPixelsY += delta.y;
            }
        }

        if (io.MouseWheel != 0.0f)
        {
            viewport.DollyNotches += io.MouseWheel;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_F))
        {
            viewport.RequestFrameSelection = true;
        }
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Main toolbar
//
// Only wired actions appear here, deliberately: no New/Open (there is no file dialog and no new-scene command yet), no
// Lit/Unlit/Wireframe (shading mode is not plumbed through the renderer — notes/EDITOR_UI_REVIEW.md F4). A greyed or
// dead button in a toolbar is worse than an absent one, because it advertises a feature that will fail when clicked,
// and this editor's history has exactly one bug of that shape (a Viewport menu whose items all said "pick at"). Each
// item runs the same console command as its menu twin so there is one implementation per action.
// ---------------------------------------------------------------------------

void DrawMainToolBar(UiState& state, const PanelContext& context)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float height = ToolbarHeight();
    const float iconSize = ToolbarIconSize(state);
    const EditorIconSet* icons = context.Icons;

    // Positioned from the menu bar window's measured bottom edge — see MeasureChrome, which exists because both
    // plausible ways of computing this (WorkPos.y alone, or WorkPos.y + GetFrameHeight()) were wrong here in different
    // directions.
    const ChromeStrips strips = MeasureChrome(height);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, strips.BelowMenuBar));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * state.UiScale, 3.0f * state.UiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f * state.UiScale, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg]);

    if (ImGui::Begin("##MainToolBar", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                         | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings
                         | ImGuiWindowFlags_NoNavFocus))
    {
        // Vertical rule between groups. IconButton already leaves the cursor on the same row, so this must not call
        // SameLine itself — doing it twice moves the next item past its intended slot.
        auto separator = []() {
            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
            ImGui::SameLine();
        };

        // --- Edit actions ---------------------------------------------------------------------------
        if (IconButton(icons, "actor_cube", "create-cube", "Create a cube actor", iconSize))
        {
            state.Run("entity.create Cube engine:cube");
        }
        const bool hasSelection = !state.Selection.IsEmpty();
        if (IconButton(icons, "close_x", "delete-selected", "Delete the selection (no undo)", iconSize, hasSelection))
        {
            state.Run("entity.destroy");
        }
        if (IconButton(icons, "save", "save-scene", "Save the scene (Ctrl+S)", iconSize))
        {
            state.Run("scene.save");
        }

        separator();

        // --- Script play state ----------------------------------------------------------------------
        // Labels say "scripts" on purpose: this resumes the Lua tick only. There is no PIE snapshot to restore and no
        // undo boundary, so Stop can leave the world mutated (notes/EDITOR.md §2) — a control that implied otherwise
        // would be a lie in the most-used button in the window.
        const bool playing = context.ScriptsPlaying;
        if (IconButton(icons, playing ? "pause" : "play", "scripts-toggle",
                  playing ? "Pause the gameplay scripts" : "Run the gameplay scripts (no PIE snapshot)",
                  iconSize))
        {
            state.Run(std::string("editor.scripts.") + (playing ? "pause" : "play"));
        }
        if (IconButton(icons, "stop", "scripts-stop", "Stop the scripts; mutations made while running are kept", iconSize, playing))
        {
            state.Run("editor.scripts.stop");
        }

        separator();

        // --- View / layout --------------------------------------------------------------------------
        if (IconButton(icons, "view_bookmark", "frame-selected", "Frame the selection in the viewport (F)", iconSize, hasSelection))
        {
            state.Run("view.frame");
        }
        if (IconButton(icons, "settings", "layout-reset", "Restore the default docked layout", iconSize))
        {
            state.RequestLayoutReset = true;
        }
        if (IconButton(icons, "search", "content-list", "List content browser entries in the Output Log", iconSize))
        {
            state.Run("content.list");
        }

        // --- Right-aligned: UI scale ----------------------------------------------------------------
        // Scale on the toolbar because it is a per-machine setting people adjust while working; UE puts its equivalent
        // in Edit > Editor Preferences, which is three menus away from "that text is too small". Plain -/+ buttons: no
        // glyph in the set means "zoom", and inventing one by reusing an unrelated icon would be worse than text.
        ImGui::SameLine(viewport->WorkSize.x - 150.0f * state.UiScale);
        if (ImGui::Button("-"))
        {
            state.Run("ui.scale " + std::to_string(std::max(0.75f, state.UiScale - 0.1f)));
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%.2fx", static_cast<double>(state.UiScale));
        ImGui::SameLine();
        if (ImGui::Button("+"))
        {
            state.Run("ui.scale " + std::to_string(std::min(2.0f, state.UiScale + 0.1f)));
        }
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void DrawMenuBar(UiState& state, const PanelContext& context)
{
    if (!ImGui::BeginMainMenuBar())
    {
        return;
    }

    // Menus are grouped the way UE groups them, because that is where someone who has never seen this editor will look
    // first. Every item runs a console command rather than calling engine code directly: one implementation per action,
    // and `help` stays an honest index of everything the editor can do.
    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("Save Scene", "Ctrl+S"))
        {
            state.Run("scene.save");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit"))
        {
            state.Run("editor.quit");
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit"))
    {
        if (ImGui::MenuItem("Create Entity"))
        {
            state.Run("entity.create");
        }
        if (ImGui::MenuItem("Create Cube"))
        {
            state.Run("entity.create Cube engine:cube");
        }
        ImGui::Separator();
        const bool hasSelection = !state.Selection.IsEmpty();
        if (!hasSelection)
        {
            ImGui::BeginDisabled();
        }
        if (ImGui::MenuItem("Delete Selected", "Del") && hasSelection)
        {
            state.Run("entity.destroy");
        }
        if (!hasSelection)
        {
            ImGui::EndDisabled();
        }
        ImGui::Separator();
        // Present and disabled rather than absent: undo is the highest-value missing feature (notes/EDITOR.md §5), and a
        // greyed item says "this tool knows you want it" far more usefully than its absence.
        if (ImGui::MenuItem("Undo", "Ctrl+Z", nullptr, false))
        {
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View"))
    {
        if (ImGui::MenuItem("Frame Selection", "F"))
        {
            state.Run("view.frame");
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Window"))
    {
        // Labels match the window titles exactly: these checkboxes toggle panels by state flag, but a person reading
        // "Console" in a menu and "Output Log" on a tab has to do the mapping themselves, and gets it wrong often
        // enough that it reads as two different things.
        ImGui::Checkbox("World Outliner", &state.ShowOutliner);
        ImGui::Checkbox("Details", &state.ShowDetails);
        ImGui::Checkbox("Content Browser", &state.ShowContent);
        ImGui::Checkbox("Output Log", &state.ShowConsole);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout"))
        {
            state.Run("layout.reset");
        }
        ImGui::EndMenu();
    }

    // The frame counter that used to live here has moved to the status bar, where there is room for it alongside the
    // numbers that actually matter (draw calls, selection, camera) — the menu bar is for commands.
    static_cast<void>(context);
    ImGui::EndMainMenuBar();

    // Ctrl+S handled as a shortcut rather than only the menu's label text: MenuItem's second argument is display-only,
    // and a label that promises a binding without implementing one is worse than no label.
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S))
    {
        state.Run("scene.save");
    }
}

void DrawStatusBar(UiState& state, const PanelContext& context)
{
    // A bottom strip fixed to the window: mode on the left, live numbers on the right. This is where frame/draw/selection
    // information belongs — visible without opening a panel, and it makes SceneDrawCalls (computed every frame by the
    // renderer and previously drawn by nobody) actually useful.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float barHeight = StatusBarHeight();

    // Positioned over the gap the dockspace left (SubmitDockspace subtracts the same value), so the bar is adjacent to
    // the panels rather than on top of them.
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - barHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, barHeight));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * state.UiScale, 2.0f * state.UiScale));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg]);

    // NoNavFocus: without it a click on the status bar makes it the navigation window, and keyboard input (WASD flight,
    // F to frame) then routes to a strip of text that can do nothing with it.
    if (ImGui::Begin("##StatusBar", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                         | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus
                         | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings))
    {
        const Vec3 camera = context.CameraPosition;
        char left[160];
        std::snprintf(left, sizeof(left), "%s  ·  %s  ·  %s",
                      context.ScriptsPlaying ? "Scripts running" : "Edit mode",
                      state.Selection.IsEmpty() ? "no selection" : "selected",
                      context.ActiveSceneName.empty() ? "unnamed" : context.ActiveSceneName.c_str());
        ImGui::TextUnformatted(left);

        ImGui::SameLine();
        char numbers[256];
        std::snprintf(numbers, sizeof(numbers),
                      "%llu draw calls  ·  %zu entities  ·  cam %.1f, %.1f, %.1f  ·  %.0f fps",
                      static_cast<unsigned long long>(context.SceneDrawCalls),
                      static_cast<size_t>(context.SceneWorld != nullptr ? context.SceneWorld->GetAliveEntityCount() : 0u),
                      static_cast<double>(camera.X), static_cast<double>(camera.Y), static_cast<double>(camera.Z),
                      static_cast<double>(context.FramesPerSecond));
        ImGui::SameLine(ImGui::GetWindowWidth() * 0.42f);
        ImGui::TextDisabled("%s", numbers);
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

std::vector<PanelRect> CollectPanelRectangles()
{
    // The same titles the panels Begin() with — one list, so a rename cannot make the diagnostic look for a window that
    // no longer exists and report "not created" for a panel that is on screen.
    static const char* const kTitles[] = {kViewportTitle, kOutlinerTitle, kDetailsTitle, kContentTitle, kConsoleTitle};

    std::vector<PanelRect> rectangles;
    rectangles.reserve(std::size(kTitles));

    if (ImGui::GetCurrentContext() == nullptr)
    {
        for (const char* title : kTitles)
        {
            PanelRect rect;
            rect.Name = title;
            rectangles.push_back(rect);
        }
        return rectangles;
    }

    for (const char* title : kTitles)
    {
        PanelRect result;
        result.Name = title;

        if (const ImGuiWindow* window = ImGui::FindWindowByName(title))
        {
            // A window that exists but was not submitted this frame is reported as 0x0, not with its stale rectangle.
            // ImGui keeps the last layout of a hidden window, so reporting it verbatim makes "the panel is hidden" and
            // "the panel is on screen at 320x700" indistinguishable — in this diagnostic *and* in any test built on it,
            // which is precisely how a reset-layout assertion ended up passing while the panel was still hidden.
            if (window->Hidden || !window->WasActive)
            {
                rectangles.push_back(result);
                continue;
            }

            const ImRect bounds = window->Rect();
            result.X = bounds.Min.x;
            result.Y = bounds.Min.y;
            result.Width = bounds.GetWidth();
            result.Height = bounds.GetHeight();
        }

        rectangles.push_back(result);
    }

    return rectangles;
}

void DrawEditorPanels(UiState& state, const PanelContext& context)
{
    // The dockspace fills the window and every panel docks into it (see BuildDefaultDockLayout). Rebuild on request —
    // frame one, or `layout.reset` / Window > Reset Layout — which is what makes an author's scattered arrangement
    // recoverable; before this existed, scattering panels was permanent.
    // The menu bar goes first: it is the strip whose height everything else is offset by, and ImGui only knows a
    // window's submitted size after it has been drawn — measuring before Begin-ing it returns null.
    DrawMenuBar(state, context);

    // Both strips sit above the dock area, so the inset is their sum. Measured (not assumed) because the menu bar's
    // height is not GetFrameHeight(): passing only one of them left the toolbar painted over the top panel row, which
    // showed up as an outliner whose first rows could not be clicked.
    SubmitDockspace(state.RequestLayoutReset, StatusBarHeight(), MeasureChrome(ToolbarHeight()).DockTopInset);
    state.RequestLayoutReset = false;

    DrawMainToolBar(state, context);

    // Order matters only for focus/stacking: the viewport first so it is the default focus target (and therefore gets
    // keyboard input) when nothing else was clicked.
    DrawViewportPanel(state, context);
    DrawOutlinerPanel(state, context);
    DrawDetailsPanel(state, context);
    DrawContentPanel(state, context);
    DrawConsolePanel(state, context);

    // Snapshot geometry last, after every panel has submitted its window, so the rectangles reflect this frame's layout
    // rather than the previous one (see UiState::PanelGeometry for why a snapshot at all).
    state.PanelGeometry = CollectPanelRectangles();
}

}  // namespace Hollowpoint::Editor
