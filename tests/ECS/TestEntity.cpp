#include <string>
#include <unordered_set>

#include <doctest/doctest.h>

#include "Hollowpoint/ECS/Entity.h"

using Hollowpoint::ECS::Entity;

TEST_SUITE("ECS/Entity")
{
    TEST_CASE("a default-constructed entity is the null handle")
    {
        const Entity entity;

        CHECK(entity.IsNull());
        CHECK(entity.Slot == Entity::NullSlot);
        CHECK(entity.Generation == 0);
    }

    TEST_CASE("handles compare by slot and generation together")
    {
        const Entity a(3, 1);
        const Entity sameSlotSameGeneration(3, 1);
        const Entity sameSlotOtherGeneration(3, 2);
        const Entity otherSlot(4, 1);

        CHECK(a == sameSlotSameGeneration);
        CHECK_FALSE(a == sameSlotOtherGeneration);
        CHECK_FALSE(a == otherSlot);
        CHECK(a != sameSlotOtherGeneration);

        // A recycled slot produces a handle equal in slot but distinct as an identity — this is the property
        // that makes stale handles detectable rather than merely suspicious.
        CHECK(a.Slot == sameSlotOtherGeneration.Slot);
    }

    TEST_CASE("the null handle is distinct from every real handle")
    {
        const Entity nullEntity;
        const Entity slotZero(Entity::NullSlot - 1, 0);

        CHECK_FALSE(nullEntity == slotZero);
        CHECK_FALSE(slotZero.IsNull());
    }

    TEST_CASE("entities hash consistently with equality")
    {
        std::unordered_set<Entity> seen;

        seen.insert(Entity(1, 0));
        seen.insert(Entity(1, 0));
        CHECK(seen.size() == 1);

        // Dense slot indices from zero would collide in a power-of-two bucket table if the generation did not
        // participate in the hash; this is the regression guard for that.
        seen.insert(Entity(1, 1));
        seen.insert(Entity(1, 2));
        CHECK(seen.size() == 3);

        // Slots differing by exactly one bucket-table power must not collide either.
        seen.insert(Entity(65, 0));
        seen.insert(Entity(129, 0));
        CHECK(seen.size() == 5);
    }

    TEST_CASE("hashes of many sequential entities spread over distinct buckets")
    {
        // Not a statistical test — just a guard against a degenerate hash (e.g. ignoring the slot entirely),
        // which would turn every target cache into a linear scan.
        std::unordered_set<Entity> seen;
        for (Hollowpoint::Core::Index slot = 0; slot < 4096; ++slot)
        {
            seen.insert(Entity(slot, slot % 7));
        }
        CHECK(seen.size() == 4096);
    }

    TEST_CASE("ToString distinguishes handles sharing a slot")
    {
        CHECK(Entity().ToString() == "Entity(null)");
        CHECK(Entity(3, 1).ToString() == "Entity(slot=3, gen=1)");
        CHECK(Entity(3, 2).ToString() == "Entity(slot=3, gen=2)");

        // Slot 0 is a real entity, not the null handle — printing it as null would hide exactly the bugs this
        // string exists to reveal.
        CHECK_FALSE(Entity(0, 0).IsNull());
        CHECK(Entity(0, 0).ToString() == "Entity(slot=0, gen=0)");
    }
}

// Layout properties, checked at compile time (doctest 2.x has no STATIC_CHECK). Entities are copied into job
// parameters and script stacks, so a size or triviality regression here costs real throughput and forbids the
// memcpys the world relies on.
static_assert(sizeof(Entity) == 8, "Entity handles must stay 8 bytes");
static_assert(std::is_trivially_copyable_v<Entity>, "Entities are memcpy'd into jobs and script stacks");
static_assert(std::is_trivially_destructible_v<Entity>, "Entities are stored in raw slot arrays");
