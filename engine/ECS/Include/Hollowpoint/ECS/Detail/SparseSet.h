#pragma once

#include <type_traits>
#include <utility>
#include <vector>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Types.h"

namespace Hollowpoint::ECS::Detail
{

using Core::Size;

//! Stand-in returned by the checked accessors when a lookup failed and the assertion allowed execution to
//! continue (see the class comments on Get). thread_local so two threads' failed lookups cannot write over each
//! other — that would be a data race on top of an already-buggy path. A per-thread default costs one TLS slot
//! per component type and no locking; writes through it are silently discarded, which is strictly better than
//! the alternatives (crash, or an exception escaping a noexcept system).
template <typename TValueLocal>
[[nodiscard]] TValueLocal& MissingValue()
{
    static thread_local TValueLocal sentinel{};
    return sentinel;
}

// ---------------------------------------------------------------------------
// Sparse set — the storage every component type gets.
//
// Layout: a dense array of values plus one sparse index per entity slot pointing into it. That buys the
// three operations an ECS performs constantly, all O(1):
//
//   * get(slot)        — two array loads, no hashing, no pointer chasing through buckets
//   * insert/remove    — append/swap-back on the dense arrays
//   * iterate          — a contiguous range of values, so systems vectorise and prefetch normally
//
// The trade-off is that removal swaps the last element into the hole, so **dense indices are not stable**:
// an index held across a removal points at a different entity afterwards. Nothing in the engine may cache a
// dense index; iteration either goes to completion or restarts. This is documented here because the failure
// is silent (a wrong entity's component, not a crash) and every system author needs to know it.
//
// Values live inline in the dense array. A pointer into it is valid only until the next insert (which may
// reallocate) or remove of that type — callers who need longer lifetimes copy the value or re-fetch it. The
// alternative, stable storage with an indirection per access, was rejected: component reads happen thousands
// of times per frame and the engine has no requirement to hold component pointers across mutations.
// ---------------------------------------------------------------------------

template <typename TValue>
class SparseSet
{
    static_assert(!std::is_reference_v<TValue>, "Sparse sets store values");
    static_assert(std::is_default_constructible_v<TValue>, "New elements are value-initialised");

public:
    using ValueType = TValue;

    [[nodiscard]] Size GetCount() const noexcept
    {
        return m_Count;
    }

    [[nodiscard]] bool IsEmpty() const noexcept
    {
        return m_Count == 0;
    }

    [[nodiscard]] bool Contains(Core::Index slot) const noexcept
    {
        return slot < m_Sparse.size() && m_Sparse[slot] != Core::InvalidIndex;
    }

    // Access -----------------------------------------------------------------

    //! Dense position of a slot, or Core::InvalidIndex. Stable for the duration of one iteration pass only —
    //! see the class comment on why it must not be cached across mutations.
    [[nodiscard]] Core::Index GetDenseIndex(Core::Index slot) const noexcept
    {
        return DenseIndexOf(slot);
    }

    [[nodiscard]] TValue* Find(Core::Index slot) noexcept
    {
        const Core::Index dense = DenseIndexOf(slot);
        return dense == Core::InvalidIndex ? nullptr : &m_Values[dense];
    }

    [[nodiscard]] const TValue* Find(Core::Index slot) const noexcept
    {
        const Core::Index dense = DenseIndexOf(slot);
        return dense == Core::InvalidIndex ? nullptr : &m_Values[dense];
    }

    //! Value at `slot`, asserting when absent.
    //!
    //! The engine's assertion contract (Core/Assert.h) allows a failed invariant to *continue* — the editor
    //! turns one into a reported error and tests divert them — so the path after the assert still has to be
    //! safe. Returning a sentinel rather than dereferencing null is what makes that true: a missing component
    //! then reads as a default value instead of crashing, which for a shipped game means "this entity is
    //"" missing its collider for a frame" rather than a hard fault.
    //!
    //! The sentinel is thread_local and shared by every failed lookup on that thread, so writes through it are
    //! silently discarded (and visible to the caller's own thread only). That is deliberate: the alternative
    //! failure modes are a crash or an exception, both worse than losing a write on a code path that already
    //! reported a bug. Callers who can meaningfully handle absence should use Find.
    [[nodiscard]] TValue& Get(Core::Index slot)
    {
        TValue* found = Find(slot);
        if (found == nullptr)
        {
            HP_ASSERT_MSG(false, "Sparse set lookup for an absent slot");
            return MissingValue<TValue>();
        }
        return *found;
    }

    [[nodiscard]] const TValue& Get(Core::Index slot) const
    {
        const TValue* found = Find(slot);
        if (found == nullptr)
        {
            HP_ASSERT_MSG(false, "Sparse set lookup for an absent slot");
            return MissingValue<TValue>();
        }
        return *found;
    }

    //! Entity slot at a dense position, so iteration can name the entity it is visiting. Returns
    //! Core::InvalidIndex when out of range — same reasoning as Get: a continued assertion must not read past
    //! the array, and an invalid slot fails safely in every caller that checks handles (as all of them do).
    [[nodiscard]] Core::Index GetSlotAt(Size denseIndex) const
    {
        if (denseIndex >= m_Count)
        {
            HP_ASSERT_MSG(false, "Dense index out of range");
            return Core::InvalidIndex;
        }
        return m_Slots[denseIndex];
    }

    [[nodiscard]] TValue& GetValueAt(Size denseIndex)
    {
        if (denseIndex >= m_Count)
        {
            HP_ASSERT_MSG(false, "Dense index out of range");
            return MissingValue<TValue>();
        }
        return m_Values[denseIndex];
    }

    [[nodiscard]] const TValue& GetValueAt(Size denseIndex) const
    {
        if (denseIndex >= m_Count)
        {
            HP_ASSERT_MSG(false, "Dense index out of range");
            return MissingValue<TValue>();
        }
        return m_Values[denseIndex];
    }

    //! Contiguous storage for vectorised systems. Invalidated by any insert or remove of this type.
    [[nodiscard]] TValue* GetData() noexcept
    {
        return m_Values.data();
    }

    [[nodiscard]] const TValue* GetData() const noexcept
    {
        return m_Values.data();
    }

    // Mutation ---------------------------------------------------------------

    //! Inserts a default-constructed value, or returns the existing one, giving its address. The result is not
    //! nodiscard: many call sites insert for effect and never touch the value.
    TValue* Insert(Core::Index slot)
    {
        if (TValue* existing = Find(slot); existing != nullptr)
        {
            return existing;
        }

        GrowSparseTo(slot);
        m_Sparse[slot] = static_cast<Core::Index>(m_Count);
        m_Values.emplace_back();
        m_Slots.push_back(slot);
        ++m_Count;
        return &m_Values[m_Count - 1];
    }

    TValue* Insert(Core::Index slot, const TValue& value)
    {
        TValue* address = Insert(slot);
        *address = value;
        return address;
    }

    //! Removes the value at `slot`, returning true when one was removed. Invalidates dense indices from the
    //! removal point onwards (the last element moves into the hole).
    bool Remove(Core::Index slot)
    {
        const Core::Index dense = DenseIndexOf(slot);
        if (dense == Core::InvalidIndex)
        {
            return false;
        }

        const Core::Index lastIndex = static_cast<Core::Index>(m_Count - 1);
        if (dense != lastIndex)
        {
            // Swap-back: keeps the dense arrays contiguous at the cost of moving one element. The moved
            // entity's sparse entry has to follow it, or its next lookup lands in the wrong place.
            m_Values[dense] = std::move(m_Values[lastIndex]);
            m_Slots[dense] = m_Slots[lastIndex];
            m_Sparse[m_Slots[dense]] = dense;
        }

        m_Values.pop_back();
        m_Slots.pop_back();
        m_Sparse[slot] = Core::InvalidIndex;
        --m_Count;
        return true;
    }

    //! Drops every value but keeps the sparse array sized, so a world that is rebuilt between levels does not
    //! reallocate on the next fill.
    void Clear() noexcept
    {
        for (Core::Index slot : m_Slots)
        {
            m_Sparse[slot] = Core::InvalidIndex;
        }
        m_Values.clear();
        m_Slots.clear();
        m_Count = 0;
    }

    void Reserve(Size capacity)
    {
        m_Values.reserve(capacity);
        m_Slots.reserve(capacity);
    }

    //! Pre-sizes the sparse array for a known entity count, avoiding reallocation during world load.
    void ReserveSlots(Core::Index slotCapacity)
    {
        if (slotCapacity > m_Sparse.size())
        {
            m_Sparse.resize(slotCapacity, Core::InvalidIndex);
        }
    }

private:
    [[nodiscard]] Core::Index DenseIndexOf(Core::Index slot) const noexcept
    {
        return slot < m_Sparse.size() ? m_Sparse[slot] : Core::InvalidIndex;
    }

    void GrowSparseTo(Core::Index slot)
    {
        if (slot >= m_Sparse.size())
        {
            // Grow past the requested slot so a run of creations does not reallocate once per entity. The
            // sparse array holds one uint32 per entity slot, so slack here is cheap.
            const Size newCapacity = slot + 1 + (m_Sparse.size() >> 1);
            m_Sparse.resize(newCapacity > slot + 1 ? newCapacity : slot + 1, Core::InvalidIndex);
        }
    }

    std::vector<TValue> m_Values;       //!< Dense values, parallel to m_Slots.
    std::vector<Core::Index> m_Slots;   //!< Entity slot per dense position (iteration names its entity).
    std::vector<Core::Index> m_Sparse;  //!< slot -> dense index, Core::InvalidIndex when absent.
    Size m_Count = 0;
};

}  // namespace Hollowpoint::ECS::Detail
