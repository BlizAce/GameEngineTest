#pragma once

#include <vector>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Core/Types.h"

namespace Hollowpoint::ECS
{

using Core::Index;
using Core::InvalidIndex;
using Core::Size;

// ---------------------------------------------------------------------------
// Hierarchy — the parent/child tree over entity slots.
//
// Why this is world state and not a component: every entity can participate in the tree, and the engine has to
// maintain its invariants whether or not any game type is loaded. Destroying an entity must unlink it from its
// parent and adopt or destroy its children; if that lived in components, the world would have to know those
// component types exist (breaking the open-type design) or leave dangling child lists behind whenever a plugin
// forgot to register a cleanup hook. Unity, Unreal and Godot all make the scene tree first-class for the same
// reason: it is structural, not optional data.
//
// It stores **topology only** — no transforms. "Keep world position when reparenting" needs matrix math, and this
// module deliberately depends on nothing but Core; the transform system (which does know linear algebra) subscribes
// to reparent events and fixes up local transforms. That split keeps the ECS includable from hot code and testable
// without a maths dependency.
//
// Representation: intrusive doubly-linked sibling lists plus a parent index, rather than a vector of children per
// entity. The cost is cache-unfriendly traversal (hierarchy walks happen once per frame at worst); the gain is no
// allocation per parent, O(1) insert and remove, and — because children are appended and never reordered — an
// iteration order that is stable across runs, which scene files depend on to stay diffable.
//
// Invariants (all asserted where violated):
//   * A node's parent's child list contains it, and sibling links are symmetric.
//   * The tree is acyclic. SetParent rejects a parent that is the node itself or one of its descendants; without
//     that check, world-transform evaluation loops forever and any recursive consumer overflows the stack — both
//     far from the code that created the loop.
//   * A destroyed entity leaves no trace: unlinked from its parent, its own children resolved per policy.
//
// There is deliberately **no cap on tree depth**. Engine walks are iterative, so depth costs time and never stack,
// while a fixed cap rejects legitimate content (long attachment chains, procedurally generated bone stacks) and
// charges every reparent for a depth walk. Depth ends up bounded in practice by the number of entities. An earlier
// draft capped depth at 256; a test building a 20k-deep chain caught it, and the cap was removed rather than
// raised — see notes/EXPERIMENTS.md.
// ---------------------------------------------------------------------------

class Hierarchy
{
public:
    //! Result of a reparent attempt, reported so callers can distinguish "already there" (not an error) from
    //! genuinely invalid requests, which scripts produce constantly.
    enum class ReparentResult : Core::UInt8
    {
        Success,         //!< Parent changed.
        Unchanged,       //!< Already parented exactly as requested.
        EntityNotAlive,  //!< Slot out of range for the world's slot count.
        CycleWouldForm   //!< New parent is this node or one of its descendants.
    };

    [[nodiscard]] static const char* ReparentResultName(ReparentResult result);

    //! Bound on how far any single walk (parent chase, sibling scan) goes before declaring the structure corrupt.
    //! This is **not** a limit on legal tree depth: it exists so that a cycle created by some other bug reports
    //! itself instead of hanging the walk. Sized far above any plausible scene.
    static constexpr Core::UInt32 MaxWalkSteps = 1u << 20;

    //! Grows node storage to cover `slotCount` slots. Called by the world whenever its slot array grows, so node
    //! indices and entity slots stay interchangeable.
    void ReserveSlotCapacity(Size slotCount);

    [[nodiscard]] Size GetNodeCount() const noexcept
    {
        return m_Nodes.size();
    }

    // Queries ---------------------------------------------------------------

    [[nodiscard]] bool HasParent(Index slot) const noexcept;
    [[nodiscard]] Index GetParent(Index slot) const noexcept;

    [[nodiscard]] Index GetFirstChild(Index slot) const noexcept;
    [[nodiscard]] Index GetLastChild(Index slot) const noexcept;
    [[nodiscard]] Index GetNextSibling(Index slot) const noexcept;
    [[nodiscard]] Index GetPreviousSibling(Index slot) const noexcept;

    //! True when the slot is part of the tree and has no parent. A slot that has never been touched is *absent*,
    //! not a root — conflating the two would silently adopt destroyed entities into the scene root.
    [[nodiscard]] bool IsRoot(Index slot) const noexcept;

    //! True when `maybeDescendant` is a descendant of `ancestor` (strict: a node is not its own descendant).
    [[nodiscard]] bool IsDescendantOf(Index maybeDescendant, Index ancestor) const noexcept;

    //! Distance to the root in edges (0 for a top-level entity). O(depth): code that needs depth for many entities
    //! per frame should cache it instead of querying. The walk is bounded only as a corruption guard.
    [[nodiscard]] Core::UInt32 GetDepth(Index slot) const noexcept;

    //! Number of direct children.
    [[nodiscard]] Size GetChildCount(Index slot) const noexcept;

    // Mutation --------------------------------------------------------------

    //! Reparents `slot` under `newParent` (InvalidIndex detaches it to the root level).
    //! Validation runs before anything is modified, so a rejected call leaves the tree untouched.
    ReparentResult SetParent(Index slot, Index newParent, Size aliveSlotCount);

    //! Removes `slot` from its parent's child list without touching its own children (they stay attached to it).
    bool DetachFromParent(Index slot);

    //! Unlinks a node that is being destroyed: removes it from its parent and detaches all of its own children,
    //! returning them so the caller can decide their fate (destroy with the parent, or promote to grandparent).
    //! After this the node has no parent and no children, so nothing can reach a dead entity through the tree.
    std::vector<Index> UnlinkForDestruction(Index slot);

    void Clear() noexcept;

private:
    // Declared before the accessors that mention it: a member function's return type is not a complete-class
    // context, so a nested type cannot be used before its declaration.
    struct Node
    {
        Index Parent = InvalidIndex;
        Index FirstChild = InvalidIndex;
        Index LastChild = InvalidIndex;
        Index NextSibling = InvalidIndex;
        Index PreviousSibling = InvalidIndex;
    };

    //! Node for a slot, or nullptr when the slot has never been part of the tree. Every query goes through this
    //! so an out-of-range slot reports "absent" instead of reading past the array.
    [[nodiscard]] const Node* FindNodeChecked(Index slot) const noexcept
    {
        return slot < m_Nodes.size() ? &m_Nodes[slot] : nullptr;
    }

    void AppendChildLocked(Index parent, Index child);
    void UnlinkFromParentLocked(Index node);

    std::vector<Node> m_Nodes;
};

}  // namespace Hollowpoint::ECS
