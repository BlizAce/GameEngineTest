#pragma once

#include <functional>
#include <string>
#include <type_traits>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Hash.h"
#include "Hollowpoint/Core/Types.h"

namespace Hollowpoint::ECS
{

// ---------------------------------------------------------------------------
// Entity — a slot handle, not a pointer and not an identity.
//
// An entity is a slot index into the world's slot array plus a generation counter that increments every
// time the slot is recycled. That combination gives the two properties a game engine actually needs:
//
//   * Validity checks are O(1) and cannot be fooled by a freed handle: a stale Entity pointing at a
//     recycled slot carries an old generation and fails IsAlive(). This is the difference between "the enemy
//     you were targeting is gone" and "you just wrote into some other entity's components", which in a game
//     with scripted damage across hundreds of frames is not a theoretical bug.
//   * Handles are 8 bytes, trivially copyable, and safe to store in script stacks and job parameters.
//
// What an Entity is NOT: a persistent identity. Slots get reused, so an Entity read from a save file means
// nothing. Serialized references go through Core::Guid, which the world keeps alongside each slot.
//
// Deliberately no operator bool: `if (entity)` reads as "is valid" but could only ever answer "is not the
// null handle", which is a different question — so validity is spelled IsAlive(world) at every call site.
//
// The slot field is named Slot rather than Index on purpose: a member called Index would hide the Core::Index
// type alias in class scope and make the constructor ill-formed.
// ---------------------------------------------------------------------------

struct Entity
{
    //! Sentinel slot for a default-constructed handle that refers to nothing.
    static constexpr Core::Index NullSlot = 0xFFFFFFFFu;

    Core::Index Slot = NullSlot;
    Core::UInt32 Generation = 0;

    constexpr Entity() noexcept = default;

    constexpr Entity(Core::Index slot, Core::UInt32 generation) noexcept : Slot(slot), Generation(generation) {}

    //! True when this is the null handle. Does NOT mean the entity exists — only IsAlive can answer that.
    [[nodiscard]] constexpr bool IsNull() const noexcept
    {
        return Slot == NullSlot;
    }

    // Equality is written out rather than defaulted: three-way comparison pulls in <compare>, and this type
    // lives in hot headers that other modules include transitively.
    friend constexpr bool operator==(const Entity& lhs, const Entity& rhs) noexcept
    {
        return lhs.Slot == rhs.Slot && lhs.Generation == rhs.Generation;
    }

    //! Debug/editor-facing description, e.g. "Entity(slot=3, gen=1)". See Entity.cpp for why the generation is
    //! always printed.
    [[nodiscard]] HP_ECS_API std::string ToString() const;
};

static_assert(sizeof(Entity) == 8, "Entities are stored per handle and copied into jobs; keep them 8 bytes");
static_assert(std::is_trivially_copyable_v<Entity>, "Entities are copied into script stacks and job parameters");
static_assert(std::is_default_constructible_v<Entity>, "Script VMs create empty entity values on the stack");

}  // namespace Hollowpoint::ECS

// Hashing lets entities key associative containers: target caches, damage accumulators, render overrides.
template <>
struct std::hash<Hollowpoint::ECS::Entity>
{
    std::size_t operator()(const Hollowpoint::ECS::Entity& entity) const noexcept
    {
        // Combined rather than concatenated: slot indices are dense from zero, so a table with power-of-two
        // buckets would collide on every entry if the generation did not participate.
        return static_cast<std::size_t>(
            Hollowpoint::Core::CombineHash(Hollowpoint::Core::HashValue(entity.Slot), entity.Generation));
    }
};
