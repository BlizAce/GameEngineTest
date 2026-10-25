// End-to-end tests for RuntimeApplication: the frame loop that owns window → device → swapchain → frame ring → forward
// HDR pass → tone map → present, plus scene loading and scripts inside it.
//
// Why this suite exists at all: every subsystem below it had its own tests, and the engine still could not run — the
// gap between "each part works" and "the engine starts and shows a frame" is exactly where integration lives, and until
// this file nothing tested it. A case here starts the real application against a real window, runs real frames, and
// asserts on what came out: draws submitted, scene entities present, a script visibly moving an entity, a screenshot
// holding a real PNG, and — because validation layers route into the engine log — no Vulkan errors from the loop
// itself.
//
// Display policy: these need a compositor, so windows are mapped (an unmapped Wayland window never receives a
// compositor-allocated extent, which would make every case here exercise the "nothing to render" path and pass at
// nothing). A host without a display skips with a WARN naming the reason.

#include "TestSupport.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Core/FileSystem.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/ECS/World.h"
#include "Hollowpoint/Game/MeshRenderer.h"
#include "Hollowpoint/Graphics/GraphicsDevice.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Platform/PlatformInit.h"
#include "Hollowpoint/Renderer/Lights.h"
#include "Hollowpoint/Runtime/RuntimeApplication.h"
#include "Hollowpoint/Scene/LocalTransform.h"

#include <unistd.h>

using namespace Hollowpoint;
using Core::Size;
using Core::UInt32;
using ECS::Entity;
using Math::Vec3;
using Platform::PlatformInitOptions;
using Platform::PlatformScope;
using Runtime::RuntimeApplication;
using Runtime::RuntimeConfig;
using Runtime::StopReason;

namespace
{

//! Process-wide windowing scope on the platform's real backend, or nullptr when it cannot initialise. Shared by every
//! case (GLFW is process-wide, so a second init would fail) and the reason these cases skip rather than crash headless.
[[nodiscard]] PlatformScope* GetPlatformScope()
{
    static PlatformScope* scope = []() -> PlatformScope* {
        static PlatformScope instance = [] {
            PlatformInitOptions options;
            options.AllowHeadlessFallback = true;
            return PlatformScope(options);
        }();

        return instance.IsInitialized() ? &instance : nullptr;
    }();

    return scope;
}

[[nodiscard]] bool DisplayAvailable()
{
    PlatformScope* scope = GetPlatformScope();
    return scope != nullptr && !scope->IsHeadless();
}

#define SKIP_WITHOUT_DISPLAY()                                \
    do                                                        \
    {                                                         \
        if (!DisplayAvailable())                              \
        {                                                     \
            WARN("skipping: no display server on this host"); \
            return;                                           \
        }                                                     \
    }                                                         \
    while (false)

//! A scratch project directory (scene file + scripts/), removed on scope exit.
class ScopedProjectDirectory
{
public:
    explicit ScopedProjectDirectory(std::string_view name)
    {
        Path = std::filesystem::temp_directory_path() /
               ("hollowpoint-runtime-test-" + std::string(name) + "-" + std::to_string(::getpid()));
        std::filesystem::remove_all(Path);
        REQUIRE(std::filesystem::create_directories(Path));
        Scripts = (Path / "scripts").string();
        REQUIRE(std::filesystem::create_directories(Scripts));
    }

    ~ScopedProjectDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(Path, ignored);
    }

    [[nodiscard]] std::string File(const std::string& name) const
    {
        return (Path / name).string();
    }

    [[nodiscard]] std::string ScriptFile(const std::string& name) const
    {
        return (std::filesystem::path(Scripts) / name).string();
    }

    void Write(const std::string& path, std::string_view contents) const
    {
        const Core::FileOperationResult result = Core::WriteFileTruncate(path, contents);
        INFO("write failed: " << result.Message);
        REQUIRE(result.Succeeded);
    }

    ScopedProjectDirectory(const ScopedProjectDirectory&) = delete;
    ScopedProjectDirectory& operator=(const ScopedProjectDirectory&) = delete;

    std::filesystem::path Path;
    std::string Scripts;
};

//! A minimal but complete scene: camera, one light, two cubes. `scriptNameOnFirstMesh` attaches a Script component to
//! the red cube so a case can watch a behaviour run; empty means no scripts at all.
/**
 * Rotations are identity throughout, so an assertion that "the script changed this transform" has no other candidate
 * cause.
 */
[[nodiscard]] std::string MakeSceneText(std::string_view scriptNameOnFirstMesh = {})
{
    const std::string script = scriptNameOnFirstMesh.empty() ? std::string()
                                                             : ",\n        \"Script\": { \"scriptName\": \"" +
                                                                   std::string(scriptNameOnFirstMesh) + "\" }";

    return std::string(R"({
  "formatVersion": 1,
  "name": "Runtime test scene",
  "entities": [
    { "id": 0, "components": {
        "LocalTransform": { "position": {"x":0,"y":1.2,"z":6}, "rotation": {"x":0,"y":0,"z":0,"w":1},
                            "scale": {"x":1,"y":1,"z":1} },
        "Camera": { "position": {"x":0,"y":1.2,"z":6}, "target": {"x":0,"y":0,"z":0},
                    "verticalFieldOfView": 1.0, "nearPlane": 0.1, "farPlane": 100 } } },
    { "id": 1, "components": {
        "Light": { "type": "Directional", "color": {"x":1,"y":1,"z":1}, "intensity": 3.0,
                   "direction": {"x":0.3,"y":-1.0,"z":-0.4} } } },
    { "id": 2, "components": {
        "LocalTransform": { "position": {"x":-1,"y":0.5,"z":0}, "rotation": {"x":0,"y":0,"z":0,"w":1},
                            "scale": {"x":1,"y":1,"z":1} },
        "MeshRenderer": { "meshPath": "engine:cube", "baseColor": {"x":0.8,"y":0.2,"z":0.2},
                          "roughness": 0.4, "metallic": 0.0 })" +
                       script + R"( } },
    { "id": 3, "components": {
        "LocalTransform": { "position": {"x":1,"y":0.5,"z":0}, "rotation": {"x":0,"y":0,"z":0,"w":1},
                            "scale": {"x":1,"y":1,"z":1} },
        "MeshRenderer": { "meshPath": "engine:cube", "baseColor": {"x":0.2,"y":0.6,"z":0.9},
                          "roughness": 0.2, "metallic": 0.5 } } }
  ]
})");
}

//! Config for a bounded run against `directory`. Windows are mapped on purpose — see the file header.
[[nodiscard]] RuntimeConfig MakeConfig(const ScopedProjectDirectory& directory, const std::string& scenePath)
{
    RuntimeConfig config;
    config.ApplicationName = "Hollowpoint runtime test";
    config.Width = 320;
    config.Height = 240;
    config.Resizable = false;
    config.StartVisible = true;
    config.ScenePath = scenePath;
    config.ScriptDirectory = directory.Scripts;
    // Unbounded: cases drive RunFrame() themselves and each one asserts on the frame count, so a limit here would stop
    // the loop mid-case (RunFrame returns false when it reaches MaxFrameCount — correct behaviour that reads as a
    // failure if the case did not expect it). The limit itself has its own case below.
    config.MaxFrameCount = 0;
    return config;
}

//! Graphics-side error lines from a log capture.
/**
 * Validation and NVRHI report through the engine log, which makes "the frame loop produced no Vulkan errors" assertable
 * here — a stronger check than any per-call test, because it covers whole recorded frames including the ones nobody
 * hand-wrote.
 *
 * Filtered to graphics channels deliberately: windowing noise from the compositor (an XKB compose-table failure on a
 * host missing locale data, a window hint a Wayland compositor declines) is real but unrelated to what these cases pin
 * down, and requiring its absence would fail the suite for reasons no engine change caused. Matching is by channel, not
 * message text, so a genuine graphics error cannot be waved through by an incidental substring.
 */
[[nodiscard]] std::vector<std::string> GraphicsErrors(const Hollowpoint::Test::ScopedLogCapture& logs)
{
    static constexpr const char* kGraphicsChannels[] = {"VULKAN", "GRAPHICS", "NVRHI", "RENDER"};

    std::vector<std::string> relevant;
    for (const std::string& line : logs.GetLines())
    {
        if (line.rfind("ERROR ", 0) != 0)
        {
            continue;
        }
        for (const char* channel : kGraphicsChannels)
        {
            const std::string prefixed = std::string(channel) + ":";
            if (line.find(prefixed) != std::string::npos)
            {
                relevant.push_back(line);
                break;
            }
        }
    }
    return relevant;
}

//! Reports every graphics error found, then returns whether there were none.
bool CheckNoGraphicsErrors(const Hollowpoint::Test::ScopedLogCapture& logs)
{
    const std::vector<std::string> errors = GraphicsErrors(logs);
    for (const std::string& error : errors)
    {
        // Pass a std::string, never a char pointer: doctest 2.5.3 stringifies `char[N]` literals through its own
        // overload but routes any other pointer via its bool conversion — so `MESSAGE(report.c_str())`, and streaming
        // `.c_str()` with <<, print "1" instead of the message. Measured against doctest 2.5.3 directly; a std::string
        // argument (or `<< std::string`) prints correctly.
        const std::string report = "unexpected graphics error: " + error;
        MESSAGE(report);
    }
    return errors.empty();
}

//! Local position of the mesh entity whose base colour red channel is `red`.
[[nodiscard]] Vec3 FindMeshPosition(const ECS::World& world, float red)
{
    Vec3 found(0.0f, 0.0f, 0.0f);
    bool seen = false;
    world.ForEachEntity([&](const Entity& entity) {
        if (seen)
        {
            return;
        }
        const Game::MeshRenderer* mesh = world.TryGetComponent<Game::MeshRenderer>(entity);
        if (mesh == nullptr || std::fabs(mesh->BaseColor.X - red) > 1.0e-3f)
        {
            return;
        }
        const Scene::LocalTransform* local = world.TryGetComponent<Scene::LocalTransform>(entity);
        if (local != nullptr)
        {
            found = local->Position;
            seen = true;
        }
    });
    REQUIRE_MESSAGE(seen, "the fixture's mesh entity is missing — the loader or MakeSceneText changed");
    return found;
}

[[nodiscard]] Size CountEntities(const ECS::World& world)
{
    Size count = 0;
    world.ForEachEntity([&](const Entity&) { ++count; });
    return count;
}

[[nodiscard]] Size CountComponentsOfType(const ECS::World& world)
{
    Size meshes = 0;
    world.ForEachEntity([&](const Entity& entity) {
        if (world.TryGetComponent<Game::MeshRenderer>(entity) != nullptr)
        {
            ++meshes;
        }
    });
    return meshes;
}

}  // namespace

TEST_SUITE("Runtime/Application")
{
    TEST_CASE("Start builds every subsystem and RunFrame submits draws")
    {
        SKIP_WITHOUT_DISPLAY();

        ScopedProjectDirectory directory("startup");
        const std::string scenePath = directory.File("scene.hpscene");
        directory.Write(scenePath, MakeSceneText());

        RuntimeApplication application(MakeConfig(directory, scenePath));
        Hollowpoint::Test::ScopedLogCapture logs;

        INFO("stop reason: ", Runtime::StopReasonName(application.GetStopReason()));
        REQUIRE_MESSAGE(application.Start(), "Start() failed — see the captured log");
        CHECK(application.IsRunning());
        CHECK(application.GetDevice() != nullptr);
        CHECK(application.GetRenderer() != nullptr);
        CHECK(application.GetWindow() != nullptr);

        // The scene landed in the world the loop renders, not in a throwaway one.
        CHECK(CountComponentsOfType(application.GetWorld()) == 2u);

        for (UInt32 frame = 0; frame < 3u; ++frame)
        {
            INFO("frame " << frame);
            REQUIRE(application.RunFrame());
        }

        // Frames counted AND draws submitted. A non-zero frame count with zero draws is the blank-window bug: the loop
        // runs, collection produces nothing, and every unit test downstream stays green.
        CHECK(application.GetFrameCount() == 3u);
        INFO("draws across all frames: " << application.GetDrawCount());
        CHECK(application.GetDrawCount() >= 6u);  // two meshes per frame, three frames

        CHECK(CheckNoGraphicsErrors(logs));
    }

    TEST_CASE("a script named in the scene moves an entity through the frame loop")
    {
        // The scripting slice end to end: `.hpscene` carries a Script component naming a table defined by the project's
        // init.lua; the runtime binds it after load, on_create initialises it, and on_update runs inside RunFrame. Each
        // step is tested separately elsewhere; this asserts they connect — which is where the first version broke
        // (behaviours were bound before their file existed and never re-bound, leaving entities frozen with a clean
        // log).
        SKIP_WITHOUT_DISPLAY();

        ScopedProjectDirectory directory("scripts");
        const std::string scenePath = directory.File("scene.hpscene");
        directory.Write(scenePath, MakeSceneText("Mover"));

        // Written to scripts/init.lua because that is the file InitialiseScripts loads by name — the real path, not a
        // simulated one.
        directory.Write(directory.ScriptFile("init.lua"), R"LUA(
            -- on_create spawns a marker entity, so "initialisation ran" is observable from C++ through the world without
            -- exposing the VM: one scripted entity means exactly one marker.
            Mover = { steps = 0 }

            function Mover.on_create(entity)
                hp.create_entity()
            end

            function Mover.on_update(entity, delta_time)
                Mover.steps = Mover.steps + 1
                local x, y, z, ok = hp.get_local_position(entity)
                if not ok then return false end
                return hp.set_position(entity, x + 0.25, y, z)
            end
        )LUA");

        RuntimeConfig config = MakeConfig(directory, scenePath);
        RuntimeApplication application(config);
        Hollowpoint::Test::ScopedLogCapture logs;

        REQUIRE(application.Start());

        // on_create ran: the world gained one entity for the one scripted entity. Asserted separately from movement
        // because when it was missing, on_update's first read returned nil and every frame errored while the frame
        // count still looked healthy.
        INFO("entity count after Start: " << CountEntities(application.GetWorld()));
        CHECK(CountEntities(application.GetWorld()) == 5u);  // four from the scene + one marker

        const Vec3 before = FindMeshPosition(application.GetWorld(), 0.8f);
        for (UInt32 frame = 0; frame < 3u; ++frame)
        {
            REQUIRE(application.RunFrame());
        }
        const Vec3 after = FindMeshPosition(application.GetWorld(), 0.8f);

        INFO("before (" << before.X << "," << before.Y << "," << before.Z << ") after (" << after.X << "," << after.Y
                        << "," << after.Z << ")");
        CHECK(after.X > before.X + 0.5f);              // moved, by roughly one quarter per frame
        CHECK(std::fabs(after.Y - before.Y) < 1e-4f);  // and only in X
        CHECK(std::fabs(after.Z - before.Z) < 1e-4f);

        // A script error inside a hook is logged through Scripting; assert none reached the log, since "it moved" plus
        // "it errored every frame" would otherwise pass.
        CHECK_FALSE(logs.AnyMessageContains("on_update"));
        CHECK(CheckNoGraphicsErrors(logs));
    }

    TEST_CASE("CaptureScreenshot writes a real PNG of the rendered frame")
    {
        SKIP_WITHOUT_DISPLAY();

        ScopedProjectDirectory directory("capture");
        const std::string scenePath = directory.File("scene.hpscene");
        directory.Write(scenePath, MakeSceneText());

        RuntimeConfig config = MakeConfig(directory, scenePath);
        RuntimeApplication application(config);
        Hollowpoint::Test::ScopedLogCapture logs;
        REQUIRE(application.Start());
        REQUIRE(application.RunFrame());

        const std::string pngPath = directory.File("frame.png");
        REQUIRE_MESSAGE(application.CaptureScreenshot(pngPath), "capture failed — see the captured log");

        // Read the artifact back instead of trusting the return value: a broken writer's signature is a file that
        // exists and opens but holds nothing. Magic bytes plus the IHDR extents prove a real PNG at a plausible size.
        const Core::BytesResult file = Core::ReadFileBytes(pngPath);
        REQUIRE(file.Succeeded);
        INFO("png size: " << file.Value.size());
        REQUIRE(file.Value.size() > 64u);

        const unsigned char magic[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
        for (std::size_t index = 0; index < sizeof(magic); ++index)
        {
            CHECK(file.Value[index] == magic[index]);
        }
        CHECK(std::string(file.Value.begin() + 12, file.Value.begin() + 16) == "IHDR");

        const UInt32 width = (static_cast<UInt32>(file.Value[16]) << 24u) |
                             (static_cast<UInt32>(file.Value[17]) << 16u) |
                             (static_cast<UInt32>(file.Value[18]) << 8u) | file.Value[19];
        const UInt32 height = (static_cast<UInt32>(file.Value[20]) << 24u) |
                              (static_cast<UInt32>(file.Value[21]) << 16u) |
                              (static_cast<UInt32>(file.Value[22]) << 8u) | file.Value[23];
        INFO("IHDR extent: " << width << "x" << height);
        // The captured frame is sized to the swapchain, which the compositor may have resized from the requested
        // 320x240 — so the contract asserted here is "a real, non-degenerate extent", not an exact match.
        CHECK(width >= 16u);
        CHECK(height >= 16u);

        // Compressed image data for a flat frame is small; a frame with geometry in it is not nothing. This is the
        // weakest assertion in the file and deliberately so — pixel-level comparison belongs to the golden-image work,
        // not here.
        CHECK(file.Value.size() > 256u);

        CHECK(CheckNoGraphicsErrors(logs));
    }

    TEST_CASE("a missing scene file does not stop the engine from running")
    {
        // Policy, documented on Start and pinned here: content problems are reported, never fatal. A mistyped scene
        // path must not make the engine look broken — it opens an empty world and says why in the log.
        SKIP_WITHOUT_DISPLAY();

        ScopedProjectDirectory directory("missingscene");
        const std::string absent = directory.File("nope.hpscene");

        RuntimeConfig config = MakeConfig(directory, absent);
        config.EnableScripting = false;

        RuntimeApplication application(config);
        Hollowpoint::Test::ScopedLogCapture logs;

        REQUIRE(application.Start());
        CHECK(application.RunFrame());
        CHECK(application.GetFrameCount() == 1u);
        // An empty world submits nothing, which is correct and distinguishable from a broken loop because the frame
        // itself completed.
        CHECK(application.GetDrawCount() == 0u);
        CHECK(logs.AnyMessageContains("nope.hpscene"));

        CHECK(CheckNoGraphicsErrors(logs));
    }

    TEST_CASE("MaxFrameCount bounds Run() and reports FrameLimit")
    {
        // The contract an automated run depends on: `--frames N` has to terminate by itself and say why, because
        // "kill it after N seconds" is not a contract and this is how the engine is verified unattended.
        SKIP_WITHOUT_DISPLAY();

        ScopedProjectDirectory directory("framelimit");
        const std::string scenePath = directory.File("scene.hpscene");
        directory.Write(scenePath, MakeSceneText());

        RuntimeConfig config = MakeConfig(directory, scenePath);
        config.MaxFrameCount = 2;
        RuntimeApplication application(config);
        REQUIRE(application.Start());

        // Run() rather than RunFrame(): the limit is enforced inside the frame and reported by the loop's return value.
        CHECK(application.Run() == StopReason::FrameLimit);
        CHECK(application.GetFrameCount() == 2u);
        INFO("draws: " << application.GetDrawCount());
        CHECK(application.GetDrawCount() >= 4u);

        // And a further Run() returns immediately instead of restarting the count.
        CHECK(application.Run() == StopReason::FrameLimit);
        CHECK(application.GetFrameCount() == 2u);
    }

    TEST_CASE("RequestStop ends the loop with the reason recorded")
    {
        SKIP_WITHOUT_DISPLAY();

        ScopedProjectDirectory directory("stop");
        const std::string scenePath = directory.File("scene.hpscene");
        directory.Write(scenePath, MakeSceneText());

        RuntimeConfig config = MakeConfig(directory, scenePath);
        config.MaxFrameCount = 0;  // otherwise unbounded: RequestStop is what has to end it
        RuntimeApplication application(config);
        REQUIRE(application.Start());

        REQUIRE(application.RunFrame());
        application.RequestStop();
        CHECK_FALSE(application.RunFrame());
        CHECK(application.GetStopReason() == StopReason::WindowClosed);

        // Run() after a stop must return rather than spin: an embedding host relies on it.
        CHECK(application.Run() == StopReason::WindowClosed);
    }
}
