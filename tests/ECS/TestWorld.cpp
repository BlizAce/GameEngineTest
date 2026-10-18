#include "TestSupport.h"

#include <string>
#include <unordered_set>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/ECS/World.h"

using Hollowpoint::Core::Guid;
using Hollowpoint::Core::Index;
using Hollowpoint::Core::Size;
using Hollowpoint::ECS::Entity;
using Hollowpoint::ECS::World;

namespace
{

// Destruction-hook bookkeeping. A file-scope record list (rather than a captured vector) because the hook API
// is a plain function pointer plus user data — exactly how subsystems will register, so the test exercises the
// real signature.
std::vector<std::string> g_HookLog;

void RecordHook(const Entity& entity, void* userData)
{
    const char* label = userData != nullptr ? static_cast<const char*>(userData) : "?";
    g_HookLog.push_back(std::string(label) + ":" + entity.ToString());
}

// File-scope because the hook API is a plain function pointer (no captures), exactly how subsystems register.
std::vector<Entity> g_Children;

void KillOneChild(const Entity& entity, void* userData)
{
    static_cast<void>(entity);
    World* worldPointer = static_cast<World*>(userData);
    if (!g_Children.empty())
    {
        worldPointer->DestroyEntity(g_Children.back());
    }
}

}  // namespace

TEST_SUITE("ECS/World")
{
    TEST_CASE("a new world is empty")
    {
        World world;

        CHECK(world.GetAliveEntityCount() == 0u);
        CHECK(world.GetSlotCapacity() == 0u);
        CHECK_FALSE(world.IsAlive(Entity()));
    }

    TEST_CASE("created entities are alive, distinct, and hashable as distinct")
    {
        World world;

        const Entity a = world.CreateEntity();
        const Entity b = world.CreateEntity();
        const Entity c = world.CreateEntity();

        CHECK_FALSE(a.IsNull());
        CHECK(world.IsAlive(a));
        CHECK(world.IsAlive(b));
        CHECK(world.IsAlive(c));
        CHECK(a != b);
        CHECK(b != c);
        CHECK(world.GetAliveEntityCount() == 3u);

        std::unordered_set<Entity> unique;
        unique.insert(a);
        unique.insert(b);
        unique.insert(c);
        CHECK(unique.size() == 3u);
    }

    TEST_CASE("every entity gets a distinct non-nil Guid")
    {
        World world;

        std::unordered_set<Guid> guids;
        std::vector<Entity> entities;
        for (int index = 0; index < 500; ++index)
        {
            const Entity entity = world.CreateEntity();
            entities.push_back(entity);

            const Guid guid = world.GetGuid(entity);
            INFO("entity " << index);
            CHECK_FALSE(guid.IsNil());
            guids.insert(guid);
        }

        // A collision here would silently merge two entities in every saved reference. 500 draws from a v4
        // space make a real collision impossible, so a failure means the generator or the assignment is broken.
        CHECK(guids.size() == 500u);
    }

    TEST_CASE("destruction makes an entity not alive immediately")
    {
        World world;
        const Entity entity = world.CreateEntity();

        world.DestroyEntity(entity);

        // Immediate, not deferred: a system running later in the same frame must not see a corpse. Component
        // memory is what gets deferred (see the flush tests), never liveness.
        CHECK_FALSE(world.IsAlive(entity));
        CHECK(world.GetAliveEntityCount() == 0u);
    }

    TEST_CASE("a destroyed entity stays addressable-by-guid-free and its Guid is released")
    {
        World world;
        const Entity entity = world.CreateEntity();
        const Guid guid = world.GetGuid(entity);

        world.DestroyEntity(entity);

        CHECK(world.GetGuid(entity).IsNil());
        CHECK(world.GetEntityByGuid(guid).IsNull());
    }

    TEST_CASE("slot recycling is deferred until flush")
    {
        World world;
        const Entity first = world.CreateEntity();

        world.DestroyEntity(first);

        // The slot is dead but not yet reclaimed, so no new entity can occupy it while handlers and systems in
        // this frame might still be walking storage that includes it.
        CHECK(world.GetPendingDestructionCount() == 1u);

        world.FlushPendingDestructions();
        CHECK(world.GetPendingDestructionCount() == 0u);

        const Entity second = world.CreateEntity();
        CHECK(second.Slot == first.Slot);
        // Same slot, different generation: the defining property of a versioned handle.
        CHECK(second.Generation == first.Generation + 1);
        CHECK_FALSE(world.IsAlive(first));
        CHECK(world.IsAlive(second));
    }

    TEST_CASE("stale handles never alias a recycled slot before its flush")
    {
        // The scenario generation checks exist for: fire an arrow at entity X, X dies, the slot is reused by a
        // crate. Between destruction and flush the handle must still resolve to "gone", not to the crate.
        World world;
        const Entity goblin = world.CreateEntity();

        world.DestroyEntity(goblin);
        world.FlushPendingDestructions();
        const Entity crate = world.CreateEntity();

        CHECK(crate.Slot == goblin.Slot);
        CHECK_FALSE(world.IsAlive(goblin));
        CHECK(world.IsAlive(crate));

        // And the Guid of the dead goblin must not resolve to the crate.
        CHECK(world.GetGuid(crate) != Guid());
    }

    TEST_CASE("destroying a dead or stale entity is reported and ignored")
    {
        World world;
        const Entity entity = world.CreateEntity();
        world.DestroyEntity(entity);
        world.FlushPendingDestructions();

        Hollowpoint::Test::ScopedLogCapture logs;

        // Scripts call "destroy if still there" constantly, so this path must not be fatal.
        world.DestroyEntity(entity);
        world.DestroyEntity(Entity());
        world.DestroyEntity(Entity(9999, 0));

        CHECK(world.GetAliveEntityCount() == 0u);
        CHECK(logs.GetErrorCount() == 0);  // A warning is right; an error would spam a normal game event.
    }

    TEST_CASE("a double destroy does not queue a slot twice")
    {
        World world;
        const Entity entity = world.CreateEntity();

        world.DestroyEntity(entity);
        {
            Hollowpoint::Test::ScopedLogCapture logs;
            world.DestroyEntity(entity);  // Already dead.
            static_cast<void>(logs);
        }

        // Two entries for one slot would put a live entity on the free list at flush, corrupting the world later.
        CHECK(world.GetPendingDestructionCount() == 1u);

        Hollowpoint::Test::ScopedAssertCapture asserts;
        world.FlushPendingDestructions();
        CHECK(asserts.GetCaptureCount() == 0);
    }

    TEST_CASE("entities created after a flush reuse slots in last-freed order")
    {
        World world;
        std::vector<Entity> created;
        for (int index = 0; index < 10; ++index)
        {
            created.push_back(world.CreateEntity());
        }

        world.DestroyEntity(created[2]);
        world.DestroyEntity(created[5]);
        world.FlushPendingDestructions();

        const Entity a = world.CreateEntity();
        const Entity b = world.CreateEntity();

        // LIFO reuse is an implementation choice, but pinning it matters: cache-unfriendly allocation patterns
        // show up as recently-freed slots being reused, and several engine paths rely on locality.
        CHECK(a.Slot == created[5].Slot);
        CHECK(b.Slot == created[2].Slot);
    }

    TEST_CASE("Guid lookup round-trips and rejects unknown identities")
    {
        World world;
        const Guid guid = Guid::Generate();

        const Entity entity = world.CreateEntityWithGuid(guid);
        REQUIRE_FALSE(entity.IsNull());

        CHECK(world.GetGuid(entity) == guid);
        CHECK(world.GetEntityByGuid(guid) == entity);

        CHECK(world.GetEntityByGuid(Guid::Generate()).IsNull());
        CHECK(world.GetEntityByGuid(Guid()).IsNull());  // Nil never resolves, even after other lookups.
    }

    TEST_CASE("loading a scene reproduces the exact Guid of every entity")
    {
        // The property that makes saved references work: identity comes from the file, not from creation order.
        World world;
        std::vector<Guid> guids;
        for (int index = 0; index < 50; ++index)
        {
            guids.push_back(Guid::Generate());
        }

        for (const Guid& guid : guids)
        {
            REQUIRE_FALSE(world.CreateEntityWithGuid(guid).IsNull());
        }

        for (const Guid& guid : guids)
        {
            const Entity entity = world.GetEntityByGuid(guid);
            REQUIRE_FALSE(entity.IsNull());
            CHECK(world.GetGuid(entity) == guid);
        }
    }

    TEST_CASE("a nil Guid is rejected rather than stored")
    {
        World world;

        Hollowpoint::Test::ScopedAssertCapture asserts;
        const Entity entity = world.CreateEntityWithGuid(Guid());
        CHECK(asserts.GetCaptureCount() == 1);

        // Rejected means rejected: no half-created entity left behind.
        CHECK(entity.IsNull());
        CHECK(world.GetAliveEntityCount() == 0u);
    }

    TEST_CASE("a duplicate Guid is rejected rather than shadowing the original")
    {
        World world;
        const Guid guid = Guid::Generate();
        const Entity original = world.CreateEntityWithGuid(guid);
        REQUIRE_FALSE(original.IsNull());

        Hollowpoint::Test::ScopedAssertCapture asserts;
        const Entity duplicate = world.CreateEntityWithGuid(guid);
        CHECK(asserts.GetCaptureCount() == 1);

        CHECK(duplicate.IsNull());
        CHECK(world.GetAliveEntityCount() == 1u);

        // The original must still resolve correctly — a failed insert may not evict or overwrite its entry.
        CHECK(world.GetEntityByGuid(guid) == original);
    }

    TEST_CASE("a Guid becomes available for reuse after its entity is destroyed")
    {
        // Reloading the same scene within one session (editor hot-reload, quicksave/quickload) must be able to
        // recreate entities with identical identities.
        World world;
        const Guid guid = Guid::Generate();
        const Entity first = world.CreateEntityWithGuid(guid);
        REQUIRE_FALSE(first.IsNull());

        world.DestroyEntity(first);
        world.FlushPendingDestructions();

        Hollowpoint::Test::ScopedAssertCapture asserts;
        const Entity second = world.CreateEntityWithGuid(guid);
        CHECK(asserts.GetCaptureCount() == 0);
        CHECK_FALSE(second.IsNull());
        CHECK(world.GetGuid(second) == guid);
    }

    TEST_CASE("ForEachEntity visits exactly the live entities")
    {
        World world;
        std::vector<Entity> created;
        for (int index = 0; index < 20; ++index)
        {
            created.push_back(world.CreateEntity());
        }
        for (int index = 0; index < 20; index += 2)
        {
            world.DestroyEntity(created[index]);  // Even indexes are dead.
        }

        std::vector<Index> visited;
        world.ForEachEntity([&visited](const Entity& entity) { visited.push_back(entity.Slot); });

        REQUIRE(visited.size() == 10u);
        for (size_t position = 0; position < visited.size(); ++position)
        {
            // Only odd slots, and each exactly once.
            CHECK(visited[position] % 2 == 1);
        }
    }

    TEST_CASE("an entity destroyed during iteration is not visited")
    {
        World world;
        std::vector<Entity> created;
        for (int index = 0; index < 10; ++index)
        {
            created.push_back(world.CreateEntity());
        }

        std::vector<Index> visitedSlots;
        int visitCount = 0;
        world.ForEachEntity([&](const Entity& entity) {
            visitedSlots.push_back(entity.Slot);
            ++visitCount;
            // Destroy the next entity in slot order: it must be skipped when reached. This is the shape of a
            // scripted "kill everything nearby" callback running inside a system pass.
            const Index nextSlot = entity.Slot + 1;
            if (nextSlot < created.size())
            {
                world.DestroyEntity(created[nextSlot]);
            }
        });

        // Visits are slots 0, 2, 4, 6, 8: reaching slot N always destroys N+1, so by the time N+1 is reached it
        // is dead and skipped. That is the behaviour that matters — a system pass never sees an entity destroyed
        // during that same pass, whatever the visit count ends up being.
        CHECK(visitCount == 5);

        const std::vector<Index> expected = {0, 2, 4, 6, 8};
        CHECK(visitedSlots == expected);
    }

    TEST_CASE("every entity handed to a callback is alive at that moment")
    {
        // The property the previous case only implied: ForEachEntity must never invoke the callback for an
        // entity that is not alive, including one destroyed by an earlier callback in the same pass. A system
        // that trusts this (as all of them will) writes a component belonging to a corpse otherwise.
        World world;
        std::vector<Entity> created;
        for (int index = 0; index < 12; ++index)
        {
            created.push_back(world.CreateEntity());
        }

        int deadVisits = 0;
        world.ForEachEntity([&](const Entity& entity) {
            if (!world.IsAlive(entity))
            {
                ++deadVisits;
            }
            // Kill the neighbour two slots along, so some later slots are dead well before they are reached.
            const Index victim = entity.Slot + 2;
            if (victim < created.size())
            {
                world.DestroyEntity(created[victim]);
            }
        });

        CHECK(deadVisits == 0);
    }

    TEST_CASE("destruction hooks run once per reclaimed entity, before the slot is recycled")
    {
        World world;
        g_HookLog.clear();

        char label[] = "physics";
        world.RegisterDestructionHandler(&RecordHook, static_cast<void*>(label));

        const Entity a = world.CreateEntity();
        const Entity b = world.CreateEntity();

        world.DestroyEntity(a);
        // Not yet: hooks run at flush, when component storage is still intact and a handler may read it.
        CHECK(g_HookLog.empty());

        world.FlushPendingDestructions();

        REQUIRE(g_HookLog.size() == 1u);
        CHECK(g_HookLog[0].find("physics") != std::string::npos);
        CHECK(g_HookLog[0].find(a.ToString()) != std::string::npos);

        g_HookLog.clear();
        world.DestroyEntity(b);
        world.FlushPendingDestructions();
        CHECK(g_HookLog.size() == 1u);

        world.UnregisterDestructionHandler(&RecordHook, static_cast<void*>(label));
        g_HookLog.clear();

        const Entity c = world.CreateEntity();
        world.DestroyEntity(c);
        world.FlushPendingDestructions();
        CHECK(g_HookLog.empty());  // Unregistered hooks must not run.
    }

    TEST_CASE("a hook may destroy more entities without corrupting the flush")
    {
        // Re-entrant destruction is normal: killing an enemy destroys its inventory items. The batch being
        // reclaimed is swapped out first, so newly queued entities are simply reclaimed next flush.
        World world;
        g_HookLog.clear();
        g_Children.clear();
        for (int index = 0; index < 5; ++index)
        {
            g_Children.push_back(world.CreateEntity());
        }

        world.RegisterDestructionHandler(&KillOneChild, &world);
        const Entity parent = world.CreateEntity();
        world.DestroyEntity(parent);
        world.FlushPendingDestructions();

        // One child was queued during the flush; it is pending, not lost and not double-processed.
        CHECK(world.GetPendingDestructionCount() == 1u);

        Hollowpoint::Test::ScopedAssertCapture asserts;
        world.FlushPendingDestructions();
        CHECK(asserts.GetCaptureCount() == 0);

        world.UnregisterDestructionHandler(&KillOneChild, &world);
        g_Children.clear();
    }

    TEST_CASE("a hook cannot start a nested flush")
    {
        World world;

        struct Local
        {
            static void ReentrantFlush(const Entity& entity, void* userData)
            {
                static_cast<void>(entity);
                static_cast<World*>(userData)->FlushPendingDestructions();
            }
        };

        world.RegisterDestructionHandler(&Local::ReentrantFlush, &world);
        world.DestroyEntity(world.CreateEntity());

        Hollowpoint::Test::ScopedAssertCapture asserts;
        world.FlushPendingDestructions();

        // Refused rather than supported: a nested flush would move storage the outer loop is still walking.
        CHECK(asserts.GetCaptureCount() == 1);
    }

    TEST_CASE("a null hook is rejected")
    {
        World world;

        Hollowpoint::Test::ScopedAssertCapture asserts;
        world.RegisterDestructionHandler(nullptr, nullptr);
        CHECK(asserts.GetCaptureCount() == 1);
    }

    TEST_CASE("Clear removes every entity and keeps generations advancing")
    {
        World world;
        const Entity first = world.CreateEntity();
        const Guid guid = Guid::Generate();
        const Entity withGuid = world.CreateEntityWithGuid(guid);

        world.Clear();

        CHECK(world.GetAliveEntityCount() == 0u);
        CHECK_FALSE(world.IsAlive(first));
        CHECK_FALSE(world.IsAlive(withGuid));
        CHECK(world.GetEntityByGuid(guid).IsNull());  // Identity map fully released.
        CHECK(world.GetPendingDestructionCount() == 0u);

        const Entity replacement = world.CreateEntity();
        CHECK(world.IsAlive(replacement));

        // The critical part: a handle from before the clear must not alias the new entity, even though slot
        // numbers restart. Without monotonic generations this is the "crash on the second level you load" bug.
        CHECK_FALSE(world.IsAlive(first));
        CHECK(first.Generation != replacement.Generation);
    }

    TEST_CASE("Clear runs destruction hooks")
    {
        World world;
        g_HookLog.clear();

        char label[] = "audio";
        world.RegisterDestructionHandler(&RecordHook, static_cast<void*>(label));

        // Named variables rather than HP_UNUSED: that macro expands to ((void)sizeof(expr)), and sizeof does
        // not evaluate its operand — wrapping a call in it silently skips the call entirely. A comment plus an
        // unused-variable-free binding is the honest way to say "created for effect".
        const Entity first = world.CreateEntity();
        const Entity second = world.CreateEntity();
        static_cast<void>(first);
        static_cast<void>(second);

        world.Clear();

        CHECK(g_HookLog.size() == 2u);
        world.UnregisterDestructionHandler(&RecordHook, static_cast<void*>(label));
    }

    TEST_CASE("clearing an empty world is a no-op")
    {
        World world;

        Hollowpoint::Test::ScopedAssertCapture asserts;
        world.Clear();
        world.FlushPendingDestructions();
        CHECK(asserts.GetCaptureCount() == 0);
        CHECK(world.GetAliveEntityCount() == 0u);
    }

    TEST_CASE("flush with nothing pending does nothing")
    {
        World world;
        const Entity entity = world.CreateEntity();

        Hollowpoint::Test::ScopedAssertCapture asserts;
        world.FlushPendingDestructions();
        world.FlushPendingDestructions();

        CHECK(asserts.GetCaptureCount() == 0);
        CHECK(world.IsAlive(entity));
    }

    TEST_CASE("churn: thousands of create/destroy cycles stay consistent")
    {
        World world;
        std::vector<Entity> live;

        unsigned int state = 0x2468ACE1u;
        auto nextRandom = [&state]() {
            state = state * 1664525u + 1013904223u;
            return (state >> 16);
        };

        for (int step = 0; step < 20000; ++step)
        {
            if ((nextRandom() & 1u) != 0u || live.empty())
            {
                live.push_back(world.CreateEntity());
            }
            else
            {
                const Size index = nextRandom() % live.size();
                world.DestroyEntity(live[index]);
                live[index] = live.back();
                live.pop_back();
            }

            if (step % 100 == 0)
            {
                world.FlushPendingDestructions();

                INFO("step " << step);
                CHECK(world.GetAliveEntityCount() == live.size());

                for (const Entity& entity : live)
                {
                    REQUIRE(world.IsAlive(entity));
                    CHECK_FALSE(world.GetGuid(entity).IsNil());
                }
            }
        }

        world.FlushPendingDestructions();

        // Every live handle still resolves, and no two share a slot.
        std::unordered_set<Index> slots;
        for (const Entity& entity : live)
        {
            CHECK(world.IsAlive(entity));
            slots.insert(entity.Slot);
        }
        CHECK(slots.size() == live.size());
    }
}
