#include "Hollowpoint/ECS/ComponentStorage.h"

namespace Hollowpoint::ECS
{

void ComponentRegistry::ForEachStorage(const std::function<void(IComponentStorage&)>& callback)
{
    for (const auto& entry : m_Storages)
    {
        callback(*entry.second);
    }
}

void ComponentRegistry::ForEachStorage(const std::function<void(const IComponentStorage&)>& callback) const
{
    for (const auto& entry : m_Storages)
    {
        callback(*entry.second);
    }
}

void ComponentRegistry::RemoveFromAll(Core::Index slot)
{
    // Removes the slot from every component array. Iterating all types is the point: the world does not know
    // which components an entity had, and a plugin can add a type that did not exist when this code was written.
    // Storages are only ever added or removed at registration, never during this loop, so direct iteration is
    // safe even though RemoveAt mutates each storage's internals.
    for (const auto& entry : m_Storages)
    {
        entry.second->RemoveAt(slot);
    }
}

void ComponentRegistry::ClearAll()
{
    for (const auto& entry : m_Storages)
    {
        entry.second->ClearAll();
    }
}

}  // namespace Hollowpoint::ECS
