#pragma once

#include <functional>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Hollowpoint/Core/Guid.h"
#include "Hollowpoint/ECS/ComponentStorage.h"
#include "Hollowpoint/ECS/Entity.h"
#include "Hollowpoint/ECS/Hierarchy.h"

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
    //! Destroys an entity and, by default, its whole subtree.
    //!
    //! Recursive destruction is the default because it matches every engine convention (Unity, Godot, Unreal)
    //! and because the alternative leaks: a parent removed with its children still attached leaves live entities
    //! nothing can reach through the tree, which in an editor means invisible objects that cannot be selected or
    //! deleted. `promoteChildrenToParent` opts into the other policy — children are re-parented to this entity's
    //! parent instead, which is what "delete this component holder but keep the nodes" needs.
    //!
    //! The subtree walk uses an explicit worklist rather than recursion, so teardown cost is bounded by the tree
    //! size and cannot overflow the stack at any depth.
    void DestroyEntity(const Entity& entity, bool promoteChildrenToParent = false);

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

    //! Bumped by anything that changes *which* entities exist or how they are nested: creation, destruction and
    //! reparenting. Derived state (world-transform caches, culling structures, navigation maps) compares against a
    //! remembered value to decide whether its view of the structure is still valid — cheaper than every subsystem
    //! hooking three lifecycle events, and impossible to forget because there is nothing to hook.
    //!
    //! Deliberately coarse: it does not change when a component value is written (the world cannot see those), so
    //! it invalidates structural caches only, never data derived from component contents.
    [[nodiscard]] Core::UInt64 GetStructureVersion() const noexcept
    {
        return m_StructureVersion;
    }

    //! Rebuilds a valid handle for a slot, or null when the slot is out of range or not alive. Needed by systems
    //! that iterate by slot index (transform evaluation, spatial structures) and must produce handles carrying the
    //! current generation — assembling one from a bare slot number would silently alias after a recycle, which is
    //! exactly what generations exist to prevent. Prefer ForEachEntity when slot order is not required.
    [[nodiscard]] Entity GetEntityAtSlot(Core::Index slot) const noexcept;

    // Identity --------------------------------------------------------------

    //! The entity's Guid, or nil when the handle is stale. Identities are assigned at creation and never
    //! change, which is what makes them usable in saved scenes and script registries.
    [[nodiscard]] Core::Guid GetGuid(const Entity& entity) const;

    [[nodiscard]] Entity GetEntityByGuid(const Core::Guid& guid) const;

    // Hierarchy -------------------------------------------------------------
    //
    // Parent/child links are world state rather than a component (see Hierarchy.h): the tree must stay
    // well-formed whether or not any game type is loaded, and destroying an entity has to resolve its children
    // without knowing about them.

    //! Reparents an entity. Returns a result code rather than asserting because scripts produce invalid requests
    //! routinely (a target destroyed between frames), and each failure mode means something different to the
    //! caller — including "already like this", which is not an error at all.
    Hierarchy::ReparentResult SetParent(const Entity& entity, const Entity& newParent);

    //! Convenience overload for detaching to the scene root.
    Hierarchy::ReparentResult DetachToRoot(const Entity& entity);

    [[nodiscard]] bool HasParent(const Entity& entity) const;
    [[nodiscard]] Entity GetParent(const Entity& entity) const;

    [[nodiscard]] Entity GetFirstChild(const Entity& entity) const;
    [[nodiscard]] Entity GetNextSibling(const Entity& entity) const;

    //! Calls `callback(Entity)` for each direct child, in stable insertion order.
    template <typename TCallback>
    void ForEachChild(const Entity& entity, TCallback&& callback) const;

    [[nodiscard]] Size GetChildCount(const Entity& entity) const;

    //! True when `maybeDescendant` sits below `ancestor` (strict). Cycle detection for scripts that build trees.
    [[nodiscard]] bool IsDescendantOf(const Entity& maybeDescendant, const Entity& ancestor) const;

    [[nodiscard]] Core::UInt32 GetDepth(const Entity& entity) const;

    // Components ------------------------------------------------------------
    //
    // Stored per type (see ComponentStorage.h). Every accessor checks liveness before touching storage, which is
    // what makes deferred physical reclamation safe: a logically dead entity cannot reach its own components even
    // though their memory is still intact.

    //! Adds (or returns the existing) component on an entity. Asserts for a stale or dead handle: attaching a
    //! component to nothing would leak it into every subsequent query, since storage is keyed by slot and a
    //! recycled slot would hand it to whoever reuses it.
    // Not nodiscard: "ensure this component exists" is a common call shape where the reference is genuinely not
    // wanted, and forcing a binding there would only add noise. Lookups are nodiscard instead — discarding those
    // means the call did nothing at all.
    template <typename TComponent, typename... TArgs>
    TComponent& AddComponent(const Entity& entity, TArgs&&... args);

    //! Adds a component only if the entity is alive; returns nullptr otherwise. For load and script paths where
    //! "the target may have vanished" is an expected outcome rather than an engine bug.
    template <typename TComponent, typename... TArgs>
    TComponent* TryAddComponent(const Entity& entity, TArgs&&... args);

    //! Returns the component or nullptr. The non-asserting form, and the one systems should use in a query loop.
    template <typename TComponent>
    [[nodiscard]] TComponent* TryGetComponent(const Entity& entity);

    template <typename TComponent>
    [[nodiscard]] const TComponent* TryGetComponent(const Entity& entity) const;

    //! Component of type T, asserting when the entity is dead or lacks one. For call sites where absence would be
    //! a bug (a render pass asking a mesh-less entity for its mesh).
    template <typename TComponent>
    [[nodiscard]] TComponent& GetComponent(const Entity& entity);

    template <typename TComponent>
    [[nodiscard]] const TComponent& GetComponent(const Entity& entity) const;

    template <typename TComponent>
    [[nodiscard]] bool HasComponent(const Entity& entity) const;

    //! Removes the component of type T. Returns false when the entity did not have one (or is dead).
    template <typename TComponent>
    bool RemoveComponent(const Entity& entity);

    //! Calls `callback(Entity, TComponent&)` for every live entity holding a TComponent.
    //!
    //! Destroying entities during the pass is safe and normal (a scripted "kill everything nearby"): an entity
    //! destroyed before its turn comes is skipped, never handed over with a component that is about to belong to
    //! somebody else. Adding or removing *this component type mid-pass is not safe — removal swaps the last element
    //! into the hole, so the pass would skip or revisit an entity. Deferred mutation is the pattern for that:
    //! collect entities while iterating and apply structural changes afterwards. Enforcing the rule at runtime
    //! would cost a check per iteration to catch a mistake that is trivially avoided, so it is documented instead.
    template <typename TComponent, typename TCallback>
    void ForEach(TCallback&& callback);

    template <typename TComponent, typename TCallback>
    void ForEach(TCallback&& callback) const;

    [[nodiscard]] Size GetComponentTypeCount() const noexcept
    {
        return m_Registry.GetStorageCount();
    }

    //! Iterates the component arrays of this world. The serializer (write every component of every entity) and
    //! the inspector (list an entity's components generically) both need it, since neither knows the concrete
    //! component types at compile time. Storage is visited in a deterministic order so saved scenes come out
    //! byte-identical between runs — otherwise every diff of a level file is noise.
    void ForEachComponentStorage(const std::function<void(IComponentStorage&)>& callback);
    void ForEachComponentStorage(const std::function<void(const IComponentStorage&)>& callback) const;

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

    //! Current generation of a slot, for rebuilding an Entity handle during iteration. Returns zero for an
    //! out-of-range slot so a stray index produces a handle that fails IsAlive rather than one that might match.
    [[nodiscard]] Core::UInt32 GetGeneration(Core::Index slot) const noexcept;

    //! Marks one slot dead and queues it for reclamation. Idempotent, which is what lets the subtree walk in
    //! DestroyEntity visit a node twice (reachable through two links after a bug elsewhere) without corrupting the
    //! pending-destruction queue — a double entry there would put a live entity on the free list later.
    void MarkSlotDead(Core::Index slot);

    //! Takes a slot from the free list or appends a new one, without assigning an identity. Both public creation
    //! paths go through it so slot bookkeeping exists exactly once.
    [[nodiscard]] Entity AllocateSlot();

    //! See GetStructureVersion. Starts at 1 so "zero" can stay a sentinel meaning "never observed" in caches.
    Core::UInt64 m_StructureVersion = 1;

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

    ComponentRegistry m_Registry;

    //! Parent/child topology over entity slots. Kept separate from the slot array so it can be cleared and
    //! rebuilt (scene reload) without touching identities.
    Hierarchy m_Hierarchy;
};

// ---------------------------------------------------------------------------
// Component template definitions.
//
// Kept in the header because components are templates over caller types. They all funnel through the registry,
// so a component type's storage is created on first use and never before — an unused component type costs
// nothing at runtime.
// ---------------------------------------------------------------------------

template <typename TComponent, typename... TArgs>
TComponent& World::AddComponent(const Entity& entity, TArgs&&... args)
{
    HP_ASSERT_MSG(IsAlive(entity), "AddComponent on a dead or stale entity");
    if (!IsAlive(entity))
    {
        // Continuing past an assert is allowed by the engine's contract (Core/Assert.h), so this path has to be
        // safe. A function-local static gives a valid object to reference; writes through it are lost, which is
        // strictly better than dereferencing null on a code path that already reported a bug.
        static TComponent fallback{};
        return fallback;
    }

    ComponentStorage<TComponent>* storage =
        m_Registry.template GetOrCreateStorage<TComponent>(typeid(TComponent).name());

    TComponent* existing = storage->Find(entity.Slot);
    if (existing != nullptr)
    {
        // Already present: returned untouched rather than reconstructed. Rebuilding a component would zero the
        // accumulated state (health, an animation timer) of whatever code called Add "just to be sure" — a silent
        // gameplay corruption that looks like a random reset.
        return *existing;
    }

    return storage->GetStorage().Emplace(entity.Slot, std::forward<TArgs>(args)...);
}

template <typename TComponent, typename... TArgs>
TComponent* World::TryAddComponent(const Entity& entity, TArgs&&... args)
{
    if (!IsAlive(entity))
    {
        return nullptr;
    }
    return &AddComponent<TComponent>(entity, std::forward<TArgs>(args)...);
}

template <typename TComponent>
TComponent* World::TryGetComponent(const Entity& entity)
{
    if (!IsAlive(entity))
    {
        return nullptr;
    }
    ComponentStorage<TComponent>* storage = m_Registry.template FindStorage<TComponent>();
    return storage != nullptr ? storage->Find(entity.Slot) : nullptr;
}

template <typename TComponent>
const TComponent* World::TryGetComponent(const Entity& entity) const
{
    if (!IsAlive(entity))
    {
        return nullptr;
    }
    const ComponentStorage<TComponent>* storage = m_Registry.template FindStorage<TComponent>();
    return storage != nullptr ? storage->Find(entity.Slot) : nullptr;
}

template <typename TComponent>
TComponent& World::GetComponent(const Entity& entity)
{
    TComponent* component = TryGetComponent<TComponent>(entity);
    HP_ASSERT_MSG(component != nullptr, "GetComponent for a component the entity does not have");
    if (component == nullptr)
    {
        static TComponent fallback{};
        return fallback;
    }
    return *component;
}

template <typename TComponent>
const TComponent& World::GetComponent(const Entity& entity) const
{
    const TComponent* component = TryGetComponent<TComponent>(entity);
    HP_ASSERT_MSG(component != nullptr, "GetComponent for a component the entity does not have");
    if (component == nullptr)
    {
        static TComponent fallback{};
        return fallback;
    }
    return *component;
}

template <typename TComponent>
bool World::HasComponent(const Entity& entity) const
{
    return TryGetComponent<TComponent>(entity) != nullptr;
}

template <typename TComponent>
bool World::RemoveComponent(const Entity& entity)
{
    if (!IsAlive(entity))
    {
        return false;
    }
    ComponentStorage<TComponent>* storage = m_Registry.template FindStorage<TComponent>();
    return storage != nullptr && storage->Remove(entity.Slot);
}

template <typename TComponent, typename TCallback>
void World::ForEach(TCallback&& callback)
{
    ComponentStorage<TComponent>* storage = m_Registry.template FindStorage<TComponent>();
    if (storage == nullptr)
    {
        // Nobody has ever added this component type. Skipping without creating storage matters: otherwise every
        // ForEach over an unused type allocates an array that lives for the rest of the session.
        return;
    }

    Detail::SparseSet<TComponent>& set = storage->GetStorage();
    const Size count = set.GetCount();
    for (Size index = 0; index < count; ++index)
    {
        // Count captured up front and elements read by position, so a callback that destroys entities cannot
        // change what this pass visits. See the ForEach contract above for the in-pass-mutation rule.
        const Core::Index slot = set.GetSlotAt(index);
        if (slot == Core::InvalidIndex)
        {
            break;
        }

        // Liveness is re-checked per entity rather than inferred from component presence. Components are reclaimed
        // at flush, so an entity destroyed earlier in this very pass still has its component sitting in the array;
        // handing it over would let a system write to a corpse, and after the flush that memory belongs to a
        // different entity. A generation compare per visited entity is cheap next to what any real system does, so
        // this guard is unconditional rather than debug-only.
        const Entity handle(slot, GetGeneration(slot));
        if (!IsAlive(handle))
        {
            continue;
        }

        TComponent* component = set.Find(slot);
        if (component == nullptr)
        {
            continue;  // Component removed during this pass by a callback that broke the documented contract.
        }

        callback(handle, *component);
    }
}

template <typename TComponent, typename TCallback>
void World::ForEach(TCallback&& callback) const
{
    const ComponentStorage<TComponent>* storage = m_Registry.template FindStorage<TComponent>();
    if (storage == nullptr)
    {
        return;
    }

    const Detail::SparseSet<TComponent>& set = storage->GetStorage();
    const Size count = set.GetCount();
    for (Size index = 0; index < count; ++index)
    {
        const Core::Index slot = set.GetSlotAt(index);
        if (slot == Core::InvalidIndex)
        {
            break;
        }

        // See the mutable overload: entities destroyed earlier in this pass are skipped, not visited.
        const Entity handle(slot, GetGeneration(slot));
        if (!IsAlive(handle))
        {
            continue;
        }

        const TComponent* component = set.Find(slot);
        if (component == nullptr)
        {
            continue;
        }

        callback(handle, *component);
    }
}

template <typename TCallback>
void World::ForEachChild(const Entity& entity, TCallback&& callback) const
{
    for (Entity child = GetFirstChild(entity); !child.IsNull(); child = GetNextSibling(child))
    {
        callback(child);
    }
}

}  // namespace Hollowpoint::ECS
