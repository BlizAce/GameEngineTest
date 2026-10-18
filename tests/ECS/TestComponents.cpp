#include "TestSupport.h"

#include <string>
#include <type_traits>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/ECS/World.h"

using Hollowpoint::Core::Index;
using Hollowpoint::Core::Size;
using Hollowpoint::ECS::Entity;
using Hollowpoint::ECS::World;

namespace
{

// Components as game code would define them: plain data, default-constructible.
struct TransformComponent
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

struct HealthComponent
{
    int Current = 100;
    int Maximum = 100;

    HealthComponent() = default;
    HealthComponent(int current, int maximum) : Current(current), Maximum(maximum) {}
};

struct MeshComponent
{
    std::string Name;
};

// Counts live instances so a test can prove components are destroyed exactly once — the property that keeps
// ref-counted resources (textures, meshes, audio buffers) held by value from leaking or double-freeing.
int g_MeshLiveCount = 0;

struct RefCountedComponent
{
    std::string Resource;

    RefCountedComponent()
    {
        ++g_MeshLiveCount;
    }
    explicit RefCountedComponent(std::string resource) : Resource(std::move(resource))
    {
        ++g_MeshLiveCount;
    }
    RefCountedComponent(const RefCountedComponent& other) : Resource(other.Resource)
    {
        ++g_MeshLiveCount;
    }
    RefCountedComponent(RefCountedComponent&& other) noexcept : Resource(std::move(other.Resource))
    {
        ++g_MeshLiveCount;
    }
    RefCountedComponent& operator=(const RefCountedComponent&) = default;
    RefCountedComponent& operator=(RefCountedComponent&&) noexcept = default;
    ~RefCountedComponent()
    {
        --g_MeshLiveCount;
    }
};

}  // namespace

TEST_SUITE("ECS/Components")
{
    TEST_CASE("adding a component makes it retrievable")
    {
        World world;
        const Entity entity = world.CreateEntity();

        TransformComponent& transform = world.AddComponent<TransformComponent>(entity);
        transform.X = 1.5f;

        REQUIRE(world.HasComponent<TransformComponent>(entity));
        CHECK(world.GetComponent<TransformComponent>(entity).X == 1.5f);
        CHECK(world.TryGetComponent<TransformComponent>(entity) != nullptr);
    }

    TEST_CASE("components are constructed from constructor arguments")
    {
        World world;
        const Entity entity = world.CreateEntity();

        HealthComponent& health = world.AddComponent<HealthComponent>(entity, 30, 200);

        CHECK(health.Current == 30);
        CHECK(health.Maximum == 200);
    }

    TEST_CASE("adding an existing component returns it untouched instead of resetting it")
    {
        // Systems call Add "to make sure" a component exists. If that rebuilt the component, accumulated state
        // (health, timers, damage-over-time stacks) would silently reset mid-game — a corruption with no crash
        // and no obvious owner.
        World world;
        const Entity entity = world.CreateEntity();

        world.AddComponent<HealthComponent>(entity, 100, 100).Current = 17;

        HealthComponent& again = world.AddComponent<HealthComponent>(entity);
        CHECK(again.Current == 17);

        // Even with constructor arguments supplied: still no rebuild.
        HealthComponent& withArgs = world.AddComponent<HealthComponent>(entity, 999, 999);
        CHECK(withArgs.Current == 17);
    }

    TEST_CASE("an entity without a component reports so rather than asserting")
    {
        World world;
        const Entity entity = world.CreateEntity();

        CHECK_FALSE(world.HasComponent<TransformComponent>(entity));
        CHECK(world.TryGetComponent<TransformComponent>(entity) == nullptr);
    }

    TEST_CASE("GetComponent asserts for a missing component and stays safe")
    {
        World world;
        const Entity entity = world.CreateEntity();

        Hollowpoint::Test::ScopedAssertCapture capture;
        const TransformComponent& transform = world.GetComponent<TransformComponent>(entity);

        CHECK(capture.GetCaptureCount() == 1);
        // Continued execution must not have dereferenced null: the fallback is a valid default object.
        CHECK(transform.X == 0.0f);
    }

    TEST_CASE("removing a component detaches it")
    {
        World world;
        const Entity entity = world.CreateEntity();
        world.AddComponent<MeshComponent>(entity).Name = "dragon";

        CHECK(world.RemoveComponent<MeshComponent>(entity));
        CHECK_FALSE(world.HasComponent<MeshComponent>(entity));
        CHECK_FALSE(world.RemoveComponent<MeshComponent>(entity));  // Already gone.
    }

    TEST_CASE("components of different types coexist independently")
    {
        World world;
        const Entity entity = world.CreateEntity();

        world.AddComponent<TransformComponent>(entity).X = 5.0f;
        world.AddComponent<HealthComponent>(entity, 50, 50);
        world.AddComponent<MeshComponent>(entity).Name = "shield";

        CHECK(world.GetComponent<TransformComponent>(entity).X == 5.0f);
        CHECK(world.GetComponent<HealthComponent>(entity).Current == 50);
        CHECK(world.GetComponent<MeshComponent>(entity).Name == "shield");

        // Removing one leaves the others alone.
        world.RemoveComponent<HealthComponent>(entity);
        CHECK(world.HasComponent<TransformComponent>(entity));
        CHECK(world.HasComponent<MeshComponent>(entity));
        CHECK_FALSE(world.HasComponent<HealthComponent>(entity));
    }

    TEST_CASE("each entity has its own component instance")
    {
        World world;
        const Entity a = world.CreateEntity();
        const Entity b = world.CreateEntity();

        world.AddComponent<HealthComponent>(a, 10, 100);
        world.AddComponent<HealthComponent>(b, 90, 100);

        CHECK(world.GetComponent<HealthComponent>(a).Current == 10);
        CHECK(world.GetComponent<HealthComponent>(b).Current == 90);

        world.GetComponent<HealthComponent>(a).Current = 1;
        CHECK(world.GetComponent<HealthComponent>(b).Current == 90);  // No aliasing.
    }

    TEST_CASE("dead entities cannot reach components")
    {
        World world;
        const Entity entity = world.CreateEntity();
        world.AddComponent<MeshComponent>(entity).Name = "corpse";

        world.DestroyEntity(entity);

        // Liveness is checked before storage, which is what makes deferred physical reclamation safe: the memory
        // is still there but unreachable.
        CHECK(world.TryGetComponent<MeshComponent>(entity) == nullptr);
        CHECK_FALSE(world.HasComponent<MeshComponent>(entity));
    }

    TEST_CASE("adding a component to a dead entity is reported and does not corrupt the slot")
    {
        World world;
        const Entity entity = world.CreateEntity();
        world.DestroyEntity(entity);
        world.FlushPendingDestructions();

        Hollowpoint::Test::ScopedAssertCapture capture;
        world.AddComponent<MeshComponent>(entity).Name = "phantom";
        CHECK(capture.GetCaptureCount() == 1);

        // The recycled slot must start clean: a component attached to a dead handle may not be inherited by the
        // next entity that reuses the slot.
        const Entity replacement = world.CreateEntity();
        CHECK(replacement.Slot == entity.Slot);
        CHECK_FALSE(world.HasComponent<MeshComponent>(replacement));
    }

    TEST_CASE("TryAddComponent returns null for a dead entity without asserting")
    {
        World world;
        const Entity entity = world.CreateEntity();
        world.DestroyEntity(entity);

        // The non-asserting variant exists for load and script paths where "the target vanished" is normal.
        Hollowpoint::Test::ScopedAssertCapture capture;
        CHECK(world.TryAddComponent<MeshComponent>(entity) == nullptr);
        CHECK(capture.GetCaptureCount() == 0);
    }

    TEST_CASE("a recycled slot never inherits the previous entity's components")
    {
        // The classic ECS leak: forget to clear components on reclaim and the next entity in that slot starts
        // with someone else's mesh, health and scripts attached.
        World world;
        const Entity first = world.CreateEntity();
        world.AddComponent<MeshComponent>(first).Name = "goblin";
        world.AddComponent<HealthComponent>(first, 1, 1);

        world.DestroyEntity(first);
        world.FlushPendingDestructions();

        const Entity second = world.CreateEntity();
        CHECK(second.Slot == first.Slot);

        CHECK_FALSE(world.HasComponent<MeshComponent>(second));
        CHECK_FALSE(world.HasComponent<HealthComponent>(second));
    }

    TEST_CASE("components are destroyed exactly once when their entity dies")
    {
        g_MeshLiveCount = 0;

        {
            World world;
            const Entity a = world.CreateEntity();
            const Entity b = world.CreateEntity();

            world.AddComponent<RefCountedComponent>(a, "mesh:goblin");
            world.AddComponent<RefCountedComponent>(b, "mesh:crate");
            CHECK(g_MeshLiveCount == 2);

            world.DestroyEntity(a);
            world.FlushPendingDestructions();
            CHECK(g_MeshLiveCount == 1);

            world.Clear();
            CHECK(g_MeshLiveCount == 0);
        }
        CHECK(g_MeshLiveCount == 0);
    }

    TEST_CASE("removing a component destroys exactly one instance")
    {
        g_MeshLiveCount = 0;
        {
            World world;
            const Entity entity = world.CreateEntity();
            world.AddComponent<RefCountedComponent>(entity, "texture");
            CHECK(g_MeshLiveCount == 1);

            CHECK(world.RemoveComponent<RefCountedComponent>(entity));
            CHECK(g_MeshLiveCount == 0);

            // Removing again must not double-destroy.
            CHECK_FALSE(world.RemoveComponent<RefCountedComponent>(entity));
            CHECK(g_MeshLiveCount == 0);
        }
        CHECK(g_MeshLiveCount == 0);
    }

    TEST_CASE("ForEach visits every entity holding the component")
    {
        World world;
        std::vector<Entity> withHealth;
        for (int index = 0; index < 10; ++index)
        {
            const Entity entity = world.CreateEntity();
            if (index % 2 == 0)
            {
                world.AddComponent<HealthComponent>(entity, index, 100);
                withHealth.push_back(entity);
            }
            else
            {
                world.AddComponent<TransformComponent>(entity);  // A different type entirely.
            }
        }

        std::vector<Entity> visited;
        world.ForEach<HealthComponent>([&visited](const Entity& entity, HealthComponent& health) {
            visited.push_back(entity);
            ++health.Current;  // Mutating access must reach the stored component.
        });

        REQUIRE(visited.size() == 5u);
        for (const Entity& entity : visited)
        {
            CHECK(world.HasComponent<HealthComponent>(entity));
        }

        // The mutation reached storage rather than a copy. withHealth[i] was created with Current = 2*i (only
        // even loop indexes got a component), so after one increment each holds 2*i + 1.
        for (size_t index = 0; index < withHealth.size(); ++index)
        {
            CHECK(world.GetComponent<HealthComponent>(withHealth[index]).Current == static_cast<int>(index) * 2 + 1);
        }
    }

    TEST_CASE("ForEach over an unused component type is a no-op and allocates nothing")
    {
        World world;
        const Entity entity = world.CreateEntity();
        world.AddComponent<TransformComponent>(entity);

        const Size typesBefore = world.GetComponentTypeCount();

        int visits = 0;
        world.ForEach<MeshComponent>([&visits](const Entity&, MeshComponent&) { ++visits; });

        CHECK(visits == 0);
        // Creating storage on sight would grow this every time a system ran against an empty type.
        CHECK(world.GetComponentTypeCount() == typesBefore);
    }

    TEST_CASE("a const world view reads and iterates but cannot mutate")
    {
        // The compile-time half of this test is the point: through `const World&` a system can read and iterate
        // components but cannot attach, detach or modify them. That is what lets render-side code hold a const
        // reference to the world instead of needing a separate read-only facade type. Uncommenting any line
        // marked ILLEGAL must fail to compile — kept as comments because a test that does not compile cannot be
        // run, but it is checked by hand whenever this area changes.
        World world;
        const Entity entity = world.CreateEntity();
        world.AddComponent<HealthComponent>(entity, 5, 10);

        const World& constWorld = world;

        int visits = 0;
        constWorld.ForEach<HealthComponent>([&visits](const Entity&, const HealthComponent& health) {
            CHECK(health.Current == 5);
            ++visits;
        });
        CHECK(visits == 1);

        CHECK(constWorld.HasComponent<HealthComponent>(entity));
        CHECK(constWorld.GetComponent<HealthComponent>(entity).Current == 5);
        REQUIRE(constWorld.TryGetComponent<HealthComponent>(entity) != nullptr);

        // The const accessors hand back a const pointer, so systems cannot write through them.
        static_assert(
            std::is_same_v<decltype(constWorld.TryGetComponent<HealthComponent>(entity)), const HealthComponent*>,
            "Const world lookups must not yield a mutable component pointer");

        // ILLEGAL: constWorld.AddComponent<HealthComponent>(entity);
        // ILLEGAL: constWorld.RemoveComponent<HealthComponent>(entity);
        // ILLEGAL: constWorld.GetComponent<HealthComponent>(entity).Current = 0;
    }

    TEST_CASE("ForEach skips entities destroyed earlier in the same pass")
    {
        World world;
        std::vector<Entity> entities;
        for (int index = 0; index < 6; ++index)
        {
            const Entity entity = world.CreateEntity();
            world.AddComponent<HealthComponent>(entity, index, 100);
            entities.push_back(entity);
        }

        std::vector<Entity> visited;
        world.ForEach<HealthComponent>([&](const Entity& entity, HealthComponent&) {
            visited.push_back(entity);
            // Destroy the *next* entity in the list: it must never be handed to the callback. This is the shape
            // of a scripted "kill everything nearby" running inside a system pass.
            const size_t position =
                static_cast<size_t>(entity.Slot) < entities.size() ? static_cast<size_t>(entity.Slot) : 0;
            if (position + 1 < entities.size())
            {
                world.DestroyEntity(entities[position + 1]);
            }
        });

        for (const Entity& entity : visited)
        {
            INFO("visited slot " << entity.Slot);
            // A corpse must never reach a system: writing its components would touch another entity's memory
            // once the slot is recycled.
            CHECK(world.IsAlive(entity));
        }
    }

    TEST_CASE("components survive unrelated entities being destroyed")
    {
        World world;
        std::vector<Entity> entities;
        for (int index = 0; index < 20; ++index)
        {
            const Entity entity = world.CreateEntity();
            world.AddComponent<MeshComponent>(entity).Name = "mesh_" + std::to_string(index);
            entities.push_back(entity);
        }

        // Remove every third entity, then check the survivors kept their own data (swap-back moves elements, so
        // a bookkeeping error shows up as names attached to the wrong entity).
        for (size_t index = 0; index < entities.size(); index += 3)
        {
            world.DestroyEntity(entities[index]);
        }
        world.FlushPendingDestructions();

        for (size_t index = 0; index < entities.size(); ++index)
        {
            const MeshComponent* mesh = world.TryGetComponent<MeshComponent>(entities[index]);
            if (index % 3 == 0)
            {
                CHECK(mesh == nullptr);
            }
            else
            {
                REQUIRE(mesh != nullptr);
                CHECK(mesh->Name == "mesh_" + std::to_string(index));
            }
        }
    }

    TEST_CASE("generic access exposes component addresses for reflection-driven tooling")
    {
        // The serializer and inspector work through Reflection descriptors, so all they need from storage is a
        // byte address plus the type name — never a typed value that would require an unchecked cast.
        World world;
        const Entity entity = world.CreateEntity();
        TransformComponent* stored = &world.AddComponent<TransformComponent>(entity);
        stored->Y = 7.0f;

        Size visitedTypes = 0;
        bool foundTransform = false;
        world.ForEachComponentStorage([&](const Hollowpoint::ECS::IComponentStorage& storage) {
            ++visitedTypes;
            if (storage.GetElementSize() == sizeof(TransformComponent))
            {
                const void* address = storage.FindAddress(entity.Slot);
                if (address != nullptr)
                {
                    foundTransform = true;
                    // The generic address must be the very object the typed API returned.
                    CHECK(address == static_cast<const void*>(stored));
                    CHECK(static_cast<const TransformComponent*>(address)->Y == 7.0f);
                }
            }
        });

        CHECK(foundTransform);
        CHECK(visitedTypes >= 1u);
    }
}
