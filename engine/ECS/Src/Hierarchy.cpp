#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/ECS/Hierarchy.h"

namespace Hollowpoint::ECS
{

const char* Hierarchy::ReparentResultName(ReparentResult result)
{
    switch (result)
    {
        case ReparentResult::Success:
            return "Success";
        case ReparentResult::Unchanged:
            return "Unchanged";
        case ReparentResult::EntityNotAlive:
            return "EntityNotAlive";
        case ReparentResult::CycleWouldForm:
            return "CycleWouldForm";
    }

    HP_ASSERT_MSG(false, "ReparentResult is outside the known range");
    return "?";
}

void Hierarchy::ReserveSlotCapacity(Size slotCount)
{
    if (slotCount > m_Nodes.size())
    {
        // Value-initialised: every link starts InvalidIndex, so a slot that has never been parented needs no
        // separate "is this node real" flag.
        m_Nodes.resize(slotCount);
    }
}

// ---------------------------------------------------------------------------
// Queries.
// ---------------------------------------------------------------------------

bool Hierarchy::HasParent(Index slot) const noexcept
{
    const Node* node = FindNodeChecked(slot);
    return node != nullptr && node->Parent != InvalidIndex;
}

Index Hierarchy::GetParent(Index slot) const noexcept
{
    const Node* node = FindNodeChecked(slot);
    return node != nullptr ? node->Parent : InvalidIndex;
}

Index Hierarchy::GetFirstChild(Index slot) const noexcept
{
    const Node* node = FindNodeChecked(slot);
    return node != nullptr ? node->FirstChild : InvalidIndex;
}

Index Hierarchy::GetLastChild(Index slot) const noexcept
{
    const Node* node = FindNodeChecked(slot);
    return node != nullptr ? node->LastChild : InvalidIndex;
}

Index Hierarchy::GetNextSibling(Index slot) const noexcept
{
    const Node* node = FindNodeChecked(slot);
    return node != nullptr ? node->NextSibling : InvalidIndex;
}

Index Hierarchy::GetPreviousSibling(Index slot) const noexcept
{
    const Node* node = FindNodeChecked(slot);
    return node != nullptr ? node->PreviousSibling : InvalidIndex;
}

bool Hierarchy::IsRoot(Index slot) const noexcept
{
    // A slot outside the tree is not "a root", it is absent — reported rather than answered, because treating a
    // destroyed entity as a top-level node would silently adopt its children into the scene root.
    if (FindNodeChecked(slot) == nullptr)
    {
        return false;
    }
    return !HasParent(slot);
}

bool Hierarchy::IsDescendantOf(Index maybeDescendant, Index ancestor) const noexcept
{
    if (maybeDescendant == InvalidIndex || ancestor == InvalidIndex)
    {
        return false;
    }
    // Strict by definition: a node is not its own descendant. Callers checking "would this create a cycle" rely
    // on the caller-side `slot == newParent` test being separate, which keeps both cases explicit.
    Index current = GetParent(maybeDescendant);
    Core::UInt32 steps = 0;

    while (current != InvalidIndex)
    {
        if (current == ancestor)
        {
            return true;
        }

        // Bounded walk: with a corrupt tree (a cycle introduced by a bug elsewhere) an unbounded parent chase
        // would hang rather than report. Legal trees cannot reach this bound — SetParent rejects cycles.
        if (++steps > MaxWalkSteps)
        {
            HP_LOG_ERROR("ECS", "Hierarchy parent walk exceeded {} steps; the links are corrupt", MaxWalkSteps);
            HP_ASSERT_MSG(false, "Cycle detected while walking hierarchy parents");
            return false;
        }

        current = GetParent(current);
    }

    return false;
}

Core::UInt32 Hierarchy::GetDepth(Index slot) const noexcept
{
    Core::UInt32 depth = 0;
    Index current = GetParent(slot);

    while (current != InvalidIndex)
    {
        if (++depth > MaxWalkSteps)
        {
            HP_LOG_ERROR("ECS", "Hierarchy parent walk exceeded {} steps from slot {}; the links are corrupt",
                         MaxWalkSteps, slot);
            HP_ASSERT_MSG(false, "Hierarchy parent walk exceeded the corruption bound");
            return 0;
        }
        current = GetParent(current);
    }

    return depth;
}

Size Hierarchy::GetChildCount(Index slot) const noexcept
{
    Size count = 0;
    for (Index child = GetFirstChild(slot); child != InvalidIndex; child = GetNextSibling(child))
    {
        ++count;
        if (count > m_Nodes.size())
        {
            // A sibling list longer than any sane scene is a corrupt linked list; stop rather than spin.
            HP_LOG_ERROR("ECS", "Hierarchy sibling chain exceeded a plausible bound; the tree links are corrupt");
            HP_ASSERT_MSG(false, "Corrupt hierarchy sibling chain");
            break;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// Mutation.
// ---------------------------------------------------------------------------

void Hierarchy::AppendChildLocked(Index parent, Index child)
{
    Node& childNode = m_Nodes[child];
    childNode.Parent = parent;
    childNode.PreviousSibling = m_Nodes[parent].LastChild;
    childNode.NextSibling = InvalidIndex;

    if (m_Nodes[parent].LastChild != InvalidIndex)
    {
        m_Nodes[m_Nodes[parent].LastChild].NextSibling = child;
    }
    else
    {
        m_Nodes[parent].FirstChild = child;
    }
    m_Nodes[parent].LastChild = child;
}

void Hierarchy::UnlinkFromParentLocked(Index node)
{
    Node& nodeRecord = m_Nodes[node];
    const Index parent = nodeRecord.Parent;
    if (parent == InvalidIndex)
    {
        return;
    }

    Node& parentRecord = m_Nodes[parent];
    const Index previous = nodeRecord.PreviousSibling;
    const Index next = nodeRecord.NextSibling;

    if (previous != InvalidIndex)
    {
        m_Nodes[previous].NextSibling = next;
    }
    else if (parentRecord.FirstChild == node)
    {
        parentRecord.FirstChild = next;
    }

    if (next != InvalidIndex)
    {
        m_Nodes[next].PreviousSibling = previous;
    }
    else if (parentRecord.LastChild == node)
    {
        parentRecord.LastChild = previous;
    }

    nodeRecord.Parent = InvalidIndex;
    nodeRecord.PreviousSibling = InvalidIndex;
    nodeRecord.NextSibling = InvalidIndex;
}

Hierarchy::ReparentResult Hierarchy::SetParent(Index slot, Index newParent, Size aliveSlotCount)
{
    // Slot bounds first: everything below indexes m_Nodes by these values.
    if (slot >= aliveSlotCount || slot >= m_Nodes.size())
    {
        HP_LOG_WARNING("ECS", "SetParent on an out-of-range slot ({})", slot);
        return ReparentResult::EntityNotAlive;
    }
    if (newParent != InvalidIndex && (newParent >= aliveSlotCount || newParent >= m_Nodes.size()))
    {
        HP_LOG_WARNING("ECS", "SetParent to an out-of-range parent slot ({})", newParent);
        return ReparentResult::EntityNotAlive;
    }

    const Index currentParent = m_Nodes[slot].Parent;
    if (currentParent == newParent)
    {
        // Not an error, and not worth a log line: "reparent to the same parent" happens every time a scene is
        // reloaded over a live world or a script re-applies a saved layout.
        return ReparentResult::Unchanged;
    }

    if (newParent != InvalidIndex)
    {
        // A node cannot be its own parent — the definition of the tree would break, and transform evaluation
        // would loop trying to resolve a matrix that depends on itself.
        if (newParent == slot)
        {
            HP_LOG_WARNING("ECS", "SetParent: entity cannot be its own parent");
            return ReparentResult::CycleWouldForm;
        }

        // Descending from the would-be parent must not reach the node. Without this check the structure becomes a
        // graph and every recursive walk (world-transform composition, recursive destroy, scene save) either loops
        // or overflows — far away from the call that created it.
        if (IsDescendantOf(newParent, slot))
        {
            HP_LOG_WARNING("ECS", "SetParent would create a cycle in the hierarchy");
            return ReparentResult::CycleWouldForm;
        }
    }

    UnlinkFromParentLocked(slot);

    if (newParent != InvalidIndex)
    {
        AppendChildLocked(newParent, slot);
    }

    return ReparentResult::Success;
}

bool Hierarchy::DetachFromParent(Index slot)
{
    if (slot >= m_Nodes.size())
    {
        return false;
    }
    if (m_Nodes[slot].Parent == InvalidIndex)
    {
        return false;
    }

    UnlinkFromParentLocked(slot);
    return true;
}

std::vector<Index> Hierarchy::UnlinkForDestruction(Index slot)
{
    std::vector<Index> orphanedChildren;

    if (slot >= m_Nodes.size())
    {
        return orphanedChildren;
    }

    // Collect the children before unlinking, so the caller can destroy them with the parent or promote them —
    // either way nothing ends up pointing at a slot that gets recycled. Returning them rather than deciding here
    // keeps the policy (destroy-with-parent vs promote-to-grandparent) with the caller, which knows whether this
    // is a level unload, a scripted kill, or a prefab instance being torn down.
    for (Index child = m_Nodes[slot].FirstChild; child != InvalidIndex;)
    {
        const Index next = m_Nodes[child].NextSibling;
        orphanedChildren.push_back(child);
        child = next;
    }

    // Remove the node from its parent first, then clear its own child list. Both ends of every removed link are
    // updated, so no sibling keeps a pointer to a slot that is about to be reused.
    UnlinkFromParentLocked(slot);

    for (const Index child : orphanedChildren)
    {
        m_Nodes[child].Parent = InvalidIndex;
        m_Nodes[child].PreviousSibling = InvalidIndex;
        m_Nodes[child].NextSibling = InvalidIndex;
    }

    m_Nodes[slot].FirstChild = InvalidIndex;
    m_Nodes[slot].LastChild = InvalidIndex;

    return orphanedChildren;
}

void Hierarchy::Clear() noexcept
{
    // Storage is retained (like the world's slot array): between levels this allocates nothing on reload.
    for (Node& node : m_Nodes)
    {
        node = Node{};
    }
}

}  // namespace Hollowpoint::ECS
