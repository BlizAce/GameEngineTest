#include "TestSupport.h"

#include <cmath>
#include <set>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Core/Guid.h"
#include "Hollowpoint/ECS/World.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Math/Transform.h"
#include "Hollowpoint/Reflection/Registry.h"
#include "Hollowpoint/Scene/LocalTransform.h"
#include "Hollowpoint/Scene/Prefab.h"
#include "Hollowpoint/Scene/SceneIO.h"
#include "Hollowpoint/Scene/TransformHierarchy.h"
#include "Hollowpoint/Serialization/JsonParser.h"
#include "Hollowpoint/Serialization/JsonWriter.h"

using Hollowpoint::Core::Guid;
using Hollowpoint::Core::Size;
using Hollowpoint::ECS::Entity;
using Hollowpoint::ECS::World;
using Hollowpoint::Math::Quat;
using Hollowpoint::Math::Transform;
using Hollowpoint::Math::Vec3;
using namespace Hollowpoint::Scene;

namespace
{

constexpr float kEpsilon = 1.0e-4f;

bool Near(const Vec3& a, const Vec3& b, float epsilon = kEpsilon)
{
    return std::fabs(a.X - b.X) < epsilon && std::fabs(a.Y - b.Y) < epsilon && std::fabs(a.Z - b.Z) < epsilon;
}

// A crate with an inner mesh child: the shape of almost every real prefab (a root node plus content), so tests
// exercise multi-entity instantiation rather than only single entities.
const char* kCratePrefab = R"({
    "formatVersion": 1,
    "name": "Crate",
    "entities": [
        { "id": 0, "guid": "11111111-1111-4111-8111-111111111111", "components": {
            "LocalTransform": { "position": {"x":0,"y":0.5,"z":0},
                                "rotation": {"x":0,"y":0,"z":0,"w":1},
                                "scale": {"x":1,"y":1,"z":1} } } },
        { "id": 1, "parent": 0, "guid": "22222222-2222-4222-8222-222222222222", "components": {
            "LocalTransform": { "position": {"x":0,"y":1.5,"z":0},
                                "rotation": {"x":0,"y":0,"z":0,"w":1},
                                "scale": {"x":1,"y":1,"z":1} } } }
    ]
})";

Prefab LoadPrefabOrThrow(std::string_view text)
{
    std::string error;
    Prefab prefab = Prefab::FromSceneText(text, error);
    INFO("prefab load error: " << error);
    REQUIRE(prefab.IsLoaded());
    return prefab;
}

Vec3 WorldPosition(World& world, TransformHierarchy& transforms, Entity entity)
{
    transforms.Update(world);
    return transforms.GetWorldMatrix(world, entity).GetTranslation();
}

Entity FindChildWithOffset(const World& world, Entity parent)
{
    for (Entity child = world.GetFirstChild(parent); !child.IsNull(); child = world.GetNextSibling(child))
    {
        return child;
    }
    return Entity();
}

}  // namespace

TEST_SUITE("Scene/Prefab")
{
    TEST_CASE("a prefab parses from scene text and reports its shape")
    {
        const Prefab prefab = LoadPrefabOrThrow(kCratePrefab);

        CHECK(prefab.GetName() == "Crate");
        CHECK(prefab.GetEntityCount() == 2u);
        CHECK(prefab.GetRootCount() == 1u);
    }

    TEST_CASE("an invalid document yields an unloaded prefab and an error, without asserting")
    {
        // A missing or corrupt asset at runtime is a normal condition (content deleted after something referenced it),
        // so failure must be data — not a crash.
        std::string error;

        const Prefab badJson = Prefab::FromSceneText(R"({ "formatVersion": 1, )", error);
        CHECK_FALSE(badJson.IsLoaded());
        CHECK_FALSE(error.empty());
        // Position included: a prefab can be thousands of lines and "invalid JSON" is not actionable.
        CHECK(error.find("line") != std::string::npos);

        const Prefab notAnObject = Prefab::FromSceneText("[1,2,3]", error);
        CHECK_FALSE(notAnObject.IsLoaded());

        // Instantiating an unloaded prefab reports rather than crashing.
        World world;
        Hollowpoint::Test::ScopedLogCapture logs;
        const InstantiateResult result = InstantiatePrefab(world, Prefab{});
        CHECK_FALSE(result.Created);
        CHECK(result.Entities.empty());
    }

    TEST_CASE("instantiating creates the entity subtree")
    {
        const Prefab prefab = LoadPrefabOrThrow(kCratePrefab);

        World world;
        const InstantiateResult result = InstantiatePrefab(world, prefab);

        REQUIRE(result.Created);
        CHECK(result.Entities.size() == 2u);
        REQUIRE(result.Roots.size() == 1u);

        Size count = 0;
        world.ForEachEntity([&](const Entity&) { ++count; });
        CHECK(count == 2u);

        // Hierarchy preserved: the root has exactly one child and that child has no parent-of-its-own sibling.
        CHECK_FALSE(world.GetFirstChild(result.Roots[0]).IsNull());
        CHECK(world.GetParent(result.Roots[0]).IsNull());
    }

    TEST_CASE("instantiating twice creates independent entities with distinct identities")
    {
        // The core prefab guarantee: two spawns of one asset must not share a Guid, because every Guid-based reference
        // (script bindings, gameplay links) would then resolve to whichever copy the registry reached first.
        const Prefab prefab = LoadPrefabOrThrow(kCratePrefab);

        World world;
        const InstantiateResult first = InstantiatePrefab(world, prefab);
        const InstantiateResult second = InstantiatePrefab(world, prefab);

        REQUIRE(first.Created);
        REQUIRE(second.Created);

        Size count = 0;
        std::set<Guid> guids;
        world.ForEachEntity([&](const Entity& entity) {
            ++count;
            guids.insert(world.GetGuid(entity));
        });

        CHECK(count == 4u);
        CHECK(guids.size() == 4u);

        for (const Entity& entity : first.Entities)
        {
            CHECK(std::find(second.Entities.begin(), second.Entities.end(), entity) == second.Entities.end());
        }
    }

    TEST_CASE("instances do not reuse the asset's own identities")
    {
        // The failure mode the "distinct identities" case cannot see on its own: if fresh-identity assignment were
        // skipped, the *first* instance would simply keep the document's guids and every entity would still end up
        // unique (the loader's duplicate guard then rescues the second instance). Asserting directly against the
        // authored guids is what pins the behaviour down.
        const Prefab prefab = LoadPrefabOrThrow(kCratePrefab);

        const Guid authoredRoot = Guid::ParseOrNil("11111111-1111-4111-8111-111111111111");
        const Guid authoredChild = Guid::ParseOrNil("22222222-2222-4222-8222-222222222222");
        REQUIRE_FALSE(authoredRoot.IsNil());
        REQUIRE_FALSE(authoredChild.IsNil());

        World world;
        const InstantiateResult result = InstantiatePrefab(world, prefab);
        REQUIRE(result.Created);
        REQUIRE(result.Entities.size() == 2u);

        for (const Entity& entity : result.Entities)
        {
            const Guid identity = world.GetGuid(entity);
            CHECK(identity != authoredRoot);
            CHECK(identity != authoredChild);
            CHECK_FALSE(identity.IsNil());
        }

        // The asset's identities are not claimed by anything in the world, so a level entity can still legitimately
        // hold them (a placed instance is not the same thing as the template).
        CHECK(world.GetEntityByGuid(authoredRoot).IsNull());
    }

    TEST_CASE("the prefab document is never modified by instantiation")
    {
        // Prefabs are shared assets instantiated many times. If an instance could edit its template, the tenth spawn
        // would differ from the first — a bug that only appears after minutes of runtime and never in a smoke test.
        const Prefab prefab = LoadPrefabOrThrow(kCratePrefab);
        const std::string documentBefore = Hollowpoint::Serialization::WriteCompactJson(prefab.GetDocument());

        World world;
        InstantiateOptions options;
        options.Parent = world.CreateEntity();
        options.RootTransform = Transform(Vec3(50.0f, 0.0f, 0.0f));

        InstantiatePrefab(world, prefab, options);
        InstantiatePrefab(world, prefab, options);

        const std::string documentAfter = Hollowpoint::Serialization::WriteCompactJson(prefab.GetDocument());
        CHECK(documentAfter == documentBefore);
    }

    TEST_CASE("instantiating never clears the world")
    {
        const Prefab prefab = LoadPrefabOrThrow(kCratePrefab);

        World world;
        const Entity preexisting = world.CreateEntity();

        const InstantiateResult result = InstantiatePrefab(world, prefab);

        REQUIRE(result.Created);
        CHECK(world.IsAlive(preexisting));

        Size count = 0;
        world.ForEachEntity([&](const Entity&) { ++count; });
        CHECK(count == 3u);
    }

    TEST_CASE("roots attach to the requested parent")
    {
        const Prefab prefab = LoadPrefabOrThrow(kCratePrefab);

        World world;
        const Entity host = world.CreateEntity();

        InstantiateOptions options;
        options.Parent = host;
        const InstantiateResult result = InstantiatePrefab(world, prefab, options);

        REQUIRE(result.Created);
        REQUIRE(result.Roots.size() == 1u);
        CHECK(world.GetParent(result.Roots[0]) == host);
        CHECK(world.GetParent(host).IsNull());
    }

    TEST_CASE("the spawn offset is relative to the parent, not doubled by it")
    {
        // The regression this guards: folding the parent's transform in here *and* through the hierarchy double-counts
        // it, so a spawned object drifts further away the deeper it is nested — which reads as a transform-system bug.
        const Prefab prefab = LoadPrefabOrThrow(kCratePrefab);

        World world;
        const Entity parent = world.CreateEntity();
        LocalTransform parentLocal;
        parentLocal.Position = Vec3(100.0f, 0.0f, 0.0f);
        world.AddComponent<LocalTransform>(parent).SetFrom(parentLocal.ToTransform());

        InstantiateOptions options;
        options.Parent = parent;
        options.RootTransform = Transform(Vec3(5.0f, 0.0f, 0.0f));

        const InstantiateResult result = InstantiatePrefab(world, prefab, options);
        REQUIRE(result.Created);

        TransformHierarchy transforms;
        // Parent (100) + spawn offset (5) + the prefab's own authored root y (0.5). Exactly one application of each.
        const Vec3 worldPos = WorldPosition(world, transforms, result.Roots[0]);
        CHECK(Near(worldPos, Vec3(105.0f, 0.5f, 0.0f)));
    }

    TEST_CASE("the authored root offset is preserved rather than replaced")
    {
        // A crate whose mesh sits half a metre up must stay half a metre up after spawning, or every instance lands
        // half-buried. Replacing the local transform instead of composing would flatten it to exactly the spawn point.
        const Prefab prefab = LoadPrefabOrThrow(kCratePrefab);

        World world;
        InstantiateOptions options;
        options.RootTransform = Transform(Vec3(10.0f, 0.0f, 0.0f));
        const InstantiateResult result = InstantiatePrefab(world, prefab, options);

        REQUIRE(result.Created);

        TransformHierarchy transforms;
        const Vec3 worldPos = WorldPosition(world, transforms, result.Roots[0]);
        CHECK(Near(worldPos, Vec3(10.0f, 0.5f, 0.0f)));

        // And the child still hangs off the root at its authored offset.
        const Entity child = FindChildWithOffset(world, result.Roots[0]);
        REQUIRE_FALSE(child.IsNull());
        CHECK(Near(WorldPosition(world, transforms, child), Vec3(10.0f, 2.0f, 0.0f)));
    }

    TEST_CASE("a spawn rotation carries the whole instance")
    {
        // A child offset along +X must swing to -Z under a quarter turn about +Y. Asserting the resulting world
        // position (rather than "the distance is preserved") is what actually proves the rotation propagated through
        // the instance: distance is invariant under rotation, so it would pass with the rotation silently dropped.
        const char* text = R"({
            "formatVersion": 1,
            "entities": [
                { "id": 0, "components": { "LocalTransform": { "position": {"x":0,"y":0,"z":0},
                                                                "rotation": {"x":0,"y":0,"z":0,"w":1},
                                                                "scale": {"x":1,"y":1,"z":1} } } },
                { "id": 1, "parent": 0, "components": { "LocalTransform": { "position": {"x":1,"y":0,"z":0},
                                                                            "rotation": {"x":0,"y":0,"z":0,"w":1},
                                                                            "scale": {"x":1,"y":1,"z":1} } } }
            ]
        })";

        const Prefab prefab = LoadPrefabOrThrow(text);

        World world;
        InstantiateOptions options;
        Transform spawn;
        spawn.Position = Vec3(1.0f, 0.0f, 0.0f);
        spawn.Rotation = Quat::FromAxisAngle(Vec3(0.0f, 1.0f, 0.0f), 1.5707963267948966f).Normalized();
        options.RootTransform = spawn;

        const InstantiateResult result = InstantiatePrefab(world, prefab, options);
        REQUIRE(result.Created);

        TransformHierarchy transforms;
        const Vec3 rootPos = WorldPosition(world, transforms, result.Roots[0]);
        CHECK(Near(rootPos, Vec3(1.0f, 0.0f, 0.0f)));

        const Entity child = FindChildWithOffset(world, result.Roots[0]);
        REQUIRE_FALSE(child.IsNull());

        // Spawn translation (1,0,0) plus the rotated child offset: +X under a quarter turn about +Y becomes -Z.
        const Vec3 childPos = WorldPosition(world, transforms, child);
        CHECK(Near(childPos, Vec3(1.0f, 0.0f, -1.0f)));
    }

    TEST_CASE("a spawn offset on a prefab without a root LocalTransform still applies")
    {
        // An entity with no LocalTransform inherits its parent exactly, so applying an offset to such an entity has
        // nowhere to go unless the component is created — silently doing nothing would stack instances at the origin.
        const char* text = R"({ "formatVersion": 1, "entities": [ { "id": 0 } ] })";
        const Prefab prefab = LoadPrefabOrThrow(text);

        World world;
        InstantiateOptions options;
        options.RootTransform = Transform(Vec3(7.0f, 8.0f, 9.0f));
        const InstantiateResult result = InstantiatePrefab(world, prefab, options);

        REQUIRE(result.Created);
        REQUIRE(result.Roots.size() == 1u);

        const LocalTransform* transform = world.TryGetComponent<LocalTransform>(result.Roots[0]);
        REQUIRE(transform != nullptr);
        CHECK(Near(transform->Position, Vec3(7.0f, 8.0f, 9.0f)));
    }

    TEST_CASE("identities can be kept when the document's guids must survive")
    {
        // Opt-in mode for deterministic replays and golden-file tests; the default (fresh identities) is covered above.
        const Prefab prefab = LoadPrefabOrThrow(kCratePrefab);
        const Guid authoredRoot = Guid::ParseOrNil("11111111-1111-4111-8111-111111111111");
        REQUIRE_FALSE(authoredRoot.IsNil());

        World world;
        InstantiateOptions options;
        options.AssignFreshIdentities = false;
        const InstantiateResult result = InstantiatePrefab(world, prefab, options);

        REQUIRE(result.Created);
        CHECK(world.GetGuid(result.Roots[0]) == authoredRoot);
    }

    TEST_CASE("instantiating into a dead parent is refused")
    {
        // Otherwise the instance would be parented to whatever now occupies that recycled slot — an object appearing
        // attached to unrelated geometry, which is maddening to debug.
        const Prefab prefab = LoadPrefabOrThrow(kCratePrefab);

        World world;
        Entity dead = world.CreateEntity();
        world.DestroyEntity(dead);
        world.FlushPendingDestructions();

        InstantiateOptions options;
        options.Parent = dead;

        Hollowpoint::Test::ScopedLogCapture logs;
        const InstantiateResult result = InstantiatePrefab(world, prefab, options);

        CHECK_FALSE(result.Created);
        CHECK(result.Entities.empty());
        CHECK(logs.GetWarningCount() >= 1u);

        Size count = 0;
        world.ForEachEntity([&](const Entity&) { ++count; });
        CHECK(count == 0u);
    }

    TEST_CASE("multi-entity content keeps its internal hierarchy")
    {
        const Prefab prefab = LoadPrefabOrThrow(kCratePrefab);

        World world;
        const InstantiateResult result = InstantiatePrefab(world, prefab);
        REQUIRE(result.Created);

        const Entity root = result.Roots[0];
        const Entity child = FindChildWithOffset(world, root);

        REQUIRE_FALSE(child.IsNull());
        CHECK(world.GetParent(child) == root);
        CHECK(world.GetFirstChild(root) == child);
    }

    TEST_CASE("prefabs round-trip through a saved world")
    {
        // Spawn into a world, save it, reload: instances must be indistinguishable from authored entities. This is
        // what makes spawning at load time (or in tools) safe rather than a second-class path.
        const Prefab prefab = LoadPrefabOrThrow(kCratePrefab);

        InstantiateOptions options;
        options.RootTransform = Transform(Vec3(3.0f, 0.0f, 0.0f));

        World world;
        const InstantiateResult spawned = InstantiatePrefab(world, prefab, options);
        REQUIRE(spawned.Created);

        TransformHierarchy sourceTransforms;
        const Vec3 expectedRoot = WorldPosition(world, sourceTransforms, spawned.Roots[0]);

        const std::string saved = Hollowpoint::Serialization::WriteJson(WriteScene(world));

        World reloaded;
        REQUIRE(LoadSceneFromText(reloaded, saved).Loaded);

        Size count = 0;
        reloaded.ForEachEntity([&](const Entity&) { ++count; });
        CHECK(count == 2u);

        // The reloaded root lands where the instance was spawned, and still owns its child — so a saved scene made of
        // instances reloads as an ordinary hierarchy rather than needing special handling.
        Entity reloadedRoot;
        reloaded.ForEachEntity([&](const Entity& entity) {
            if (reloaded.GetParent(entity).IsNull())
            {
                reloadedRoot = entity;
            }
        });
        REQUIRE_FALSE(reloadedRoot.IsNull());

        TransformHierarchy reloadedTransforms;
        CHECK(Near(WorldPosition(reloaded, reloadedTransforms, reloadedRoot), expectedRoot));
        CHECK_FALSE(FindChildWithOffset(reloaded, reloadedRoot).IsNull());
    }

    TEST_CASE("many instances stay independent and correctly placed")
    {
        const Prefab prefab = LoadPrefabOrThrow(kCratePrefab);

        World world;
        std::vector<Entity> instanceRoots;

        for (int index = 0; index < 25; ++index)
        {
            InstantiateOptions options;
            options.RootTransform = Transform(Vec3(static_cast<float>(index) * 2.0f, 0.0f, 0.0f));
            const InstantiateResult result = InstantiatePrefab(world, prefab, options);
            REQUIRE(result.Created);
            REQUIRE(result.Roots.size() == 1u);
            instanceRoots.push_back(result.Roots[0]);
        }

        Size count = 0;
        world.ForEachEntity([&](const Entity&) { ++count; });
        CHECK(count == 50u);

        TransformHierarchy transforms;
        for (Size index = 0; index < instanceRoots.size(); ++index)
        {
            INFO("instance " << index);
            const Vec3 worldPos = WorldPosition(world, transforms, instanceRoots[index]);
            CHECK(Near(worldPos, Vec3(static_cast<float>(index) * 2.0f, 0.5f, 0.0f)));

            // Each instance still owns its own child; a shared-entity bug would show up as missing or aliased children.
            CHECK_FALSE(FindChildWithOffset(world, instanceRoots[index]).IsNull());
        }
    }

    TEST_CASE("warnings from the underlying load are surfaced to the spawner")
    {
        // A prefab referencing a component this build does not have must still spawn, but the author has to learn that
        // part of it was dropped.
        const char* text = R"({
            "formatVersion": 1,
            "entities": [ { "id": 0, "components": { "ComponentThatNoLongerExists": { "x": 1 } } } ]
        })";

        const Prefab prefab = LoadPrefabOrThrow(text);

        World world;
        Hollowpoint::Test::ScopedLogCapture logs;
        const InstantiateResult result = InstantiatePrefab(world, prefab);

        CHECK(result.Created);
        CHECK(result.HasWarnings());
        REQUIRE(!result.Warnings.empty());
        CHECK(result.Warnings[0].find("ComponentThatNoLongerExists") != std::string::npos);
    }

    TEST_CASE("a prefab built from an already-parsed document is independent of it")
    {
        // The prefab owns its template: mutating the caller's document afterwards must not change what gets spawned.
        Hollowpoint::Serialization::JsonError parseError;
        JsonValue document;
        REQUIRE(Hollowpoint::Serialization::TryParseJson(kCratePrefab, document, &parseError));

        std::string loadError;
        const Prefab prefab = Prefab::FromSceneDocument(document, loadError);
        REQUIRE(prefab.IsLoaded());

        document.AsObjectMutable().Remove("entities");

        World world;
        const InstantiateResult result = InstantiatePrefab(world, prefab);
        CHECK(result.Created);
        CHECK(result.Entities.size() == 2u);
    }

    TEST_CASE("a multi-root prefab reports and attaches all of its roots")
    {
        // Swarm-style prefabs legitimately contain several independent trees. The instance must expose every root so
        // the caller can attach or move them together rather than only the first.
        const char* text = R"({
            "formatVersion": 1,
            "entities": [ { "id": 0 }, { "id": 1 }, { "id": 2, "parent": 1 } ]
        })";

        const Prefab prefab = LoadPrefabOrThrow(text);
        CHECK(prefab.GetEntityCount() == 3u);
        CHECK(prefab.GetRootCount() == 2u);

        World world;
        const Entity host = world.CreateEntity();
        InstantiateOptions options;
        options.Parent = host;

        const InstantiateResult result = InstantiatePrefab(world, prefab, options);

        REQUIRE(result.Created);
        CHECK(result.Roots.size() == 2u);
        for (const Entity& root : result.Roots)
        {
            CHECK(world.GetParent(root) == host);
        }
    }
}
