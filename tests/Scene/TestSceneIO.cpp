#include "TestSupport.h"

#include <cmath>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Core/FileSystem.h"
#include "Hollowpoint/Core/Guid.h"
#include "Hollowpoint/ECS/World.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Reflection/Registry.h"
#include "Hollowpoint/Scene/LocalTransform.h"
#include "Hollowpoint/Scene/SceneIO.h"
#include "Hollowpoint/Scene/TransformHierarchy.h"
#include "Hollowpoint/Serialization/JsonParser.h"
#include "Hollowpoint/Serialization/JsonWriter.h"

using Hollowpoint::Core::Guid;
using Hollowpoint::Core::Size;
using Hollowpoint::Core::UInt64;
using Hollowpoint::ECS::Entity;
using Hollowpoint::ECS::World;
using Hollowpoint::Math::Quat;
using Hollowpoint::Math::Transform;
using Hollowpoint::Math::Vec3;
using Hollowpoint::Reflection::Registry;
using namespace Hollowpoint::Scene;
using Hollowpoint::Serialization::JsonArray;
using Hollowpoint::Serialization::JsonObject;
using Hollowpoint::Serialization::JsonValue;

namespace
{

constexpr float kEpsilon = 1.0e-5f;

bool Near(const Vec3& a, const Vec3& b, float epsilon = kEpsilon)
{
    return std::fabs(a.X - b.X) < epsilon && std::fabs(a.Y - b.Y) < epsilon && std::fabs(a.Z - b.Z) < epsilon;
}

// A component used only by these tests. Registering it through the public factory API is what a real game component
// does, so the tests exercise exactly the path production code takes rather than a test-only back door.
struct TestGameplayTag
{
    std::string Tag = "untagged";
    int Priority = 0;
    bool Visible = true;
};

const bool kTestTypesRegistered = []() {
    RegisterSceneTypes();

    Registry::Get()
        .RegisterType<TestGameplayTag>("TestGameplayTag")
        .Field("tag", &TestGameplayTag::Tag)
        .Field("priority", &TestGameplayTag::Priority)
        .Field("visible", &TestGameplayTag::Visible);

    ComponentFactory::Get().Register<TestGameplayTag>("TestGameplayTag");
    return true;
}();

Entity MakePlaceable(World& world, const Vec3& position)
{
    const Entity entity = world.CreateEntity();
    LocalTransform transform;
    transform.Position = position;
    world.AddComponent<LocalTransform>(entity).SetFrom(transform.ToTransform());
    return entity;
}

const JsonValue* FindEntityById(const JsonValue& document, int id)
{
    const JsonValue* entities = document.FindMember("entities");
    if (entities == nullptr || !entities->IsArray())
    {
        return nullptr;
    }

    for (const JsonValue& entry : entities->AsArray())
    {
        const JsonValue* idValue = entry.FindMember("id");
        if (idValue != nullptr && idValue->IsNumber() && idValue->AsInt() == id)
        {
            return &entry;
        }
    }
    return nullptr;
}

std::string WriteAndReload(World& world, World& destination)
{
    const JsonValue document = WriteScene(world);
    const std::string text = Hollowpoint::Serialization::WriteJson(document);

    const SceneLoadReport report = LoadSceneFromText(destination, text);
    REQUIRE(report.Loaded);
    return text;
}

// ---------------------------------------------------------------------------
// Scratch file for the file-I/O cases at the bottom of this file.
//
// Names must be unique per case AND per process: ctest runs test binaries concurrently, and two tests sharing a scratch
// path means one deletes the other's fixture — producing failures that only appear under -j and blame whichever test
// lost the race. random_device supplies the cross-process entropy (fs::unique_path does this job but is deprecated in
// C++20); the counter keeps cases inside one process apart even if the RNG repeats.
// ---------------------------------------------------------------------------

[[nodiscard]] std::filesystem::path MakeScratchScenePath(std::string_view label)
{
    static Hollowpoint::Core::UInt64 s_Counter = 0;
    const Hollowpoint::Core::UInt64 token = s_Counter++;

    std::random_device entropy;
    const Hollowpoint::Core::UInt64 randomPart =
        (static_cast<Hollowpoint::Core::UInt64>(entropy()) << 32) ^ static_cast<Hollowpoint::Core::UInt64>(entropy());

    // The whole path lives inside one scratch DIRECTORY so cleanup can remove the tree: SaveSceneToFile creates parent
    // directories, and leaving "…/levels/level01/" behind under /tmp is how a suite slowly fills a tmpfs.
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("hp_scene_" + std::string(label) + "_" + std::to_string(randomPart) + "_" + std::to_string(token));

    return directory / "scene.hpscene";
}

class ScopedScratchScene
{
public:
    explicit ScopedScratchScene(std::string_view label) : Path(MakeScratchScenePath(label))
    {
        std::error_code ignored;
        std::filesystem::remove_all(Path.parent_path(), ignored);
    }

    ~ScopedScratchScene()
    {
        std::error_code ignored;
        std::filesystem::remove_all(Path.parent_path(), ignored);
    }

    ScopedScratchScene(const ScopedScratchScene&) = delete;
    ScopedScratchScene& operator=(const ScopedScratchScene&) = delete;

    std::filesystem::path Path;
};

}  // namespace

TEST_SUITE("Scene/SceneIO")
{
    TEST_CASE("registration is idempotent and covers the transform types")
    {
        RegisterSceneTypes();
        RegisterSceneTypes();  // Twice must be a no-op rather than a duplicate descriptor.

        CHECK(AreSceneTypesRegistered());
        CHECK(Registry::Get().FindType("LocalTransform") != nullptr);
        CHECK(Registry::Get().FindType("Math.Vec3") != nullptr);
        CHECK(Registry::Get().FindType("Math.Quat") != nullptr);

        // Registering twice must not add a second descriptor under one name — otherwise FindType would resolve to
        // whichever the registry reached first, with no error pointing at the cause.
        const Size typesBefore = Registry::Get().GetTypeCount();
        RegisterSceneTypes();
        CHECK(Registry::Get().GetTypeCount() == typesBefore);
    }

    TEST_CASE("the factory knows the engine's components and reports unknown names")
    {
        RegisterSceneTypes();

        CHECK(ComponentFactory::Get().IsRegistered("LocalTransform"));
        CHECK_FALSE(ComponentFactory::Get().IsRegistered("NoSuchComponent"));

        const std::vector<std::string> names = ComponentFactory::Get().GetRegisteredNames();
        CHECK(!names.empty());
        // Sorted, so editor lists and any tool diffing "what can this build create" are stable across runs.
        CHECK(std::is_sorted(names.begin(), names.end()));
    }

    TEST_CASE("an empty world saves as an empty scene")
    {
        World world;
        const JsonValue document = WriteScene(world);

        CHECK(document.IsObject());
        CHECK(document.FindMember("formatVersion")->AsInt() == 1);
        REQUIRE(document.FindMember("entities") != nullptr);
        CHECK(document.FindMember("entities")->GetElementCount() == 0u);
    }

    TEST_CASE("an entity with a transform round-trips through text")
    {
        World world;
        const Entity entity = MakePlaceable(world, Vec3(1.5f, -2.25f, 3.0f));
        const Guid identity = world.GetGuid(entity);

        World destination;
        const std::string text = WriteAndReload(world, destination);

        Size count = 0;
        Entity loadedEntity;
        destination.ForEachEntity([&](const Entity& candidate) {
            ++count;
            loadedEntity = candidate;
        });

        REQUIRE(count == 1u);

        const LocalTransform* transform = destination.TryGetComponent<LocalTransform>(loadedEntity);
        REQUIRE(transform != nullptr);
        CHECK(Near(transform->Position, Vec3(1.5f, -2.25f, 3.0f)));

        // Identity is the contract that cross-file references depend on, so it must survive exactly.
        CHECK(destination.GetGuid(loadedEntity) == identity);
        CHECK(destination.GetEntityByGuid(identity) == loadedEntity);
    }

    TEST_CASE("rotation and scale round-trip")
    {
        World world;
        const Entity entity = world.CreateEntity();
        LocalTransform& stored = world.AddComponent<LocalTransform>(entity);
        stored.Position = Vec3(0.0f);
        stored.Rotation = Quat::FromAxisAngle(Vec3(1.0f, 2.0f, 3.0f), 0.9f).Normalized();
        stored.Scale = Vec3(1.0f, 2.5f, 0.25f);

        World destination;
        WriteAndReload(world, destination);

        Entity loadedEntity;
        destination.ForEachEntity([&](const Entity& candidate) { loadedEntity = candidate; });

        const LocalTransform* transform = destination.TryGetComponent<LocalTransform>(loadedEntity);
        REQUIRE(transform != nullptr);
        CHECK(Near(transform->Scale, Vec3(1.0f, 2.5f, 0.25f)));

        // Quaternions are compared by their action rather than component-wise: q and -q are the same rotation, so a
        // sign flip is not a bug while a changed angle is.
        const Vec3 rotatedSource = stored.Rotation * Vec3(1.0f, 0.0f, 0.0f);
        const Vec3 rotatedLoaded = transform->Rotation * Vec3(1.0f, 0.0f, 0.0f);
        CHECK(Near(rotatedSource, rotatedLoaded, 1.0e-4f));
    }

    TEST_CASE("hierarchy round-trips")
    {
        World world;
        const Entity root = MakePlaceable(world, Vec3(10.0f, 0.0f, 0.0f));
        const Entity child = MakePlaceable(world, Vec3(0.0f, 5.0f, 0.0f));
        const Entity grandchild = MakePlaceable(world, Vec3(0.0f, 0.0f, 1.0f));
        world.SetParent(child, root);
        world.SetParent(grandchild, child);

        World destination;
        WriteAndReload(world, destination);

        std::vector<Entity> loaded;
        destination.ForEachEntity([&](const Entity& entity) { loaded.push_back(entity); });
        REQUIRE(loaded.size() == 3u);

        // Resolve by position (each entity's local transform is unique here) rather than assuming slot order.
        auto findByPosition = [&](const Vec3& position) -> Entity {
            for (const Entity& entity : loaded)
            {
                const LocalTransform* transform = destination.TryGetComponent<LocalTransform>(entity);
                if (transform != nullptr && Near(transform->Position, position))
                {
                    return entity;
                }
            }
            return Entity();
        };

        const Entity loadedRoot = findByPosition(Vec3(10.0f, 0.0f, 0.0f));
        const Entity loadedChild = findByPosition(Vec3(0.0f, 5.0f, 0.0f));
        const Entity loadedGrandchild = findByPosition(Vec3(0.0f, 0.0f, 1.0f));

        REQUIRE_FALSE(loadedRoot.IsNull());
        REQUIRE_FALSE(loadedChild.IsNull());
        REQUIRE_FALSE(loadedGrandchild.IsNull());

        CHECK(destination.GetParent(loadedChild) == loadedRoot);
        CHECK(destination.GetParent(loadedGrandchild) == loadedChild);
        CHECK(destination.GetParent(loadedRoot).IsNull());
    }

    TEST_CASE("world transforms are correct after a load, not just the hierarchy")
    {
        // The point of saving at all: a reloaded scene must place objects identically. Comparing world matrices rather
        // than stored locals catches a broken parent chain that still looks right field by field.
        World world;
        const Entity pivot = MakePlaceable(world, Vec3(10.0f, 0.0f, 0.0f));
        const Entity arm = MakePlaceable(world, Vec3(0.0f, 3.0f, 0.0f));
        world.SetParent(arm, pivot);

        Transform rotated;
        rotated.Position = Vec3(10.0f, 0.0f, 0.0f);
        rotated.Rotation = Quat::FromAxisAngle(Vec3(0.0f, 1.0f, 0.0f), 1.5707963267948966f).Normalized();
        world.GetComponent<LocalTransform>(pivot).SetFrom(rotated);

        TransformHierarchy source;
        source.Update(world);
        const Vec3 expectedWorld = source.GetWorldMatrix(world, arm).GetTranslation();

        World destination;
        WriteAndReload(world, destination);

        Entity loadedArm;
        destination.ForEachEntity([&](const Entity& entity) {
            const LocalTransform* transform = destination.TryGetComponent<LocalTransform>(entity);
            if (transform != nullptr && Near(transform->Position, Vec3(0.0f, 3.0f, 0.0f)))
            {
                loadedArm = entity;
            }
        });
        REQUIRE_FALSE(loadedArm.IsNull());

        TransformHierarchy destinationTransforms;
        destinationTransforms.Update(destination);

        const Vec3 loadedWorld = destinationTransforms.GetWorldMatrix(destination, loadedArm).GetTranslation();
        CHECK(Near(loadedWorld, expectedWorld, 1.0e-4f));
    }

    TEST_CASE("a child listed before its parent still links (two-phase load)")
    {
        // Hand-written and tool-generated files are not guaranteed to be in topological order. A single-pass loader
        // would silently drop this parent link, so the file is constructed with the dependency backwards on purpose.
        const char* text = R"({
            "formatVersion": 1,
            "entities": [
                { "id": 0, "components": { "LocalTransform": { "position": {"x":0,"y":5,"z":0},
                                                              "rotation": {"x":0,"y":0,"z":0,"w":1},
                                                              "scale": {"x":1,"y":1,"z":1} } } },
                { "id": 1, "parent": 0, "components": { "LocalTransform": { "position": {"x":1,"y":0,"z":0},
                                                                            "rotation": {"x":0,"y":0,"z":0,"w":1},
                                                                            "scale": {"x":1,"y":1,"z":1} } } }
            ]
        })";

        World world;
        const SceneLoadReport report = LoadSceneFromText(world, text);
        REQUIRE(report.Loaded);
        CHECK_FALSE(report.HasErrors());

        std::vector<Entity> entities;
        world.ForEachEntity([&](const Entity& entity) { entities.push_back(entity); });
        REQUIRE(entities.size() == 2u);

        // Exactly one root and one child, whichever slot order the world used.
        Entity foundRoot;
        Entity foundChild;
        for (const Entity& entity : entities)
        {
            if (world.GetParent(entity).IsNull())
            {
                foundRoot = entity;
            }
            else
            {
                foundChild = entity;
            }
        }

        REQUIRE_FALSE(foundRoot.IsNull());
        REQUIRE_FALSE(foundChild.IsNull());

        // The link itself, not just the counts: a loader that resolves parents only when they happen to be created
        // already leaves *a* root and *a* child in some cases while dropping the actual edge, so counting alone can
        // pass with the hierarchy broken. Asserting the specific parent makes the ordering guarantee testable.
        CHECK(world.GetParent(foundChild) == foundRoot);

        // And the positions confirm which is which (id 0 is the y=5 root, id 1 is its x=1 child).
        const LocalTransform* rootTransform = world.TryGetComponent<LocalTransform>(foundRoot);
        const LocalTransform* childTransform = world.TryGetComponent<LocalTransform>(foundChild);
        REQUIRE(rootTransform != nullptr);
        REQUIRE(childTransform != nullptr);
        CHECK(Near(rootTransform->Position, Vec3(0.0f, 5.0f, 0.0f)));
        CHECK(Near(childTransform->Position, Vec3(1.0f, 0.0f, 0.0f)));
    }

    TEST_CASE("an entity whose parent is a *later* id still links")
    {
        // The order-dependence this pins down: entity 0 names entity 1 as its parent, so a loader that resolves each
        // parent while creating entities cannot know about entity 1 yet and would leave entity 0 a root. A document
        // where the child happens to come second passes either way, which is why both directions are covered here and
        // in "a child listed before its parent still links".
        const char* text = R"({
            "formatVersion": 1,
            "entities": [
                { "id": 0, "parent": 1 },
                { "id": 1 }
            ]
        })";

        World world;
        const SceneLoadReport report = LoadSceneFromText(world, text);
        REQUIRE(report.Loaded);
        CHECK(report.Warnings.empty());  // A valid reference to a later entity must not warn.

        std::vector<Entity> entities;
        world.ForEachEntity([&](const Entity& entity) { entities.push_back(entity); });
        REQUIRE(entities.size() == 2u);

        Size roots = 0;
        Size linked = 0;
        for (const Entity& entity : entities)
        {
            if (world.GetParent(entity).IsNull())
            {
                ++roots;
            }
            else
            {
                ++linked;
            }
        }

        // One root and one linked child, and the link points at a live entity rather than nowhere.
        CHECK(roots == 1u);
        CHECK(linked == 1u);

        for (const Entity& entity : entities)
        {
            const Entity parent = world.GetParent(entity);
            if (!parent.IsNull())
            {
                CHECK(world.IsAlive(parent));
                CHECK(world.GetFirstChild(parent) == entity);
            }
        }
    }

    TEST_CASE("a custom component round-trips through the factory")
    {
        World world;
        const Entity entity = world.CreateEntity();
        TestGameplayTag& tag = world.AddComponent<TestGameplayTag>(entity);
        tag.Tag = "enemy/boss";
        tag.Priority = 7;
        tag.Visible = false;

        World destination;
        WriteAndReload(world, destination);

        Entity loaded;
        destination.ForEachEntity([&](const Entity& candidate) { loaded = candidate; });

        const TestGameplayTag* loadedTag = destination.TryGetComponent<TestGameplayTag>(loaded);
        REQUIRE(loadedTag != nullptr);
        CHECK(loadedTag->Tag == "enemy/boss");
        CHECK(loadedTag->Priority == 7);
        CHECK(loadedTag->Visible == false);
    }

    TEST_CASE("an unknown component type is skipped with a warning naming it")
    {
        // The normal signature of a scene saved by a build that had a component this one does not. Refusing to load
        // would block the level; silently dropping it would lose data invisibly — so: skip, and name it exactly.
        const char* text = R"({
            "formatVersion": 1,
            "entities": [
                { "id": 0, "components": {
                    "TestGameplayTag": { "tag": "kept", "priority": 3, "visible": true },
                    "ComponentFromTheFuture": { "whatever": 1 } } }
            ]
        })";

        World world;
        const SceneLoadReport report = LoadSceneFromText(world, text);

        CHECK(report.Loaded);             // The scene is still usable.
        CHECK_FALSE(report.HasErrors());  // Not an error...
        REQUIRE(report.Warnings.size() == 1u);
        CHECK(report.Warnings[0].Message.find("ComponentFromTheFuture") != std::string::npos);

        Entity loaded;
        world.ForEachEntity([&](const Entity& candidate) { loaded = candidate; });
        const TestGameplayTag* tag = world.TryGetComponent<TestGameplayTag>(loaded);
        REQUIRE(tag != nullptr);
        CHECK(tag->Tag == "kept");  // The known component still loaded.
    }

    TEST_CASE("a bad field value is reported with its path and does not abort the load")
    {
        const char* text = R"({
            "formatVersion": 1,
            "entities": [
                { "id": 0, "components": { "TestGameplayTag": { "tag": 123, "priority": 4 } } }
            ]
        })";

        World world;
        const SceneLoadReport report = LoadSceneFromText(world, text);

        CHECK(report.Loaded);
        REQUIRE_FALSE(report.Warnings.empty());
        // Path-qualified and naming the component, so a twelve-problem file gives twelve actionable lines.
        CHECK(report.Warnings[0].Message.find("TestGameplayTag") != std::string::npos);
        CHECK(report.Warnings[0].Message.find("tag") != std::string::npos);

        Entity loaded;
        world.ForEachEntity([&](const Entity& candidate) { loaded = candidate; });
        const TestGameplayTag* tag = world.TryGetComponent<TestGameplayTag>(loaded);
        REQUIRE(tag != nullptr);
        CHECK(tag->Priority == 4);  // The good sibling field still applied.
    }

    TEST_CASE("entities without components still round-trip")
    {
        // A bare entity is meaningful: it is a group node in the hierarchy, and dropping it would reparent its
        // children onto nothing.
        World world;
        const Entity root = world.CreateEntity();
        const Entity child = MakePlaceable(world, Vec3(1.0f, 0.0f, 0.0f));
        world.SetParent(child, root);

        World destination;
        WriteAndReload(world, destination);

        Size count = 0;
        destination.ForEachEntity([&](const Entity&) { ++count; });
        CHECK(count == 2u);
    }

    TEST_CASE("a scene with no version is refused rather than guessed")
    {
        const char* text = R"({ "entities": [ { "id": 0 } ] })";

        World world;
        MakePlaceable(world, Vec3(9.0f, 0.0f, 0.0f));  // Existing content that must survive.

        const SceneLoadReport report = LoadSceneFromText(world, text);

        CHECK_FALSE(report.Loaded);
        REQUIRE(report.Errors.size() == 1u);
        CHECK(report.Errors[0].Message.find("formatVersion") != std::string::npos);

        // Refusing must mean "untouched", not "half replaced" — otherwise a bad file destroys the open scene.
        Size count = 0;
        world.ForEachEntity([&](const Entity&) { ++count; });
        CHECK(count == 1u);
    }

    TEST_CASE("a newer format version is refused instead of partially loaded")
    {
        // The bridge tolerates unknown fields, so a v2 file would otherwise "load successfully" while discarding
        // whatever structure v2 relies on — data loss reported as success.
        const char* text = R"({ "formatVersion": 99, "entities": [ { "id": 0 } ] })";

        World world;
        const SceneLoadReport report = LoadSceneFromText(world, text);

        CHECK_FALSE(report.Loaded);
        REQUIRE(report.Errors.size() == 1u);
        CHECK(report.Errors[0].Message.find("99") != std::string::npos);
    }

    TEST_CASE("an older-but-readable version loads")
    {
        // Forward compatibility: the current loader must keep opening files written by earlier builds, since missing
        // fields simply take their defaults.
        const char* text = R"({ "formatVersion": 1, "entities": [ { "id": 0 } ] })";

        World world;
        CHECK(LoadSceneFromText(world, text).Loaded);
    }

    TEST_CASE("a JSON syntax error is reported through the same channel as content errors")
    {
        World world;
        const SceneLoadReport report = LoadSceneFromText(world, R"({ "formatVersion": 1, "entities": [ )");

        CHECK_FALSE(report.Loaded);
        REQUIRE(report.Errors.size() == 1u);
        // Position is included: a scene file can be thousands of lines and "invalid JSON" alone is not actionable.
        CHECK(report.Errors[0].Message.find("line") != std::string::npos);
    }

    TEST_CASE("a non-object root and a missing entity array are refused")
    {
        World world;
        CHECK_FALSE(LoadSceneFromText(world, "[1, 2, 3]").Loaded);
        CHECK_FALSE(LoadSceneFromText(world, R"({ "formatVersion": 1 })").Loaded);
        CHECK_FALSE(LoadSceneFromText(world, R"({ "formatVersion": 1, "entities": {} })").Loaded);
    }

    TEST_CASE("an invalid guid warns and assigns a fresh identity")
    {
        const char* text = R"({ "formatVersion": 1, "entities": [ { "id": 0, "guid": "not-a-guid" } ] })";

        World world;
        const SceneLoadReport report = LoadSceneFromText(world, text);

        CHECK(report.Loaded);
        REQUIRE(report.Warnings.size() == 1u);
        CHECK(report.Warnings[0].Message.find("guid") != std::string::npos);

        Entity loaded;
        world.ForEachEntity([&](const Entity& candidate) { loaded = candidate; });
        CHECK_FALSE(world.GetGuid(loaded).IsNil());
    }

    TEST_CASE("a duplicate guid warns and the second entity gets a fresh one")
    {
        const char* text = R"({ "formatVersion": 1, "entities": [
            { "id": 0, "guid": "00112233-4455-6677-8899-aabbccddeeff" },
            { "id": 1, "guid": "00112233-4455-6677-8899-aabbccddeeff" } ] })";

        World world;
        const SceneLoadReport report = LoadSceneFromText(world, text);

        CHECK(report.Loaded);  // Still usable: one entity just has a different identity.
        REQUIRE(report.Warnings.size() == 1u);

        Size count = 0;
        std::vector<Guid> guids;
        world.ForEachEntity([&](const Entity& entity) {
            ++count;
            guids.push_back(world.GetGuid(entity));
        });
        CHECK(count == 2u);
        REQUIRE(guids.size() == 2u);
        CHECK(guids[0] != guids[1]);
    }

    TEST_CASE("an out-of-range parent id leaves the entity a root")
    {
        const char* text = R"({ "formatVersion": 1, "entities": [
            { "id": 0 }, { "id": 1, "parent": 57 } ] })";

        World world;
        const SceneLoadReport report = LoadSceneFromText(world, text);

        CHECK(report.Loaded);
        REQUIRE(report.Warnings.size() == 1u);

        Size roots = 0;
        world.ForEachEntity([&](const Entity& entity) {
            if (world.GetParent(entity).IsNull())
            {
                ++roots;
            }
        });
        CHECK(roots == 2u);
    }

    TEST_CASE("a self-parenting entity is refused and left a root")
    {
        const char* text = R"({ "formatVersion": 1, "entities": [ { "id": 0, "parent": 0 } ] })";

        World world;
        const SceneLoadReport report = LoadSceneFromText(world, text);

        CHECK(report.Loaded);
        REQUIRE(report.Warnings.size() == 1u);
        CHECK(report.Warnings[0].Message.find("own parent") != std::string::npos);
    }

    TEST_CASE("a cycle written into the file cannot hang the transform pass")
    {
        // A -> B -> A. SetParent rejects the closing link, which is the only thing standing between a hand-edited
        // scene and an infinite loop during world-transform evaluation.
        const char* text = R"({ "formatVersion": 1, "entities": [
            { "id": 0, "parent": 1 }, { "id": 1, "parent": 0 } ] })";

        World world;
        const SceneLoadReport report = LoadSceneFromText(world, text);

        CHECK(report.Loaded);
        REQUIRE(report.Warnings.size() == 1u);

        // Exactly one root remains, and evaluating transforms terminates.
        Size roots = 0;
        world.ForEachEntity([&](const Entity& entity) {
            if (world.GetParent(entity).IsNull())
            {
                ++roots;
            }
        });
        CHECK(roots == 1u);

        TransformHierarchy transforms;
        transforms.Update(world);  // Must not hang or overflow.
    }

    TEST_CASE("a malformed entity entry is an error that aborts the whole load")
    {
        // Structural damage means the document cannot be trusted as a whole: half-loading would leave a scene with a
        // missing object and no signal that anything was wrong.
        const char* text = R"({ "formatVersion": 1, "entities": [ { "id": 0 }, 42 ] })";

        World world;
        const SceneLoadReport report = LoadSceneFromText(world, text);

        CHECK_FALSE(report.Loaded);
        REQUIRE(report.Errors.size() == 1u);
        CHECK(report.Errors[0].EntityIndex == 1u);

        Size count = 0;
        world.ForEachEntity([&](const Entity&) { ++count; });
        CHECK(count == 0u);
    }

    TEST_CASE("ClearWorldFirst controls whether existing entities survive")
    {
        World world;
        MakePlaceable(world, Vec3(1.0f, 0.0f, 0.0f));

        const JsonValue document = WriteScene(world);

        // Append mode (prefab instantiation / level streaming) must add rather than replace.
        SceneLoadOptions append;
        append.ClearWorldFirst = false;

        World destination;
        MakePlaceable(destination, Vec3(99.0f, 0.0f, 0.0f));

        REQUIRE(LoadScene(destination, document, append).Loaded);

        Size count = 0;
        destination.ForEachEntity([&](const Entity&) { ++count; });
        CHECK(count == 2u);

        // Default mode replaces.
        World replaced;
        MakePlaceable(replaced, Vec3(99.0f, 0.0f, 0.0f));
        REQUIRE(LoadScene(replaced, document).Loaded);

        count = 0;
        replaced.ForEachEntity([&](const Entity&) { ++count; });
        CHECK(count == 1u);
    }

    TEST_CASE("saving is deterministic: the same scene produces identical bytes")
    {
        World world;
        const Entity root = MakePlaceable(world, Vec3(1.0f, 2.0f, 3.0f));
        const Entity child = MakePlaceable(world, Vec3(4.0f, 5.0f, 6.0f));
        world.SetParent(child, root);
        world.AddComponent<TestGameplayTag>(child).Tag = "prop";

        const std::string first = Hollowpoint::Serialization::WriteJson(WriteScene(world));
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            CHECK(Hollowpoint::Serialization::WriteJson(WriteScene(world)) == first);
        }
    }

    TEST_CASE("save -> load -> save is byte-identical")
    {
        // The property that makes "open a level and save it" a no-op in version control. If this breaks, every scene
        // file in the project churns on touch and merges become impossible.
        World world;
        const Entity root = MakePlaceable(world, Vec3(1.0f, 2.0f, 3.0f));
        const Entity child = MakePlaceable(world, Vec3(4.0f, 5.0f, 6.0f));
        const Entity grandchild = MakePlaceable(world, Vec3(7.0f, 8.0f, 9.0f));
        world.SetParent(child, root);
        world.SetParent(grandchild, child);
        world.AddComponent<TestGameplayTag>(root).Tag = "checkpoint";

        const std::string original = Hollowpoint::Serialization::WriteJson(WriteScene(world));

        World destination;
        REQUIRE(LoadSceneFromText(destination, original).Loaded);
        const std::string resaved = Hollowpoint::Serialization::WriteJson(WriteScene(destination));

        if (resaved != original)
        {
            INFO("original:\n" << original << "\nresaved:\n" << resaved);
            FAIL("save -> load -> save changed the file");
        }
    }

    TEST_CASE("the document has the documented shape")
    {
        // Asserting on file structure directly, not only on round-trip behaviour: a writer that emits "entity" instead
        // of "entities", or nests components under the wrong key, still round-trips against its own reader while being
        // unreadable by every other tool and by any future loader.
        World world;
        const Entity root = MakePlaceable(world, Vec3(1.0f, 2.0f, 3.0f));
        const Entity child = MakePlaceable(world, Vec3(4.0f, 5.0f, 6.0f));
        world.SetParent(child, root);

        SceneSaveOptions options;
        options.SceneName = "ShapeTest";
        const JsonValue document = WriteScene(world, options);

        CHECK(document.FindMember("formatVersion") != nullptr);
        CHECK(document.FindMember("name")->AsString() == "ShapeTest");
        REQUIRE(document.FindMember("entities") != nullptr);
        CHECK(document.FindMember("entities")->GetElementCount() == 2u);

        const JsonValue* rootEntry = FindEntityById(document, 0);
        const JsonValue* childEntry = FindEntityById(document, 1);
        REQUIRE(rootEntry != nullptr);
        REQUIRE(childEntry != nullptr);

        // Roots omit "parent"; nested entities carry the file-local id. Both are part of the documented format and
        // both are what a hand-editor relies on.
        CHECK(rootEntry->FindMember("parent") == nullptr);
        REQUIRE(childEntry->FindMember("parent") != nullptr);
        CHECK(childEntry->FindMember("parent")->AsInt() == 0);

        // Components live under "components", keyed by reflected type name.
        const JsonValue* components = childEntry->FindMember("components");
        REQUIRE(components != nullptr);
        REQUIRE(components->FindMember("LocalTransform") != nullptr);
        CHECK(components->FindMember("LocalTransform")->FindMember("position") != nullptr);

        // Identity is a string guid, distinct per entity.
        const JsonValue* rootGuid = rootEntry->FindMember("guid");
        const JsonValue* childGuid = childEntry->FindMember("guid");
        REQUIRE(rootGuid != nullptr);
        REQUIRE(childGuid != nullptr);
        CHECK(rootGuid->IsString());
        CHECK(rootGuid->AsString() != childGuid->AsString());
    }

    TEST_CASE("a scene name is written when supplied and omitted when not")
    {
        World world;

        SceneSaveOptions unnamed;
        CHECK(WriteScene(world).FindMember("name") == nullptr);

        SceneSaveOptions named;
        named.SceneName = "Level01";
        const JsonValue document = WriteScene(world, named);
        REQUIRE(document.FindMember("name") != nullptr);
        CHECK(document.FindMember("name")->AsString() == "Level01");
    }

    TEST_CASE("a large scene round-trips with the hierarchy intact")
    {
        World world;

        std::vector<Entity> entities;
        for (int index = 0; index < 400; ++index)
        {
            entities.push_back(MakePlaceable(world, Vec3(static_cast<float>(index), 0.0f, 0.5f)));
        }
        for (Size index = 1; index < entities.size(); ++index)
        {
            world.SetParent(entities[index], entities[(index * 7u + 3u) % index]);
        }

        World destination;
        WriteAndReload(world, destination);

        Size count = 0;
        destination.ForEachEntity([&](const Entity&) { ++count; });
        CHECK(count == entities.size());

        // The number of roots must match exactly: a single dropped parent link changes it and is invisible otherwise.
        Size sourceRoots = 0;
        world.ForEachEntity([&](const Entity& entity) {
            if (world.GetParent(entity).IsNull())
            {
                ++sourceRoots;
            }
        });
        Size destinationRoots = 0;
        destination.ForEachEntity([&](const Entity& entity) {
            if (destination.GetParent(entity).IsNull())
            {
                ++destinationRoots;
            }
        });
        CHECK(destinationRoots == sourceRoots);

        // And every entity's world position must match, which is the property anyone actually cares about.
        TransformHierarchy sourceTransforms;
        sourceTransforms.Update(world);
        TransformHierarchy destinationTransforms;
        destinationTransforms.Update(destination);

        Size mismatches = 0;
        for (const Entity& entity : entities)
        {
            const Guid identity = world.GetGuid(entity);
            const Entity mirror = destination.GetEntityByGuid(identity);
            if (mirror.IsNull())
            {
                ++mismatches;
                continue;
            }

            const Vec3 expected = sourceTransforms.GetWorldMatrix(world, entity).GetTranslation();
            const Vec3 actual = destinationTransforms.GetWorldMatrix(destination, mirror).GetTranslation();
            if (!Near(expected, actual, 1.0e-4f))
            {
                ++mismatches;
            }
        }
        CHECK(mismatches == 0u);
    }

    TEST_CASE("a destroyed entity is not saved")
    {
        World world;
        const Entity keep = MakePlaceable(world, Vec3(1.0f, 0.0f, 0.0f));
        const Entity remove = MakePlaceable(world, Vec3(2.0f, 0.0f, 0.0f));
        world.DestroyEntity(remove);
        world.FlushPendingDestructions();

        World destination;
        WriteAndReload(world, destination);

        Size count = 0;
        destination.ForEachEntity([&](const Entity&) { ++count; });
        CHECK(count == 1u);
        CHECK(destination.IsAlive(destination.GetEntityByGuid(world.GetGuid(keep))));
        CHECK(destination.GetEntityByGuid(world.GetGuid(remove)).IsNull());
    }

    TEST_CASE("destroying a subtree removes it from the saved scene")
    {
        World world;
        const Entity root = MakePlaceable(world, Vec3(1.0f, 0.0f, 0.0f));
        const Entity doomedChild = MakePlaceable(world, Vec3(2.0f, 0.0f, 0.0f));
        const Entity grandchild = MakePlaceable(world, Vec3(3.0f, 0.0f, 0.0f));
        world.SetParent(doomedChild, root);
        world.SetParent(grandchild, doomedChild);

        // Default destruction is subtree-destruction (the promote-children form is an explicit opt-in and covered in
        // the ECS suite), so both descendants go with the child.
        world.DestroyEntity(doomedChild);
        world.FlushPendingDestructions();

        World destination;
        WriteAndReload(world, destination);

        Size count = 0;
        destination.ForEachEntity([&](const Entity&) { ++count; });
        CHECK(count == 1u);
        CHECK(destination.IsAlive(destination.GetEntityByGuid(world.GetGuid(root))));
    }

    TEST_CASE("promoted children survive as roots and reload correctly")
    {
        // The other destruction mode: children are re-parented onto the grandparent rather than destroyed, so a saved
        // scene must contain them — still parented to what remains, not orphaned into nothing.
        World world;
        const Entity root = MakePlaceable(world, Vec3(1.0f, 0.0f, 0.0f));
        const Entity removedMiddle = MakePlaceable(world, Vec3(2.0f, 0.0f, 0.0f));
        const Entity leaf = MakePlaceable(world, Vec3(3.0f, 0.0f, 0.0f));
        world.SetParent(removedMiddle, root);
        world.SetParent(leaf, removedMiddle);

        world.DestroyEntity(removedMiddle, /*promoteChildrenToParent=*/true);
        world.FlushPendingDestructions();

        World destination;
        WriteAndReload(world, destination);

        const Entity loadedRoot = destination.GetEntityByGuid(world.GetGuid(root));
        const Entity loadedLeaf = destination.GetEntityByGuid(world.GetGuid(leaf));
        REQUIRE(destination.IsAlive(loadedRoot));
        REQUIRE(destination.IsAlive(loadedLeaf));

        // The leaf must reload attached to the root: a promotion that did not survive the save would silently flatten
        // the hierarchy every time a middle node was deleted.
        CHECK(destination.GetParent(loadedLeaf) == loadedRoot);
    }

    TEST_CASE("the report can be logged without crashing on empty or populated state")
    {
        SceneLoadReport empty;
        Hollowpoint::Test::ScopedLogCapture logs;
        empty.ReportToLog("level.hpscene");
        CHECK(logs.GetErrorCount() == 0);

        SceneLoadReport populated;
        populated.Errors.push_back({3, "bad thing"});
        populated.Warnings.push_back({4, "mild thing"});
        populated.ReportToLog("level.hpscene");
        CHECK(logs.GetErrorCount() >= 1u);
        CHECK(logs.GetWarningCount() >= 1u);
    }

    TEST_CASE("the factory creates a component on a live entity and refuses a dead one")
    {
        RegisterSceneTypes();

        World world;
        const Entity entity = world.CreateEntity();

        const Hollowpoint::Reflection::TypeDescriptor* type = nullptr;
        void* address = ComponentFactory::Get().CreateOrGet(world, entity, "LocalTransform", type);

        CHECK(address != nullptr);
        CHECK(type != nullptr);

        const Entity dead = world.CreateEntity();
        world.DestroyEntity(dead);
        world.FlushPendingDestructions();

        const Hollowpoint::Reflection::TypeDescriptor* deadType = nullptr;
        CHECK(ComponentFactory::Get().CreateOrGet(world, dead, "LocalTransform", deadType) == nullptr);
        CHECK(ComponentFactory::Get().CreateOrGet(world, entity, "NotRegistered", deadType) == nullptr);
    }

    TEST_CASE("re-registering a component name keeps the first factory")
    {
        // Two components claiming one scene-file name would make loads depend on registration order, so the second
        // registration must be rejected (and reported) rather than replacing the first.
        struct ConflictingTag
        {
            int Different = 0;
        };

        Registry::Get().RegisterType<ConflictingTag>("ConflictingTagForTest");
        ComponentFactory::Get().Register<TestGameplayTag>("AmbiguousName");

        Hollowpoint::Test::ScopedLogCapture logs;
        ComponentFactory::Get().Register<ConflictingTag>("AmbiguousName");

        CHECK(logs.GetWarningCount() >= 1u);
        CHECK(ComponentFactory::Get().IsRegistered("AmbiguousName"));
    }
    TEST_CASE("a world saved to a file reloads with the same entities and transforms")
    {
        ScopedScratchScene scratch("roundtrip");

        World source;
        MakePlaceable(source, Vec3(1.0f, 2.0f, 3.0f));
        MakePlaceable(source, Vec3(-4.0f, 0.5f, 0.0f));

        const SceneSaveFileResult saved = SaveSceneToFile(source, scratch.Path.string());
        INFO(saved.Message);
        REQUIRE(saved.Saved);
        REQUIRE(Hollowpoint::Core::FileExists(scratch.Path.string()));

        World destination;
        const SceneLoadReport loaded = LoadSceneFromFile(destination, scratch.Path.string());
        loaded.ReportToLog("roundtrip");
        REQUIRE(loaded.Loaded);
        CHECK_FALSE(loaded.HasErrors());
        REQUIRE(loaded.CreatedEntities.size() == 2u);

        // Spot-check that a transform came back through the file, not merely the entity count: a load creating two
        // entities with default transforms would satisfy the count while being completely wrong.
        int matched = 0;
        for (const Entity& entity : loaded.CreatedEntities)
        {
            const LocalTransform* transform = destination.TryGetComponent<LocalTransform>(entity);
            if (transform == nullptr)
            {
                continue;
            }
            if (Near(transform->Position, Vec3(1.0f, 2.0f, 3.0f)))
            {
                ++matched;
            }
            if (Near(transform->Position, Vec3(-4.0f, 0.5f, 0.0f)))
            {
                ++matched;
            }
        }
        CHECK(matched == 2);
    }

    TEST_CASE("file output is byte-identical to the text API")
    {
        // The two paths must produce the same bytes. Scenes are checked into version control and diffed, so a file
        // writer that reformatted (or silently used compact JSON) would show every scene as changed on save. This pins
        // the wrapper as a wrapper rather than a second serialiser with its own formatting opinions.
        ScopedScratchScene scratch("bytes");

        World world;
        MakePlaceable(world, Vec3(7.0f, 8.0f, 9.0f));

        const std::string expectedText = Hollowpoint::Serialization::WriteJson(WriteScene(world));
        REQUIRE(SaveSceneToFile(world, scratch.Path.string()).Saved);

        const Hollowpoint::Core::TextResult onDisk = Hollowpoint::Core::ReadFileText(scratch.Path.string());
        REQUIRE(onDisk.Succeeded);
        CHECK(onDisk.Value == expectedText);
    }

    TEST_CASE("saving creates missing parent directories")
    {
        ScopedScratchScene scratch("parents");
        const std::filesystem::path nested = scratch.Path.parent_path() / "levels" / "level01" / "scene.hpscene";

        World world;
        MakePlaceable(world, Vec3(0.0f));

        const SceneSaveFileResult saved = SaveSceneToFile(world, nested.string());
        INFO(saved.Message);
        REQUIRE(saved.Saved);
        CHECK(Hollowpoint::Core::FileExists(nested.string()));

        // And the scene at that new depth still loads — creating directories is worthless if the path is then unusable.
        World reloaded;
        CHECK(LoadSceneFromFile(reloaded, nested.string()).Loaded);
    }

    TEST_CASE("loading a missing file reports through Errors with the path, and leaves the world alone")
    {
        ScopedScratchScene scratch("missing");

        World world;
        const Entity existing = MakePlaceable(world, Vec3(1.0f));
        REQUIRE_FALSE(existing.IsNull());

        const std::filesystem::path absent = scratch.Path.parent_path() / "does_not_exist.hpscene";
        const SceneLoadReport report = LoadSceneFromFile(world, absent.string());
        CHECK_FALSE(report.Loaded);
        REQUIRE(report.HasErrors());
        // The path must appear in the message: an editor told "cannot read scene" across five open files cannot say
        // which one is unusable.
        CHECK(report.Errors[0].Message.find("does_not_exist.hpscene") != std::string::npos);

        // A failed load must not have cleared or corrupted what was already loaded — LoadSceneFromText only clears on a
        // document it accepts, and the read-failure path must honour the same rule.
        CHECK(world.IsAlive(existing));
    }

    TEST_CASE("loading a corrupt file reports a parse error naming a position")
    {
        ScopedScratchScene scratch("corrupt");
        REQUIRE(Hollowpoint::Core::WriteFileTruncate(scratch.Path.string(), "{ this is not json").Succeeded);

        World world;
        const SceneLoadReport report = LoadSceneFromFile(world, scratch.Path.string());
        CHECK_FALSE(report.Loaded);
        REQUIRE(report.HasErrors());
        // Syntax errors carry line/column from the parser. Distinguishing "unreadable" from "unparseable" is what lets
        // an editor offer to create a scene in the first case and show a caret in the second.
        CHECK(report.Errors[0].Message.find("line") != std::string::npos);
    }

    TEST_CASE("a failed save leaves the previous file intact and reports why")
    {
        ScopedScratchScene scratch("torn");

        World first;
        MakePlaceable(first, Vec3(1.0f, 1.0f, 1.0f));
        REQUIRE(SaveSceneToFile(first, scratch.Path.string()).Saved);
        const std::string originalBytes = Hollowpoint::Core::ReadFileText(scratch.Path.string()).Value;
        REQUIRE_FALSE(originalBytes.empty());

        // An empty path cannot be written to, and the contract is that the existing file survives untouched. A naive
        // truncate-then-write has already destroyed it by the time it notices the bad argument — which is exactly how a
        // user loses a level to a mistyped filename.
        {
            Hollowpoint::Test::ScopedLogCapture logs;
            const SceneSaveFileResult failed = SaveSceneToFile(first, "");
            CHECK_FALSE(failed.Saved);
            CHECK(failed.Error == Hollowpoint::Core::FileError::NotSupported);
        }

        CHECK(Hollowpoint::Core::ReadFileText(scratch.Path.string()).Value == originalBytes);

        // And it still loads afterwards: the failure was clean rather than partial.
        World reloaded;
        CHECK(LoadSceneFromFile(reloaded, scratch.Path.string()).Loaded);
    }

    TEST_CASE("a hierarchy survives a save and load through the file")
    {
        ScopedScratchScene scratch("hierarchy");

        World source;
        const Entity root = MakePlaceable(source, Vec3(10.0f, 0.0f, 0.0f));
        const Entity child = MakePlaceable(source, Vec3(0.0f, 5.0f, 0.0f));
        source.SetParent(child, root);

        REQUIRE(SaveSceneToFile(source, scratch.Path.string()).Saved);

        World destination;
        const SceneLoadReport loaded = LoadSceneFromFile(destination, scratch.Path.string());
        REQUIRE(loaded.Loaded);
        REQUIRE(loaded.CreatedEntities.size() == 2u);

        // Resolve by position rather than assuming slot order survives a file round-trip.
        auto findByPosition = [&](const Vec3& position) -> Entity {
            for (const Entity& entity : loaded.CreatedEntities)
            {
                const LocalTransform* transform = destination.TryGetComponent<LocalTransform>(entity);
                if (transform != nullptr && Near(transform->Position, position))
                {
                    return entity;
                }
            }
            return Entity();
        };

        const Entity loadedRoot = findByPosition(Vec3(10.0f, 0.0f, 0.0f));
        const Entity loadedChild = findByPosition(Vec3(0.0f, 5.0f, 0.0f));
        REQUIRE_FALSE(loadedRoot.IsNull());
        REQUIRE_FALSE(loadedChild.IsNull());

        // The parent LINK must be restored, not just both entities: without the two-phase parenting pass the child
        // loads as a second root and drifts out of place the moment either is moved in the editor.
        CHECK(destination.GetParent(loadedChild) == loadedRoot);
        CHECK(destination.GetParent(loadedRoot).IsNull());

        // Exactly one root reported, matching the document's structure.
        CHECK(loaded.RootEntities.size() == 1u);
    }
}
