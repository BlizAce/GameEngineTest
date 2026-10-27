#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Editor/ContentBrowserModel.h"
#include "Hollowpoint/Editor/EditorApplication.h"
#include "Hollowpoint/Editor/EditorConsole.h"
#include "Hollowpoint/Editor/EditorInput.h"
#include "Hollowpoint/Editor/EditorPanels.h"
#include "Hollowpoint/Editor/EditorSelection.h"
#include "Hollowpoint/Editor/EntityName.h"
#include "Hollowpoint/Editor/Picking.h"
#include "Hollowpoint/Editor/PropertyView.h"
#include "Hollowpoint/Editor/UiRenderer.h"
#include "Hollowpoint/Game/MeshRenderer.h"
#include "Hollowpoint/Game/SceneRenderer.h"
#include "Hollowpoint/Graphics/RenderTarget.h"
#include "Hollowpoint/Graphics/RhiAccess.h"
#include "Hollowpoint/Graphics/Texture.h"
#include "Hollowpoint/Platform/Window.h"
#include "Hollowpoint/Renderer/Camera.h"
#include "Hollowpoint/Renderer/Geometry.h"
#include "Hollowpoint/Runtime/BuiltInMeshes.h"
#include "Hollowpoint/Runtime/Screenshot.h"
#include "Hollowpoint/Scene/LocalTransform.h"
#include "Hollowpoint/Scene/SceneIO.h"

#include <imgui.h>

// ---------------------------------------------------------------------------
// EditorApplication — see the header for the contract and frame-ownership rules.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Editor
{

using Core::Int64;
using Graphics::GraphicsDevice;
using Graphics::GraphicsFormat;
using Graphics::RenderTarget;
using Graphics::RenderTargetDesc;
using Graphics::Texture;
using Graphics::TextureDesc;
using Math::Vec3;
using Math::Vec4;
using Renderer::CameraComponent;
using Renderer::CameraState;
using Runtime::FrameContext;
using Runtime::RuntimeApplication;
using Runtime::RuntimeConfig;

namespace
{

constexpr const char* kLogChannel = "EDITOR";

//! Smallest viewport target we will allocate. Below this the panel is not a viewport, and a zero-extent texture fails
//  creation on frame 1 of every run (the first ImGui frame has not laid anything out yet).
constexpr UInt32 kMinimumViewportExtent = 32u;

//! Viewport extents are quantised to this step. Without it, dragging a dock boundary recreates two textures and one
//  pipeline per pixel of movement — allocator churn and a stutter that reads as a renderer problem.
constexpr UInt32 kViewportExtentQuantum = 16u;

[[nodiscard]] constexpr UInt32 Quantise(UInt32 value) noexcept
{
    const UInt32 rounded = ((value + kViewportExtentQuantum / 2u) / kViewportExtentQuantum) * kViewportExtentQuantum;
    return std::max(kMinimumViewportExtent, rounded);
}

bool TryParseReal(const std::string& text, double& outValue)
{
    if (text.empty())
    {
        return false;
    }
    try
    {
        size_t consumed = 0;
        outValue = std::stod(text, &consumed);
        // Trailing garbage ("1.5abc") is a typo, not a value with an ignored suffix: accepting it would let
        // `entity.set Roughness 0.2x` store 0.2 and report success.
        return consumed == text.size();
    }
    catch (const std::exception&)
    {
        return false;
    }
}

//! Registers the commands that need the hosted loop. UI-only commands (help, select, panels) live in EditorPanels.cpp,
//  which owns the state they touch — splitting by dependency rather than by file size.
void RegisterEngineCommands(EditorConsole& console, UiState& state, EditorApplication& app)
{
    console.RegisterFixed(
        "scene.load", "Load a .hpscene, replacing the current world.", ArgumentCount::Exact, 1,
        [&app, &state](const std::vector<std::string>& arguments, std::string& outReply, std::string& outError) {
            if (!app.LoadProjectScene(arguments[0]))
            {
                outError = "could not load scene \"" + arguments[0] + "\"";
                return false;
            }
            // Dropped rather than pruned: slots from the previous world are reused immediately by the
            // new one, so a surviving handle could name an unrelated entity — which reads as "the
            // inspector is showing something I never selected".
            state.Selection.Clear();
            outReply = "loaded " + arguments[0];
            return true;
        });

    console.RegisterFixed(
        "scene.save", "Save the world: scene.save [path].", ArgumentCount::AtLeast, 0,
        [&app, &state](const std::vector<std::string>& arguments, std::string& outReply, std::string& outError) {
            const std::string path = arguments.empty() ? app.GetScenePath() : arguments[0];
            if (path.empty())
            {
                outError = "no scene path — give one (scene.save my.hpscene) or open a scene first";
                return false;
            }
            if (!app.SaveProjectScene(path))
            {
                outError = "could not save to \"" + path + "\"";
                return false;
            }
            state.AddLog("saved " + path);
            outReply = "saved " + path;
            return true;
        });

    console.RegisterFixed("entity.create", "Create an entity: entity.create [name] [mesh].", ArgumentCount::Any, 0,
                          [&app](const std::vector<std::string>& arguments, std::string& outReply, std::string&) {
                              const std::string name = arguments.size() > 0 ? arguments[0] : "Entity";
                              const std::string mesh = arguments.size() > 1 ? arguments[1] : "";

                              const Entity entity = app.CreateEntity(name, mesh);
                              if (entity.IsNull())
                              {
                                  outReply = "entity creation failed (see log)";
                                  return false;
                              }

                              char buffer[160];
                              std::snprintf(buffer, sizeof(buffer), "created \"%s\" at slot %llu", name.c_str(),
                                            static_cast<unsigned long long>(entity.Slot));
                              outReply = buffer;
                              return true;
                          });

    console.RegisterFixed("entity.destroy", "Destroy the selected entities.", ArgumentCount::Exact, 0,
                          [&app, &state](const std::vector<std::string>&, std::string& outReply, std::string&) {
                              const Size count = state.Selection.GetCount();
                              if (count == 0u)
                              {
                                  outReply = "nothing selected";
                                  return true;
                              }
                              app.DestroySelected();
                              outReply = "destroyed " + std::to_string(count) + " entity(ies)";
                              return true;
                          });

    console.RegisterFixed(
        "entity.inspect", "Print the selected entity's reflected properties.", ArgumentCount::Exact, 0,
        [&app, &state](const std::vector<std::string>&, std::string& outReply, std::string& outError) {
            const Entity primary = state.Selection.GetPrimary();
            if (primary.IsNull())
            {
                outError = "nothing selected";
                return false;
            }

            // Rebuilt here rather than reusing the panel's list: a command can run between frames, and
            // a stale property list would print values that no longer exist. Building for one entity
            // walks a handful of descriptors, not the world.
            state.Properties.Build(app.GetWorld(), primary);

            outReply.clear();
            for (const PropertyField& field : state.Properties.GetFields())
            {
                if (!outReply.empty())
                {
                    outReply += "\n";
                }
                outReply += field.Path + " = " + state.Properties.FormatValue(field);
            }
            if (outReply.empty())
            {
                outReply = "entity has no reflected components";
            }
            return true;
        });

    console.RegisterFixed(
        "entity.set", "Set a property: entity.set <Component.Path> <value>.", ArgumentCount::Exact, 2,
        [&app, &state](const std::vector<std::string>& arguments, std::string& outReply, std::string& outError) {
            const Entity primary = state.Selection.GetPrimary();
            if (primary.IsNull())
            {
                outError = "nothing selected";
                return false;
            }

            state.Properties.Build(app.GetWorld(), primary);
            const PropertyField* field = state.Properties.FindByPath(arguments[0]);
            if (field == nullptr)
            {
                outError = "no property \"" + arguments[0] + "\" on the selected entity";
                return false;
            }
            if (!field->IsEditable())
            {
                outError = "\"" + arguments[0] + "\" is not editable yet";
                return false;
            }

            // Bool words, then a number, then text. `entity.set MeshRenderer.Visible false` and
            // `... Roughness 0.2` both do the obvious thing without asking which type it meant, and
            // anything left over is written as a string — which fails at the reflection layer rather
            // than silently storing zero when the kind does not match.
            const std::string& value = arguments[1];

            if (value == "true" || value == "false")
            {
                if (!state.Properties.WriteBool(*field, value == "true"))
                {
                    outError = "\"" + value + "\" is not valid for this property";
                    return false;
                }
                outReply = arguments[0] + " = " + value;
                return true;
            }

            double number = 0.0;
            if (TryParseReal(value, number) && state.Properties.WriteReal(*field, number))
            {
                char buffer[160];
                std::snprintf(buffer, sizeof(buffer), "%s = %g", arguments[0].c_str(), number);
                outReply = buffer;
                return true;
            }

            if (state.Properties.WriteString(*field, value))
            {
                outReply = arguments[0] + " = " + value;
                return true;
            }

            outError = "\"" + value + "\" is not valid for this property";
            return false;
        });

    console.RegisterFixed(
        "content.list", "List a project directory: content.list [subpath].", ArgumentCount::AtLeast, 0,
        [&state](const std::vector<std::string>& arguments, std::string& outReply, std::string& outError) {
            const std::filesystem::path sub =
                arguments.empty() ? std::filesystem::path{} : std::filesystem::path(arguments[0]);
            std::string error;
            if (!state.Content.Refresh(sub, &error))
            {
                outError = error;
                return false;
            }

            outReply.clear();
            for (const ContentEntry& entry : state.Content.GetEntries())
            {
                if (!outReply.empty())
                {
                    outReply += "\n";
                }
                outReply += entry.IsDirectory ? "d " : "- ";
                outReply += entry.Name;
                if (!entry.IsDirectory)
                {
                    outReply += "  (" + std::to_string(entry.SizeBytes) + " bytes)";
                }
            }
            if (outReply.empty())
            {
                outReply = "(empty)";
            }
            return true;
        });

    console.RegisterFixed(
        "content.open", "Open a project file (.hpscene or .lua).", ArgumentCount::Exact, 1,
        [&app, &state](const std::vector<std::string>& arguments, std::string& outReply, std::string& outError) {
            std::filesystem::path absolute;
            if (!state.Content.ResolveWithinRoot(arguments[0], absolute))
            {
                outError = "\"" + arguments[0] + "\" is not inside the project root";
                return false;
            }

            if (!app.OpenProjectFile(absolute))
            {
                outError = "no handler for \"" + absolute.string() + "\"";
                return false;
            }
            outReply = "opened " + absolute.string();
            return true;
        });

    console.RegisterFixed(
        "viewport.camera", "Move the viewport camera: viewport.camera <x y z> [tx ty tz].", ArgumentCount::AtLeast, 3,
        [&app](const std::vector<std::string>& arguments, std::string& outReply, std::string& outError) {
            float values[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
            for (Size index = 0; index < arguments.size(); ++index)
            {
                double parsed = 0.0;
                if (!TryParseReal(arguments[index], parsed))
                {
                    outError = "\"" + arguments[index] + "\" is not a number";
                    return false;
                }
                values[index] = static_cast<float>(parsed);
            }

            const Vec3 eye(values[0], values[1], values[2]);
            const Vec3 target = arguments.size() >= 6 ? Vec3(values[3], values[4], values[5]) : Vec3(0.0f, 0.0f, 0.0f);
            app.SetViewportCamera(eye, target);

            char buffer[192];
            std::snprintf(buffer, sizeof(buffer), "camera eye (%g, %g, %g) target (%g, %g, %g)",
                          static_cast<double>(eye.X), static_cast<double>(eye.Y), static_cast<double>(eye.Z),
                          static_cast<double>(target.X), static_cast<double>(target.Y), static_cast<double>(target.Z));
            outReply = buffer;
            return true;
        });

    console.RegisterFixed(
        "pick.at", "Select what is under a viewport pixel: pick.at <x> <y>.", ArgumentCount::Exact, 2,
        [&app](const std::vector<std::string>& arguments, std::string& outReply, std::string& outError) {
            double x = 0.0;
            double y = 0.0;
            if (!TryParseReal(arguments[0], x) || !TryParseReal(arguments[1], y))
            {
                outError = "pick.at needs two pixel coordinates";
                return false;
            }

            const Entity hit = app.PickAtViewportPixel(static_cast<float>(x), static_cast<float>(y));
            if (hit.IsNull())
            {
                // Not an error. Clicking empty space is a normal thing to do and the console should
                // say so in the same voice it uses for everything else.
                outReply = "nothing under that pixel";
                return true;
            }

            char buffer[128];
            std::snprintf(buffer, sizeof(buffer), "selected slot %llu", static_cast<unsigned long long>(hit.Slot));
            outReply = buffer;
            return true;
        });

    console.RegisterFixed(
        "screenshot", "Write a PNG of what the editor shows: screenshot <path>.", ArgumentCount::Exact, 1,
        [&app](const std::vector<std::string>& arguments, std::string& outReply, std::string& outError) {
            if (!app.CaptureComposedFrame(arguments[0]))
            {
                outError = "could not write \"" + arguments[0] + "\"";
                return false;
            }
            outReply = "wrote " + arguments[0];
            return true;
        });

    console.RegisterFixed(
        "script.run", "Run a Lua file: script.run <path>.", ArgumentCount::Exact, 1,
        [&app](const std::vector<std::string>& arguments, std::string& outReply, std::string& outError) {
            if (!app.RunScript(arguments[0]))
            {
                outError = "could not run \"" + arguments[0] + "\"";
                return false;
            }
            outReply = "ran " + arguments[0];
            return true;
        });
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction / lifetime
// ---------------------------------------------------------------------------

EditorApplication::EditorApplication(EditorConfig config) : m_Config(std::move(config))
{
    // A three-quarter view of the origin. Deliberate: an editor that opens with its eye inside the demo cube produces a
    // black first screenshot and reads as "the renderer is broken" on every automated capture test.
    m_Camera.Position = Vec3(4.0f, 3.0f, 6.0f);
    m_Camera.Target = Vec3(0.0f, 0.5f, 0.0f);
}

EditorApplication::~EditorApplication()
{
    // Detach first: the hosted loop's teardown destroys the renderer and swapchain, and a host pointer still registered
    // through that would be live for any hook the teardown path ever adds. Explicit beats implicit ordering here.
    if (m_Application != nullptr && m_AttachedHost)
    {
        m_Application->SetEditorHost(nullptr);
        m_AttachedHost = false;
    }

    // UI resources before the device goes away: UiRenderer holds NVRHI handles, and ImGui's context owns none but must
    // be destroyed while its allocator (the C runtime's) is obviously still up.
    DestroyViewportTargets();
    m_CaptureTarget = nullptr;
    m_CaptureColour = nullptr;
    m_Ui = nullptr;
    m_State.reset();

    if (m_ImGuiContext != nullptr)
    {
        ImGui::DestroyContext(m_ImGuiContext);
        m_ImGuiContext = nullptr;
    }
}

bool EditorApplication::Start()
{
    if (m_Started)
    {
        return true;
    }

    RegisterEditorTypes();

    // The hosted loop is a normal runtime. Scripting is enabled so `script.run` works, and AreScriptsPaused() is what
    // keeps gameplay code still while an author places things.
    RuntimeConfig runtime;
    runtime.ApplicationName = m_Config.ApplicationName;
    runtime.Width = m_Config.Width;
    runtime.Height = m_Config.Height;
    runtime.StartVisible = m_Config.StartVisible;
    runtime.EnableScripting = true;
    runtime.EnableValidation = m_Config.EnableValidation;
    // The editor owns its own frame budget (see Run/Frame), so the loop's limit stays unset: a limit enforced in two
    // places means whichever hits first decides, and their definitions of "frame" differ by exactly the skipped frames.
    runtime.MaxFrameCount = 0;

    if (!m_Config.ScenePath.empty())
    {
        runtime.ScenePath = m_Config.ScenePath;
        m_ActiveScenePath = m_Config.ScenePath;
    }

    m_Application = std::make_unique<RuntimeApplication>(std::move(runtime));
    m_Application->SetEditorHost(this);
    m_AttachedHost = true;

    if (!m_Application->Start())
    {
        HP_LOG_ERROR(kLogChannel, "hosted runtime failed to start — the editor cannot run without a device and window");
        m_Application->SetEditorHost(nullptr);
        m_AttachedHost = false;
        m_Application.reset();
        return false;
    }

    // Docking branch (the UE-style layout depends on it) with no OS backends: input arrives through ConsumeInputEvent,
    // the only path that also sees injected events. See EditorInput.h for why that matters.
    m_ImGuiContext = ImGui::CreateContext();
    if (m_ImGuiContext == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "could not create an ImGui context");
        return false;
    }
    ImGui::SetCurrentContext(m_ImGuiContext);

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // This renderer honours ImDrawCmd::VtxOffset by concatenating draw lists and offsetting indices, so ImGui may emit
    // large meshes without splitting them. Claiming the capability without honouring it truncates long lists — an
    // outliner whose lower rows vanish, at exactly the moment a scene gets big enough to care about.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    // Declares that this backend services ImTextureData status transitions (WantCreate/WantUpdates/WantDestroy), which
    // UiRenderer::RefreshTextures does. Without it ImGui 1.93 falls back to the legacy "build the atlas yourself" path:
    // it asserts on frame 1 demanding a built atlas nobody owns, because there is no longer a GetTexDataAsRGBA32 call
    // for a backend to build it in. This flag is what turns that assert into the texture-request loop we implement.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.IniFilename = nullptr;  // no layout file yet: persistence needs per-project settings (notes/EDITOR.md §3)

    m_Ui = UiRenderer::Create(*m_Application->GetDevice());
    if (m_Ui == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "could not create the UI renderer; the editor has no panels without it");
        return false;
    }

    m_State = std::make_unique<UiState>();

    // Content root defaults to the working directory so the browser shows something when launched without --project. A
    // configured path that does not exist is reported and leaves the panel empty rather than failing the whole start:
    // an editor that will not open because one path is wrong is worse than one with an empty content browser.
    std::error_code error;
    const std::filesystem::path projectRoot = m_Config.ProjectDirectory.empty()
                                                  ? std::filesystem::current_path(error)
                                                  : std::filesystem::path(m_Config.ProjectDirectory);
    if (!m_State->Content.SetRoot(projectRoot))
    {
        HP_LOG_WARNING(kLogChannel, "content browser disabled: \"{}\" is not a usable project directory",
                       projectRoot.string());
    }

    RegisterEditorCommands(m_State->Console, *m_State, *this);
    m_State->AddLog("Hollowpoint editor — type `help` for commands.");

    m_Started = true;
    return true;
}

bool EditorApplication::RunStartupCommand(const std::string& line)
{
    if (m_State == nullptr)
    {
        return false;
    }

    const Size before = m_State->Log.size();
    m_State->Run(line);

    // Success is inferred from the transcript rather than from ExecuteLine directly, because Run() is the only route
    // into a command (the text box uses it too) and having two paths that could disagree about what "worked" means is
    // exactly the divergence this editor's design exists to avoid. The appended error line, if any, stays visible.
    const Size after = m_State->Log.size();
    for (Size index = before; index < after; ++index)
    {
        if (!m_State->Log[index].empty() && m_State->Log[index][0] == '!')
        {
            return false;
        }
    }
    return after > before;
}

bool EditorApplication::Frame()
{
    if (!m_Started || m_Application == nullptr || m_StopReason != EditorStopReason::Running)
    {
        return false;
    }

    // The runtime drives input, time, transforms and collection and calls back into this class for frame contents.
    if (!m_Application->RunFrame())
    {
        const Runtime::StopReason reason = m_Application->GetStopReason();
        m_StopReason =
            reason == Runtime::StopReason::FrameLimit ? EditorStopReason::FrameLimit : EditorStopReason::WindowClosed;
        return false;
    }

    if (m_Config.MaxFrameCount != 0ull && m_Application->GetFrameCount() >= m_Config.MaxFrameCount)
    {
        m_StopReason = EditorStopReason::FrameLimit;
        return false;
    }

    return true;
}

EditorStopReason EditorApplication::Run()
{
    if (!m_Started && !Start())
    {
        return EditorStopReason::StartupFailed;
    }

    while (Frame())
    {
        // No idle wait, deliberately: the runtime's own backgrounding logic is disabled because an editor must stay
        // responsive to its UI even when the game window is unfocused. A frozen viewport while typing in the console is
        // a broken tool, not a backgrounded game.
    }

    if (!m_Config.ScreenshotPath.empty())
    {
        CaptureComposedFrame(m_Config.ScreenshotPath);
    }

    return m_StopReason;
}

void EditorApplication::RequestStop(EditorStopReason reason) noexcept
{
    m_StopReason = reason;
    if (m_Application != nullptr)
    {
        // Also stops the hosted loop, so a command issued mid-frame ends both loops that same frame rather than letting
        // the runtime render one more after the editor decided to quit.
        m_Application->RequestStop(Runtime::StopReason::WindowClosed);
    }
}

// ---------------------------------------------------------------------------
// Editing operations — the single implementation each widget and command routes through.
// ---------------------------------------------------------------------------

World& EditorApplication::GetWorld() noexcept
{
    return m_Application->GetWorld();
}

Entity EditorApplication::GetSelectedEntity() const noexcept
{
    return m_State != nullptr ? m_State->Selection.GetPrimary() : Entity{};
}

EditorSelection& EditorApplication::GetSelection() noexcept
{
    return m_State->Selection;
}

bool EditorApplication::SelectBySlot(Index slot)
{
    if (m_Application == nullptr || m_State == nullptr)
    {
        return false;
    }

    World& world = m_Application->GetWorld();
    // Core::Index is unsigned, so "negative slot" is not representable and a value that came from parsing could be
    // InvalidIndex; the capacity check covers both.
    if (slot == Core::InvalidIndex || static_cast<Size>(slot) >= world.GetSlotCapacity())
    {
        return false;
    }

    const Entity entity = world.GetEntityAtSlot(slot);
    if (!world.IsAlive(entity))
    {
        return false;
    }

    m_State->Selection.Select(entity);
    return true;
}

Entity EditorApplication::CreateEntity(const std::string& name, const std::string& meshPath)
{
    if (m_Application == nullptr || m_State == nullptr)
    {
        return Entity{};
    }

    World& world = m_Application->GetWorld();
    const Entity entity = world.CreateEntity();

    // A transform always. An entity without one still renders at identity, but the details panel would offer nothing to
    // position it with — and moving the new thing is every author's first action.
    world.AddComponent<Scene::LocalTransform>(entity);

    EntityName& entityName = world.AddComponent<EntityName>(entity);
    entityName.Name = name.empty() ? "Entity" : name;

    if (!meshPath.empty())
    {
        Game::MeshRenderer& renderer = world.AddComponent<Game::MeshRenderer>(entity);
        renderer.MeshPath = meshPath;
    }

    // Selected immediately: creating something means wanting to see it in the inspector, and a script that creates ten
    // entities ends with the last one current — which is what "the new thing is the selected thing" means everywhere.
    m_State->Selection.Select(entity);
    return entity;
}

void EditorApplication::DestroySelected()
{
    if (m_Application == nullptr || m_State == nullptr)
    {
        return;
    }

    World& world = m_Application->GetWorld();
    for (const Entity& entity : m_State->Selection.GetEntities())
    {
        world.DestroyEntity(entity);
    }
    // Cleared here rather than left to a per-frame prune: those handles name slots that can be recycled by the next
    // CreateEntity, and one frame of a stale handle is how "it deleted the wrong thing" becomes possible.
    m_State->Selection.Clear();
}

bool EditorApplication::LoadProjectScene(const std::string& path)
{
    if (m_Application == nullptr)
    {
        return false;
    }
    if (!m_Application->LoadScene(path))
    {
        return false;
    }
    // Recorded so `scene.save` with no argument writes back to the file that was opened.
    m_ActiveScenePath = path;
    return true;
}

bool EditorApplication::SaveProjectScene(const std::string& path)
{
    if (m_Application == nullptr || path.empty())
    {
        return false;
    }

    const Scene::SceneSaveFileResult result = Scene::SaveSceneToFile(m_Application->GetWorld(), path);
    if (!result.Saved)
    {
        HP_LOG_ERROR(kLogChannel, "scene save to \"{}\" failed", path);
        return false;
    }

    m_ActiveScenePath = path;
    return true;
}

bool EditorApplication::OpenProjectFile(const std::filesystem::path& path)
{
    // Extension dispatch rather than content sniffing: the editor has two openable kinds and both are named by
    // extension everywhere they appear (the content browser's Type column, `content.open`).
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (extension == ".hpscene")
    {
        if (m_State != nullptr)
        {
            m_State->Selection.Clear();
        }
        return LoadProjectScene(path.string());
    }

    if (extension == ".lua")
    {
        return RunScript(path.string());
    }

    if (m_State != nullptr)
    {
        m_State->AddLog("! no importer for \"" + path.string() + "\" (notes/EDITOR.md §1)");
    }
    return false;
}

bool EditorApplication::RunScript(const std::string& path)
{
    if (m_Application == nullptr)
    {
        return false;
    }
    // An absolute path is passed straight through: RuntimeApplication resolves relative names against the configured
    // script directory, and an absolute name is already fully qualified.
    return m_Application->RunScriptFile(path);
}

bool EditorApplication::CaptureComposedFrame(const std::string& path)
{
    if (!RenderIntoCaptureTarget())
    {
        return false;
    }

    std::string error;
    if (!Runtime::WriteScreenshot(*m_Application->GetDevice(), *m_CaptureColour, path, error))
    {
        HP_LOG_ERROR(kLogChannel, "screenshot failed: {}", error);
        return false;
    }

    HP_LOG_INFO(kLogChannel, "wrote {} ({}x{}, composed frame)", path, m_CaptureWidth, m_CaptureHeight);
    return true;
}

bool EditorApplication::CaptureComposedRgba8(std::vector<unsigned char>& outRgba, UInt32& outWidth, UInt32& outHeight)
{
    outRgba.clear();
    outWidth = 0;
    outHeight = 0;

    if (!RenderIntoCaptureTarget())
    {
        return false;
    }

    // ReadBack returns the target's own row pitch, which may be padded past the texel row. ConvertToRgba8 takes the
    // pitch explicitly for exactly that reason — assuming width*4 silently truncates or overlaps rows on tiling formats
    // and produces an image that looks "almost right" in every corner but one.
    GraphicsDevice* device = m_Application->GetDevice();
    if (device == nullptr)
    {
        return false;
    }

    UInt32 rowPitchBytes = 0;
    const std::vector<Core::UInt8> raw = m_CaptureColour->ReadBack(*device, rowPitchBytes);
    if (raw.empty())
    {
        HP_LOG_ERROR(kLogChannel, "capture readback returned no bytes");
        return false;
    }

    std::string error;
    if (!Runtime::ConvertToRgba8(raw.data(), raw.size(), rowPitchBytes, m_CaptureColour->GetDesc().Format,
                                 m_CaptureWidth, m_CaptureHeight, outRgba, error))
    {
        HP_LOG_ERROR(kLogChannel, "capture conversion failed: {}", error);
        return false;
    }

    outWidth = m_CaptureWidth;
    outHeight = m_CaptureHeight;
    return true;
}

bool EditorApplication::RenderIntoCaptureTarget()
{
    if (m_Application == nullptr || m_Ui == nullptr)
    {
        return false;
    }

    if (!m_HasComposedFrame)
    {
        HP_LOG_ERROR(kLogChannel, "no frame has been composed yet; refusing to write an empty screenshot");
        return false;
    }

    GraphicsDevice* device = m_Application->GetDevice();
    if (device == nullptr || m_LastBackBufferWidth == 0u || m_LastBackBufferHeight == 0u)
    {
        HP_LOG_ERROR(kLogChannel, "capture needs a known back-buffer extent");
        return false;
    }

    if (!EnsureCaptureTargets(m_LastBackBufferWidth, m_LastBackBufferHeight))
    {
        return false;
    }

    nvrhi::IDevice* rhi = device->GetNativeDevice();
    const Texture* captureColour = m_CaptureTarget->GetColourTexture();
    if (rhi == nullptr || captureColour == nullptr)
    {
        return false;
    }

    // Background first, and deliberately in its own submission *before* the UI draw: the composed frame's clear
    // happened on the back buffer, so this target would otherwise receive only the UI pass over uninitialised memory —
    // which readback happily returns as black, making a broken capture indistinguishable from an empty scene. Same
    // queue, so submission order is execution order and the clear cannot land after the draws it is meant to sit under.
    {
        nvrhi::CommandListHandle commandList = rhi->createCommandList();
        if (!static_cast<bool>(commandList))
        {
            return false;
        }

        commandList->open();
        commandList->clearTextureFloat(
            captureColour->GetNativeTexture(), nvrhi::TextureSubresourceSet(),
            nvrhi::Color(m_Config.BackgroundRed, m_Config.BackgroundGreen, m_Config.BackgroundBlue, 1.0f));
        commandList->close();

        nvrhi::ICommandList* raw = commandList;
        rhi->executeCommandLists(&raw, 1);
    }

    // ImGui's draw data from the last RenderScenePass is still valid (nothing has called NewFrame since), so replaying
    // it into this target reproduces the composite. The scene appears because a widget samples the viewport texture —
    // which is why this captures what the user saw rather than just the panels floating over black.
    if (!m_Ui->Render(*m_CaptureTarget))
    {
        HP_LOG_ERROR(kLogChannel, "could not render the UI into the capture target");
        return false;
    }

    // ReadBack blocks on the GPU, so both entry points get a settled target whether they write a file or return bytes.
    rhi->waitForIdle();
    return true;
}

void EditorApplication::SetViewportCamera(const Vec3& eye, const Vec3& target) noexcept
{
    m_Camera.Position = eye;
    m_Camera.Target = target;
}

UInt64 EditorApplication::GetFrameCount() const noexcept
{
    return m_Application != nullptr ? m_Application->GetFrameCount() : 0ull;
}

Entity EditorApplication::PickAtViewportPixel(float x, float y)
{
    if (m_Application == nullptr || m_State == nullptr || m_ViewportWidth == 0u || m_ViewportHeight == 0u)
    {
        return Entity{};
    }

    // Pixel → NDC. ImGui coordinates and Platform mouse positions are both content-space with y down; the flip into
    // NDC's y-up happens here and nowhere else, which is the convention Picking.h asks its caller to own once.
    const float ndcX = (x / static_cast<float>(m_ViewportWidth)) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (y / static_cast<float>(m_ViewportHeight)) * 2.0f;

    CameraState camera =
        BuildViewportCamera(static_cast<float>(m_ViewportWidth) / static_cast<float>(std::max(1u, m_ViewportHeight)));

    const Ray ray = ScreenPointToRay(ndcX, ndcY, camera.InverseViewProjection);

    // Candidates are gathered per pick rather than cached: scenes change between clicks (that is the point of an
    // editor), and a cache would need invalidation hooks into every mutation path. Picking runs on click, not per
    // frame, so the cost lands where it does not matter. A BVH goes between broad and narrow phase when scenes get big
    // enough to need one (Picking.h says the same thing about the algorithm).
    World& world = m_Application->GetWorld();

    std::vector<PickCandidate> candidates;
    std::vector<Entity> owners;
    std::vector<std::vector<Core::UInt32>> indexCopies;  // PickCandidate wants UInt32*; GeometryData owns them per mesh

    // The same resolver the renderer drew with, so a pick cannot disagree with what is on screen. "I clicked the cube
    // and selected nothing" is exactly what an independent mesh lookup produces.
    const Runtime::BuiltInMeshes& meshes = Runtime::BuiltInMeshes::GetSingleton();

    world.ForEach<Game::MeshRenderer>([&](const Entity& entity, const Game::MeshRenderer& renderer) {
        if (!renderer.IsDrawable())
        {
            return;
        }

        const Renderer::GeometryData* geometry = meshes.Lookup(renderer.MeshPath);
        if (geometry == nullptr || geometry->Vertices.empty() || geometry->Indices.empty())
        {
            // An asset that has not loaded is a normal state, and Picking.h documents why this is skipped rather than
            // asserted: a content problem must not become an editor crash.
            return;
        }

        Mat4 transform = Mat4::Identity();
        if (const Scene::LocalTransform* local = world.TryGetComponent<Scene::LocalTransform>(entity); local != nullptr)
        {
            transform = local->ToTransform().ToMatrix();
        }

        PickCandidate candidate;
        candidate.OwnerSlot = entity.Slot;
        candidate.WorldTransform = transform;
        candidate.Vertices = geometry->Vertices.data();
        candidate.VertexCount = static_cast<Index>(geometry->Vertices.size());
        // GeometryVertex is position/normal/uv of floats; the offsets are stated rather than assumed because Picking.h
        // reads positions through a stride and a wrong offset picks vertices that are actually normals.
        candidate.VertexStrideBytes = static_cast<Index>(sizeof(Renderer::GeometryVertex));
        candidate.PositionOffsetBytes = 0;

        // GeometryData stores indices in a std::vector<UInt32>, which is exactly PickCandidate's expected layout — but
        // the candidate holds a raw pointer, so the vector has to be kept alive for the pick. Copied here rather than
        // const_casting the source (which would outlive it if the asset layer ever reloads the mesh mid-pick).
        indexCopies.push_back(geometry->Indices);
        candidate.Indices = indexCopies.back().data();
        candidate.IndexCount = static_cast<Index>(geometry->Indices.size());

        candidates.push_back(candidate);
        owners.push_back(entity);
    });

    if (candidates.empty())
    {
        return Entity{};
    }

    const PickResult result = PickRay(ray, candidates.data(), static_cast<Index>(candidates.size()), true);
    if (!result.HasHit || result.OwnerSlot == Core::InvalidIndex ||
        static_cast<size_t>(result.OwnerSlot) >= owners.size())
    {
        return Entity{};
    }

    const Entity hit = owners[static_cast<size_t>(result.OwnerSlot)];
    m_State->Selection.Select(hit);
    return hit;
}

// ---------------------------------------------------------------------------
// IApplicationHost
// ---------------------------------------------------------------------------

bool EditorApplication::ConsumeInputEvent(const Platform::InputEvent& event)
{
    if (m_ImGuiContext == nullptr || m_State == nullptr)
    {
        return false;
    }

    if (event.EventType == Platform::InputEvent::Type::Focus)
    {
        m_WindowFocused = event.Focused;
    }

    // Queued rather than applied inline. ImGui's IO wants a whole frame's events before NewFrame, and the runtime hands
    // them over one at a time — so they accumulate here and are fed in RenderScenePass, which is where NewFrame
    // happens. Feeding per event would be equivalent for state but would make "events this frame" unreportable.
    m_PendingUiEvents.push_back(event);

    // Nothing is consumed. ImGui's WantCaptureMouse/WantCaptureKeyboard decide routing once a gameplay input consumer
    // exists; this engine has none yet (docs/ARCHITECTURE.md), so claiming events here would drop them with nothing
    // downstream to compare against. Returning false keeps every event visible in GetUnconsumedEvents, which is what
    // the host-routing tests assert on. See ApplicationHost.h for the contract.
    return false;
}

bool EditorApplication::GetCameraOverride(CameraState& outCamera) const
{
    if (m_ViewportWidth == 0u || m_ViewportHeight == 0u)
    {
        // No viewport extent means no aspect ratio, and a projection built from the window's aspect would be wrong for
        // the panel. Declining lets the runtime use its own rule meanwhile rather than rendering a frame that has to be
        // thrown away.
        return false;
    }

    outCamera = BuildViewportCamera(static_cast<float>(m_ViewportWidth) / static_cast<float>(m_ViewportHeight));
    return true;
}

bool EditorApplication::AreScriptsPaused() const
{
    // Edit mode is the only mode implemented, and it pauses gameplay scripts: they move things the author is placing.
    // "Play" (scripts running with the editor attached) is written up in notes/EDITOR.md §2 rather than half-built — a
    // button that resumes script ticks without pausing physics/undo semantics would be worse than no button.
    return !m_Config.PlayModeScripts;
}

bool EditorApplication::RenderScenePass(const FrameContext& context)
{
    if (context.BackBuffer == nullptr || m_Ui == nullptr || m_State == nullptr || m_Application == nullptr)
    {
        return false;
    }

    GraphicsDevice* device = m_Application->GetDevice();
    if (device == nullptr)
    {
        return false;
    }

    ImGui::SetCurrentContext(m_ImGuiContext);
    ImGuiIO& io = ImGui::GetIO();

    // --- 1. input + frame state, before NewFrame --------------------------------------
    // The return value is the same count as the queue size by construction (every queued event maps to at least one
    // ImGui input call except unmapped keys), so the queue length is what `editor.input` reports.
    static_cast<void>(FeedImGuiInput(m_PendingUiEvents));
    m_State->LastInputEvents = static_cast<Index>(m_PendingUiEvents.size());
    m_PendingUiEvents.clear();

    // Display size comes from the window, not from ImGui's main viewport. Without a platform backend, ImGui copies
    // io.DisplaySize into the main viewport *during* NewFrame — so reading the viewport before NewFrame yields 1x1 on
    // the first frame (the clamped minimum), which clips every window to nothing: the composite then draws one pixel of
    // UI and looks like a dead renderer. The framebuffer scale is the ratio between this window's physical and content
    // extents, and ImGui needs it to size its atlas and scissors for a HiDPI display.
    Platform::Window* window = m_Application->GetWindow();
    const float contentWidth = window != nullptr ? static_cast<float>(window->GetContentWidth()) : 0.0f;
    const float contentHeight = window != nullptr ? static_cast<float>(window->GetContentHeight()) : 0.0f;
    const float framebufferWidth = window != nullptr ? static_cast<float>(window->GetFramebufferWidth()) : 0.0f;
    const float framebufferHeight = window != nullptr ? static_cast<float>(window->GetFramebufferHeight()) : 0.0f;

    if (contentWidth > 1.0f)
    {
        io.DisplayFramebufferScale.x = framebufferWidth / contentWidth;
    }
    if (contentHeight > 1.0f)
    {
        io.DisplayFramebufferScale.y = framebufferHeight / contentHeight;
    }

    FeedImGuiFrameState(contentWidth, contentHeight, static_cast<float>(std::max(0.0001, context.DeltaSeconds)),
                        m_WindowFocused);

    // --- 2. size the viewport target ---------------------------------------------------
    // Estimated from the main viewport rather than read from the panel: on the first frame ImGui has no layout yet, and
    // reading a panel rect before NewFrame returns last frame's (zero on frame one). The estimate leaves room for two
    // side panels and a bottom dock; once docking settles, the Image widget is drawn at exactly the target size so the
    // scene and its panel cannot drift apart by an accumulating rounding term.
    const UInt32 desiredWidth = Quantise(static_cast<UInt32>(
        std::max(static_cast<float>(kMinimumViewportExtent), contentWidth > 700.0f ? contentWidth - 660.0f : 480.0f)));
    const UInt32 desiredHeight = Quantise(static_cast<UInt32>(std::max(
        static_cast<float>(kMinimumViewportExtent), contentHeight > 400.0f ? contentHeight - 260.0f : 320.0f)));

    if (!EnsureViewportTargets(desiredWidth, desiredHeight))
    {
        HP_LOG_ERROR(kLogChannel, "no viewport targets; frame not composed");
        return false;
    }

    // --- 3. the scene, at the panel's extent -------------------------------------------
    Game::SceneRenderer* renderer = m_Application->GetRenderer();
    if (renderer == nullptr)
    {
        return false;
    }

    const Vec4 background(m_Config.BackgroundRed, m_Config.BackgroundGreen, m_Config.BackgroundBlue, 1.0f);
    if (!renderer->RenderForward(m_Application->GetLastRenderPlan(), *m_ViewportHdrTarget, context.Slot, background))
    {
        HP_LOG_ERROR(kLogChannel, "viewport forward pass failed");
        return false;
    }

    const Texture* hdrColour = m_ViewportHdrTarget->GetColourTexture();
    if (hdrColour == nullptr ||
        !renderer->BlitTonemap(*hdrColour, *m_ViewportDisplayTarget, m_Config.Exposure, context.Slot))
    {
        HP_LOG_ERROR(kLogChannel, "viewport tone map failed");
        return false;
    }

    // --- 4. panels ---------------------------------------------------------------------
    ImGui::NewFrame();

    PanelContext panelContext;
    panelContext.SceneWorld = &m_Application->GetWorld();
    panelContext.Application = this;
    panelContext.Console = &m_State->Console;
    panelContext.ViewportTextureId = m_ViewportTextureId;
    panelContext.ViewportWidth = static_cast<float>(m_ViewportWidth);
    panelContext.ViewportHeight = static_cast<float>(m_ViewportHeight);
    panelContext.FrameCount = context.FrameIndex;
    panelContext.SceneDrawCalls = renderer->GetDrawCount();
    panelContext.ContentWidth = contentWidth;
    panelContext.ContentHeight = contentHeight;

    DrawEditorPanels(*m_State, panelContext);

    ImGui::Render();


    m_LastBackBufferWidth = context.BackBuffer->GetWidth();
    m_LastBackBufferHeight = context.BackBuffer->GetHeight();

    // --- 5. clear the back buffer, then composite --------------------------------------
    // Clearing is this host's job because returning true from RenderScenePass means nothing else wrote the back buffer:
    // Vulkan leaves attachment contents undefined unless they are cleared or rendered to, so skipping it shows whatever
    // the previous frame left in that image. The runtime's own pass does the same clear (SceneRenderer::RenderForward),
    // and the swapchain requests TRANSFER_DST for it — see Session 15's finding 4.
    nvrhi::IDevice* rhi = device->GetNativeDevice();
    const Texture* backBufferColour = context.BackBuffer->GetColourTexture();
    if (rhi == nullptr || backBufferColour == nullptr)
    {
        return false;
    }

    nvrhi::CommandListHandle commandList = rhi->createCommandList();
    if (!static_cast<bool>(commandList))
    {
        return false;
    }

    commandList->open();
    commandList->clearTextureFloat(backBufferColour->GetNativeTexture(), nvrhi::TextureSubresourceSet(),
                                   nvrhi::Color(background.X, background.Y, background.Z, 1.0f));
    commandList->close();

    nvrhi::ICommandList* raw = commandList;
    rhi->executeCommandLists(&raw, 1);

    if (!m_Ui->Render(*context.BackBuffer))
    {
        if (!m_UiFailureReported)
        {
            HP_LOG_ERROR(kLogChannel, "UI render failed; the viewport continues without panels");
            m_UiFailureReported = true;
        }
        // Still returning true: losing the overlay must not take the scene with it (the degradation UiRenderer.h
        // specifies), and reporting failure here would stop the whole loop instead of just the panels.
    }
    else
    {
        // Only a fully composed frame is worth capturing, and "has ever been" rather than "this one": capture happens
        // after Run() returns, when the only live draw data is the last successful composite's.
        m_HasComposedFrame = true;
    }

    return true;
}

void EditorApplication::OnResize(UInt32 framebufferWidth, UInt32 framebufferHeight)
{
    // Nothing to do, and that is deliberate: viewport targets are sized to the panel, not the window, so a resize
    // invalidates them through the normal per-frame size check. Reallocating here would recreate textures at the wrong
    // size whenever a dock node owns the space between them.
    HP_UNUSED(framebufferWidth);
    HP_UNUSED(framebufferHeight);
}

// ---------------------------------------------------------------------------
// Viewport targets
// ---------------------------------------------------------------------------

CameraState EditorApplication::BuildViewportCamera(Float32 aspectRatio) const
{
    // The no-transform overload: this camera is not an entity in the world, it is the tool's eye. Using the hierarchy
    // version would imply an entity that does not exist and whose transform nobody maintains.
    return Renderer::ComputeCameraState(m_Camera, aspectRatio);
}

bool EditorApplication::EnsureViewportTargets(UInt32 width, UInt32 height)
{
    if (m_Application == nullptr)
    {
        return false;
    }

    width = std::max(kMinimumViewportExtent, width);
    height = std::max(kMinimumViewportExtent, height);

    if (m_ViewportHdrTarget != nullptr && m_ViewportWidth == width && m_ViewportHeight == height)
    {
        return true;
    }

    GraphicsDevice* device = m_Application->GetDevice();
    if (device == nullptr)
    {
        return false;
    }

    DestroyViewportTargets();

    TextureDesc hdrDesc;
    hdrDesc.Name = "editor-viewport-hdr";
    hdrDesc.Width = width;
    hdrDesc.Height = height;
    hdrDesc.Format = GraphicsFormat::Rgba16Float;
    hdrDesc.IsRenderTarget = true;
    // Sampled because the tone-map blit reads it. Keeping IsShaderResource here rather than creating a second texture
    // is what makes this identical to how the runtime sizes its own HDR target.
    hdrDesc.IsShaderResource = true;

    Ref<Texture> hdrColour = Texture::Create(*device, hdrDesc);
    if (hdrColour == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "could not create the viewport HDR texture ({}x{})", width, height);
        return false;
    }

    TextureDesc depthDesc;
    depthDesc.Name = "editor-viewport-depth";
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.Format = GraphicsFormat::D32Float;
    depthDesc.IsRenderTarget = true;
    depthDesc.IsShaderResource = false;

    Ref<Texture> depth = Texture::Create(*device, depthDesc);
    if (depth == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "could not create the viewport depth texture");
        return false;
    }

    RenderTargetDesc hdrTargetDesc;
    hdrTargetDesc.Name = "editor-viewport-hdr-target";
    hdrTargetDesc.Colour = hdrColour;
    hdrTargetDesc.Depth = depth;

    m_ViewportHdrTarget = RenderTarget::Create(*device, hdrTargetDesc);
    if (m_ViewportHdrTarget == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "could not create the viewport HDR render target");
        return false;
    }

    // The 8-bit display-referred copy widgets sample. Separate from HDR because ImGui composites it with straight alpha
    // over the panels, and an HDR source would carry values above 1.0 into that blend — the tone map exists precisely
    // to land in range.
    TextureDesc displayDesc;
    displayDesc.Name = "editor-viewport-colour";
    displayDesc.Width = width;
    displayDesc.Height = height;
    displayDesc.Format = GraphicsFormat::Rgba8Unorm;
    displayDesc.IsRenderTarget = true;
    displayDesc.IsShaderResource = true;

    Ref<Texture> colour = Texture::Create(*device, displayDesc);
    if (colour == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "could not create the viewport display texture");
        return false;
    }

    RenderTargetDesc displayTargetDesc;
    displayTargetDesc.Name = "editor-viewport-display";
    displayTargetDesc.Colour = colour;
    // No depth: the pass that writes this target is a full-screen blit, and declaring an attachment the blit never
    // clears costs memory and forces a layout the shader does not care about.
    displayTargetDesc.Depth = nullptr;

    m_ViewportDisplayTarget = RenderTarget::Create(*device, displayTargetDesc);
    if (m_ViewportDisplayTarget == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "could not create the viewport display render target");
        return false;
    }

    m_ViewportHdrColour = hdrColour;
    m_ViewportDepth = depth;
    m_ViewportDisplayColour = colour;
    m_ViewportWidth = width;
    m_ViewportHeight = height;

    // Re-registered because the previous id names a texture that no longer exists. Widgets read the new id from
    // PanelContext in this same frame, so there is no frame where they could sample the dead one.
    m_ViewportTextureId = m_Ui != nullptr ? m_Ui->RegisterTexture(colour) : 0ull;

    HP_LOG_INFO(kLogChannel, "viewport targets: {}x{}", width, height);
    return true;
}

bool EditorApplication::EnsureCaptureTargets(UInt32 width, UInt32 height)
{
    if (m_Application == nullptr)
    {
        return false;
    }

    if (m_CaptureTarget != nullptr && m_CaptureWidth == width && m_CaptureHeight == height)
    {
        return true;
    }

    GraphicsDevice* device = m_Application->GetDevice();
    if (device == nullptr)
    {
        return false;
    }

    m_CaptureTarget = nullptr;
    m_CaptureColour = nullptr;

    // Rgba8Unorm rather than the swapchain's format: the capture is read back by WriteScreenshot, which converts from a
    // named set of formats, and an 8-bit unorm target is exactly what a PNG holds — no float conversion step whose
    // clamping could hide an exposure bug behind "it looked fine in the PNG".
    TextureDesc desc;
    desc.Name = "editor-capture";
    desc.Width = width;
    desc.Height = height;
    desc.Format = GraphicsFormat::Rgba8Unorm;
    desc.IsRenderTarget = true;
    desc.IsShaderResource = true;

    m_CaptureColour = Texture::Create(*device, desc);
    if (m_CaptureColour == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "could not create the {}x{} capture texture", width, height);
        return false;
    }

    RenderTargetDesc targetDesc;
    targetDesc.Name = "editor-capture-target";
    targetDesc.Colour = m_CaptureColour;
    // No depth: the UI pass is flat 2D and does not read or write depth, and an unused attachment costs memory.
    targetDesc.Depth = nullptr;

    m_CaptureTarget = RenderTarget::Create(*device, targetDesc);
    if (m_CaptureTarget == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "could not create the capture render target");
        m_CaptureColour = nullptr;
        return false;
    }

    m_CaptureWidth = width;
    m_CaptureHeight = height;
    return true;
}

void EditorApplication::DestroyViewportTargets() noexcept
{
    if (m_ViewportTextureId != 0ull && m_Ui != nullptr)
    {
        m_Ui->UnregisterTexture(m_ViewportTextureId);
    }
    m_ViewportTextureId = 0ull;

    // Targets before their attachments: a RenderTarget holds Refs to its textures anyway, but writing the dependent
    // object down first means the order is right even if the desc's ownership ever changed.
    m_ViewportDisplayTarget = nullptr;
    m_ViewportHdrTarget = nullptr;
    m_ViewportDisplayColour = nullptr;
    m_ViewportHdrColour = nullptr;
    m_ViewportDepth = nullptr;

    m_ViewportWidth = 0;
    m_ViewportHeight = 0;
}

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

const char* EditorStopReasonName(EditorStopReason reason) noexcept
{
    switch (reason)
    {
        case EditorStopReason::Running:
            return "running";
        case EditorStopReason::WindowClosed:
            return "window closed";
        case EditorStopReason::FrameLimit:
            return "frame limit reached";
        case EditorStopReason::StartupFailed:
            return "startup failed";
    }
    return "unknown";
}

std::string FormatEntityLabel(const ECS::World& world, const Entity& entity)
{
    if (const EntityName* name = world.TryGetComponent<EntityName>(entity); name != nullptr && !name->Name.empty())
    {
        return name->Name;
    }
    // The slot in the fallback keeps two unnamed entities distinguishable and greppable in a log — which is what makes
    // "the third one" an answerable request instead of an argument.
    return "Entity (slot " + std::to_string(entity.Slot) + ")";
}

void RegisterEditorCommands(EditorConsole& console, UiState& state, EditorApplication& app)
{
    // Two halves by dependency: EditorPanels.cpp registers what only needs UI state, this file registers what needs the
    // hosted loop. One entry point from the outside either way (see EditorPanels.h).
    RegisterUiCommands(console, state, app);
    RegisterEngineCommands(console, state, app);
}

}  // namespace Hollowpoint::Editor
