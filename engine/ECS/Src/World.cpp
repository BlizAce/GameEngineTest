#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/ECS/World.h"

namespace Hollowpoint::ECS
{

Entity World::CreateEntity()
{
    const Entity entity = AllocateSlot();
    if (entity.IsNull())
    {
        return entity;
    }

    // Every entity gets an identity at creation. The alternative — assigning Guids lazily, or only to entities
    // that get serialized — was rejected: it creates two kinds of entity, makes "references survive" conditional
    // in exactly the way that produces corrupt saves (a script builds a reference to an unidentified entity, the
    // level is saved, and the reference has nothing to point at), and pushes identity management into every
    // caller. The cost is 16 bytes per slot plus one RNG draw and one hash insert per creation; if profiling ever
    // says that matters for a particle system, an explicit no-identity fast path can be added then with the
    // trade-off in front of us rather than discovered as a save-format bug.
    const Core::Guid identity = Core::Guid::Generate();

    m_Slots[entity.Slot].Identity = identity;

    auto inserted = m_SlotsByGuid.emplace(identity, entity.Slot);
    if (!inserted.second)
    {
        // Only reachable if Guid::Generate produced a duplicate, which would be a broken generator rather than bad
        // luck (the space is 2^122). Reported loudly because everything downstream assumes identity uniqueness.
        HP_LOG_ERROR("ECS", "Guid generator produced a duplicate: {}", identity.ToString());
        HP_ASSERT_MSG(false, "Duplicate Guid from the generator");
    }

    return entity;
}

Entity World::CreateEntityWithGuid(const Core::Guid& guid)
{
    if (guid.IsNil())
    {
        // A nil Guid means "no reference" in every serialized format the engine uses, so an entity cannot own
        // one — it would make the entity indistinguishable from a dangling reference.
        HP_LOG_ERROR("ECS", "Cannot create an entity with a nil Guid");
        HP_ASSERT_MSG(false, "Entity created with a nil Guid");
        return Entity();
    }

    if (m_SlotsByGuid.find(guid) != m_SlotsByGuid.end())
    {
        // Two entities sharing an identity would make every saved reference to that Guid ambiguous — the scene
        // would load with one of them silently shadowing the other.
        HP_LOG_ERROR("ECS", "Guid {} is already present in this world", guid.ToString());
        HP_ASSERT_MSG(false, "Duplicate entity Guid");
        return Entity();
    }

    const Entity entity = AllocateSlot();
    if (entity.IsNull())
    {
        return entity;
    }

    m_Slots[entity.Slot].Identity = guid;
    m_SlotsByGuid.emplace(guid, entity.Slot);
    return entity;
}

Entity World::AllocateSlot()
{
    if (!m_FreeSlots.empty())
    {
        const Core::Index slot = m_FreeSlots.back();
        m_FreeSlots.pop_back();

        SlotRecord& record = m_Slots[slot];

        // A live slot on the free list means the bookkeeping in Flush/Destroy has drifted; recovering here would
        // hand out a slot that something still believes it owns.
        HP_ASSERT_MSG(!record.Alive, "Free list handed out a live slot");

        // The generation was advanced when the slot was reclaimed (Flush), and Identity is cleared at destruction,
        // so a recycled slot can never inherit its predecessor's Guid.
        record.Alive = true;
        ++m_AliveCount;
        return Entity(slot, record.Generation);
    }

    // The sentinel slot value cannot name an entity, so it bounds the addressable space. Exhausting 4 billion
    // slots is a leak rather than a design limit, but wrapping would silently alias every existing entity —
    // creation must fail loudly and return null instead.
    if (m_Slots.size() >= static_cast<Size>(Entity::NullSlot))
    {
        HP_LOG_ERROR("ECS", "Entity slot space exhausted ({} slots); refusing to create more", m_Slots.size());
        HP_ASSERT_MSG(false, "Entity slot space exhausted");
        return Entity();
    }

    SlotRecord record;
    record.Alive = true;
    record.Generation = 0;
    m_Slots.push_back(record);
    ++m_AliveCount;

    return Entity(static_cast<Core::Index>(m_Slots.size() - 1), 0);
}

void World::DestroyEntity(const Entity& entity)
{
    if (!IsAlive(entity))
    {
        // Scripts call "destroy if still there" constantly and a stale handle is the normal symptom of a
        // reference outliving its target, so this must not be fatal. Logged at warning because a *lot* of these
        // means something upstream is holding references it should have dropped.
        HP_LOG_WARNING("ECS", "DestroyEntity on a non-alive entity ({})", entity.ToString());
        return;
    }

    SlotRecord& record = m_Slots[entity.Slot];
    record.Alive = false;
    --m_AliveCount;

    // Identity is released immediately, with liveness: the Guid must not keep resolving to a dead slot during
    // the rest of the frame, and freeing it now lets a scene reload reuse the same Guid within one tick.
    if (!record.Identity.IsNil())
    {
        m_SlotsByGuid.erase(record.Identity);
        record.Identity = Core::Guid();
    }

    m_PendingDestructions.push_back(entity.Slot);
}

void World::FlushPendingDestructions()
{
    // Checked before the empty-queue shortcut on purpose. A hook that destroyed something leaves the queue
    // non-empty, and a nested flush reaching past this point would swap out and reclaim storage while the outer
    // loop is still walking it — so the guard has to be the first thing that can fire, not the second.
    if (m_IsFlushing)
    {
        HP_LOG_ERROR("ECS", "FlushPendingDestructions called re-entrantly from a destruction handler");
        HP_ASSERT_MSG(false, "Re-entrant world flush");
        return;
    }

    if (m_PendingDestructions.empty())
    {
        return;
    }

    // Swapped out rather than iterated in place: handlers are allowed to destroy more entities, and those must
    // land on a fresh queue instead of extending a loop that is walking the array it appends to. Anything queued
    // during this flush is reclaimed by the next one — predictable, and it keeps one frame's worth of memory
    // alive rather than risking an unbounded teardown cascade.
    std::vector<Core::Index> batch;
    batch.swap(m_PendingDestructions);

    m_IsFlushing = true;

    for (const Core::Index slot : batch)
    {
        SlotRecord& record = m_Slots[slot];

        // A live slot here means the same slot was queued twice, or destroyed during a flush. Both would put a
        // live entity on the free list, which is unrecoverable later.
        HP_ASSERT_MSG(!record.Alive, "Pending destruction queue contained a live slot");

        const Entity entity(static_cast<Core::Index>(slot), record.Generation);

        // Hooks run first and while component storage is still intact, so a handler can read the entity's last
        // state (its collider shape, its audio asset) to release what it owns.
        for (const DestructionHook& hook : m_DestructionHooks)
        {
            hook.Handler(entity, hook.UserData);
        }

        // Components are released after the hooks and before the slot returns to the free list, so no component
        // can outlive its entity and none can be inherited by whatever reuses the slot. Skipping this is the
        // classic ECS leak: the next entity created in this slot starts with the dead one's components attached.
        m_Registry.RemoveFromAll(slot);

        // The generation advances **here**, not at Destroy: handles held across the frame (script registries,
        // job parameters, render overrides) must keep failing IsAlive until the slot is genuinely recycled, and
        // only then start referring to whatever replaces it. That is what makes "the arrow you fired at the
        // goblin now hits the crate that replaced it" impossible rather than merely unlikely.
        ++record.Generation;

        m_FreeSlots.push_back(slot);
    }

    m_IsFlushing = false;
}

void World::RegisterDestructionHandler(DestructionHandler handler, void* userData)
{
    if (handler == nullptr)
    {
        HP_LOG_ERROR("ECS", "Cannot register a null destruction handler");
        HP_ASSERT_MSG(false, "Null destruction handler");
        return;
    }

    m_DestructionHooks.push_back(DestructionHook{handler, userData});
}

void World::UnregisterDestructionHandler(DestructionHandler handler, void* userData)
{
    // Removes every registration of this function/user-data pair. Subsystems tear down once, but a hot-reload
    // path that re-registers without unregistering would otherwise leak hooks and run them repeatedly.
    for (Size index = m_DestructionHooks.size(); index > 0; --index)
    {
        const DestructionHook& hook = m_DestructionHooks[index - 1];
        if (hook.Handler == handler && hook.UserData == userData)
        {
            m_DestructionHooks.erase(m_DestructionHooks.begin() + static_cast<std::ptrdiff_t>(index - 1));
        }
    }
}

void World::Clear()
{
    // Implemented as "destroy everything, then reclaim" rather than dropping the slot array. That keeps one
    // invariant for free: generations never restart. A level unload followed by a load reuses slot 0, and if its
    // generation went back to zero, any handle left behind in a script registry, a save-game preview or a queued
    // job would silently alias a brand-new entity — a bug that only appears on the second level you play, in the
    // worst possible way. Reclaiming through the normal path also means component storage is torn down by exactly
    // one code path instead of two that can drift, and destruction hooks still run so subsystems release state
    // they key by entity.
    //
    // Slot memory itself is retained (bounded by peak concurrent entities), which is what we want between levels:
    // the next load then allocates nothing.
    for (Size index = 0; index < m_Slots.size(); ++index)
    {
        SlotRecord& record = m_Slots[index];
        if (!record.Alive)
        {
            continue;
        }

        record.Alive = false;
        --m_AliveCount;

        if (!record.Identity.IsNil())
        {
            m_SlotsByGuid.erase(record.Identity);
            record.Identity = Core::Guid();
        }

        m_PendingDestructions.push_back(static_cast<Core::Index>(index));
    }

    FlushPendingDestructions();
}

void World::ForEachComponentStorage(const std::function<void(IComponentStorage&)>& callback)
{
    m_Registry.ForEachStorage(callback);
}

void World::ForEachComponentStorage(const std::function<void(const IComponentStorage&)>& callback) const
{
    m_Registry.ForEachStorage(callback);
}

Core::UInt32 World::GetGeneration(Core::Index slot) const noexcept
{
    if (slot >= m_Slots.size())
    {
        return 0;
    }
    return m_Slots[slot].Generation;
}

bool World::IsAlive(const Entity& entity) const noexcept
{
    if (entity.IsNull() || entity.Slot >= m_Slots.size())
    {
        return false;
    }
    const SlotRecord& record = m_Slots[entity.Slot];
    return record.Alive && record.Generation == entity.Generation;
}

Core::Guid World::GetGuid(const Entity& entity) const
{
    if (!IsAlive(entity))
    {
        return Core::Guid();
    }
    return m_Slots[entity.Slot].Identity;
}

Entity World::GetEntityByGuid(const Core::Guid& guid) const
{
    if (guid.IsNil())
    {
        return Entity();
    }

    auto found = m_SlotsByGuid.find(guid);
    if (found == m_SlotsByGuid.end())
    {
        return Entity();
    }

    const Core::Index slot = found->second;
    if (slot >= m_Slots.size())
    {
        // Reachable only if the map and the slot array ever drift apart, which is a bug in this file. Reported
        // rather than trusted, because returning a plausible-looking stale handle would be much harder to trace.
        HP_LOG_ERROR("ECS", "Guid lookup returned an out-of-range slot; identity map is inconsistent");
        HP_ASSERT_MSG(false, "Entity Guid map is inconsistent with the slot array");
        return Entity();
    }

    const SlotRecord& record = m_Slots[slot];
    if (!record.Alive)
    {
        return Entity();
    }

    return Entity(slot, record.Generation);
}

}  // namespace Hollowpoint::ECS
