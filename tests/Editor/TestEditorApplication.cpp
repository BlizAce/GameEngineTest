#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/ECS/World.h"
#include "Hollowpoint/Editor/EditorApplication.h"
#include "Hollowpoint/Editor/EditorConsole.h"
#include "Hollowpoint/Editor/EditorPanels.h"
#include "Hollowpoint/Editor/EditorSelection.h"
#include "Hollowpoint/Editor/EntityName.h"
#include "Hollowpoint/Game/MeshRenderer.h"
#include "Hollowpoint/Platform/PlatformInit.h"
#include "Hollowpoint/Scene/LocalTransform.h"

#include <unistd.h>

// ---------------------------------------------------------------------------
// EditorApplication integration: the editor starts, composes frames, edits the world and writes a PNG.
//
// This is the suite that would have caught the editor not being an editor. Before it existed, every model test was
// green while the composed frame contained one widget: ImGui numbers vertices per draw list, so concatenating the lists
// into a single vertex buffer without a per-list binding offset made each window redraw the menu bar's geometry and
// clip it away. No amount of unit-level assertion sees that; comparing two screenshots of the viewport region does
// (Session 16).
//
// Cases needing a display skip with a WARN where there is none, matching tests/Runtime/TestRuntimeApplication.cpp — the
// engine cannot create a swapchain without a compositor, and pretending otherwise would make CI fail for an
// environmental reason rather than a code one.
// ---------------------------------------------------------------------------

namespace
{

using namespace Hollowpoint;
using Core::Size;
using Core::UInt32;
using ECS::Entity;
using Editor::EditorApplication;
using Editor::EditorConfig;
using Editor::EditorStopReason;
using Platform::PlatformInitOptions;
using Platform::PlatformScope;

[[nodiscard]] PlatformScope* GetPlatformScope()
{
    static PlatformScope* scope = []() -> PlatformScope* {
        static PlatformScope instance([] {
            PlatformInitOptions options;
            options.AllowHeadlessFallback = true;
            return PlatformScope(options);
        }());

        return instance.IsInitialized() ? &instance : nullptr;
    }();

    return scope;
}

[[nodiscard]] bool DisplayAvailable()
{
    PlatformScope* scope = GetPlatformScope();
    return scope != nullptr && !scope->IsHeadless();
}

//! Reports a skip and returns. MESSAGE rather than WARN, measured with a probe rather than assumed: doctest's WARN is
//! an
// assertion macro, so in a normal run it prints **nothing** at all (and stringifies a `.c_str()` argument as `1`).
// Using it here would make this suite's "skips with a clear message" claim untrue for exactly the cases that need
// explaining. MESSAGE reaches the console; `return` ends the case without failing it, since no display is an
// environment fact rather than a defect. (Several existing suites use the WARN idiom and so print no reason — a
// repo-wide test-infra fix, out of scope here.)
#define SKIP_WITHOUT_DISPLAY()                     \
    do                                             \
    {                                              \
        if (!DisplayAvailable())                   \
        {                                          \
            MESSAGE("skipped: no display server"); \
            return;                                \
        }                                          \
    }                                              \
    while (false)

[[nodiscard]] std::filesystem::path ScratchPath(std::string_view name)
{
    return std::filesystem::temp_directory_path() /
           ("hollowpoint-editor-test-" + std::string(name) + "-" + std::to_string(::getpid()));
}

//! An editor configured for automated runs: hidden, small, bounded frame count, no content dependency.
[[nodiscard]] EditorConfig MakeTestConfig(const std::filesystem::path& projectDirectory)
{
    EditorConfig config;
    config.ApplicationName = "Hollowpoint Editor Test";
    config.Width = 1280;
    config.Height = 720;
    // Unmapped: a test window that steals focus is a hostile test, and the compositor still allocates an extent for a
    // mapped-offscreen window on Wayland. (Mapped-but-invisible is not a thing; --hidden uses GLFW's visible flag.)
    config.StartVisible = false;
    config.ProjectDirectory = projectDirectory.string();
    // Magenta background, matched by the constants above: it makes "did anything draw here?" a colour comparison
    // instead of a statistical guess. See CoveredFraction for why the alternatives (modal colour, distinct-colour
    // count) mislead.
    config.BackgroundRed = 1.0f;
    config.BackgroundGreen = 0.0f;
    config.BackgroundBlue = 1.0f;
    return config;
}

//! Captures the composed frame as RGBA8, failing the test (not just returning false) if it cannot.
/**
 * Uses EditorApplication::CaptureComposedRgba8 rather than reading back a PNG: the engine compiles stb's *writer* only,
 * so a file-based comparison would need a decoder dependency or would compare encoded bytes. Reading the capture target
 * gives the exact pixels the editor composited, with no filesystem in the loop.
 */
[[nodiscard]] std::vector<unsigned char> Capture(EditorApplication& editor, UInt32& outWidth, UInt32& outHeight)
{
    std::vector<unsigned char> rgba;
    REQUIRE(editor.CaptureComposedRgba8(rgba, outWidth, outHeight));
    REQUIRE(outWidth > 0u);
    REQUIRE(outHeight > 0u);
    REQUIRE(rgba.size() == static_cast<size_t>(outWidth) * outHeight * 4u);
    return rgba;
}

//! Fraction of pixels differing between two same-sized images. The core assertion of this suite: it is the only check
//  that can distinguish "the editor drew a scene" from "the editor drew a scene *and its panels*".
[[nodiscard]] float DifferenceFraction(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b)
{
    if (a.size() != b.size() || a.empty())
    {
        return 0.0f;
    }

    Size different = 0;
    const Size pixels = a.size() / 4u;
    for (Size index = 0; index < pixels; ++index)
    {
        const Size base = index * 4u;
        // A threshold rather than exact equality: font anti-aliasing and the tone map's rounding make neighbouring runs
        // differ by a least-significant bit or two, and an exact comparison would report "the UI changed" for noise.
        bool differs = false;
        for (Size channel = 0; channel < 3u && !differs; ++channel)
        {
            const int delta = static_cast<int>(a[base + channel]) - static_cast<int>(b[base + channel]);
            differs = delta > 2 || delta < -2;
        }
        if (differs)
        {
            ++different;
        }
    }

    return static_cast<float>(different) / static_cast<float>(pixels);
}

//! Counts distinct colours inside a fractional rectangle of a captured frame.
/**
 * The per-panel check that makes "did the editor draw its UI" a real assertion. Measured on this suite's own regression
 * (ImGui numbers vertices per draw list, so concatenating them without a per-list binding offset made every window
 * redraw the menu bar's geometry and clip it away): each panel region held exactly **1** distinct colour while broken
 * and 4–8 when drawn.
 *
 * Why not whole-frame statistics:
 *   * distinct colours overall — a frame with only a menu bar scores *higher* (gradients plus anti-aliased text) than
 * one whose large panels are flat fills: 51 broken against 41 fixed;
 *   * coverage of non-background — the dockspace itself paints most of the window, so it stays high even when no panel
 *     draws.
 * Both looked like they should work and neither discriminates. Region-local structure does.
 */
struct FractionalRegion
{
    float X0 = 0.0f;
    float Y0 = 0.0f;
    float X1 = 0.0f;
    float Y1 = 0.0f;
};

[[nodiscard]] Size CountDistinctColours(const std::vector<unsigned char>& rgba, UInt32 imageWidth, UInt32 imageHeight,
                                        const FractionalRegion& region)
{
    if (rgba.empty() || imageWidth == 0u || imageHeight == 0u)
    {
        return 0u;
    }

    // 5 bits per channel: a panel's fill and its border land in different buckets, while anti-aliasing along one glyph
    // edge does not add a new bucket per pixel.
    std::vector<bool> seen(1 << 15, false);
    Size distinct = 0;

    const auto x0 = static_cast<UInt32>(region.X0 * static_cast<float>(imageWidth));
    const auto x1 = static_cast<UInt32>(region.X1 * static_cast<float>(imageWidth));
    const auto y0 = static_cast<UInt32>(region.Y0 * static_cast<float>(imageHeight));
    const auto y1 = static_cast<UInt32>(region.Y1 * static_cast<float>(imageHeight));

    for (UInt32 y = y0; y < y1 && y < imageHeight; ++y)
    {
        for (UInt32 x = x0; x < x1 && x < imageWidth; ++x)
        {
            const Size base = (static_cast<Size>(y) * imageWidth + x) * 4u;
            if (base + 3u >= rgba.size())
            {
                continue;
            }

            const Size key = ((rgba[base] >> 3) << 10) | ((rgba[base + 1] >> 3) << 5) | (rgba[base + 2] >> 3);
            if (!seen[key])
            {
                seen[key] = true;
                ++distinct;
            }
        }
    }

    return distinct;
}

//! Where the default layout puts each panel, as fractions of the window. See PlaceInitially in EditorPanels.cpp: these
//  are the same proportions, sampled well inside each rectangle so a neighbouring panel's border cannot satisfy it.
const FractionalRegion kOutlinerRegion{0.01f, 0.15f, 0.20f, 0.75f};
const FractionalRegion kDetailsRegion{0.80f, 0.15f, 0.98f, 0.70f};
const FractionalRegion kConsoleRegion{0.30f, 0.78f, 0.95f, 0.97f};

}  // namespace

TEST_SUITE("EditorApplication")
{
    TEST_CASE("A config with an unusable project directory still starts, reporting it")
    {
        SKIP_WITHOUT_DISPLAY();

        EditorConfig config = MakeTestConfig("/definitely/not/a/project/directory-42");
        EditorApplication editor(config);

        // Deliberately a successful start. The content browser is one panel; refusing to open the editor because a path
        // was mistyped would be worse than opening with an empty browser and a logged warning — and this pins that
        // choice so a later "harden the startup path" change does not quietly invert it.
        CHECK(editor.Start());
        CHECK(editor.IsRunning());
    }

    TEST_CASE("Start creates a context, then frames advance and stop reasons are reported")
    {
        SKIP_WITHOUT_DISPLAY();

        const std::filesystem::path project = ScratchPath("frames");
        std::filesystem::remove_all(project);
        std::filesystem::create_directories(project);

        EditorConfig config = MakeTestConfig(project);
        config.MaxFrameCount = 3;

        EditorApplication editor(config);
        REQUIRE(editor.Start());
        CHECK(editor.GetFrameCount() == 0ull);

        const EditorStopReason reason = editor.Run();
        CHECK(reason == EditorStopReason::FrameLimit);
        // Three frames composed, not three-and-a-bit: the frame budget is checked once per Frame(), so an off-by-one
        // here shows up as an extra present — invisible on screen and worth pinning for capture tests that assume a
        // state.
        CHECK(editor.GetFrameCount() == 3ull);

        std::filesystem::remove_all(project);
    }

    TEST_CASE("RunStartupCommand edits the world before frames are composed")
    {
        SKIP_WITHOUT_DISPLAY();

        const std::filesystem::path project = ScratchPath("commands");
        std::filesystem::create_directories(project);

        EditorConfig config = MakeTestConfig(project);
        EditorApplication editor(config);
        REQUIRE(editor.Start());

        CHECK(editor.RunStartupCommand("entity.create Cube engine:cube"));
        // Commands land before the loop so `--frames 1 --command ...` produces one frame containing their effect. A
        // command applied after the first frame instead would make captures depend on compositor timing.
        CHECK(editor.GetWorld().GetAliveEntityCount() == 1u);

        const Entity created = editor.GetSelectedEntity();
        REQUIRE_FALSE(created.IsNull());

        const Editor::EntityName* name = editor.GetWorld().TryGetComponent<Editor::EntityName>(created);
        REQUIRE(name != nullptr);
        CHECK(name->Name == "Cube");

        // `entity.create` with no arguments is valid by design (it makes "Entity"), so the failure cases have to be a
        // genuinely unknown name and a handler that cannot succeed. A test asserting the wrong thing here would pass
        // while telling the next reader that bare `entity.create` is an error, which it is not.
        CHECK(editor.RunStartupCommand("entity.create"));
        CHECK(editor.GetWorld().GetAliveEntityCount() == 2u);

        CHECK_FALSE(editor.RunStartupCommand("no-such-command-here"));
        CHECK(editor.GetWorld().GetAliveEntityCount() == 2u);  // the failed command changed nothing

        // An argument-count violation is refused before the handler runs, so a half-applied edit is not possible.
        CHECK_FALSE(editor.RunStartupCommand("editor.select"));

        editor.RequestStop();
        std::filesystem::remove_all(project);
    }

    TEST_CASE("Selection survives frames and dies with its entity")
    {
        SKIP_WITHOUT_DISPLAY();

        const std::filesystem::path project = ScratchPath("selection");
        std::filesystem::create_directories(project);

        EditorConfig config = MakeTestConfig(project);
        EditorApplication editor(config);
        REQUIRE(editor.Start());

        const Entity entity = editor.CreateEntity("Pickme", "");
        REQUIRE_FALSE(entity.IsNull());
        CHECK(editor.GetSelection().IsSelected(entity));

        REQUIRE(editor.Frame());
        CHECK(editor.GetSelection().IsSelected(entity));

        CHECK(editor.SelectBySlot(entity.Slot));

        // A slot that is not alive must be refused: `editor.select 999` doing nothing silently reads as a broken
        // command.
        CHECK_FALSE(editor.SelectBySlot(99999));

        editor.DestroySelected();
        CHECK(editor.GetSelection().IsEmpty());
        CHECK_FALSE(editor.GetWorld().IsAlive(entity));

        std::filesystem::remove_all(project);
    }

    TEST_CASE("The composed frame contains the UI, and editing the scene changes the viewport")
    {
        SKIP_WITHOUT_DISPLAY();

        const std::filesystem::path project = ScratchPath("composed");
        std::filesystem::create_directories(project);

        UInt32 width = 0;
        UInt32 height = 0;
        UInt32 afterWidth = 0;
        UInt32 afterHeight = 0;

        EditorConfig config = MakeTestConfig(project);
        EditorApplication editor(config);
        REQUIRE(editor.Start());

        // One entity up front so every panel has something to draw: an empty Details panel is legitimately flat fill,
        // and asserting "structure" on it would test the placeholder rather than the panel.
        static_cast<void>(editor.CreateEntity("Alpha", ""));

        // Two frames before the first capture: frame one allocates the viewport targets, and ImGui's first layout pass
        // has no rectangles to place panels in, so capturing then would assert on a half-built UI.
        REQUIRE(editor.Frame());
        REQUIRE(editor.Frame());
        const std::vector<unsigned char> beforePixels = Capture(editor, width, height);
        CHECK(width == 1280u);
        CHECK(height == 720u);

        // Each panel region must contain structure — a fill, a border and text, not one flat colour. See
        // CountDistinctColours for why whole-frame statistics were tried first and do not discriminate. The names are
        // written out per check rather than looped: doctest renders a `const char*` member in a message as 1 (it binds
        // through the bool conversion), which turns a precise failure into "the 1 region" — exactly when precision
        // matters.
        CHECK_MESSAGE(CountDistinctColours(beforePixels, width, height, kOutlinerRegion) >= 3u,
                      "the outliner region is flat; that panel did not draw its contents");
        CHECK_MESSAGE(CountDistinctColours(beforePixels, width, height, kDetailsRegion) >= 3u,
                      "the details region is flat; that panel did not draw its contents");
        CHECK_MESSAGE(CountDistinctColours(beforePixels, width, height, kConsoleRegion) >= 3u,
                      "the console region is flat; that panel did not draw its contents");

        // Now change the scene and confirm pixels change with it. Adding a mesh to an empty world must alter the image:
        // if nothing moved, the viewport is not showing the world the editor edits — the two-sources-of-truth bug this
        // host exists to prevent.
        static_cast<void>(editor.CreateEntity("Cube", "engine:cube"));
        REQUIRE(editor.Frame());
        const std::vector<unsigned char> afterPixels = Capture(editor, afterWidth, afterHeight);
        CHECK(afterWidth == width);
        CHECK(afterHeight == height);

        const float changed = DifferenceFraction(beforePixels, afterPixels);
        CHECK_MESSAGE(changed > 0.0005f,
                      "adding an entity changed " << changed * 100.0f << "% of pixels; the viewport is not live");

        std::filesystem::remove_all(project);
    }

    TEST_CASE("Capturing before any frame has been composed fails rather than writing a blank PNG")
    {
        SKIP_WITHOUT_DISPLAY();

        const std::filesystem::path project = ScratchPath("earlycapture");
        std::filesystem::create_directories(project);
        const std::filesystem::path out = project / "none.png";

        EditorConfig config = MakeTestConfig(project);
        EditorApplication editor(config);
        REQUIRE(editor.Start());

        // An empty PNG from a mistyped --screenshot reads like a renderer bug. Refusing says what actually happened:
        // the capture was asked for before there was a frame.
        CHECK_FALSE(editor.CaptureComposedFrame(out.string()));
        CHECK_FALSE(std::filesystem::exists(out));

        std::filesystem::remove_all(project);
    }

    TEST_CASE("Scene save and load round-trip through the editor's own commands")
    {
        SKIP_WITHOUT_DISPLAY();

        const std::filesystem::path project = ScratchPath("sceneio");
        std::filesystem::create_directories(project);
        const std::filesystem::path sceneFile = project / "saved.hpscene";

        EditorConfig config = MakeTestConfig(project);
        EditorApplication editor(config);
        REQUIRE(editor.Start());

        const Entity a = editor.CreateEntity("Alpha", "");
        const Entity b = editor.CreateEntity("Beta", "");
        REQUIRE_FALSE(a.IsNull());
        REQUIRE_FALSE(b.IsNull());

        CHECK(editor.RunStartupCommand(("scene.save " + sceneFile.string()).c_str()));
        CHECK(std::filesystem::exists(sceneFile));

        // Loading replaces the world, so the previous entities are gone and selection is dropped with them — a
        // surviving handle could name an unrelated entity in the freshly loaded world.
        static_cast<void>(editor.CreateEntity("Gamma", ""));
        REQUIRE(editor.GetWorld().GetAliveEntityCount() == 3u);

        CHECK(editor.RunStartupCommand(("scene.load " + sceneFile.string()).c_str()));
        CHECK(editor.GetWorld().GetAliveEntityCount() == 2u);
        CHECK(editor.GetSelection().IsEmpty());

        std::filesystem::remove_all(project);
    }
}
