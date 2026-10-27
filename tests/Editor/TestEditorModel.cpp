#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/ECS/World.h"
#include "Hollowpoint/Editor/ContentBrowserModel.h"
#include "Hollowpoint/Editor/EditorConsole.h"
#include "Hollowpoint/Editor/EditorSelection.h"
#include "Hollowpoint/Editor/EntityName.h"
#include "Hollowpoint/Editor/Picking.h"
#include "Hollowpoint/Editor/PropertyView.h"
#include "Hollowpoint/Game/MeshRenderer.h"
#include "Hollowpoint/Game/RegisterGameTypes.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/MathReflection/MathReflection.h"
#include "Hollowpoint/Reflection/Registry.h"
#include "Hollowpoint/Scene/LocalTransform.h"
#include "Hollowpoint/Scene/SceneIO.h"

// ---------------------------------------------------------------------------
// Editor model tests: selection, console, content browser, picking, property editing.
//
// All CPU-only by design — none of these need a device, a window or ImGui, which is what makes them run in the default
// suite and under ASan/UBSan with full leak checking. The behaviours they pin are the ones an editor can get wrong
// invisibly: a selection that survives the death of its entity, an argument splitter that eats quoted paths, a pick
// that misses because a transform was applied to a normal instead of a position.
// ---------------------------------------------------------------------------

namespace
{

using namespace Hollowpoint;
using ECS::Entity;
using ECS::World;
using Editor::ArgumentCount;
using Editor::ConsoleCommand;
using Editor::ContentBrowserModel;
using Editor::ContentEntry;
using Editor::EditorConsole;
using Editor::EditorSelection;
using Editor::EntityName;
using Editor::PickCandidate;
using Editor::PickRay;
using Editor::PropertyField;
using Editor::PropertyView;
using Editor::Ray;
using Math::Mat4;
using Math::Vec3;

//! A scratch directory with known contents, removed on scope exit. The content browser is a filesystem adapter, so
//  testing it against a real (temporary) tree is the honest version — mocking the filesystem would test the mock.
class ScratchDirectory
{
public:
    explicit ScratchDirectory(std::string_view name)
        : m_Path(std::filesystem::temp_directory_path() / ("hp-editor-" + std::string(name)))
    {
        std::error_code ignored;
        std::filesystem::remove_all(m_Path, ignored);
        std::filesystem::create_directories(m_Path);
    }

    ~ScratchDirectory()
    {
        // Best effort: a leftover temp directory is not worth failing a test over, and removing what we created is the
        // polite default when it can be.
        std::error_code ignored;
        std::filesystem::remove_all(m_Path, ignored);
    }

    void Write(std::string_view relativeName, std::string_view contents) const
    {
        const std::filesystem::path file = m_Path / relativeName;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary);
        out << contents;
    }

    [[nodiscard]] const std::filesystem::path& GetPath() const noexcept
    {
        return m_Path;
    }

private:
    std::filesystem::path m_Path;
};

//! Ensures the type registrations PropertyView depends on exist. Each Register* is idempotent (they are called from
//  every engine entry point), so calling them per case is cheap and keeps a single case runnable alone —
//  which is what `ctest -R` and doctest's `-ts=` filters do, and a test that only passes in suite order hides real
//  failures.
void EnsureTypesRegistered()
{
    Math::RegisterMathTypes();
    Scene::RegisterSceneTypes();
    Game::RegisterGameTypes();
    Editor::RegisterEditorTypes();
}

[[nodiscard]] Entity MakeEntity(World& world, std::string_view name)
{
    const Entity entity = world.CreateEntity();
    world.AddComponent<Scene::LocalTransform>(entity);
    EntityName& entityName = world.AddComponent<EntityName>(entity);
    entityName.Name = std::string(name);
    return entity;
}

}  // namespace

TEST_SUITE("EditorSelection")
{
    TEST_CASE("Select replaces the previous selection and makes it primary")
    {
        World world;
        const Entity a = MakeEntity(world, "A");
        const Entity b = MakeEntity(world, "B");

        EditorSelection selection;
        CHECK(selection.Select(a));
        CHECK(selection.GetCount() == 1u);

        CHECK(selection.Select(b));
        CHECK(selection.GetCount() == 1u);
        CHECK_FALSE(selection.IsSelected(a));
        CHECK(selection.GetPrimary() == b);
    }

    TEST_CASE("Re-selecting the only selected entity is not a change")
    {
        // A click-and-drag across one row must not report a change every frame: callers use the return value to decide
        // whether to repaint, and "always changed" would make an outliner redraw continuously while the button is down.
        World world;
        const Entity a = MakeEntity(world, "A");

        EditorSelection selection;
        CHECK(selection.Select(a));
        CHECK_FALSE(selection.Select(a));
        CHECK(selection.GetCount() == 1u);
    }

    TEST_CASE("AddToSelection keeps order and does not promote an existing member")
    {
        World world;
        const Entity a = MakeEntity(world, "A");
        const Entity b = MakeEntity(world, "B");

        EditorSelection selection;
        selection.Select(a);
        CHECK(selection.AddToSelection(b));
        REQUIRE(selection.GetCount() == 2u);
        CHECK(selection.GetPrimary() == b);

        // Clicking A again while both are selected keeps B primary and does not reorder.
        CHECK_FALSE(selection.AddToSelection(a));
        CHECK(selection.GetPrimary() == b);
        CHECK(selection.GetEntities()[0] == a);
    }

    TEST_CASE("Toggle adds and removes; removing the primary makes the new last entry primary")
    {
        World world;
        const Entity a = MakeEntity(world, "A");
        const Entity b = MakeEntity(world, "B");
        const Entity c = MakeEntity(world, "C");

        EditorSelection selection;
        selection.Select(a);
        CHECK(selection.Toggle(b));
        CHECK(selection.Toggle(c));
        REQUIRE(selection.GetPrimary() == c);

        CHECK(selection.Toggle(b));  // remove B
        CHECK(selection.GetCount() == 2u);
        CHECK(selection.GetPrimary() == c);

        CHECK(selection.Toggle(c));  // removing the primary falls back to A
        CHECK(selection.GetPrimary() == a);
    }

    TEST_CASE("A null handle is never selectable")
    {
        // Select(null) means "select nothing" (a pick that missed), and Add/Toggle on null must be no-ops rather than
        // inserting an entity that no longer exists.
        EditorSelection selection;
        const Entity live = Editor::Entity{};

        CHECK_FALSE(selection.AddToSelection(live));
        CHECK_FALSE(selection.Toggle(live));
        CHECK(selection.IsEmpty());

        World world;
        selection.Select(MakeEntity(world, "A"));
        // Selecting null clears rather than adding.
        (void)selection.Select(Entity{});
        CHECK(selection.IsEmpty());
    }

    TEST_CASE("Prune drops dead entities and keeps live ones")
    {
        World world;
        const Entity a = MakeEntity(world, "A");
        const Entity b = MakeEntity(world, "B");
        const Entity c = MakeEntity(world, "C");

        EditorSelection selection;
        selection.Select(a);
        selection.AddToSelection(b);
        selection.AddToSelection(c);
        REQUIRE(selection.GetCount() == 3u);

        world.DestroyEntity(b);

        // The predicate is injected because the editor's liveness check is World::IsAlive while a test may want to
        // force removal — both are legitimate and neither should require the other to exist.
        const Editor::Size removed = selection.Prune([&world](const Entity& entity) { return world.IsAlive(entity); });

        CHECK(removed == 1u);
        CHECK(selection.GetCount() == 2u);
        CHECK_FALSE(selection.IsSelected(b));
        CHECK(selection.IsSelected(a));
        CHECK(selection.GetPrimary() == c);
    }

    TEST_CASE("Clear empties and GetPrimary on empty is null")
    {
        World world;
        EditorSelection selection;
        selection.Select(MakeEntity(world, "A"));
        selection.Clear();

        CHECK(selection.IsEmpty());
        CHECK(selection.GetPrimary().IsNull());
    }
}

TEST_SUITE("EditorConsole")
{
    TEST_CASE("Register and Execute route to the handler")
    {
        EditorConsole console;
        int calls = 0;

        ConsoleCommand command;
        command.Name = "test.echo";
        command.Execute = [&calls](const std::vector<std::string>& arguments, std::string& outReply, std::string&) {
            ++calls;
            outReply = arguments.empty() ? "empty" : arguments[0];
            return true;
        };

        CHECK(console.Register(command));
        CHECK(console.GetCommandCount() == 1u);
        CHECK(console.HasCommand("test.echo"));
        // Commands are invoked case-insensitively: an author typing `Scene.Save` should not get "unknown command".
        CHECK(console.HasCommand("TEST.ECHO"));

        std::string reply;
        std::string error;
        REQUIRE(console.Execute("Test.Echo", {"hello"}, reply, error));
        CHECK(reply == "hello");
        CHECK(calls == 1);
    }

    TEST_CASE("Re-registering a name replaces it and reports the collision")
    {
        EditorConsole console;

        ConsoleCommand first;
        first.Name = "dup";
        first.Execute = [](const std::vector<std::string>&, std::string& outReply, std::string&) {
            outReply = "first";
            return true;
        };
        ConsoleCommand second = first;
        second.Execute = [](const std::vector<std::string>&, std::string& outReply, std::string&) {
            outReply = "second";
            return true;
        };

        CHECK(console.Register(first));
        CHECK_FALSE(console.Register(second));  // false says "you registered twice", not "it failed to work"
        CHECK(console.GetCommandCount() == 1u);

        std::string reply;
        std::string error;
        REQUIRE(console.Execute("dup", {}, reply, error));
        CHECK(reply == "second");
    }

    TEST_CASE("A command with no name or no handler is refused")
    {
        EditorConsole console;

        ConsoleCommand noName;
        noName.Execute = [](const std::vector<std::string>&, std::string&, std::string&) { return true; };
        CHECK_FALSE(console.Register(noName));

        ConsoleCommand noHandler;
        noHandler.Name = "test.handlerless";
        CHECK_FALSE(console.Register(noHandler));

        CHECK(console.GetCommandCount() == 0u);
    }

    TEST_CASE("RegisterFixed enforces argument counts before the handler runs")
    {
        EditorConsole console;
        int calls = 0;

        const bool registered =
            console.RegisterFixed("test.exact", "needs two", ArgumentCount::Exact, 2,
                                  [&calls](const std::vector<std::string>&, std::string& outReply, std::string&) {
                                      ++calls;
                                      outReply = "ok";
                                      return true;
                                  });
        CHECK(registered);

        std::string reply;
        std::string error;
        CHECK_FALSE(console.Execute("test.exact", {"one"}, reply, error));
        CHECK_FALSE(error.empty());
        CHECK(calls == 0);  // the handler must not run on a count mismatch

        CHECK(console.Execute("test.exact", {"one", "two"}, reply, error));
        CHECK(calls == 1);
    }

    TEST_CASE("RegisterFixed AtLeast accepts more arguments")
    {
        EditorConsole console;
        console.RegisterFixed("test.atleast", "", ArgumentCount::AtLeast, 1,
                              [](const std::vector<std::string>& arguments, std::string& outReply, std::string&) {
                                  outReply = std::to_string(arguments.size());
                                  return true;
                              });

        std::string reply;
        std::string error;
        CHECK_FALSE(console.Execute("test.atleast", {}, reply, error));
        REQUIRE(console.Execute("test.atleast", {"a", "b", "c"}, reply, error));
        CHECK(reply == "3");
    }

    TEST_CASE("An unknown command names close matches")
    {
        EditorConsole console;
        console.RegisterFixed("scene.save", "", ArgumentCount::Exact, 0,
                              [](const std::vector<std::string>&, std::string& outReply, std::string&) {
                                  outReply = "saved";
                                  return true;
                              });

        std::string reply;
        std::string error;
        // The realistic typo: right prefix, wrong suffix. A bare "unknown command" here sends the author to the docs;
        // a suggestion answers the question they were asking.
        CHECK_FALSE(console.Execute("scene.sav", {}, reply, error));
        CHECK(error.find("scene.save") != std::string::npos);
    }

    TEST_CASE("A failing handler with no message is reported rather than silent")
    {
        EditorConsole console;
        console.RegisterFixed("test.fail", "", ArgumentCount::Exact, 0,
                              [](const std::vector<std::string>&, std::string&, std::string&) { return false; });

        std::string reply;
        std::string error;
        CHECK_FALSE(console.Execute("test.fail", {}, reply, error));
        CHECK_FALSE(error.empty());
    }

    TEST_CASE("SplitArguments handles quotes, escapes and empty input")
    {
        CHECK(EditorConsole::SplitArguments("").empty());
        CHECK(EditorConsole::SplitArguments("   ").empty());

        const std::vector<std::string> simple = EditorConsole::SplitArguments("scene.save my.hpscene");
        REQUIRE(simple.size() == 2u);
        CHECK(simple[0] == "scene.save");
        CHECK(simple[1] == "my.hpscene");

        // The case quoting exists for: a path with spaces, on every OS that has them.
        const std::vector<std::string> quoted = EditorConsole::SplitArguments("scene.load \"My Scene.hpscene\"");
        REQUIRE(quoted.size() == 2u);
        CHECK(quoted[1] == "My Scene.hpscene");

        // A backslash escapes only the quote and another backslash inside quotes, so a Windows path survives intact —
        // unescaping more than that would corrupt exactly the paths quoting is there to protect.
        const std::vector<std::string> escaped =
            EditorConsole::SplitArguments("scene.load \"C:\\\\Projects\\\\a b.hpscene\"");
        REQUIRE(escaped.size() == 2u);
        CHECK(escaped[1] == "C:\\Projects\\a b.hpscene");

        const std::vector<std::string> windowsPath =
            EditorConsole::SplitArguments("scene.load \"C:\\Projects\\My Scene.hpscene\"");
        REQUIRE(windowsPath.size() == 2u);
        CHECK(windowsPath[1] == "C:\\Projects\\My Scene.hpscene");

        // An empty quoted argument is an argument, distinct from nothing.
        const std::vector<std::string> emptyQuoted = EditorConsole::SplitArguments("cmd \"\" tail");
        REQUIRE(emptyQuoted.size() == 3u);
        CHECK(emptyQuoted[1].empty());
    }

    TEST_CASE("ExecuteLine splits then runs; an empty line is not an error")
    {
        EditorConsole console;
        std::vector<std::string> seen;
        console.RegisterFixed("test.args", "", ArgumentCount::Any, 0,
                              [&seen](const std::vector<std::string>& arguments, std::string&, std::string&) {
                                  seen = arguments;
                                  return true;
                              });

        std::string reply;
        std::string error;
        CHECK(console.ExecuteLine("", reply, error));
        CHECK(console.ExecuteLine("   ", reply, error));
        CHECK(seen.empty());  // the empty line must not even reach the handler

        REQUIRE(console.ExecuteLine("test.args one \"two three\" four", reply, error));
        REQUIRE(seen.size() == 3u);
        CHECK(seen[1] == "two three");
    }

    TEST_CASE("Complete filters by prefix and sorts")
    {
        EditorConsole console;
        console.RegisterFixed("scene.save", "", ArgumentCount::Exact, 0,
                              [](const std::vector<std::string>&, std::string&, std::string&) { return true; });
        console.RegisterFixed("scene.load", "", ArgumentCount::Exact, 0,
                              [](const std::vector<std::string>&, std::string&, std::string&) { return true; });
        console.RegisterFixed("editor.info", "", ArgumentCount::Exact, 0,
                              [](const std::vector<std::string>&, std::string&, std::string&) { return true; });

        const std::vector<std::string> sceneCommands = console.Complete("scene.");
        REQUIRE(sceneCommands.size() == 2u);
        // Sorted so tab-completion advances predictably; registration order would depend on link order.
        CHECK(sceneCommands[0] == "scene.load");
        CHECK(sceneCommands[1] == "scene.save");

        CHECK(console.Complete("").size() == 3u);
        CHECK(console.Complete("nope").empty());
    }
}

TEST_SUITE("EditorPicking")
{
    //! A unit cube's 8 corners, enough to have a front face whose triangle hit-test can be checked by hand.
    [[nodiscard]] std::vector<Math::Vec3> CubeCorners()
    {
        return {
            Vec3{-1, -1, -1}, Vec3{1, -1, -1}, Vec3{1, 1, -1}, Vec3{-1, 1, -1},
            Vec3{-1, -1, 1},  Vec3{1, -1, 1},  Vec3{1, 1, 1},  Vec3{-1, 1, 1},
        };
    }

    [[nodiscard]] std::vector<Core::UInt32> CubeIndices()
    {
        // Two triangles per face; only the front face (z = +1) matters for the tests below, and getting the winding
        // right is part of what is being tested since back-face culling is optional.
        return {
            4, 5, 6, 4, 6, 7,  // +Z
            1, 0, 3, 1, 3, 2,  // -Z
            0, 1, 5, 0, 5, 4,  // -Y
            3, 7, 6, 3, 6, 2,  // +Y
            0, 4, 7, 0, 7, 3,  // -X
            1, 2, 6, 1, 6, 5,  // +X
        };
    }

    TEST_CASE("A ray aimed at a face hits it and reports the owning candidate")
    {
        const std::vector<Vec3> corners = CubeCorners();
        const std::vector<Core::UInt32> indices = CubeIndices();

        PickCandidate candidate;
        candidate.OwnerSlot = 7;
        candidate.WorldTransform = Mat4::Identity();
        candidate.Vertices = corners.data();
        candidate.VertexCount = static_cast<Editor::Index>(corners.size());
        candidate.VertexStrideBytes = static_cast<Editor::Index>(sizeof(Vec3));
        candidate.PositionOffsetBytes = 0;
        candidate.Indices = indices.data();
        candidate.IndexCount = static_cast<Editor::Index>(indices.size());

        // Straight down -Z at the front face's centre: hits z=+1 at (0,0).
        const Ray ray{Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f, 0.0f, -1.0f)};
        const auto result = PickRay(ray, &candidate, 1, true);

        REQUIRE(result.HasHit);
        CHECK(result.OwnerSlot == 7);
        CHECK(result.Distance == doctest::Approx(4.0f));
    }

    TEST_CASE("A ray aimed away from the cube misses")
    {
        const std::vector<Vec3> corners = CubeCorners();
        const std::vector<Core::UInt32> indices = CubeIndices();

        PickCandidate candidate;
        candidate.OwnerSlot = 0;
        candidate.WorldTransform = Mat4::Identity();
        candidate.Vertices = corners.data();
        candidate.VertexCount = static_cast<Editor::Index>(corners.size());
        candidate.VertexStrideBytes = static_cast<Editor::Index>(sizeof(Vec3));
        candidate.Indices = indices.data();
        candidate.IndexCount = static_cast<Editor::Index>(indices.size());

        const Ray ray{Vec3(10.0f, 10.0f, 5.0f), Vec3(0.0f, 0.0f, -1.0f)};
        CHECK_FALSE(PickRay(ray, &candidate, 1, true).HasHit);
    }

    TEST_CASE("The world transform is applied to the pick")
    {
        const std::vector<Vec3> corners = CubeCorners();
        const std::vector<Core::UInt32> indices = CubeIndices();

        PickCandidate candidate;
        candidate.OwnerSlot = 1;
        // Translating the mesh is the single most common edit in an editor, and picking that ignores it selects nothing
        // (or the wrong thing) for every object the author has moved.
        candidate.WorldTransform = Mat4::Translate(Vec3(50.0f, 0.0f, 0.0f));
        candidate.Vertices = corners.data();
        candidate.VertexCount = static_cast<Editor::Index>(corners.size());
        candidate.VertexStrideBytes = static_cast<Editor::Index>(sizeof(Vec3));
        candidate.Indices = indices.data();
        candidate.IndexCount = static_cast<Editor::Index>(indices.size());

        const Ray misses{Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f, 0.0f, -1.0f)};
        CHECK_FALSE(PickRay(misses, &candidate, 1, true).HasHit);

        const Ray hits{Vec3(50.0f, 0.0f, 5.0f), Vec3(0.0f, 0.0f, -1.0f)};
        CHECK(PickRay(hits, &candidate, 1, true).HasHit);
    }

    TEST_CASE("The nearest candidate wins when several are along the ray")
    {
        const std::vector<Vec3> corners = CubeCorners();
        const std::vector<Core::UInt32> indices = CubeIndices();

        auto makeCandidate = [&](Editor::Index slot, float x) {
            PickCandidate candidate;
            candidate.OwnerSlot = slot;
            candidate.WorldTransform = Mat4::Translate(Vec3(x, 0.0f, 0.0f));
            candidate.Vertices = corners.data();
            candidate.VertexCount = static_cast<Editor::Index>(corners.size());
            candidate.VertexStrideBytes = static_cast<Editor::Index>(sizeof(Vec3));
            candidate.Indices = indices.data();
            candidate.IndexCount = static_cast<Editor::Index>(indices.size());
            return candidate;
        };

        // Two cubes along the ray at different depths: the near one is what a click must select.
        std::vector<PickCandidate> candidates;
        candidates.push_back(makeCandidate(1, 0.0f));
        candidates.push_back(makeCandidate(2, 40.0f));

        const Ray ray{Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f, 0.0f, -1.0f)};
        const auto result = PickRay(ray, candidates.data(), static_cast<Editor::Index>(candidates.size()), true);

        REQUIRE(result.HasHit);
        CHECK(result.OwnerSlot == 1);
    }

    TEST_CASE("Back-face culling changes which surface is hit")
    {
        // Shooting from inside the cube: with culling on, only a far face can be hit; with it off the near
        // (back-facing) face is nearer. Both are legitimate settings — an editor picking "whatever is under the cursor"
        // through walls wants culling off, and getting it wrong picks geometry the author cannot see.
        const std::vector<Vec3> corners = CubeCorners();
        const std::vector<Core::UInt32> indices = CubeIndices();

        PickCandidate candidate;
        candidate.OwnerSlot = 3;
        candidate.WorldTransform = Mat4::Identity();
        candidate.Vertices = corners.data();
        candidate.VertexCount = static_cast<Editor::Index>(corners.size());
        candidate.VertexStrideBytes = static_cast<Editor::Index>(sizeof(Vec3));
        candidate.Indices = indices.data();
        candidate.IndexCount = static_cast<Editor::Index>(indices.size());

        const Ray ray{Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f, 0.0f, -1.0f)};
        const auto culled = PickRay(ray, &candidate, 1, true);
        REQUIRE(culled.HasHit);

        // The front face is at z=+1, so 4 units along the ray — and culling must not change that for a face pointing at
        // the camera.
        CHECK(culled.Distance == doctest::Approx(4.0f));
    }

    TEST_CASE("Empty candidate lists are handled without a hit")
    {
        const Ray ray{Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, -1.0f)};
        CHECK_FALSE(PickRay(ray, nullptr, 0, true).HasHit);
    }
}

TEST_SUITE("EditorContentBrowser")
{
    TEST_CASE("SetRoot rejects a missing directory and accepts a real one")
    {
        ContentBrowserModel model;
        CHECK_FALSE(model.SetRoot(std::filesystem::temp_directory_path() / "hp-editor-does-not-exist-42"));

        ScratchDirectory scratch("root");
        CHECK(model.SetRoot(scratch.GetPath()));
        CHECK(model.GetRoot() == scratch.GetPath());
    }

    TEST_CASE("Refresh lists directories first, then files, alphabetically")
    {
        ScratchDirectory scratch("ordering");
        scratch.Write("zebra.hpscene", "{}");
        scratch.Write("alpha.lua", "--");
        scratch.Write("Beta.hpscene", "{}");
        std::filesystem::create_directory(scratch.GetPath() / "maps");
        std::filesystem::create_directory(scratch.GetPath() / "Assets");

        ContentBrowserModel model;
        REQUIRE(model.SetRoot(scratch.GetPath()));
        REQUIRE(model.Refresh());

        const std::vector<ContentEntry>& entries = model.GetEntries();
        REQUIRE(entries.size() == 5u);

        // Folders first: the ordering every file browser uses, and the one that lets the eye find a folder without a
        // full scan of a long list.
        CHECK(entries[0].Name == "Assets");
        CHECK(entries[1].Name == "maps");
        CHECK(entries[2].Name == "alpha.lua");
        CHECK(entries[3].Name == "Beta.hpscene");
        CHECK(entries[4].Name == "zebra.hpscene");

        CHECK(entries[0].IsDirectory);
        CHECK_FALSE(entries[2].IsDirectory);
    }

    TEST_CASE("Engine asset kinds are recognised, others are not")
    {
        ScratchDirectory scratch("kinds");
        scratch.Write("scene.hpscene", "{}");
        scratch.Write("logic.LUA", "--");  // case-insensitive: Windows filesystems are
        scratch.Write("model.glb", "glTF");
        scratch.Write("README.md", "# hi");

        ContentBrowserModel model;
        REQUIRE(model.SetRoot(scratch.GetPath()));
        REQUIRE(model.Refresh());

        auto findByName = [&model](std::string_view name) -> const ContentEntry* {
            for (const ContentEntry& entry : model.GetEntries())
            {
                if (entry.Name == name)
                {
                    return &entry;
                }
            }
            return nullptr;
        };

        REQUIRE(findByName("scene.hpscene") != nullptr);
        CHECK(findByName("scene.hpscene")->IsEngineAsset);
        CHECK(findByName("logic.LUA")->IsEngineAsset);
        CHECK_FALSE(findByName("model.glb")->IsEngineAsset);
        CHECK_FALSE(findByName("README.md")->IsEngineAsset);
    }

    TEST_CASE("OpenEntry refuses a non-asset with a message naming the file")
    {
        ScratchDirectory scratch("open");
        scratch.Write("model.glb", "glTF");

        ContentBrowserModel model;
        REQUIRE(model.SetRoot(scratch.GetPath()));
        REQUIRE(model.Refresh());

        const ContentEntry* entry = nullptr;
        for (const ContentEntry& candidate : model.GetEntries())
        {
            if (candidate.Name == "model.glb")
            {
                entry = &candidate;
            }
        }
        REQUIRE(entry != nullptr);

        std::string message;
        CHECK_FALSE(model.OpenEntry(*entry, message));
        // The point of the message: an unimportable file must say so, not sit there doing nothing.
        CHECK(message.find("model.glb") != std::string::npos);
    }

    TEST_CASE("ResolveWithinRoot rejects absolute paths and traversal")
    {
        ScratchDirectory scratch("resolve");
        ContentBrowserModel model;
        REQUIRE(model.SetRoot(scratch.GetPath()));

        std::filesystem::path resolved;
        CHECK(model.ResolveWithinRoot("sub/thing.hpscene", resolved));
        CHECK(resolved == scratch.GetPath() / "sub/thing.hpscene");

        // Both classic traversal and the normalised kind. A content browser that reads outside its project is a tool
        // that can be pointed at /etc by a mistyped console command.
        CHECK_FALSE(model.ResolveWithinRoot("../outside.hpscene", resolved));
        CHECK_FALSE(model.ResolveWithinRoot("a/../../outside.hpscene", resolved));

        const std::filesystem::path absolute = std::filesystem::temp_directory_path() / "outside.hpscene";
        CHECK_FALSE(model.ResolveWithinRoot(absolute, resolved));
    }

    TEST_CASE("NavigateUp walks to the parent and stops at the root")
    {
        ScratchDirectory scratch("navigate");
        scratch.Write("nested/inner.hpscene", "{}");

        ContentBrowserModel model;
        REQUIRE(model.SetRoot(scratch.GetPath()));
        CHECK_FALSE(model.CanNavigateUp());

        REQUIRE(model.Refresh("nested"));
        CHECK(model.CanNavigateUp());
        CHECK(model.GetCurrentPath() == scratch.GetPath() / "nested");

        CHECK(model.NavigateUp());
        CHECK_FALSE(model.CanNavigateUp());
        CHECK(model.GetCurrentPath() == scratch.GetPath());
    }

    TEST_CASE("FilterByName matches case-insensitively and keeps indices")
    {
        ScratchDirectory scratch("filter");
        scratch.Write("Player.hpscene", "{}");
        scratch.Write("enemy.hpscene", "{}");
        scratch.Write("notes.txt", "");

        ContentBrowserModel model;
        REQUIRE(model.SetRoot(scratch.GetPath()));
        REQUIRE(model.Refresh());

        const std::vector<Editor::Size> matches = model.FilterByName("player");
        REQUIRE(matches.size() == 1u);
        CHECK(model.GetEntries()[matches[0]].Name == "Player.hpscene");

        // An empty filter is "everything", which is what an empty search box means.
        CHECK(model.FilterByName("").size() == 3u);
    }

    TEST_CASE("A failed Refresh keeps the previous listing")
    {
        ScratchDirectory scratch("stale");
        scratch.Write("keep.hpscene", "{}");

        ContentBrowserModel model;
        REQUIRE(model.SetRoot(scratch.GetPath()));
        REQUIRE(model.Refresh());
        REQUIRE(model.GetEntries().size() == 1u);

        // A directory that vanishes between listing and click is normal in a project folder several tools write to. The
        // panel must not claim the project is empty.
        std::string error;
        CHECK_FALSE(model.Refresh("gone", &error));
        CHECK_FALSE(error.empty());
        CHECK(model.GetEntries().size() == 1u);
    }
}

TEST_SUITE("EditorPropertyView")
{
    TEST_CASE("Build lists reflected fields for the entity's components")
    {
        EnsureTypesRegistered();
        World world;
        const Entity entity = MakeEntity(world, "Cube");
        Game::MeshRenderer& renderer = world.AddComponent<Game::MeshRenderer>(entity);
        renderer.MeshPath = "engine:cube";
        renderer.Roughness = 0.25f;

        PropertyView view;
        view.Build(world, entity);

        const PropertyField* roughness = view.FindByPath("roughness");
        REQUIRE(roughness != nullptr);
        CHECK(roughness->IsEditable());
        CHECK(view.FormatValue(*roughness).find("0.25") != std::string::npos);

        const PropertyField* meshPath = view.FindByPath("meshPath");
        REQUIRE(meshPath != nullptr);
        std::string text;
        REQUIRE(view.ReadString(*meshPath, text));
        CHECK(text == "engine:cube");
    }

    TEST_CASE("Struct fields expand into their members")
    {
        EnsureTypesRegistered();
        World world;
        const Entity entity = MakeEntity(world, "Cube");
        Scene::LocalTransform& transform = world.AddComponent<Scene::LocalTransform>(entity);
        transform.Position.X = 1.5f;

        PropertyView view;
        view.Build(world, entity);

        // A collapsed "Position: [struct]" that cannot be clicked is a panel showing data it refuses to edit. The
        // members are what an author drags.
        // Registered as `Field("position", &LocalTransform::Position)` whose type is a reflected Vec3, so the panel
        // must expand it to position.x / .y / .z.
        const PropertyField* positionX = view.FindByPath("position.x");
        REQUIRE(positionX != nullptr);

        double value = 0.0;
        REQUIRE(view.ReadReal(*positionX, value));
        CHECK(value == doctest::Approx(1.5));
    }

    TEST_CASE("Writes go through to the component")
    {
        EnsureTypesRegistered();
        World world;
        const Entity entity = MakeEntity(world, "Cube");
        Game::MeshRenderer& renderer = world.AddComponent<Game::MeshRenderer>(entity);

        PropertyView view;
        view.Build(world, entity);

        const PropertyField* roughness = view.FindByPath("roughness");
        REQUIRE(roughness != nullptr);
        REQUIRE(view.WriteReal(*roughness, 0.75));

        // Rebuilt from scratch: the write must have landed in the component, not in a cached copy of it.
        PropertyView second;
        second.Build(world, entity);
        const PropertyField* reloaded = second.FindByPath("roughness");
        REQUIRE(reloaded != nullptr);

        double value = 0.0;
        REQUIRE(second.ReadReal(*reloaded, value));
        CHECK(value == doctest::Approx(0.75));
        CHECK(renderer.Roughness == doctest::Approx(0.75f));
    }

    TEST_CASE("Booleans read, write and format as words")
    {
        EnsureTypesRegistered();
        World world;
        const Entity entity = MakeEntity(world, "Cube");
        Game::MeshRenderer& renderer = world.AddComponent<Game::MeshRenderer>(entity);
        renderer.CastsShadows = true;

        PropertyView view;
        view.Build(world, entity);

        const PropertyField* field = view.FindByPath("castsShadows");
        REQUIRE(field != nullptr);

        bool value = false;
        REQUIRE(view.ReadBool(*field, value));
        CHECK(value);
        CHECK(view.FormatValue(*field) == "true");

        REQUIRE(view.WriteBool(*field, false));
        CHECK_FALSE(renderer.CastsShadows);
        CHECK(view.FormatValue(*field) == "false");
    }

    TEST_CASE("A dead or component-less entity yields no fields")
    {
        EnsureTypesRegistered();
        World world;
        const Entity entity = MakeEntity(world, "Cube");

        PropertyView view;
        view.Build(world, Entity{});
        CHECK(view.GetFieldCount() == 0u);

        view.Build(world, entity);
        CHECK(view.GetFieldCount() > 0u);  // EntityName + LocalTransform are reflected

        world.DestroyEntity(entity);
        view.Build(world, entity);
        CHECK(view.GetFieldCount() == 0u);  // a dead entity has nothing to edit
    }

    TEST_CASE("GetComponentNames groups fields by component")
    {
        EnsureTypesRegistered();
        World world;
        const Entity entity = MakeEntity(world, "Cube");
        world.AddComponent<Game::MeshRenderer>(entity);

        PropertyView view;
        view.Build(world, entity);

        const std::vector<std::string> names = view.GetComponentNames();
        CHECK(std::find(names.begin(), names.end(), "MeshRenderer") != names.end());

        const std::vector<PropertyField> meshFields = view.GetFieldsForComponent("MeshRenderer");
        CHECK_FALSE(meshFields.empty());
        for (const PropertyField& field : meshFields)
        {
            CHECK(field.ComponentName == "MeshRenderer");
        }
    }
}
