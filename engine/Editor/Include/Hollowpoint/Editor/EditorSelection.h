#pragma once

#include <vector>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/ECS/Entity.h"

// ---------------------------------------------------------------------------
// EditorSelection — which entities the editor has selected, and nothing else.
//
// Deliberately not a component and not stored in the world: selection is transient tool state, never serialized, and
// an entity that is destroyed must leave the selection without the world having to notify anyone (the check is
// liveness at read time — see Prune). Putting it in the ECS would also mean every scene save could pick up a
// "Selected" flag nobody asked for.
//
// Why it is its own type rather than a member of the editor UI: outliner, viewport picking, the details panel and the
// console all read and write selection, and each of them needs to agree on one set of rules (primary vs secondary,
// dedup, dead-entity removal). Those rules are worth testing without a window; `editor.select` / `editor.clear` exist
// as console commands precisely so an agent can drive selection with no UI at all.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Editor
{

using Core::Index;
using Core::Size;
using ECS::Entity;

//! An ordered set of selected entities, with the notion of one primary.
/**
 * Order is insertion order and the primary is the last selected, which is what UE-style editors do (the primary is
 * where a gizmo appears and what "rename" applies to). Kept explicit rather than "back() by convention" because the
 * console needs to report it and a test must be able to assert on it.
 */
class HP_EDITOR_API EditorSelection
{
public:
    //! Selects `entity` as the only selection, clearing any previous one. Selecting an already-selected entity keeps it
    //  primary without churning the list (a click-and-drag over one entity must not reorder the outliner under the
    //  cursor). Returns true when anything changed.
    bool Select(const Entity& entity);

    //! Adds to the selection, making `entity` primary. Duplicates are ignored. Returns true when anything changed.
    bool AddToSelection(const Entity& entity);

    //! Toggles membership (Ctrl-click). Removing the primary makes the new last entry primary; an empty result leaves
    //! no
    //  primary. Returns true when anything changed.
    bool Toggle(const Entity& entity);

    //! Removes one entity. Selecting nothing is legal and is what `editor.clear` does. Returns true on change.
    bool Remove(const Entity& entity);

    //! Drops every selection without touching the world.
    void Clear() noexcept;

    [[nodiscard]] bool IsSelected(const Entity& entity) const noexcept;

    //! The primary selection — the last one added, or a null handle when empty. Named rather than `GetPrimary`
    //  returning an optional because Entity is already nullable-by-sentinel (Entity::IsNull), and inventing a second
    //  "no value" type over a type that has one is two ways to ask the same question.
    [[nodiscard]] Entity GetPrimary() const noexcept;

    [[nodiscard]] Size GetCount() const noexcept
    {
        return m_Entities.size();
    }

    [[nodiscard]] bool IsEmpty() const noexcept
    {
        return m_Entities.empty();
    }

    [[nodiscard]] const std::vector<Entity>& GetEntities() const noexcept
    {
        return m_Entities;
    }

    //! Forgets entities `isAlive` rejects. Called once per frame by the editor: an entity destroyed by a script, by
    //  undo, or by loading a different scene would otherwise leave the details panel editing memory that now belongs to
    //  a recycled slot — which is not a crash (the ECS checks liveness) but a panel showing an unrelated entity's
    //  data, i.e. a bug report that reads as corruption. Returns how many were dropped.
    template <typename TIsAlive>
    Size Prune(TIsAlive&& isAlive)
    {
        Size removed = 0;
        for (Size index = m_Entities.size(); index > 0; --index)
        {
            if (!isAlive(m_Entities[index - 1]))
            {
                m_Entities.erase(m_Entities.begin() + static_cast<Index>(index - 1));
                ++removed;
            }
        }
        return removed;
    }

private:
    std::vector<Entity> m_Entities;  //!< Insertion order; back() is the primary.
};

}  // namespace Hollowpoint::Editor
