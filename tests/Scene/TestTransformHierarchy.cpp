#include "TestSupport.h"

#include <cmath>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/ECS/World.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Math/Transform.h"
#include "Hollowpoint/Scene/LocalTransform.h"
#include "Hollowpoint/Scene/TransformHierarchy.h"

using Hollowpoint::Core::Size;
using Hollowpoint::Core::UInt64;
using Hollowpoint::ECS::Entity;
using Hollowpoint::ECS::Hierarchy;
using Hollowpoint::ECS::World;
using Hollowpoint::Math::Mat4;
using Hollowpoint::Math::Quat;
using Hollowpoint::Math::Transform;
using Hollowpoint::Math::Vec3;
using Hollowpoint::Scene::LocalTransform;
using Hollowpoint::Scene::TransformHierarchy;

namespace
{

constexpr float kEpsilon = 1.0e-5f;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kHalfPi = kPi * 0.5f;

bool Near(const Vec3& a, const Vec3& b, float epsilon = kEpsilon)
{
    return std::fabs(a.X - b.X) < epsilon && std::fabs(a.Y - b.Y) < epsilon && std::fabs(a.Z - b.Z) < epsilon;
}

bool Near(const Mat4& a, const Mat4& b, float epsilon = kEpsilon)
{
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            if (std::fabs(a[column][row] - b[column][row]) > epsilon)
            {
                return false;
            }
        }
    }
    return true;
}

Entity MakePlaceable(World& world, const Transform& local = Transform::Identity())
{
    const Entity entity = world.CreateEntity();
    world.AddComponent<LocalTransform>(entity).SetFrom(local);
    return entity;
}

//! Composes the expected world matrix by hand (walking parents) so tests never validate an implementation against
//! itself: the reference path is the definition, the system under test is the optimisation of it.
Mat4 ExpectedWorld(const World& world, const Entity& entity)
{
    Mat4 chain = Mat4::Identity();
    std::vector<Transform> locals;

    for (Entity cursor = entity; !cursor.IsNull();)
    {
        const LocalTransform* local = world.TryGetComponent<LocalTransform>(cursor);
        if (local != nullptr)
        {
            locals.push_back(local->ToTransform());
        }
        cursor = world.GetParent(cursor);
    }

    for (auto it = locals.rbegin(); it != locals.rend(); ++it)
    {
        chain = chain * it->ToMatrix();
    }

    return chain;
}

void CheckMatchesExpected(const World& world, const TransformHierarchy& hierarchy, const Entity& entity)
{
    INFO("entity slot " << entity.Slot);
    CHECK(Near(hierarchy.GetWorldMatrix(world, entity), ExpectedWorld(world, entity)));
}

}  // namespace

TEST_SUITE("Scene/TransformHierarchy")
{
    TEST_CASE("a root entity's world transform is its local transform")
    {
        World world;
        TransformHierarchy hierarchy;

        const Entity entity = MakePlaceable(world, Transform(Vec3(1.0f, 2.0f, 3.0f)));
        hierarchy.Update(world);

        CHECK(Near(hierarchy.GetWorldMatrix(world, entity).GetTranslation(), Vec3(1.0f, 2.0f, 3.0f)));
        CheckMatchesExpected(world, hierarchy, entity);
    }

    TEST_CASE("a child composes against its parent")
    {
        World world;
        TransformHierarchy hierarchy;

        const Entity parent = MakePlaceable(world, Transform(Vec3(10.0f, 0.0f, 0.0f)));
        const Entity child = MakePlaceable(world, Transform(Vec3(0.0f, 5.0f, 0.0f)));
        world.SetParent(child, parent);

        hierarchy.Update(world);

        CHECK(Near(hierarchy.GetWorldMatrix(world, child).GetTranslation(), Vec3(10.0f, 5.0f, 0.0f)));
    }

    TEST_CASE("a parent rotation carries the child around it")
    {
        // The classic nested-transform bug: a child offset must rotate with the parent, not stay axis-aligned.
        World world;
        TransformHierarchy hierarchy;

        const Entity pivot = MakePlaceable(world);
        const Entity child = MakePlaceable(world, Transform(Vec3(1.0f, 0.0f, 0.0f)));
        world.SetParent(child, pivot);

        // With no rotation the child sits at +X.
        hierarchy.Update(world);
        CHECK(Near(hierarchy.GetWorldMatrix(world, child).GetTranslation(), Vec3(1.0f, 0.0f, 0.0f)));

        // Rotate the pivot a quarter turn about +Y: the child must swing to -Z (right-handed convention).
        Transform rotated;
        rotated.Rotation = Quat::FromAxisAngle(Vec3(0.0f, 1.0f, 0.0f), kHalfPi).Normalized();
        world.GetComponent<LocalTransform>(pivot).SetFrom(rotated);

        hierarchy.Update(world);

        const Vec3 swung = hierarchy.GetWorldMatrix(world, child).GetTranslation();
        CHECK(Near(swung, Vec3(0.0f, 0.0f, -1.0f)));
        CheckMatchesExpected(world, hierarchy, child);
    }

    TEST_CASE("deep chains compose correctly at every level")
    {
        World world;
        TransformHierarchy hierarchy;

        std::vector<Entity> chain;
        for (int index = 0; index < 40; ++index)
        {
            const Entity entity = MakePlaceable(world, Transform(Vec3(1.0f, 0.0f, 0.0f)));
            if (!chain.empty())
            {
                REQUIRE(world.SetParent(entity, chain.back()) == Hierarchy::ReparentResult::Success);
            }
            chain.push_back(entity);
        }

        hierarchy.Update(world);

        for (Size level = 0; level < chain.size(); ++level)
        {
            INFO("level " << level);
            CHECK(Near(hierarchy.GetWorldMatrix(world, chain[level]).GetTranslation(),
                       Vec3(static_cast<float>(level) + 1.0f, 0.0f, 0.0f)));
            CheckMatchesExpected(world, hierarchy, chain[level]);
        }
    }

    TEST_CASE("scale multiplies down the chain")
    {
        World world;
        TransformHierarchy hierarchy;

        const Entity parent = MakePlaceable(world, Transform(Vec3(0.0f), Quat::Identity(), Vec3(2.0f)));
        const Entity child = MakePlaceable(world, Transform(Vec3(1.0f, 0.0f, 0.0f), Quat::Identity(), Vec3(3.0f)));
        world.SetParent(child, parent);

        hierarchy.Update(world);

        // A unit offset under 2x scale lands at 2, and the child's own 3x composes to 6.
        const Mat4 worldMatrix = hierarchy.GetWorldMatrix(world, child);
        CHECK(Near(worldMatrix.GetScale(), Vec3(6.0f, 6.0f, 6.0f)));
        CheckMatchesExpected(world, hierarchy, child);
    }

    TEST_CASE("evaluation order is parent-before-child regardless of creation or slot order")
    {
        // Children created *before* their parent (so a lower slot number) must still compose against an updated
        // parent. Composing in slot order would read the parent's stale matrix here — a one-frame lag that shows up
        // as jitter on nested objects and vanishes when things stop moving, which is why it needs a test.
        World world;
        TransformHierarchy hierarchy;

        std::vector<Entity> entities;
        for (int index = 0; index < 30; ++index)
        {
            entities.push_back(MakePlaceable(world, Transform(Vec3(1.0f, 0.0f, 0.0f))));
        }

        // Build the tree backwards: high slots become parents of low slots.
        for (int index = 0; index < 29; ++index)
        {
            REQUIRE(world.SetParent(entities[index], entities[index + 1]) == Hierarchy::ReparentResult::Success);
        }

        hierarchy.Update(world);

        // entities[29] is the root at x=1, entities[28] at x=2, ... entities[0] at x=30.
        for (int index = 0; index < 30; ++index)
        {
            INFO("slot " << index);
            CHECK(Near(hierarchy.GetWorldMatrix(world, entities[index]).GetTranslation(),
                       Vec3(static_cast<float>(30 - index), 0.0f, 0.0f)));
            CheckMatchesExpected(world, hierarchy, entities[index]);
        }
    }

    TEST_CASE("reparenting takes effect on the next update")
    {
        World world;
        TransformHierarchy hierarchy;

        const Entity parentA = MakePlaceable(world, Transform(Vec3(100.0f, 0.0f, 0.0f)));
        const Entity parentB = MakePlaceable(world, Transform(Vec3(0.0f, 200.0f, 0.0f)));
        const Entity child = MakePlaceable(world, Transform(Vec3(1.0f, 1.0f, 0.0f)));

        world.SetParent(child, parentA);
        hierarchy.Update(world);
        CHECK(Near(hierarchy.GetWorldMatrix(world, child).GetTranslation(), Vec3(101.0f, 1.0f, 0.0f)));

        world.SetParent(child, parentB);
        hierarchy.Update(world);
        CHECK(Near(hierarchy.GetWorldMatrix(world, child).GetTranslation(), Vec3(1.0f, 201.0f, 0.0f)));
    }

    TEST_CASE("moving a parent moves its whole subtree in one update")
    {
        World world;
        TransformHierarchy hierarchy;

        const Entity root = MakePlaceable(world);
        const Entity child = MakePlaceable(world, Transform(Vec3(1.0f, 0.0f, 0.0f)));
        const Entity grandchild = MakePlaceable(world, Transform(Vec3(1.0f, 0.0f, 0.0f)));
        world.SetParent(child, root);
        world.SetParent(grandchild, child);

        hierarchy.Update(world);
        CHECK(Near(hierarchy.GetWorldMatrix(world, grandchild).GetTranslation(), Vec3(2.0f, 0.0f, 0.0f)));

        Transform moved;
        moved.Position = Vec3(0.0f, 50.0f, 0.0f);
        world.GetComponent<LocalTransform>(root).SetFrom(moved);

        hierarchy.Update(world);
        CHECK(Near(hierarchy.GetWorldMatrix(world, child).GetTranslation(), Vec3(1.0f, 50.0f, 0.0f)));
        CHECK(Near(hierarchy.GetWorldMatrix(world, grandchild).GetTranslation(), Vec3(2.0f, 50.0f, 0.0f)));
    }

    TEST_CASE("entities without a LocalTransform pass their inherited transform to children")
    {
        // A logic-only group node must not reset its children to the world origin: users expect nesting under it to
        // be transparent, and silently breaking the chain is the kind of thing that costs an afternoon.
        World world;
        TransformHierarchy hierarchy;

        const Entity root = MakePlaceable(world, Transform(Vec3(10.0f, 0.0f, 0.0f)));
        const Entity group = world.CreateEntity();  // No LocalTransform.
        const Entity mesh = MakePlaceable(world, Transform(Vec3(0.0f, 3.0f, 0.0f)));

        world.SetParent(group, root);
        world.SetParent(mesh, group);

        hierarchy.Update(world);

        CHECK(Near(hierarchy.GetWorldMatrix(world, mesh).GetTranslation(), Vec3(10.0f, 3.0f, 0.0f)));
    }

    TEST_CASE("queries on entities without a transform return identity")
    {
        World world;
        TransformHierarchy hierarchy;

        const Entity plain = world.CreateEntity();
        hierarchy.Update(world);

        CHECK(Near(hierarchy.GetWorldMatrix(world, plain), Mat4::Identity()));
        CHECK(Near(hierarchy.GetWorldTransform(world, plain).Position, Vec3(0.0f)));
    }

    TEST_CASE("queries on dead or stale handles return identity without asserting")
    {
        World world;
        TransformHierarchy hierarchy;

        const Entity entity = MakePlaceable(world, Transform(Vec3(5.0f, 0.0f, 0.0f)));
        hierarchy.Update(world);

        const Entity stale = entity;
        world.DestroyEntity(entity);
        world.FlushPendingDestructions();

        Hollowpoint::Test::ScopedAssertCapture asserts;
        CHECK(Near(hierarchy.GetWorldMatrix(world, stale), Mat4::Identity()));
        CHECK(Near(hierarchy.GetWorldMatrix(world, Entity()), Mat4::Identity()));
        CHECK(asserts.GetCaptureCount() == 0);
    }

    TEST_CASE("stale caches are detected and answered correctly rather than wrongly")
    {
        World world;
        TransformHierarchy hierarchy;

        const Entity parent = MakePlaceable(world, Transform(Vec3(1.0f, 0.0f, 0.0f)));
        const Entity child = MakePlaceable(world, Transform(Vec3(1.0f, 0.0f, 0.0f)));
        world.SetParent(child, parent);

        CHECK(hierarchy.IsStale(world));  // Nothing evaluated yet.

        hierarchy.Update(world);
        CHECK_FALSE(hierarchy.IsStale(world));
        CHECK(hierarchy.GetCachedCount() > 0u);

        // Structural change makes the cache stale.
        const Entity other = MakePlaceable(world, Transform(Vec3(9.0f, 0.0f, 0.0f)));
        world.SetParent(child, other);
        CHECK(hierarchy.IsStale(world));

        // Reading while stale must still give the right answer (spawn → parent → query-in-one-frame is normal code),
        // not the pre-change matrix.
        CheckMatchesExpected(world, hierarchy, child);
        CHECK(Near(hierarchy.GetWorldMatrix(world, child).GetTranslation(), Vec3(10.0f, 0.0f, 0.0f)));

        hierarchy.Update(world);
        CHECK_FALSE(hierarchy.IsStale(world));
    }

    TEST_CASE("Invalidate forces a full rebuild and drops stale entries")
    {
        World world;
        TransformHierarchy hierarchy;

        const Entity entity = MakePlaceable(world, Transform(Vec3(7.0f, 0.0f, 0.0f)));
        hierarchy.Update(world);
        CHECK(Near(hierarchy.GetWorldMatrix(world, entity).GetTranslation(), Vec3(7.0f, 0.0f, 0.0f)));

        hierarchy.Invalidate();
        CHECK(hierarchy.IsStale(world));
        CHECK(hierarchy.GetCachedCount() == 0u);

        // Still correct afterwards (via the on-demand path), and after Update as well.
        CheckMatchesExpected(world, hierarchy, entity);
        hierarchy.Update(world);
        CheckMatchesExpected(world, hierarchy, entity);
    }

    TEST_CASE("a world cleared between levels leaves no inherited state in recycled slots")
    {
        World world;
        TransformHierarchy hierarchy;

        const Entity first = MakePlaceable(world, Transform(Vec3(123.0f, 0.0f, 0.0f)));
        hierarchy.Update(world);

        world.Clear();
        hierarchy.Invalidate();

        const Entity second = MakePlaceable(world, Transform(Vec3(1.0f, 0.0f, 0.0f)));
        CHECK(second.Slot == first.Slot);  // Slot reuse is the whole point of this test.

        hierarchy.Update(world);
        CHECK(Near(hierarchy.GetWorldMatrix(world, second).GetTranslation(), Vec3(1.0f, 0.0f, 0.0f)));
    }

    TEST_CASE("destroying a subtree leaves no reachable stale transforms")
    {
        World world;
        TransformHierarchy hierarchy;

        const Entity root = MakePlaceable(world, Transform(Vec3(5.0f, 0.0f, 0.0f)));
        const Entity child = MakePlaceable(world, Transform(Vec3(1.0f, 0.0f, 0.0f)));
        world.SetParent(child, root);
        const Entity survivor = MakePlaceable(world, Transform(Vec3(2.0f, 0.0f, 0.0f)));

        hierarchy.Update(world);
        world.DestroyEntity(root);
        world.FlushPendingDestructions();
        hierarchy.Update(world);

        CheckMatchesExpected(world, hierarchy, survivor);
        CHECK(Near(hierarchy.GetWorldMatrix(world, survivor).GetTranslation(), Vec3(2.0f, 0.0f, 0.0f)));
    }

    TEST_CASE("local/world point conversions round-trip")
    {
        World world;
        TransformHierarchy hierarchy;

        const Entity parent =
            MakePlaceable(world, Transform(Vec3(3.0f, 4.0f, 5.0f),
                                           Quat::FromAxisAngle(Vec3(0.0f, 1.0f, 0.0f), 0.7f).Normalized(), Vec3(2.0f)));
        const Entity child = MakePlaceable(world, Transform(Vec3(1.0f, 0.0f, 0.0f)));
        world.SetParent(child, parent);

        hierarchy.Update(world);

        const Vec3 localPoint(0.5f, 1.0f, -2.0f);
        const Vec3 worldPoint = hierarchy.LocalToWorldPoint(world, child, localPoint);
        const Vec3 backAgain = hierarchy.WorldToLocalPoint(world, child, worldPoint);

        CHECK(Near(backAgain, localPoint, 1.0e-4f));
    }

    TEST_CASE("world-to-local on a zero-scale entity reports rather than crashing")
    {
        // A hidden entity has scale 0 in real content, so this must degrade gracefully — asserting here would crash
        // on data the engine deliberately allows.
        World world;
        TransformHierarchy hierarchy;

        const Entity squashed = MakePlaceable(world, Transform(Vec3(1.0f), Quat::Identity(), Vec3(0.0f)));
        hierarchy.Update(world);

        Hollowpoint::Test::ScopedLogCapture logs;
        const Vec3 point(4.0f, 5.0f, 6.0f);
        const Vec3 result = hierarchy.WorldToLocalPoint(world, squashed, point);

        CHECK(Near(result, point));  // Unchanged: no meaningful inverse exists.
        CHECK(logs.GetWarningCount() >= 1);
    }

    TEST_CASE("a wide forest evaluates every branch")
    {
        World world;
        TransformHierarchy hierarchy;

        std::vector<Entity> roots;
        for (int branch = 0; branch < 8; ++branch)
        {
            const Entity root = MakePlaceable(world, Transform(Vec3(static_cast<float>(branch), 0.0f, 0.0f)));
            roots.push_back(root);

            Entity previous = root;
            for (int depth = 1; depth <= 5; ++depth)
            {
                const Entity child = MakePlaceable(world, Transform(Vec3(0.0f, 1.0f, 0.0f)));
                world.SetParent(child, previous);
                previous = child;
            }
        }

        hierarchy.Update(world);

        // Every entity in the forest must match the reference composition.
        Size checked = 0;
        world.ForEachEntity([&](const Entity& entity) {
            if (world.TryGetComponent<LocalTransform>(entity) != nullptr)
            {
                CheckMatchesExpected(world, hierarchy, entity);
                ++checked;
            }
        });
        CHECK(checked == roots.size() * 6u);

        // Bottom of branch 3 sits at y=5.
        Entity cursor = roots[3];
        for (int depth = 0; depth < 5; ++depth)
        {
            cursor = world.GetFirstChild(cursor);
        }
        CHECK(Near(hierarchy.GetWorldMatrix(world, cursor).GetTranslation(), Vec3(3.0f, 5.0f, 0.0f)));
    }

    TEST_CASE("repeated updates are idempotent")
    {
        World world;
        TransformHierarchy hierarchy;

        const Entity parent = MakePlaceable(world, Transform(Vec3(1.0f, 2.0f, 3.0f)));
        const Entity child = MakePlaceable(world, Transform(Vec3(4.0f, 5.0f, 6.0f)));
        world.SetParent(child, parent);

        hierarchy.Update(world);
        const Mat4 first = hierarchy.GetWorldMatrix(world, child);

        for (int iteration = 0; iteration < 5; ++iteration)
        {
            hierarchy.Update(world);
            CHECK(Near(hierarchy.GetWorldMatrix(world, child), first));
        }
    }

    TEST_CASE("a large scene evaluates every entity against the reference")
    {
        World world;
        TransformHierarchy hierarchy;

        std::vector<Entity> entities;
        for (int index = 0; index < 500; ++index)
        {
            const Entity entity = MakePlaceable(
                world, Transform(Vec3(static_cast<float>(index % 7), 0.0f, static_cast<float>(index % 5))));
            entities.push_back(entity);
        }

        // Attach each to an earlier entity so the tree is random-ish but shallow and deterministic.
        for (Size index = 1; index < entities.size(); ++index)
        {
            world.SetParent(entities[index], entities[(index * 7u + 3u) % index]);
        }

        hierarchy.Update(world);

        Size checked = 0;
        for (const Entity& entity : entities)
        {
            INFO("slot " << entity.Slot);
            CheckMatchesExpected(world, hierarchy, entity);
            ++checked;
        }
        CHECK(checked == entities.size());
    }

    TEST_CASE("a deep chain does not overflow the stack")
    {
        // The traversal and the compose pass are iterative; 20k levels is far beyond real content and well beyond
        // what a recursive implementation survives.
        World world;
        TransformHierarchy hierarchy;

        constexpr int chainLength = 20000;
        std::vector<Entity> chain;
        chain.reserve(chainLength);

        for (int index = 0; index < chainLength; ++index)
        {
            chain.push_back(MakePlaceable(world, Transform(Vec3(1.0f, 0.0f, 0.0f))));
        }
        for (int index = 1; index < chainLength; ++index)
        {
            REQUIRE(world.SetParent(chain[index], chain[index - 1]) == Hierarchy::ReparentResult::Success);
        }

        hierarchy.Update(world);

        // Scale accumulates, so compare translation only (float range keeps the tail finite but huge in scale).
        const float expectedX = static_cast<float>(chainLength);
        const float actualX = hierarchy.GetWorldMatrix(world, chain[chainLength - 1]).GetTranslation().X;
        CHECK(std::fabs(actualX - expectedX) < 1.0f);
    }

    TEST_CASE("structure version changes only on structural edits")
    {
        World world;
        TransformHierarchy hierarchy;

        const UInt64 before = world.GetStructureVersion();
        const Entity entity = MakePlaceable(world);
        CHECK(world.GetStructureVersion() != before);

        const UInt64 afterCreate = world.GetStructureVersion();
        world.SetParent(entity, Entity());  // Unchanged: already a root.
        CHECK(world.GetStructureVersion() == afterCreate);

        const Entity other = MakePlaceable(world);
        const UInt64 afterSecondCreate = world.GetStructureVersion();
        world.SetParent(other, entity);
        CHECK(world.GetStructureVersion() != afterSecondCreate);
    }
}
