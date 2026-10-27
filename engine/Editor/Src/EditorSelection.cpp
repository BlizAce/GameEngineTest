#include "Hollowpoint/Editor/EditorSelection.h"

namespace Hollowpoint::Editor
{

bool EditorSelection::Select(const Entity& entity)
{
    // A null handle is never selectable: the callers that could pass one (a pick that missed, a console command given a
    // slot that no longer exists) mean "select nothing", and Select(null) doing that silently would hide a caller bug
    // from its author. Clearing here is also what makes `editor.select` with no valid argument behave like a miss.
    if (entity.IsNull())
    {
        return !m_Entities.empty() && (Clear(), true);
    }

    if (m_Entities.size() == 1 && m_Entities[0] == entity)
    {
        return false;  // already the sole selection — no change, so no repaint churn
    }

    m_Entities.clear();
    m_Entities.push_back(entity);
    return true;
}

bool EditorSelection::AddToSelection(const Entity& entity)
{
    if (entity.IsNull())
    {
        return false;
    }

    for (const Entity& existing : m_Entities)
    {
        if (existing == entity)
        {
            // Already a member. Not promoted to primary: re-clicking an item in a multi-selection must not reorder the
            // list under the cursor, and "which one is primary" visibly drives the gizmo position.
            return false;
        }
    }

    m_Entities.push_back(entity);
    return true;
}

bool EditorSelection::Toggle(const Entity& entity)
{
    if (entity.IsNull())
    {
        return false;
    }

    for (Size index = 0; index < m_Entities.size(); ++index)
    {
        if (m_Entities[index] == entity)
        {
            m_Entities.erase(m_Entities.begin() + static_cast<Index>(index));
            return true;
        }
    }

    m_Entities.push_back(entity);
    return true;
}

bool EditorSelection::Remove(const Entity& entity)
{
    for (Size index = 0; index < m_Entities.size(); ++index)
    {
        if (m_Entities[index] == entity)
        {
            m_Entities.erase(m_Entities.begin() + static_cast<Index>(index));
            return true;
        }
    }
    return false;
}

void EditorSelection::Clear() noexcept
{
    m_Entities.clear();
}

bool EditorSelection::IsSelected(const Entity& entity) const noexcept
{
    for (const Entity& existing : m_Entities)
    {
        if (existing == entity)
        {
            return true;
        }
    }
    return false;
}

Entity EditorSelection::GetPrimary() const noexcept
{
    return m_Entities.empty() ? Entity{} : m_Entities.back();
}

}  // namespace Hollowpoint::Editor
