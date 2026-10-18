#include "TestSupport.h"

#include <algorithm>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/ECS/World.h"

using Hollowpoint::Core::Index;
using Hollowpoint::Core::Size;
using Hollowpoint::ECS::Entity;
using Hollowpoint::ECS::Hierarchy;
using Hollowpoint::ECS::World;

namespace
{

struct TagComponent
{
    int Id = 0;
};

// Collects the direct children of an entity as handles.
std::vector<Entity> Children(World& world, const Entity& entity)
{
    std::vector<Entity> children;
    world.ForEachChild(entity, [&children](const Entity& child) { children.push_back(child); });
    return children;
}

// Counts descendants by walking the tree — used to verify subtree destruction reached every node. Recursive and
// fine here because every call site uses a handful of levels; engine code walks with a worklist for exactly this
// reason.
Size CountDescendants(World& world, const Entity& entity)
{
    Size count = 0;
    world.ForEachChild(entity, [&world, &count](const Entity& child) {
        ++count;
        count += CountDescendants(world, child);
    });
    return count;
}

}  // namespace

TEST_SUITE("ECS/WorldHierarchy")
{
    TEST_CASE("entities start at the scene root")
    {
        World world;
        const Entity entity = world.CreateEntity();

        CHECK_FALSE(world.HasParent(entity));
        CHECK(world.GetParent(entity).IsNull());
        CHECK(world.GetDepth(entity) == 0u);
        CHECK(world.GetChildCount(entity) == 0u);
    }

    TEST_CASE("reparenting links both directions")
    {
        World world;
        const Entity parent = world.CreateEntity();
        const Entity child = world.CreateEntity();

        CHECK(world.SetParent(child, parent) == Hierarchy::ReparentResult::Success);

        CHECK(world.HasParent(child));
        CHECK(world.GetParent(child) == parent);
        CHECK(world.GetChildCount(parent) == 1u);

        const std::vector<Entity> children = Children(world, parent);
        REQUIRE(children.size() == 1u);
        CHECK(children[0] == child);
    }

    TEST_CASE("handles returned by tree queries carry the right generation")
    {
        // A handle rebuilt from a slot without its generation would fail IsAlive and make every tree query look
        // like it returned nothing — so this checks the plumbing, not just the topology.
        World world;
        const Entity parent = world.CreateEntity();
        const Entity child = world.CreateEntity();
        world.SetParent(child, parent);

        const Entity fetchedParent = world.GetParent(child);
        CHECK(fetchedParent == parent);
        CHECK(world.IsAlive(fetchedParent));

        const Entity fetchedChild = world.GetFirstChild(parent);
        CHECK(fetchedChild == child);
        CHECK(world.IsAlive(fetchedChild));
    }

    TEST_CASE("child order is insertion order and stable")
    {
        World world;
        const Entity root = world.CreateEntity();

        std::vector<Entity> created;
        for (int index = 0; index < 8; ++index)
        {
            const Entity child = world.CreateEntity();
            world.SetParent(child, root);
            created.push_back(child);
        }

        CHECK(Children(world, root) == created);
        // Reading it twice must give the same order (scene saving relies on this).
        CHECK(Children(world, root) == created);
    }

    TEST_CASE("reparenting moves a child between parents")
    {
        World world;
        const Entity parentA = world.CreateEntity();
        const Entity parentB = world.CreateEntity();
        const Entity child = world.CreateEntity();

        world.SetParent(child, parentA);
        REQUIRE(world.GetParent(child) == parentA);

        CHECK(world.SetParent(child, parentB) == Hierarchy::ReparentResult::Success);
        CHECK(world.GetParent(child) == parentB);
        CHECK(world.GetChildCount(parentA) == 0u);
        CHECK(world.GetChildCount(parentB) == 1u);
    }

    TEST_CASE("detaching to root clears the parent")
    {
        World world;
        const Entity parent = world.CreateEntity();
        const Entity child = world.CreateEntity();
        world.SetParent(child, parent);

        CHECK(world.DetachToRoot(child) == Hierarchy::ReparentResult::Success);
        CHECK_FALSE(world.HasParent(child));
        CHECK(world.GetChildCount(parent) == 0u);
    }

    TEST_CASE("reparenting to the same parent reports Unchanged")
    {
        World world;
        const Entity parent = world.CreateEntity();
        const Entity child = world.CreateEntity();
        world.SetParent(child, parent);

        Hollowpoint::Test::ScopedAssertCapture asserts;
        CHECK(world.SetParent(child, parent) == Hierarchy::ReparentResult::Unchanged);
        CHECK(asserts.GetCaptureCount() == 0);
        CHECK(world.GetChildCount(parent) == 1u);  // Not duplicated.
    }

    TEST_CASE("a cycle is refused and the tree is unchanged")
    {
        World world;
        const Entity root = world.CreateEntity();
        const Entity middle = world.CreateEntity();
        const Entity leaf = world.CreateEntity();

        world.SetParent(middle, root);
        world.SetParent(leaf, middle);

        Hollowpoint::Test::ScopedLogCapture logs;

        CHECK(world.SetParent(root, leaf) == Hierarchy::ReparentResult::CycleWouldForm);
        CHECK(world.SetParent(root, root) == Hierarchy::ReparentResult::CycleWouldForm);
        CHECK(world.SetParent(middle, leaf) == Hierarchy::ReparentResult::CycleWouldForm);

        // Nothing moved.
        CHECK(world.GetParent(root).IsNull());
        CHECK(world.GetParent(middle) == root);
        CHECK(world.GetParent(leaf) == middle);
        CHECK(logs.GetWarningCount() >= 3);
    }

    TEST_CASE("IsDescendantOf works through entity handles")
    {
        World world;
        const Entity root = world.CreateEntity();
        const Entity middle = world.CreateEntity();
        const Entity leaf = world.CreateEntity();
        const Entity unrelated = world.CreateEntity();

        world.SetParent(middle, root);
        world.SetParent(leaf, middle);

        CHECK(world.IsDescendantOf(leaf, root));
        CHECK(world.IsDescendantOf(leaf, middle));
        CHECK_FALSE(world.IsDescendantOf(root, leaf));
        CHECK_FALSE(world.IsDescendantOf(unrelated, root));
        CHECK_FALSE(world.IsDescendantOf(leaf, leaf));
    }

    TEST_CASE("destroying an entity destroys its whole subtree")
    {
        // The default policy, matching every engine convention. Anything else leaks unreachable entities that an
        // editor can neither see nor delete.
        World world;
        const Entity root = world.CreateEntity();
        const Entity child = world.CreateEntity();
        const Entity grandchild = world.CreateEntity();
        const Entity greatGrandchild = world.CreateEntity();
        const Entity unrelated = world.CreateEntity();

        world.SetParent(child, root);
        world.SetParent(grandchild, child);
        world.SetParent(greatGrandchild, grandchild);

        world.DestroyEntity(root);

        CHECK_FALSE(world.IsAlive(root));
        CHECK_FALSE(world.IsAlive(child));
        CHECK_FALSE(world.IsAlive(grandchild));
        CHECK_FALSE(world.IsAlive(greatGrandchild));
        CHECK(world.IsAlive(unrelated));  // Sibling subtrees are untouched.
    }

    TEST_CASE("promoting children re-parents them to the grandparent")
    {
        World world;
        const Entity root = world.CreateEntity();
        const Entity middle = world.CreateEntity();
        const Entity leaf = world.CreateEntity();

        world.SetParent(middle, root);
        world.SetParent(leaf, middle);

        world.DestroyEntity(middle, /*promoteChildrenToParent=*/true);

        CHECK_FALSE(world.IsAlive(middle));
        CHECK(world.IsAlive(leaf));
        CHECK(world.GetParent(leaf) == root);
        CHECK(world.GetChildCount(root) == 1u);
    }

    TEST_CASE("promoting children of a root-level entity detaches them")
    {
        World world;
        const Entity parent = world.CreateEntity();
        const Entity child = world.CreateEntity();
        world.SetParent(child, parent);

        world.DestroyEntity(parent, /*promoteChildrenToParent=*/true);

        CHECK(world.IsAlive(child));
        CHECK_FALSE(world.HasParent(child));
    }

    TEST_CASE("destroying a subtree leaves no dangling child links")
    {
        World world;
        const Entity root = world.CreateEntity();
        std::vector<Entity> children;
        for (int index = 0; index < 5; ++index)
        {
            const Entity child = world.CreateEntity();
            world.SetParent(child, root);
            children.push_back(child);
        }

        world.DestroyEntity(root);
        world.FlushPendingDestructions();

        // The recycled slots must be clean: a leftover link would silently attach the next entity created in that
        // slot to somebody else's tree.
        const Entity replacement = world.CreateEntity();
        CHECK_FALSE(world.HasParent(replacement));
        CHECK(world.GetChildCount(replacement) == 0u);

        // And no live entity reports a dead parent.
        world.ForEachEntity([&world](const Entity& entity) {
            if (world.HasParent(entity))
            {
                CHECK(world.IsAlive(world.GetParent(entity)));
            }
        });
    }

    TEST_CASE("destroying a deep subtree does not overflow the stack")
    {
        // Recursion here would cost stack proportional to depth. 20k levels is far beyond any real hierarchy and
        // well beyond what a recursive teardown could survive, so this test fails loudly if the walk ever becomes
        // recursive again.
        World world;
        constexpr int chainLength = 20000;

        std::vector<Entity> chain;
        chain.reserve(chainLength);
        for (int index = 0; index < chainLength; ++index)
        {
            chain.push_back(world.CreateEntity());
        }
        for (int index = 1; index < chainLength; ++index)
        {
            REQUIRE(world.SetParent(chain[index], chain[index - 1]) == Hierarchy::ReparentResult::Success);
        }

        world.DestroyEntity(chain[0]);

        for (const Entity& entity : chain)
        {
            CHECK_FALSE(world.IsAlive(entity));
        }
    }

    TEST_CASE("destroying members of an already-destroyed subtree adds no queue entries")
    {
        // A duplicate entry in the pending-destruction queue would later push a live slot onto the free list and
        // hand out two handles for one entity. Destroying a subtree then re-destroying its members (a script that
        // walks a saved child list after killing the parent) must therefore be a no-op per entity.
        World world;
        const Entity parent = world.CreateEntity();
        const Entity child = world.CreateEntity();
        world.SetParent(child, parent);

        world.DestroyEntity(parent);
        const Size pendingAfterFirst = world.GetPendingDestructionCount();

        // Destroying again is a no-op (already dead) and must not add queue entries.
        Hollowpoint::Test::ScopedLogCapture logs;
        world.DestroyEntity(parent);
        world.DestroyEntity(child);

        CHECK(world.GetPendingDestructionCount() == pendingAfterFirst);
    }

    TEST_CASE("tree queries on dead or stale handles return empty rather than asserting")
    {
        World world;
        const Entity parent = world.CreateEntity();
        const Entity child = world.CreateEntity();
        world.SetParent(child, parent);

        const Entity stale = child;
        world.DestroyEntity(child);
        world.FlushPendingDestructions();

        Hollowpoint::Test::ScopedAssertCapture asserts;

        CHECK_FALSE(world.HasParent(stale));
        CHECK(world.GetParent(stale).IsNull());
        CHECK(world.GetFirstChild(stale).IsNull());
        CHECK(world.GetNextSibling(stale).IsNull());
        CHECK(world.GetChildCount(stale) == 0u);
        CHECK(world.GetDepth(stale) == 0u);
        CHECK_FALSE(world.IsDescendantOf(stale, parent));

        CHECK(asserts.GetCaptureCount() == 0);
    }

    TEST_CASE("reparenting with dead handles is refused")
    {
        World world;
        const Entity entity = world.CreateEntity();
        const Entity doomed = world.CreateEntity();
        world.DestroyEntity(doomed);
        world.FlushPendingDestructions();

        CHECK(world.SetParent(entity, doomed) == Hierarchy::ReparentResult::EntityNotAlive);
        CHECK(world.SetParent(doomed, entity) == Hierarchy::ReparentResult::EntityNotAlive);
        CHECK_FALSE(world.HasParent(entity));
    }

    TEST_CASE("Clear wipes the hierarchy along with the entities")
    {
        World world;
        const Entity root = world.CreateEntity();
        const Entity child = world.CreateEntity();
        world.SetParent(child, root);

        world.Clear();

        CHECK(world.GetAliveEntityCount() == 0u);

        // A recycled slot must not inherit a parent.
        const Entity replacement = world.CreateEntity();
        CHECK_FALSE(world.HasParent(replacement));
        CHECK(world.GetChildCount(replacement) == 0u);
    }

    TEST_CASE("ForEachChild visits every child exactly once")
    {
        World world;
        const Entity root = world.CreateEntity();
        std::vector<Entity> created;
        for (int index = 0; index < 12; ++index)
        {
            const Entity child = world.CreateEntity();
            world.SetParent(child, root);
            created.push_back(child);
        }

        std::vector<Entity> visited = Children(world, root);
        CHECK(visited == created);

        // No children at all for a leaf.
        CHECK(Children(world, created[0]).empty());
    }

    TEST_CASE("a large tree keeps every invariant under random reparenting")
    {
        World world;
        std::vector<Entity> entities;
        for (int index = 0; index < 48; ++index)
        {
            entities.push_back(world.CreateEntity());
        }

        unsigned int state = 0xCAFEBABEu;
        auto nextRandom = [&state]() {
            state = state * 1664525u + 1013904223u;
            return (state >> 16);
        };

        for (int step = 0; step < 5000; ++step)
        {
            const Entity& subject = entities[nextRandom() % entities.size()];
            const Index parentChoice = static_cast<Index>(nextRandom() % (entities.size() + 1));

            world.SetParent(subject, parentChoice == entities.size() ? Entity() : entities[parentChoice]);

            if (step % 100 == 0)
            {
                INFO("step " << step);

                // No entity may have a dead parent, and every reported parent must list the child back.
                world.ForEachEntity([&world](const Entity& entity) {
                    if (!world.HasParent(entity))
                    {
                        return;
                    }
                    const Entity parent = world.GetParent(entity);
                    REQUIRE(world.IsAlive(parent));

                    bool found = false;
                    world.ForEachChild(parent, [&found, &entity](const Entity& child) {
                        if (child == entity)
                        {
                            found = true;
                        }
                    });
                    CHECK(found);
                });
            }
        }

        // Every node must still be walkable to its root within the corruption bound: a cycle would have been
        // refused at SetParent time, and this checks that no sequence of legal moves created one anyway.
        world.ForEachEntity([&world](const Entity& entity) {
            const Hollowpoint::Core::UInt32 depth = world.GetDepth(entity);
            CHECK(depth < Hierarchy::MaxWalkSteps);
        });
    }

    TEST_CASE("destroying a parent with mixed children destroys the whole forest beneath it")
    {
        World world;
        const Entity root = world.CreateEntity();

        // Three levels under each of three children.
        std::vector<Entity> all;
        for (int branch = 0; branch < 3; ++branch)
        {
            const Entity child = world.CreateEntity();
            world.SetParent(child, root);
            all.push_back(child);

            Entity previous = child;
            for (int level = 0; level < 3; ++level)
            {
                const Entity next = world.CreateEntity();
                world.SetParent(next, previous);
                all.push_back(next);
                previous = next;
            }
        }

        // One unrelated tree that must survive.
        const Entity survivorRoot = world.CreateEntity();
        const Entity survivorChild = world.CreateEntity();
        world.SetParent(survivorChild, survivorRoot);

        REQUIRE(CountDescendants(world, root) == 12u);

        world.DestroyEntity(root);

        for (const Entity& entity : all)
        {
            CHECK_FALSE(world.IsAlive(entity));
        }
        CHECK(world.IsAlive(survivorRoot));
        CHECK(world.IsAlive(survivorChild));
        CHECK(CountDescendants(world, survivorRoot) == 1u);
    }

    TEST_CASE("components of a destroyed subtree are released")
    {
        World world;
        const Entity parent = world.CreateEntity();
        const Entity child = world.CreateEntity();
        world.SetParent(child, parent);
        world.AddComponent<TagComponent>(parent).Id = 1;
        world.AddComponent<TagComponent>(child).Id = 2;

        world.DestroyEntity(parent);
        world.FlushPendingDestructions();

        // Recycled slots must not inherit components from either the parent or the destroyed child.
        const Entity replacement = world.CreateEntity();
        CHECK_FALSE(world.HasComponent<TagComponent>(replacement));
    }
}
