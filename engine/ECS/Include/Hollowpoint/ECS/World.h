#pragma once

#include <unordered_map>
#include <vector>

#include "Hollowpoint/Core/Guid.h"
#include "Hollowpoint/ECS/Entity.h"

namespace Hollowpoint::ECS
{

using Core::Size;

// ---------------------------------------------------------------------------
// World — owns entity slots and their identities.
//
// This file is about entities only: creation, destruction, identity, liveness. Components live on top (see
// ComponentStorage.h / World's component interface) because they are what systems iterate, not what makes a
// slot exist.
//
// Slot lifecycle
// --------------
// A slot holds an index and a generation. Destroying an entity marks its slot dead and queues it; the slot is
// recycled (generation incremented) when pending destructions are flushed. Two consequences worth stating
// because they are where engine bugs live:
//
//   * IsAlive() goes false **immediately** on Destroy, not at flush. A script that destroys an entity and a
//     system that later touches it in the same frame must both see "gone" — otherwise a projectile keeps
//     colliding with a corpse for one frame, which is exactly the class of bug generation checks exist to kill.
//   * Component memory, by contrast, is only released at flush. A system holding a pointer into a component
//     array mid-iteration must not have it freed underneath; that is why destruction is deferred at all.
//
// So: logically dead right away, physically reclaimed later. Any access to a dead entity's components is
// rejected (the liveness check precedes the storage lookup), so the retained memory cannot be observed.
//
// Why deferred rather than immediate
// ----------------------------------
// Scripting makes destruction reentrant: an overlap callback destroys the player, which destroys its weapon,
// which iterates a list containing entities the current pass has not visited yet. Freeing slots in place would
// corrupt that pass. Instead Destroy() is O(1), safe from anywhere, and FlushPendingDestructions() runs at a
// defined point in the frame (after all systems and script callbacks). The editor's single-step and the
// runtime's tick both call it, so there is exactly one place where slots move.
//
// Threading
// ---------
// Entity creation and destruction are main-thread only — documented rather than locked, because every caller
// (systems, scripting, asset hot-reload) already runs on the main thread and a mutex here would be theatre.
// Queries (IsAlive, GetGuid, iteration) are read-only over stable arrays and safe from any thread while no
// mutation is in progress.
// ---------------------------------------------------------------------------

class World
{
public:
    World() = default;

    // No copying: entities and component storage refer to this world's slot array by index. Moving would
    // invalidate every outstanding handle's meaning, which is not something a caller can fix up.
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;

    // Creation and destruction ---------------------------------------------

    //! Creates an entity with a fresh Guid. Returns the null handle if the slot space is exhausted (which
    //! means 4 billion live entities — reported rather than wrapped, since wrapping would silently alias every
    //! existing entity).
    [[nodiscard]] Entity CreateEntity();

    //! Creates an entity carrying a specific Guid. Used when loading a scene, where identities come from the
    //! file and must be reproduced exactly so references between entities resolve. Asserts when the Guid is
    //! already present or nil: two entities sharing an identity would make every saved reference ambiguous.
    [[nodiscard]] Entity CreateEntityWithGuid(const Core::Guid& guid);

    //! Destroys an entity, immediately making it not alive. Idempotent: destroying an already-dead entity (or a
    //! stale handle whose generation no longer matches) is reported and otherwise ignored, because scripts do
    //! this constantly ("destroy if still there") and crashing on the second call would be hostile.
    void DestroyEntity(const Entity& entity);

    //! Number of entities queued for reclamation. Zero outside a frame's script/system phase in practice.
    [[nodiscard]] Size GetPendingDestructionCount() const noexcept
    {
        return m_PendingDestructions.size();
    }

    //! Releases slots queued by DestroyEntity and returns them to the free list. Called once per frame after
    //! all systems and script callbacks have run; safe to call more often (it only reclaims what is queued).
    void FlushPendingDestructions();

    // Queries ---------------------------------------------------------------

    [[nodiscard]] bool IsAlive(const Entity& entity) const noexcept;

    //! Number of live entities (excludes slots awaiting flush).
    [[nodiscard]] Size GetAliveEntityCount() const noexcept
    {
        return m_AliveCount;
    }

    //! Total slots ever allocated, including dead ones awaiting flush and recycled-but-unused ones. This is
    //! the bound for iterating raw slot arrays, not a count of things that exist.
    [[nodiscard]] Size GetSlotCapacity() const noexcept
    {
        return m_Slots.size();
    }

    // Identity --------------------------------------------------------------

    //! The entity's Guid, or nil when the handle is stale. Identities are assigned at creation and never
    //! change, which is what makes them usable in saved scenes and script registries.
    [[nodiscard]] Core::Guid GetGuid(const Entity& entity) const;

    [[nodiscard]] Entity GetEntityByGuid(const Core::Guid& guid) const;

    // Frame maintenance -----------------------------------------------------

    //! Reclaims dead entities: releases their slots (advancing generations), runs the registered destruction
    //! hooks, and frees their component storage. Called once per frame after all systems and script callbacks,
    //! so nothing can be holding a pointer into storage that is about to move.
    //!
    //! Destruction hooks exist because subsystems own side-state keyed by entity — physics bodies, audio
    //! emitters, script instances, navigation agents — and that state cannot live in a component the world
    //! knows how to free. Each hook must release what it holds for the given entity; they run before component
    //! memory is released, so a handler may still read the entity's components one last time (its collider
    //! radius, its sound asset) to tear down correctly.
    using DestructionHandler = void (*)(const Entity& entity, void* userData);

    void RegisterDestructionHandler(DestructionHandler handler, void* userData = nullptr);
    void UnregisterDestructionHandler(DestructionHandler handler, void* userData = nullptr);

    //! Removes every entity and reclaims every slot through the normal destruction path, so destruction hooks
    //! still run and generations keep advancing (a handle from before the clear can never alias an entity created
    //! after it). Slot memory is retained for reuse by the next level.
    void Clear();

    // Iteration -------------------------------------------------------------

    //! Calls `callback(entity)` for every live entity, in slot order. Stable with respect to IsAlive checks
    //! inside the callback: an entity destroyed during iteration is skipped if its slot has already been
    //! visited-and-passed, and reported-but-skipped if not, so a system never sees a corpse.
    template <typename TCallback>
    void ForEachEntity(TCallback&& callback) const
    {
        for (Size index = 0; index < m_Slots.size(); ++index)
        {
            const SlotRecord& slot = m_Slots[index];
            if (!slot.Alive)
            {
                continue;
            }
            callback(Entity(static_cast<Core::Index>(index), slot.Generation));
        }
    }

private:
    struct SlotRecord
    {
        Core::UInt32 Generation = 0;
        bool Alive = false;
        Core::Guid Identity;
    };

    struct DestructionHook
    {
        DestructionHandler Handler = nullptr;
        void* UserData = nullptr;
    };

    //! Takes a slot from the free list or appends a new one, without assigning an identity. Both public creation
    //! paths go through it so slot bookkeeping exists exactly once.
    [[nodiscard]] Entity AllocateSlot();

    std::vector<SlotRecord> m_Slots;
    std::vector<Core::Index> m_FreeSlots;
    std::vector<DestructionHook> m_DestructionHooks;

    // Guards against a destruction handler calling Flush: a nested flush would move component storage while the
    // outer loop is still walking it, so re-entrancy is refused rather than supported.
    bool m_IsFlushing = false;

    // Guid -> slot. Kept as a map rather than scanned: scene loads and script registries resolve identities
    // constantly, and 4 bytes per entity is cheaper than an O(n) lookup on every reference.
    std::unordered_map<Core::Guid, Core::Index> m_SlotsByGuid;

    std::vector<Core::Index> m_PendingDestructions;
    Size m_AliveCount = 0;
};

}  // namespace Hollowpoint::ECS
