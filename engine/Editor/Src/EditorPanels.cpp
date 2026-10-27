#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Editor/ContentBrowserModel.h"
#include "Hollowpoint/Editor/EditorApplication.h"
#include "Hollowpoint/Editor/EditorConsole.h"
#include "Hollowpoint/Editor/EditorPanels.h"
#include "Hollowpoint/Editor/EditorSelection.h"
#include "Hollowpoint/Editor/EntityName.h"
#include "Hollowpoint/Editor/PropertyView.h"
#include "Hollowpoint/Reflection/Descriptors.h"
#include "Hollowpoint/Scene/LocalTransform.h"

#include <imgui.h>

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

//! Nods a [[nodiscard]] result away without HP_UNUSED: that macro expands through sizeof, which does not evaluate,
//  so using it on a call would discard the call itself (Core/API.h says so in as many words).
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

//! Stable id for the editor's dockspace. Never 0: ImGui treats a dockspace created with id 0 differently from one it
//! assigns itself, and passing GetID("...") here would make the layout depend on window-list order.
constexpr ImGuiID kDockSpaceId = 0x4850444BU;  // 'HPDK'

//! Places the next window at `rect` (content units) if it has never been positioned.
/**
 * ImGuiCond_FirstUseEver rather than Always: an author's move or dock must survive the next frame, while a window with
 * no saved state — this editor writes no ini file — needs *somewhere* to start. Without it every panel opens at ImGui's
 * cascade position stacked on the others, so a fresh editor's first screenshot shows one panel and four invisible ones,
 * which is exactly the class of failure this project's "verify rendered output" rule exists to catch (Session 16).
 */
void PlaceInitially(float x, float y, float width, float height)
{
    if (width <= 0.0f || height <= 0.0f)
    {
        return;  // nothing sane to lay out against; let ImGui place it and try again next frame
    }

    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_FirstUseEver);
}

//! Default docked layout, as fractions of the window. ImGui writes no ini file (see EditorApplication::Start), so these
// are the layout every fresh editor session starts from; an author's own arrangement then overrides it for that
// session.
constexpr float kSidePanelFraction = 0.24f;
constexpr float kBottomPanelFraction = 0.30f;

[[nodiscard]] float SideWidth(const PanelContext& context)
{
    return std::clamp(context.ContentWidth * kSidePanelFraction, 220.0f, 520.0f);
}

[[nodiscard]] float BottomHeight(const PanelContext& context)
{
    return std::clamp(context.ContentHeight * kBottomPanelFraction, 130.0f, 460.0f);
}

//! Height taken by the main menu bar, which is drawn before every panel so ImGui already knows it here.
[[nodiscard]] float TopInset()
{
    return ImGui::GetFrameHeight();
}

[[nodiscard]] float PanelsTop(const PanelContext& context)
{
    return std::min(context.ContentHeight - 40.0f, TopInset());
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

}  // namespace

//! Registers the commands that operate on UI state (help, quit, select, info, panels). Scene/entity/content commands
//  live in EditorApplication.cpp because they need the hosted loop; splitting by dependency rather than by file size is
//  what lets either be understood — or stubbed — without the other. EditorApplication::RegisterEditorCommands calls
//  this and then adds its own, so callers see one registration entry point.
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

void DrawOutlinerPanel(UiState& state, const PanelContext& context)
{
    if (!state.ShowOutliner)
    {
        return;
    }

    PlaceInitially(0.0f, PanelsTop(context), SideWidth(context),
                   std::max(120.0f, context.ContentHeight - BottomHeight(context) - PanelsTop(context)));
    ImGui::Begin("Outliner", &state.ShowOutliner);

    char filter[256];
    FillBuffer(state.OutlinerFilter, filter, sizeof(filter));
    if (ImGui::InputText("Filter", filter, sizeof(filter)))
    {
        state.OutlinerFilter = filter;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Case-insensitive substring match on entity names.");
    }

    // Rows are gathered first, then drawn. Iterating the world while widgets are live would let a widget callback run
    // code that mutates the world mid-iteration (a rename commits text, which is user-visible code in a scripting
    // build) — and World's iteration contract says structural changes belong outside it.
    struct Row
    {
        Entity Handle;
        std::string Label;
    };
    std::vector<Row> rows;

    context.SceneWorld->ForEachEntity([&](const Entity& entity) {
        if (!context.SceneWorld->IsAlive(entity))
        {
            return;
        }
        std::string label = FormatEntityLabel(*context.SceneWorld, entity);
        if (ContainsIgnoreCase(label, state.OutlinerFilter))
        {
            rows.push_back(Row{entity, std::move(label)});
        }
    });

    if (ImGui::BeginTable("entities", 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Slot");
        ImGui::TableHeadersRow();

        for (const Row& row : rows)
        {
            ImGui::TableNextRow();
            const bool selected = state.Selection.IsSelected(row.Handle);

            char label[512];
            std::snprintf(label, sizeof(label), "%s##ent%llu", row.Label.c_str(),
                          static_cast<unsigned long long>(row.Handle.Slot));

            ImGui::TableNextColumn();
            // Click selects, Ctrl+click toggles — the behaviour every outliner has, and both routes go through
            // EditorSelection so the viewport, the console and this list can never disagree about what is selected.
            if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_SpanAllColumns))
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

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(std::to_string(row.Handle.Slot).c_str());
        }

        ImGui::EndTable();
    }

    ImGui::Separator();
    if (ImGui::Button("New Entity"))
    {
        static_cast<void>(context.Application->CreateEntity("Entity", ""));
    }
    ImGui::SameLine();
    if (ImGui::Button("New Cube"))
    {
        static_cast<void>(context.Application->CreateEntity("Cube", "engine:cube"));
    }
    ImGui::SameLine();

    const bool canDelete = !state.Selection.IsEmpty();
    if (!canDelete)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Delete") && canDelete)
    {
        for (const Entity& entity : state.Selection.GetEntities())
        {
            context.SceneWorld->DestroyEntity(entity);
        }
        // Cleared here rather than left to Prune: the selection vector holds handles whose slots are now recycled, and
        // although every read checks liveness, a stale handle surviving one extra frame is how "deleted the wrong
        // thing" becomes possible if a script creates an entity in between.
        state.Selection.Clear();
    }
    if (!canDelete)
    {
        ImGui::EndDisabled();
    }

    ImGui::End();
}

void DrawDetailsPanel(UiState& state, const PanelContext& context)
{
    if (!state.ShowDetails)
    {
        return;
    }

    PlaceInitially(std::max(0.0f, context.ContentWidth - SideWidth(context)), PanelsTop(context), SideWidth(context),
                   std::max(120.0f, context.ContentHeight - BottomHeight(context) - PanelsTop(context)));
    ImGui::Begin("Details", &state.ShowDetails);

    const Entity primary = state.Selection.GetPrimary();
    if (primary.IsNull() || !context.SceneWorld->IsAlive(primary))
    {
        ImGui::TextUnformatted("Nothing selected.");
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

    for (const std::string& componentName : state.Properties.GetComponentNames())
    {
        if (!ImGui::CollapsingHeader(componentName.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            continue;
        }

        for (const PropertyField& field : state.Properties.GetFieldsForComponent(componentName))
        {
            if (!field.IsEditable())
            {
                ImGui::TextDisabled("%s: %s", field.Path.c_str(), state.Properties.FormatValue(field).c_str());
                continue;
            }

            ImGui::PushID(field.Path.c_str());

            switch (static_cast<ValueKind>(field.Kind))
            {
                case ValueKind::Boolean:
                {
                    bool value = false;
                    static_cast<void>(state.Properties.ReadBool(field, value));
                    if (ImGui::Checkbox(field.Name.c_str(), &value))
                    {
                        static_cast<void>(state.Properties.WriteBool(field, value));
                    }
                    break;
                }

                case ValueKind::String:
                {
                    std::string text;
                    static_cast<void>(state.Properties.ReadString(field, text));
                    char buffer[512];
                    FillBuffer(text, buffer, sizeof(buffer));
                    if (ImGui::InputText(field.Name.c_str(), buffer, sizeof(buffer)))
                    {
                        static_cast<void>(state.Properties.WriteString(field, buffer));
                    }
                    break;
                }

                case ValueKind::Float32:
                case ValueKind::Float64:
                {
                    // ImGui has no 64-bit drag widget, so the reflected double is narrowed to a float for editing and
                    // widened back on write. A Float64 field edited through this panel therefore keeps float precision
                    // — stated because it is a real limitation of the widget, not of the storage behind it.
                    double existing = 0.0;
                    static_cast<void>(state.Properties.ReadReal(field, existing));
                    float value = static_cast<float>(existing);
                    // A drag rather than a slider: roughness wants a slider and a world position does not, and choosing
                    // per field is the per-type special-casing reflection exists to remove. FieldAttributes could carry
                    // ranges later without this code changing shape.
                    if (ImGui::DragFloat(field.Name.c_str(), &value, 0.01f))
                    {
                        static_cast<void>(state.Properties.WriteReal(field, value));
                    }
                    break;
                }

                case ValueKind::Enum:
                {
                    Int64 raw = 0;
                    static_cast<void>(state.Properties.ReadEnum(field, raw));
                    int index = static_cast<int>(raw);
                    if (ImGui::InputInt(field.Name.c_str(), &index))
                    {
                        static_cast<void>(state.Properties.WriteEnum(field, static_cast<Int64>(index)));
                    }
                    if (!field.EnumName.empty())
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(%s)", field.EnumName.c_str());
                    }
                    break;
                }

                default:
                {
                    // Integers of every width, and any kind reflection gains later: shown formatted rather than
                    // dropped. A read-only row is honest about what the panel does not edit yet; silently omitting it
                    // is not.
                    ImGui::TextUnformatted(state.Properties.FormatValue(field).c_str());
                    break;
                }
            }

            ImGui::PopID();
        }
    }

    ImGui::End();
}

void DrawContentPanel(UiState& state, const PanelContext& context)
{
    if (!state.ShowContent)
    {
        return;
    }

    // Bottom-left quadrant: the console owns the bottom-right, so the two split the strip rather than stacking.
    PlaceInitially(0.0f, std::max(0.0f, context.ContentHeight - BottomHeight(context)), SideWidth(context),
                   BottomHeight(context));
    ImGui::Begin("Content Browser", &state.ShowContent);

    ImGui::TextUnformatted(state.Content.GetRoot().string().c_str());

    if (ImGui::Button("Refresh"))
    {
        std::string error;
        if (!state.Content.Refresh(error))
        {
            state.AddLog("! " + error);
        }
    }
    ImGui::SameLine();
    const bool canGoUp = state.Content.CanNavigateUp();
    if (!canGoUp)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Up") && canGoUp)
    {
        state.Content.NavigateUp();
    }
    if (!canGoUp)
    {
        ImGui::EndDisabled();
    }

    char filter[256];
    FillBuffer(state.ContentFilter, filter, sizeof(filter));
    if (ImGui::InputText("Search", filter, sizeof(filter)))
    {
        state.ContentFilter = filter;
    }

    const std::vector<Size> visible = state.Content.FilterByName(state.ContentFilter);

    if (ImGui::BeginTable("files", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
    {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Type");
        ImGui::TableHeadersRow();

        for (const Size index : visible)
        {
            const ContentEntry& entry = state.Content.GetEntries()[index];
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            const std::string rowLabel = entry.Name + "##file" + std::to_string(index);
            if (ImGui::Selectable(rowLabel.c_str(), false, ImGuiSelectableFlags_SpanAllColumns))
            {
                // Single click only highlights; opening is the double-click below so an accidental click never loads a
                // scene over the author's unsaved work.
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
                ImGui::TextUnformatted("-");
            }
            else
            {
                ImGui::Text("%llu KB", static_cast<unsigned long long>(entry.SizeBytes / 1024ull));
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(entry.IsDirectory ? "folder" : (entry.IsEngineAsset ? "engine asset" : "unmanaged"));
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

    PlaceInitially(SideWidth(context), std::max(0.0f, context.ContentHeight - BottomHeight(context)),
                   std::max(200.0f, context.ContentWidth - SideWidth(context) * 2.0f), BottomHeight(context));
    ImGui::Begin("Console", &state.ShowConsole);

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

    char input[512];
    FillBuffer(state.ConsoleInput, input, sizeof(input));
    if (ImGui::InputText("command", input, sizeof(input),
                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
    {
        state.Run(input);
        state.ConsoleInput.clear();
    }

    if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Tab))
    {
        const std::vector<std::string> matches = state.Console.Complete(state.ConsoleInput);
        if (matches.size() == 1u)
        {
            state.ConsoleInput = matches[0] + " ";
        }
        else if (!matches.empty())
        {
            state.AddLog(JoinNames(matches, "   "));
        }
    }

    ImGui::End();
}

void DrawViewportPanel(UiState& state, const PanelContext& context)
{
    // Centre: whatever the side panels and bottom strip leave. The scene renders at this extent, so a wrong rectangle
    // is not just an overlapping window — it is the viewport's aspect ratio.
    PlaceInitially(SideWidth(context), PanelsTop(context),
                   std::max(160.0f, context.ContentWidth - SideWidth(context) * 2.0f),
                   std::max(120.0f, context.ContentHeight - BottomHeight(context) - PanelsTop(context)));
    if (!ImGui::Begin("Viewport"))
    {
        ImGui::End();
        return;
    }

    if (context.ViewportTextureId != 0ull && context.ViewportWidth > 1.0f && context.ViewportHeight > 1.0f)
    {
        // The scene, rendered at the panel's own extent and sampled here. See the file header of
        // EditorApplication.cpp for why sampling rather than drawing into the back buffer is what makes a docked
        // viewport possible at all.
        ImGui::Image(ImTextureRef{static_cast<ImTextureID>(context.ViewportTextureId)},
                     ImVec2(context.ViewportWidth, context.ViewportHeight));
    }
    else
    {
        ImGui::TextUnformatted("Viewport not ready.");
    }

    if (!state.Selection.IsEmpty())
    {
        ImGui::Text("Selected: %zu", static_cast<size_t>(state.Selection.GetCount()));
    }

    ImGui::End();
}

void DrawMenuBar(UiState& state, const PanelContext& context)
{
    if (!ImGui::BeginMainMenuBar())
    {
        return;
    }

    if (ImGui::BeginMenu("Scene"))
    {
        if (ImGui::MenuItem("Save", nullptr, false))
        {
            state.Run("scene.save");
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Window"))
    {
        ImGui::Checkbox("Outliner", &state.ShowOutliner);
        ImGui::Checkbox("Details", &state.ShowDetails);
        ImGui::Checkbox("Content Browser", &state.ShowContent);
        ImGui::Checkbox("Console", &state.ShowConsole);
        ImGui::EndMenu();
    }

    ImGui::Text("frames %llu   input %d", static_cast<unsigned long long>(context.FrameCount),
                static_cast<int>(state.LastInputEvents));

    ImGui::EndMainMenuBar();
}

void DrawEditorPanels(UiState& state, const PanelContext& context)
{
    // The dockspace fills the window and every panel can be dragged into it. Docking is what makes this UE-shaped
    // rather than four free-floating windows; the initial rectangles in each Draw*Panel are where they land before an
    // author rearranges them (no ini file is written, so that arrangement lasts a session — tracked in
    // notes/EDITOR.md §3).
    ImGui::DockSpaceOverViewport(kDockSpaceId, ImGui::GetMainViewport(), ImGuiDockNodeFlags_None);

    DrawMenuBar(state, context);
    DrawOutlinerPanel(state, context);
    DrawDetailsPanel(state, context);
    DrawContentPanel(state, context);
    DrawConsolePanel(state, context);
    DrawViewportPanel(state, context);
}

}  // namespace Hollowpoint::Editor
