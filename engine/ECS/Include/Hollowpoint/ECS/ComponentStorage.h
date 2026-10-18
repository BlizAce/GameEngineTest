#pragma once

#include <functional>
#include <map>
#include <memory>
#include <typeindex>
#include <vector>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/ECS/Detail/SparseSet.h"
#include "Hollowpoint/ECS/Entity.h"

namespace Hollowpoint::ECS
{

using Core::Size;

// ---------------------------------------------------------------------------
// IComponentStorage — type-erased component array.
//
// The world stores components by type so it can free an entity's components without knowing any of their types
// (and so a plugin can introduce a component the engine has never seen). One virtual call per operation is the
// cost; systems that iterate do not pay it, because they cache the concrete storage pointer once per pass.
//
// The interface exposes *addresses and sizes*, never typed values. That is deliberate: the generic consumers
// (serializer writing a scene, inspector drawing fields) work through Reflection descriptors anyway, and giving
// them a byte address plus a type name is enough to resolve one. The alternative — void* "scalar" entry points
// on this interface — would move an unchecked cast from one obvious place into the base class API, where every
// future implementer has to get it right.
// ---------------------------------------------------------------------------

class IComponentStorage
{
public:
    virtual ~IComponentStorage() = default;

    //! Registration name of the component type ("MeshRenderer"). Written into scene files, so it must be stable.
    [[nodiscard]] virtual const char* GetTypeName() const = 0;

    [[nodiscard]] virtual Size GetCount() const = 0;
    [[nodiscard]] virtual bool Contains(Core::Index slot) const = 0;
    [[nodiscard]] virtual Size GetElementSize() const = 0;

    //! Component address for `slot`, or nullptr when the entity does not have one.
    [[nodiscard]] virtual void* FindAddress(Core::Index slot) = 0;
    [[nodiscard]] virtual const void* FindAddress(Core::Index slot) const = 0;

    //! Removes the component from one entity (called for every type when an entity is reclaimed). Invalidates
    //! dense indices beyond the removal point, as any sparse-set removal does.
    virtual void RemoveAt(Core::Index slot) = 0;

    virtual void ClearAll() = 0;
    virtual void ReserveSlots(Core::Index slotCapacity) = 0;
};

// ---------------------------------------------------------------------------
// ComponentStorage<TComponent> — the sparse set backing one component type.
// ---------------------------------------------------------------------------

template <typename TComponent>
class ComponentStorage final : public IComponentStorage
{
public:
    //! `typeName` comes from registration (a readable, stable string) rather than typeid, because scene files
    //! and the inspector must show engine names, not compiler-mangled ones.
    explicit ComponentStorage(const char* typeName) : m_TypeName(typeName) {}

    // IComponentStorage ------------------------------------------------------

    [[nodiscard]] const char* GetTypeName() const override
    {
        return m_TypeName;
    }
    [[nodiscard]] Size GetCount() const override
    {
        return m_Set.GetCount();
    }
    [[nodiscard]] bool Contains(Core::Index slot) const override
    {
        return m_Set.Contains(slot);
    }
    [[nodiscard]] Size GetElementSize() const override
    {
        return sizeof(TComponent);
    }

    [[nodiscard]] void* FindAddress(Core::Index slot) override
    {
        return m_Set.Find(slot);
    }
    [[nodiscard]] const void* FindAddress(Core::Index slot) const override
    {
        return m_Set.Find(slot);
    }

    void RemoveAt(Core::Index slot) override
    {
        m_Set.Remove(slot);
    }
    void ClearAll() override
    {
        m_Set.Clear();
    }
    void ReserveSlots(Core::Index slotCapacity) override
    {
        m_Set.ReserveSlots(slotCapacity);
    }

    // Typed access -----------------------------------------------------------

    [[nodiscard]] TComponent* Find(Core::Index slot)
    {
        return m_Set.Find(slot);
    }
    [[nodiscard]] const TComponent* Find(Core::Index slot) const
    {
        return m_Set.Find(slot);
    }

    //! Default-constructs the component if absent and returns it. Idempotent: an existing component is returned
    //! untouched, because systems call this every frame in some patterns and resetting accumulated state (health,
    //! timers) mid-game would be a silent corruption.
    [[nodiscard]] TComponent& GetOrCreate(Core::Index slot)
    {
        return *m_Set.Insert(slot);
    }

    [[nodiscard]] TComponent& GetOrAssign(Core::Index slot, const TComponent& value)
    {
        return *m_Set.Insert(slot, value);
    }

    bool Remove(Core::Index slot)
    {
        return m_Set.Remove(slot);
    }

    [[nodiscard]] Detail::SparseSet<TComponent>& GetStorage() noexcept
    {
        return m_Set;
    }
    [[nodiscard]] const Detail::SparseSet<TComponent>& GetStorage() const noexcept
    {
        return m_Set;
    }

private:
    const char* m_TypeName = nullptr;
    Detail::SparseSet<TComponent> m_Set;
};

// ---------------------------------------------------------------------------
// ComponentRegistry — the component arrays belonging to one world.
//
// Why a registry rather than a member per type: the set of component types is open (game code, plugins, and
// eventually script-defined components), and reclaiming an entity needs "remove this slot from every array" —
// which requires iterating types the engine does not know at compile time.
// ---------------------------------------------------------------------------

class ComponentRegistry
{
public:
    //! Storage for TComponent, created on first use.
    template <typename TComponent>
    [[nodiscard]] ComponentStorage<TComponent>* GetOrCreateStorage(const char* typeName = nullptr);

    template <typename TComponent>
    [[nodiscard]] const ComponentStorage<TComponent>* FindStorage() const;

    template <typename TComponent>
    [[nodiscard]] ComponentStorage<TComponent>* FindStorage();

    //! True when any entity has a TComponent. Lets a system skip its pass entirely instead of setting up an
    //! empty iteration — worth having because a given level uses a fraction of the registered types.
    template <typename TComponent>
    [[nodiscard]] bool HasAny() const;

    void ForEachStorage(const std::function<void(IComponentStorage&)>& callback);
    void ForEachStorage(const std::function<void(const IComponentStorage&)>& callback) const;

    //! Removes `slot` from every component array — the "this entity is gone" path.
    void RemoveFromAll(Core::Index slot);

    void ClearAll();

    [[nodiscard]] Size GetStorageCount() const noexcept
    {
        return m_Storages.size();
    }

private:
    using StoragePtr = std::unique_ptr<IComponentStorage>;

    // Keyed by std::type_index. A map rather than an unordered_map: lookups happen once per system pass (the
    // concrete pointer is then cached for the loop), so the tree walk is never hot, and a sorted container keeps
    // ForEachStorage order deterministic — which makes scene files byte-identical between runs. That last point
    // matters more than it sounds: nondeterministic save output turns every diff of a level file into noise.
    std::map<std::type_index, StoragePtr> m_Storages;
};

// ---------------------------------------------------------------------------
// Template definitions
// ---------------------------------------------------------------------------

template <typename TComponent>
ComponentStorage<TComponent>* ComponentRegistry::GetOrCreateStorage(const char* typeName)
{
    const auto existing = m_Storages.find(std::type_index(typeid(TComponent)));
    if (existing != m_Storages.end())
    {
        return static_cast<ComponentStorage<TComponent>*>(existing->second.get());
    }

    auto storage = std::make_unique<ComponentStorage<TComponent>>(typeName != nullptr ? typeName : "Component");
    auto* raw = storage.get();
    m_Storages.emplace(std::type_index(typeid(TComponent)), std::move(storage));
    return raw;
}

template <typename TComponent>
const ComponentStorage<TComponent>* ComponentRegistry::FindStorage() const
{
    const auto found = m_Storages.find(std::type_index(typeid(TComponent)));
    if (found == m_Storages.end())
    {
        return nullptr;
    }
    return static_cast<const ComponentStorage<TComponent>*>(found->second.get());
}

template <typename TComponent>
ComponentStorage<TComponent>* ComponentRegistry::FindStorage()
{
    const auto found = m_Storages.find(std::type_index(typeid(TComponent)));
    if (found == m_Storages.end())
    {
        return nullptr;
    }
    return static_cast<ComponentStorage<TComponent>*>(found->second.get());
}

template <typename TComponent>
bool ComponentRegistry::HasAny() const
{
    const ComponentStorage<TComponent>* storage = FindStorage<TComponent>();
    return storage != nullptr && storage->GetCount() > 0;
}

}  // namespace Hollowpoint::ECS
