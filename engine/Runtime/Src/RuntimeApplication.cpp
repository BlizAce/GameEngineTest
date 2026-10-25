#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/FileSystem.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Game/RegisterGameTypes.h"
#include "Hollowpoint/Game/SceneRenderer.h"
#include "Hollowpoint/Graphics/FrameRing.h"
#include "Hollowpoint/Graphics/GraphicsDevice.h"
#include "Hollowpoint/Graphics/RenderTarget.h"
#include "Hollowpoint/Graphics/Swapchain.h"
#include "Hollowpoint/Graphics/Texture.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Platform/PlatformInit.h"
#include "Hollowpoint/Platform/Window.h"
#include "Hollowpoint/Renderer/Camera.h"
#include "Hollowpoint/Renderer/Geometry.h"
#include "Hollowpoint/Renderer/Lights.h"
#include "Hollowpoint/Runtime/BuiltInMeshes.h"
#include "Hollowpoint/Runtime/RuntimeApplication.h"
#include "Hollowpoint/Runtime/Screenshot.h"
#include "Hollowpoint/Scene/LocalTransform.h"
#include "Hollowpoint/Scene/SceneIO.h"
#include "Hollowpoint/Scripting/EngineAPI.h"
#include "Hollowpoint/Scripting/RegisterScriptTypes.h"
#include "Hollowpoint/Scripting/ScriptSystem.h"
#include "Hollowpoint/Scripting/ScriptVM.h"

namespace Hollowpoint::Runtime
{

using Core::ReadFileText;
using ECS::Entity;
using ECS::World;
using Game::CollectInput;
using Game::RenderMeshEntry;
using Graphics::FrameRing;
using Graphics::GraphicsDevice;
using Graphics::GraphicsDeviceDesc;
using Graphics::GraphicsFormat;
using Graphics::RenderTarget;
using Graphics::RenderTargetDesc;
using Graphics::Swapchain;
using Graphics::SwapchainDesc;
using Graphics::Texture;
using Graphics::TextureDesc;
using Math::Float32;
using Math::Mat4;
using Math::Vec3;
using Math::Vec4;
using Platform::InputEvent;
using Platform::PlatformInitOptions;
using Platform::PlatformScope;
using Platform::Window;
using Platform::WindowDesc;
using Renderer::CameraComponent;
using Renderer::CameraState;
using Renderer::ComputeCameraState;
using Renderer::LightComponent;
using Scene::LoadSceneFromFile;
using Scene::LocalTransform;
using Scene::SceneLoadReport;
using Scripting::EngineAPIConfig;
using Scripting::RegisterEngineAPI;
using Scripting::RegisterScriptTypes;
using Scripting::ScriptSystem;
using Scripting::ScriptVM;

namespace
{

constexpr const char* kLogChannel = "RUNTIME";

[[nodiscard]] std::string DescribeSize(UInt32 width, UInt32 height)
{
    return std::to_string(width) + "x" + std::to_string(height);
}

}  // namespace

const char* StopReasonName(StopReason reason) noexcept
{
    switch (reason)
    {
        case StopReason::Running:
            return "running";
        case StopReason::WindowClosed:
            return "window closed";
        case StopReason::FrameLimit:
            return "frame limit reached";
        case StopReason::DeviceLost:
            return "device lost";
        case StopReason::StartupFailed:
            return "startup failed";
    }
    return "unknown";
}

RuntimeApplication::RuntimeApplication(RuntimeConfig config) : m_Config(std::move(config)) {}

RuntimeApplication::~RuntimeApplication()
{
    // Explicit reverse-order teardown rather than relying on member order alone, because the HDR target and the
    // swapchain's render targets are what the renderer's pipelines were created against: releasing a pipeline after its
    // framebuffer's device has gone is use-after-free inside NVRHI. Scripts go first (they may hold entity handles into
    // the world), then GPU objects, then the device, then GLFW.
    m_Scripts.reset();
    m_VM.reset();

    m_CaptureTarget = nullptr;
    m_CaptureColour = nullptr;
    m_HdrTarget = nullptr;
    m_HdrDepth = nullptr;
    m_HdrColour = nullptr;
    m_Renderer = nullptr;
    m_FrameRing = nullptr;
    m_Swapchain = nullptr;

    // GraphicsDevice and Window are raw pointers because their classes' destructors are private/protected and they are
    // handed out by Create() factories; delete is the documented release (see each header).
    delete m_Device;
    m_Device = nullptr;

    delete m_Window;
    m_Window = nullptr;

    delete m_Platform;
    m_Platform = nullptr;
}

bool RuntimeApplication::Start()
{
    if (m_Started)
    {
        return m_StopReason == StopReason::Running;
    }

    // Component types must be registered before any scene is read: SceneIO resolves components by name through the
    // factory, and an unregistered type is silently skipped rather than failing the load — which presents as "my scene
    // loaded but has no meshes", with the cause a file away from the symptom. Idempotent, so hosting this inside an
    // editor that already registered them is safe.
    Game::RegisterGameTypes();

    // ScriptComponent's scene-file name (`Script`) is registered here rather than inside Game::RegisterGameTypes for
    // one reason: Game must not link Scripting, so the type that carries a script name registers itself (see
    // RegisterScriptTypes.h). It still has to happen BEFORE the scene load below, because SceneIO skips an unregistered
    // component with a warning naming only the entity — and InitialiseScripts, which is where it was, runs after that
    // load. Observed exactly that way: `unknown component type 'Script'; skipped` on a scene whose scripts then never
    // bound, with the log line pointing at the scene file rather than at this call.
    RegisterScriptTypes();

    if (!StartWindowAndDevice())
    {
        m_StopReason = StopReason::StartupFailed;
        return false;
    }

    const UInt32 width = m_Window->GetFramebufferWidth();
    const UInt32 height = m_Window->GetFramebufferHeight();

    if (!CreateSwapchain(width, height))
    {
        m_StopReason = StopReason::StartupFailed;
        return false;
    }

    if (!EnsureHdrTarget(width, height))
    {
        m_StopReason = StopReason::StartupFailed;
        return false;
    }

    // The frame ring and the renderer must agree on the slot count. A renderer with fewer slots than the ring hands out
    // would assert (correctly) on the first frame past its own count — a startup crash whose message names neither
    // setting. Asserting the relationship here turns that into a one-line diagnostic at the place the two values are
    // set.
    if (m_Renderer == nullptr)
    {
        Game::SceneRendererDesc rendererDesc;
        rendererDesc.Name = m_Config.ApplicationName + "-renderer";
        rendererDesc.MaxLightsPerDraw = m_Config.MaxLightsPerDraw;
        // The renderer gets AT LEAST as many slots as the ring: it is legal for a renderer to own more state than the
        // loop uses, and asserting equality would forbid a host from pre-creating one sized for its own purposes.
        rendererDesc.FrameSlots = m_Config.FrameSlots;
        m_Renderer = Game::SceneRenderer::Create(*m_Device, rendererDesc);
        if (m_Renderer == nullptr)
        {
            HP_LOG_ERROR(kLogChannel, "scene renderer could not be created");
            m_StopReason = StopReason::StartupFailed;
            return false;
        }
    }

    if (m_Renderer->GetFrameSlotCount() < m_Config.FrameSlots)
    {
        HP_LOG_ERROR(kLogChannel,
                     "renderer has {} frame slot(s) but the frame ring has {} — frames would overwrite in-flight data",
                     m_Renderer->GetFrameSlotCount(), m_Config.FrameSlots);
        m_StopReason = StopReason::StartupFailed;
        return false;
    }

    if (m_FrameRing == nullptr)
    {
        m_FrameRing = FrameRing::Create(*m_Device, m_Config.FrameSlots);
        if (m_FrameRing == nullptr)
        {
            HP_LOG_ERROR(kLogChannel, "frame ring could not be created");
            m_StopReason = StopReason::StartupFailed;
            return false;
        }
    }

    if (!m_Config.ScenePath.empty() && !LoadScene(m_Config.ScenePath))
    {
        // Not fatal. A project starts with an empty world far more often than a renderer is broken, and refusing to
        // open a window because a scene path was mistyped makes the typo harder to see than a logged error plus an
        // empty view.
        HP_LOG_WARNING(kLogChannel, "startup scene '{}' did not load; continuing with an empty world",
                       m_Config.ScenePath);
    }

    if (m_Config.EnableScripting && !InitialiseScripts())
    {
        // Also not fatal, for the same reason: scripts are content and a project under construction references them
        // before writing them. The engine renders without them.
        HP_LOG_WARNING(kLogChannel, "scripting is enabled but did not initialise; running without scripts");
    }

    m_Started = true;
    HP_LOG_INFO(kLogChannel, "{} ready: {} on {}, swapchain {}, {} frame slot(s)", m_Config.ApplicationName,
                DescribeSize(width, height), m_Device->GetAdapter().Name,
                DescribeSize(m_SwapchainWidth, m_SwapchainHeight), m_Config.FrameSlots);
    return true;
}

bool RuntimeApplication::StartWindowAndDevice()
{
    PlatformInitOptions platformOptions;
    // An application that must show a window should not silently run headless: with fallback enabled the loop would
    // spin, render into nothing and report success. Callers who want headless behaviour set it explicitly via a
    // Null-preferred config path (a baker or dedicated server), not by accident on a machine with no display.
    platformOptions.AllowHeadlessFallback = false;
    m_Platform = new PlatformScope(platformOptions);

    if (!m_Platform->IsInitialized())
    {
        HP_LOG_ERROR(kLogChannel, "windowing could not initialise: {}", PlatformScope::GetFailureReason());
        return false;
    }

    WindowDesc windowDesc;
    windowDesc.Title = m_Config.ApplicationName;
    windowDesc.Width = m_Config.Width;
    windowDesc.Height = m_Config.Height;
    windowDesc.Resizable = m_Config.Resizable;
    windowDesc.Visible = m_Config.StartVisible;

    m_Window = Window::Create(windowDesc);
    if (m_Window == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "could not create a {} window", DescribeSize(m_Config.Width, m_Config.Height));
        return false;
    }

    // The instance extensions must be read from the window BEFORE the device is created: VK_KHR_wayland_surface / _xlib
    // are VkInstance extensions, and GraphicsDevice builds its own instance. Omitting them does not fail here — it
    // fails later as "no window attached" at swapchain creation, naming the wrong thing entirely (documented on
    // AttachWindow). This ordering is the single most error-prone line in the startup path.
    GraphicsDeviceDesc deviceDesc;
    deviceDesc.ApplicationName = m_Config.ApplicationName;
    deviceDesc.EnableValidation = m_Config.EnableValidation;
    deviceDesc.ExtraInstanceExtensions = m_Window->GetRequiredVulkanInstanceExtensions();

    m_Device = GraphicsDevice::Create(deviceDesc);
    if (m_Device == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "no graphics device: {}", GraphicsDevice::GetFailureReason());
        return false;
    }

    HP_LOG_INFO(kLogChannel, "adapter: {} ({} MiB dedicated video memory), validation {}", m_Device->GetAdapter().Name,
                m_Device->GetAdapter().DedicatedVideoMemoryBytes / (1024ull * 1024ull),
                m_Device->IsValidationEnabled() ? "on" : "off");

    if (!m_Device->AttachWindow(m_Window))
    {
        HP_LOG_ERROR(kLogChannel, "could not attach the window to the device (no presentation path)");
        return false;
    }

    return true;
}

bool RuntimeApplication::CreateSwapchain(UInt32 width, UInt32 height)
{
    if (!m_Device->CanPresent())
    {
        HP_LOG_ERROR(kLogChannel, "this adapter cannot present (VK_KHR_swapchain unavailable)");
        return false;
    }

    SwapchainDesc swapchainDesc;
    // The window's current framebuffer size, falling back to the configured size: on Wayland a surface may report 0
    // until the compositor sizes it, and SwapchainDesc takes a preferred extent for exactly that case.
    swapchainDesc.Width = width != 0u ? width : m_Config.Width;
    swapchainDesc.Height = height != 0u ? height : m_Config.Height;
    swapchainDesc.Mode = m_Config.PresentMode;
    swapchainDesc.Name = m_Config.ApplicationName + "-swapchain";

    m_Swapchain = Swapchain::Create(*m_Device, swapchainDesc);
    if (m_Swapchain == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "could not create a swapchain at {}",
                     DescribeSize(swapchainDesc.Width, swapchainDesc.Height));
        return false;
    }

    m_SwapchainWidth = m_Swapchain->GetWidth();
    m_SwapchainHeight = m_Swapchain->GetHeight();
    return true;
}

bool RuntimeApplication::EnsureHdrTarget(UInt32 width, UInt32 height)
{
    if (m_HdrTarget != nullptr && m_SwapchainWidth == width && m_SwapchainHeight == height)
    {
        return true;  // already the right size
    }

    DestroyHdrTarget();

    if (width == 0u || height == 0u)
    {
        // A zero-size target is not creatable, and a window that is minimised legitimately reports one. Reported once
        // by the caller's resize logic; here it is simply "nothing to render into yet".
        HP_LOG_WARNING(kLogChannel, "cannot create an HDR target with size {}", DescribeSize(width, height));
        return false;
    }

    TextureDesc colourDesc;
    colourDesc.Name = m_Config.ApplicationName + "-hdr";
    colourDesc.Width = width;
    colourDesc.Height = height;
    // Rgba16Float: the scene accumulates radiance where values above 1 are meaningful (the sun is thousands of nits),
    // and the tone map consumes that range. An 8-bit target would clip before the curve ever saw it.
    colourDesc.Format = GraphicsFormat::Rgba16Float;
    colourDesc.IsRenderTarget = true;
    // Sampled by the tone-map blit — without this flag the blit's binding set has no view to name, which SceneRenderer
    // now refuses with a message saying exactly that.
    colourDesc.IsShaderResource = true;

    m_HdrColour = Texture::Create(*m_Device, colourDesc);
    if (m_HdrColour == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "could not create the HDR colour target");
        return false;
    }

    TextureDesc depthDesc;
    depthDesc.Name = m_Config.ApplicationName + "-hdr-depth";
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.Format = GraphicsFormat::D32Float;  // the engine's depth default (GraphicsTypes.h)
    depthDesc.IsRenderTarget = true;

    m_HdrDepth = Texture::Create(*m_Device, depthDesc);
    if (m_HdrDepth == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "could not create the HDR depth target");
        return false;
    }

    RenderTargetDesc targetDesc;
    targetDesc.Name = m_Config.ApplicationName + "-hdr-framebuffer";
    targetDesc.Colour = m_HdrColour;
    targetDesc.Depth = m_HdrDepth;

    m_HdrTarget = RenderTarget::Create(*m_Device, targetDesc);
    if (m_HdrTarget == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "could not create the HDR framebuffer");
        return false;
    }

    return true;
}

void RuntimeApplication::DestroyHdrTarget() noexcept
{
    // Order matters only in that the framebuffer must not outlive its attachments by NVRHI's accounting; releasing all
    // three together is enough, and the renderer holds no reference to them (its pipelines are keyed by format, not
    // identity).
    m_HdrTarget = nullptr;
    m_HdrDepth = nullptr;
    m_HdrColour = nullptr;
}

bool RuntimeApplication::HandleResize(UInt32 framebufferWidth, UInt32 framebufferHeight)
{
    if (framebufferWidth == 0u || framebufferHeight == 0u)
    {
        // A minimised window reports a zero size. Rendering into nothing would fail every allocation; skipping the
        // frame is correct and the next resize restores it.
        HP_LOG_INFO(kLogChannel, "resize to {} ignored (minimised?)",
                    DescribeSize(framebufferWidth, framebufferHeight));
        return true;  // not an error: just nothing to draw this frame
    }

    if (framebufferWidth == m_SwapchainWidth && framebufferHeight == m_SwapchainHeight && m_HdrTarget != nullptr)
    {
        return true;  // nothing actually changed — resize events arrive in bursts
    }

    HP_LOG_INFO(kLogChannel, "resizing to {}", DescribeSize(framebufferWidth, framebufferHeight));

    if (!m_Swapchain->Resize(framebufferWidth, framebufferHeight))
    {
        HP_LOG_ERROR(kLogChannel, "swapchain resize failed; the old chain remains usable but is the wrong size");
        return false;
    }

    m_SwapchainWidth = m_Swapchain->GetWidth();
    m_SwapchainHeight = m_Swapchain->GetHeight();

    // The capture target is sized to the swapchain, so it is dropped rather than resized: recreating on next use costs
    // one texture and keeps this path to a single allocation site. Keeping a stale-extent target would render a
    // stretched image that looks like a projection bug.
    DestroyCaptureTarget();

    if (!EnsureHdrTarget(m_SwapchainWidth, m_SwapchainHeight))
    {
        return false;
    }

    // The renderer's pipelines are keyed by (format, depth presence, cull mode) and the swapchain format does not
    // change on resize, so no pipeline is rebuilt here — which is the point of that keying. Nothing to invalidate.
    return true;
}

bool RuntimeApplication::LoadScene(const std::string& path)
{
    const SceneLoadReport report = LoadSceneFromFile(m_World, path);
    report.ReportToLog(kLogChannel);

    if (!report.Loaded)
    {
        return false;
    }

    // Scripts must be (re)bound after a load: ScriptComponent stores only a script *name*, and binding is what turns a
    // name into callable code. Doing it here rather than in the loader keeps SceneIO free of Lua, and means loading a
    // scene with an unknown script name produces one warning per name instead of a hard failure.
    if (m_Scripts != nullptr)
    {
        BindScriptsInWorld();
    }

    return true;
}

void RuntimeApplication::BindScriptsInWorld()
{
    if (m_Scripts == nullptr)
    {
        return;
    }

    // Collect names first: BindScript touches the VM, and calling it while iterating the world would let a script's
    // loader side effect mutate the very component array being walked.
    std::vector<std::string> names;
    m_World.ForEachEntity([&](const Entity& entity) {
        const auto* component = m_World.TryGetComponent<Scripting::ScriptComponent>(entity);
        if (component != nullptr && !component->ScriptName.empty())
        {
            names.push_back(component->ScriptName);
        }
    });

    for (const std::string& name : names)
    {
        if (m_Scripts->IsScriptBound(name))
        {
            continue;
        }
        // Bind failure is reported by ScriptSystem itself (once per name). A scene referencing a script that has not
        // been loaded yet is normal — the entities simply do not tick until it arrives.
        //
        // BindAndInitialize rather than BindScript: these entities were created by the loader, before any VM existed,
        // so nothing had run their on_create (see ScriptSystem::BindAndInitialize for why binding is the moment that
        // belongs).
        m_Scripts->BindAndInitialize(name, m_World);
    }
}

bool RuntimeApplication::InitialiseScripts()
{
    m_VM = std::make_unique<ScriptVM>();
    if (!m_VM->IsInitialized())
    {
        HP_LOG_ERROR(kLogChannel, "the script VM could not be created");
        m_VM.reset();
        return false;
    }

    EngineAPIConfig apiConfig;
    apiConfig.World = &m_World;
    // No prefab resolver yet: hp.spawn_prefab reports "unknown asset" rather than guessing. The asset database is the
    // next module after this one, and its interface should be designed against a real caller rather than guessed at
    // here.
    RegisterEngineAPI(*m_VM, apiConfig);

    m_Scripts = std::make_unique<ScriptSystem>(*m_VM);

    // Startup script: `scripts/init.lua` if present. Loaded before the scene's scripts are bound so it can define
    // globals behaviours depend on. A missing file is not an error — plenty of projects have no bootstrap script.
    const std::string initPath = m_Config.ScriptDirectory + "/init.lua";
    const auto initFile = ReadFileText(initPath);
    if (initFile.Succeeded)
    {
        std::string error;
        if (!m_VM->Run(initFile.Value, error, initPath))
        {
            HP_LOG_ERROR(kLogChannel, "startup script failed: {}", error);
        }
    }

    // A scene loaded above (Start loads Config.ScenePath before reaching here) already named its scripts, and
    // BindScriptsInWorld can only bind tables that exist in the VM — so a behaviour defined by init.lua was unavoidably
    // "not found" at scene-load time. Re-binding now is what makes `scripts/init.lua` + `"Script"` in a scene work as
    // one feature, and it is cheap: already-bound names are skipped.
    BindScriptsInWorld();
    return true;
}

bool RuntimeApplication::RunScriptFile(const std::string& filename)
{
    if (m_VM == nullptr || !m_VM->IsInitialized())
    {
        HP_LOG_WARNING(kLogChannel, "RunScriptFile called with no script VM");
        return false;
    }

    const std::string path = m_Config.ScriptDirectory + "/" + filename;
    const auto file = ReadFileText(path);
    if (!file.Succeeded)
    {
        HP_LOG_ERROR(kLogChannel, "script '{}' could not be read: {}", filename, file.Message);
        return false;
    }

    std::string error;
    if (!m_VM->Run(file.Value, error, path))
    {
        HP_LOG_ERROR(kLogChannel, "script '{}' failed: {}", filename, error);
        return false;
    }

    // The file may have defined new behaviour tables; anything the world now references must be bound.
    BindScriptsInWorld();
    return true;
}

void RuntimeApplication::UpdateAndCollect()
{
    // --- scripts ------------------------------------------------------------------
    // Before transforms: a script that moves an entity this frame must show up this frame, not next. Running after the
    // transform pass is the classic one-frame-late camera and "physics lags input" bug.
    if (m_Scripts != nullptr)
    {
        m_Scripts->Update(m_World, static_cast<float>(m_Time.GetDeltaSeconds()));
    }

    // --- transforms ---------------------------------------------------------------
    m_Transforms.Update(m_World);

    // --- camera -------------------------------------------------------------------
    // The first active camera wins, deterministically by entity slot order. An explicit "which camera is live"
    // component is a later feature; until then the rule has to be stated because two cameras in a scene otherwise make
    // the view depend on creation order nobody chose deliberately.
    const CameraComponent* camera = nullptr;
    Entity cameraEntity{};

    m_World.ForEachEntity([&](const Entity& entity) {
        if (camera != nullptr)
        {
            return;
        }
        const auto* found = m_World.TryGetComponent<CameraComponent>(entity);
        if (found != nullptr && found->Active)
        {
            camera = found;
            cameraEntity = entity;
        }
    });

    // Aspect from the HDR target, which is sized to the swapchain: not from the window's content size, which differs by
    // the DPI scale factor and would stretch the image on a HiDPI display.
    const Float32 aspect = (m_SwapchainWidth != 0u && m_SwapchainHeight != 0u)
                               ? static_cast<Float32>(m_SwapchainWidth) / static_cast<Float32>(m_SwapchainHeight)
                               : 1.0f;

    CameraState cameraState;
    if (camera != nullptr)
    {
        // The hierarchy's Transform overload, not the no-transform one: a camera parented to a moving object (a
        // vehicle, an editor's orbit rig) must move with it. Passing only the component would render such a camera
        // stuck at its authored position while the thing carrying it drove away — and in a scene where every mesh IS
        // parented, that is all of them.
        const Math::Transform worldTransform = m_Transforms.GetWorldTransform(m_World, cameraEntity);
        cameraState = ComputeCameraState(*camera, &worldTransform, aspect);
    }
    else
    {
        // No camera: a default one pulled back on +Z so an authored scene without a camera still shows its content
        // rather than rendering nothing. Reported once per startup rather than per frame — after the first warning the
        // situation is established, and 144 lines a second buries everything else.
        CameraComponent fallback;
        fallback.Position = Vec3(0.0f, 1.5f, 6.0f);
        fallback.Target = Vec3(0.0f, 0.0f, 0.0f);
        cameraState = ComputeCameraState(fallback, aspect);

        static bool s_LoggedNoCamera = false;
        if (!s_LoggedNoCamera)
        {
            HP_LOG_WARNING(kLogChannel, "scene has no active CameraComponent; using a default view");
            s_LoggedNoCamera = true;
        }
    }

    // --- collection ---------------------------------------------------------------
    m_MeshEntities.clear();
    m_MeshTransforms.clear();
    m_LightEntities.clear();
    m_LightTransforms.clear();

    m_World.ForEachEntity([&](const Entity& entity) {
        if (m_World.TryGetComponent<Game::MeshRenderer>(entity) != nullptr)
        {
            m_MeshEntities.push_back(entity);
            m_MeshTransforms.push_back(m_Transforms.GetWorldMatrix(m_World, entity));
        }
        if (m_World.TryGetComponent<LightComponent>(entity) != nullptr)
        {
            m_LightEntities.push_back(entity);
            m_LightTransforms.push_back(m_Transforms.GetWorldMatrix(m_World, entity));
        }
    });

    CollectInput input;
    input.World = &m_World;
    input.MeshEntities = m_MeshEntities.data();
    input.MeshWorldTransforms = m_MeshTransforms.data();
    input.MeshCount = m_MeshEntities.size();
    input.LightEntities = m_LightEntities.data();
    input.LightWorldTransforms = m_LightTransforms.data();
    input.LightCount = m_LightEntities.size();
    input.CameraPosition = cameraState.EyePosition;
    // The only built-in resolver for now. An asset-backed one replaces this single line — which is the seam
    // RenderPrimitives.h was written around.
    input.ResolveGeometry = &BuiltInMeshes::Resolve;

    m_Plan = CollectRenderables(input);

    // The collector deliberately does not build matrices (it has no camera state), so the pass owner supplies them here
    // — leaving the plan's identity default would render everything at the origin at unit scale, plausibly and wrongly.
    m_Plan.Pass.ViewProjection = cameraState.ViewProjection;
}

//! How many consecutive frames may be skipped before the run gives up. A Wayland compositor can take a handful of
//! frames to size and map a new surface, so this must not be tight; it exists only to turn "spins forever producing
//! nothing" into a reported failure. At that many skips something is structurally wrong rather than transiently busy.
constexpr UInt64 kMaxConsecutiveSkippedFrames = 240ull;

bool RuntimeApplication::NoteSkippedFrame(const char* reason)
{
    ++m_ConsecutiveSkippedFrames;

    if (m_ConsecutiveSkippedFrames >= kMaxConsecutiveSkippedFrames)
    {
        HP_LOG_ERROR(
            kLogChannel,
            "no frame has presented for {} consecutive attempts (last reason: {}) — stopping rather than spinning "
            "without output",
            m_ConsecutiveSkippedFrames, reason);
        m_StopReason = StopReason::DeviceLost;
        return false;
    }

    // One line on the first skip: a run that produces nothing while otherwise appearing to succeed is the thing to
    // surface.
    if (m_ConsecutiveSkippedFrames == 1ull)
    {
        HP_LOG_INFO(kLogChannel, "frame skipped ({})", reason);
    }
    return true;
}

bool RuntimeApplication::RunFrame()
{
    if (!m_Started)
    {
        HP_LOG_ERROR(kLogChannel, "RunFrame called before a successful Start()");
        return false;
    }

    if (m_StopReason != StopReason::Running)
    {
        return false;
    }

    // --- 1. input -------------------------------------------------------------------
    m_Events.clear();
    m_Window->PollEvents(m_Events);

    for (const InputEvent& event : m_Events)
    {
        if (event.EventType == InputEvent::Type::WindowResize)
        {
            // Recorded, not acted on: resizing a swapchain while an image is acquired is exactly the state Vulkan
            // dislikes. The rebuild happens below, before this frame acquires anything.
            m_ResizePending = true;
            m_PendingWidth = event.Width;
            m_PendingHeight = event.Height;
        }
    }

    if (m_Window->ShouldClose())
    {
        m_StopReason = StopReason::WindowClosed;
        return false;
    }

    // --- resize, before acquiring ----------------------------------------------------
    // Prefer the authoritative framebuffer size over the event's payload: on Wayland the event can arrive with the
    // compositor's preferred size while the window reports something else, and creating a swapchain that disagrees with
    // the surface is VK_ERROR_SURFACE_LOST territory.
    if (m_ResizePending)
    {
        m_ResizePending = false;
        const UInt32 width = m_Window->GetFramebufferWidth();
        const UInt32 height = m_Window->GetFramebufferHeight();
        if (!HandleResize(width != 0u ? width : m_PendingWidth, height != 0u ? height : m_PendingHeight))
        {
            m_StopReason = StopReason::StartupFailed;
            return false;
        }
    }

    // A window with no size yet (first Wayland frame) has nothing to render into. Skipping is normal startup behaviour,
    // not error — and it must not consume a frame ring slot, which would leak one slot per skipped frame until
    // BeginFrame reports Full and the loop wedges.
    //
    // Skipped frames do NOT count toward MaxFrameCount (only presented frames do), so this path needs its own bound:
    // with
    // `--frames 3` against a window that never gets a size, "return true" without a counter is an infinite loop in
    // exactly the automated invocation used to verify the engine headlessly. NoteSkippedFramesExceeded reports where it
    // stalled.
    if (m_HdrTarget == nullptr || m_SwapchainWidth == 0u || m_SwapchainHeight == 0u)
    {
        if (!NoteSkippedFrame("no renderable extent"))
        {
            return false;
        }
        return true;
    }

    // --- 2. time ---------------------------------------------------------------------
    m_Time.Tick();

    // --- 3-5. scripts, transforms, collection ----------------------------------------
    UpdateAndCollect();

    // --- 6. acquire + render ----------------------------------------------------------
    if (m_FrameRing->BeginFrame(true) != Graphics::FrameQueueStatus::Began)
    {
        // BeginFrame(waitForFree=true) blocks for a free slot; failing anyway means every slot is held by a frame that
        // never ended, which is a bug in this loop rather than a runtime condition. Reported loudly because the symptom
        // (a frozen window at 0% GPU) gives no hint of it.
        HP_LOG_ERROR(kLogChannel, "frame ring refused a new frame; {} slot(s) outstanding",
                     m_FrameRing->GetOutstandingCount());
        m_StopReason = StopReason::DeviceLost;
        return false;
    }

    const UInt32 slot = m_FrameRing->GetCurrentSlot();

    const Graphics::AcquiredImage acquired = m_Swapchain->Acquire();
    if (acquired.Status != Graphics::AcquireStatus::Success)
    {
        // The ring slot was begun but nothing was submitted into it, so it must be ended or the loop leaks slots and
        // wedges. EndFrame with no work is explicitly supported ("completion recorded against everything queued so
        // far").
        m_FrameRing->EndFrame(slot);

        if (acquired.Status == Graphics::AcquireStatus::OutOfDate)
        {
            if (!HandleResize(m_Window->GetFramebufferWidth(), m_Window->GetFramebufferHeight()))
            {
                m_StopReason = StopReason::StartupFailed;
                return false;
            }
            // A resize rebuild is progress, not a stall — but it still has to be bounded, or a compositor that keeps
            // reporting the chain out of date holds the loop open forever. Same guard as every other skip.
            if (!NoteSkippedFrame("swapchain out of date"))
            {
                return false;
            }
            return true;  // retry next frame with the rebuilt chain
        }

        if (acquired.Status == Graphics::AcquireStatus::Unsupported)
        {
            HP_LOG_ERROR(kLogChannel, "presentation is not supported by this device");
            m_StopReason = StopReason::DeviceLost;
            return false;
        }

        // Timeout: compositor busy or window minimised. Bounded for the same reason as above — a run asked to stop
        // after N frames must stop even when the compositor never hands out another image.
        if (!NoteSkippedFrame("acquire timeout"))
        {
            return false;
        }
        return true;
    }

    RenderTarget* backBuffer = m_Swapchain->GetRenderTarget(acquired.ImageIndex);
    if (backBuffer == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "swapchain has no render target for image {}", acquired.ImageIndex);
        m_FrameRing->EndFrame(slot);
        m_StopReason = StopReason::DeviceLost;
        return false;
    }

    const Vec4 background(m_Config.BackgroundRed, m_Config.BackgroundGreen, m_Config.BackgroundBlue, 1.0f);
    if (!m_Renderer->RenderFrame(m_Plan, *m_HdrTarget, *backBuffer, m_Config.Exposure, slot, background))
    {
        // The renderer logs the specific cause (shader, pipeline, buffer). Stopping on the first failure would hide it
        // under a wall of identical messages if it recurred, and continuing past a device-level failure produces
        // nothing but errors.
        HP_LOG_ERROR(kLogChannel, "frame {} failed to render", m_Frames + 1ull);
        m_FrameRing->EndFrame(slot);
        m_StopReason = StopReason::DeviceLost;
        return false;
    }

    // --- 7. present + end frame -------------------------------------------------------
    if (!m_Swapchain->Present(acquired.ImageIndex))
    {
        m_FrameRing->EndFrame(slot);

        // A presentation that went out of date mid-frame is a resize landing between acquire and present: rebuild and
        // carry on, which is what the API documents. Anything else means the chain cannot present.
        if (m_Swapchain->NeedsRebuild(m_Window->GetFramebufferWidth(), m_Window->GetFramebufferHeight()))
        {
            if (!HandleResize(m_Window->GetFramebufferWidth(), m_Window->GetFramebufferHeight()))
            {
                m_StopReason = StopReason::StartupFailed;
                return false;
            }
            ++m_Frames;
            return true;
        }

        HP_LOG_ERROR(kLogChannel, "presentation failed and the swapchain does not want a rebuild");
        m_StopReason = StopReason::DeviceLost;
        return false;
    }

    // Last, deliberately: completion is recorded against everything queued at this moment, so ending earlier would
    // report the frame finished while its draws were still in flight — and the next acquire of this slot would then
    // overwrite them.
    m_FrameRing->EndFrame(slot);

    ++m_Frames;
    m_ConsecutiveSkippedFrames = 0;  // a presented frame is progress, whatever preceded it

    if (m_Config.MaxFrameCount != 0ull && m_Frames >= m_Config.MaxFrameCount)
    {
        m_StopReason = StopReason::FrameLimit;
        return false;
    }

    return true;
}

StopReason RuntimeApplication::Run()
{
    while (RunFrame())
    {
        // Idling when the window is not visible keeps a backgrounded application off the CPU. WaitEvents processes
        // messages, so the next RunFrame still sees input and close requests promptly.
        if (m_Config.IdleWaitSeconds > 0.0 && m_Window != nullptr && !m_Window->IsFocused())
        {
            m_Window->WaitEvents(m_Config.IdleWaitSeconds);
        }
    }

    HP_LOG_INFO(kLogChannel, "stopped after {} frame(s): {}", m_Frames, StopReasonName(m_StopReason));
    return m_StopReason;
}

void RuntimeApplication::RequestStop(StopReason reason) noexcept
{
    m_StopReason = reason;
}

StopReason RuntimeApplication::GetStopReason() const noexcept
{
    return m_StopReason;
}

bool RuntimeApplication::IsRunning() const noexcept
{
    return m_Started && m_StopReason == StopReason::Running;
}

World& RuntimeApplication::GetWorld() noexcept
{
    return m_World;
}

const World& RuntimeApplication::GetWorld() const noexcept
{
    return m_World;
}

GraphicsDevice* RuntimeApplication::GetDevice() const noexcept
{
    return m_Device;
}

Window* RuntimeApplication::GetWindow() const noexcept
{
    return m_Window;
}

Game::SceneRenderer* RuntimeApplication::GetRenderer() const noexcept
{
    return m_Renderer.Get();
}

UInt64 RuntimeApplication::GetFrameCount() const noexcept
{
    return m_Frames;
}

UInt64 RuntimeApplication::GetDrawCount() const noexcept
{
    return m_Renderer != nullptr ? m_Renderer->GetDrawCount() : 0ull;
}

const RuntimeConfig& RuntimeApplication::GetConfig() const noexcept
{
    return m_Config;
}

bool RuntimeApplication::EnsureCaptureTarget()
{
    if (m_CaptureTarget != nullptr)
    {
        return true;
    }

    if (m_Device == nullptr || m_SwapchainWidth == 0u || m_SwapchainHeight == 0u)
    {
        HP_LOG_ERROR(kLogChannel, "cannot capture a screenshot before the swapchain has an extent");
        return false;
    }

    TextureDesc colourDesc;
    colourDesc.Name = m_Config.ApplicationName + "-capture";
    colourDesc.Width = m_SwapchainWidth;
    colourDesc.Height = m_SwapchainHeight;
    // 8-bit UNORM, matching what a display receives: the point of a capture is to see what the player sees, including
    // the quantisation the real back buffer applies. Capturing the HDR target instead would show a brighter,
    // banding-free image that hides exactly the post-transform problems a screenshot is taken to check.
    colourDesc.Format = GraphicsFormat::Rgba8Unorm;
    colourDesc.IsRenderTarget = true;

    m_CaptureColour = Texture::Create(*m_Device, colourDesc);
    if (m_CaptureColour == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "could not create the {} capture texture",
                     DescribeSize(m_SwapchainWidth, m_SwapchainHeight));
        return false;
    }

    RenderTargetDesc targetDesc;
    targetDesc.Name = m_Config.ApplicationName + "-capture-framebuffer";
    targetDesc.Colour = m_CaptureColour;
    // No depth: the tone-map blit reads only colour, so a depth attachment would be allocated, cleared and never
    // touched.

    m_CaptureTarget = RenderTarget::Create(*m_Device, targetDesc);
    if (m_CaptureTarget == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "could not create the capture framebuffer");
        m_CaptureColour = nullptr;
        return false;
    }

    return true;
}

void RuntimeApplication::DestroyCaptureTarget() noexcept
{
    m_CaptureTarget = nullptr;
    m_CaptureColour = nullptr;
}

bool RuntimeApplication::CaptureScreenshot(const std::string& path)
{
    if (m_Device == nullptr || m_Renderer == nullptr || m_HdrColour == nullptr)
    {
        HP_LOG_ERROR(kLogChannel, "CaptureScreenshot called before the renderer and HDR target existed");
        return false;
    }

    if (!EnsureCaptureTarget())
    {
        return false;
    }

    // Re-run the display transform into our own target rather than reading back what was presented — see the header for
    // why a presented image must not be touched. Same source, same exposure, same extents, so the pixels match the
    // window.
    if (!m_Renderer->BlitTonemap(*m_HdrColour, *m_CaptureTarget, m_Config.Exposure))
    {
        HP_LOG_ERROR(kLogChannel, "the tone-map blit into the capture target failed");
        return false;
    }

    std::string error;
    if (!WriteScreenshot(*m_Device, *m_CaptureColour, path, error))
    {
        HP_LOG_ERROR(kLogChannel, "screenshot failed: {}", error);
        return false;
    }

    HP_LOG_INFO(kLogChannel, "wrote {} ({})", path, DescribeSize(m_SwapchainWidth, m_SwapchainHeight));
    return true;
}

}  // namespace Hollowpoint::Runtime
