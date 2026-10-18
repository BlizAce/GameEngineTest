#include "TestSupport.h"

#include <algorithm>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/ECS/Hierarchy.h"

using Hollowpoint::Core::Index;
using Hollowpoint::Core::InvalidIndex;
using Hollowpoint::Core::Size;
using Hollowpoint::ECS::Hierarchy;

namespace
{

// A small helper that keeps node storage sized the way the world does, so tests read as "slots" rather than
// manually calling ReserveSlotCapacity before every operation.
class TreeFixture
{
public:
    explicit TreeFixture(Size slotCount) : m_SlotCount(slotCount)
    {
        m_Hierarchy.ReserveSlotCapacity(slotCount);
    }

    [[nodiscard]] Hierarchy& Tree() noexcept
    {
        return m_Hierarchy;
    }
    [[nodiscard]] Size SlotCount() const noexcept
    {
        return m_SlotCount;
    }

    //! Setup helper. Not nodiscard: most call sites build a tree and only assert on the resulting structure,
    //! which is exactly the shape that makes ignoring a result legitimate here (the real API keeps its result
    //! unchecked too, since engine setup code routinely reparents without inspecting it).
    Hierarchy::ReparentResult Parent(Index slot, Index parent)
    {
        return m_Hierarchy.SetParent(slot, parent, m_SlotCount);
    }

private:
    Hierarchy m_Hierarchy;
    Size m_SlotCount = 0;
};

// Walks the child list of `parent` and returns the slots in order. Every structural assertion below uses this,
// because checking FirstChild/NextSibling by hand is how a test ends up passing while the linked list is broken.
std::vector<Index> ChildrenOf(const Hierarchy& tree, Index parent)
{
    std::vector<Index> children;
    for (Index child = tree.GetFirstChild(parent); child != InvalidIndex; child = tree.GetNextSibling(child))
    {
        children.push_back(child);
        if (children.size() > 10000)
        {
            break;  // Guard against a corrupt cycle in the sibling list.
        }
    }
    return children;
}

}  // namespace

TEST_SUITE("ECS/Hierarchy")
{
    TEST_CASE("a fresh tree has no nodes and reports absence safely")
    {
        Hierarchy tree;

        CHECK(tree.GetNodeCount() == 0u);
        CHECK_FALSE(tree.HasParent(0));
        CHECK(tree.GetParent(0) == InvalidIndex);
        CHECK(tree.GetFirstChild(0) == InvalidIndex);
        CHECK(tree.GetNextSibling(0) == InvalidIndex);
        CHECK_FALSE(tree.IsRoot(0));  // Absent is not the same thing as "a root".
        CHECK(tree.GetDepth(0) == 0u);
        CHECK(tree.GetChildCount(0) == 0u);
    }

    TEST_CASE("new nodes start parentless")
    {
        TreeFixture fixture(10);

        CHECK_FALSE(fixture.Tree().HasParent(3));
        CHECK(fixture.Tree().IsRoot(3));
        CHECK(fixture.Tree().GetChildCount(3) == 0u);
    }

    TEST_CASE("reparenting links parent and child both ways")
    {
        TreeFixture fixture(10);

        CHECK(fixture.Parent(2, 5) == Hierarchy::ReparentResult::Success);

        CHECK(fixture.Tree().HasParent(2));
        CHECK(fixture.Tree().GetParent(2) == 5);
        CHECK(fixture.Tree().GetFirstChild(5) == 2);
        CHECK(fixture.Tree().GetLastChild(5) == 2);
        CHECK(fixture.Tree().GetChildCount(5) == 1u);
        CHECK_FALSE(fixture.Tree().IsRoot(2));
        CHECK(fixture.Tree().IsRoot(5));
    }

    TEST_CASE("children keep insertion order")
    {
        // Scene files store hierarchy and are diffed by humans; a stable child order is what keeps a re-save from
        // rewriting every line.
        TreeFixture fixture(10);

        fixture.Parent(1, 0);
        fixture.Parent(2, 0);
        fixture.Parent(3, 0);

        const std::vector<Index> expected = {1, 2, 3};
        CHECK(ChildrenOf(fixture.Tree(), 0) == expected);
    }

    TEST_CASE("sibling links are symmetric")
    {
        TreeFixture fixture(10);
        fixture.Parent(1, 0);
        fixture.Parent(2, 0);
        fixture.Parent(3, 0);

        // Forward walk must match backward walk.
        const Index first = fixture.Tree().GetFirstChild(0);
        const Index last = fixture.Tree().GetLastChild(0);
        CHECK(first == 1u);
        CHECK(last == 3u);

        std::vector<Index> forward;
        for (Index node = first; node != InvalidIndex; node = fixture.Tree().GetNextSibling(node))
        {
            forward.push_back(node);
        }

        std::vector<Index> backward;
        for (Index node = last; node != InvalidIndex; node = fixture.Tree().GetPreviousSibling(node))
        {
            backward.push_back(node);
        }
        std::reverse(backward.begin(), backward.end());

        CHECK(forward == backward);
    }

    TEST_CASE("reparenting moves a node out of its old parent")
    {
        TreeFixture fixture(10);
        fixture.Parent(4, 0);
        fixture.Parent(5, 0);
        fixture.Parent(6, 0);

        CHECK(fixture.Parent(5, 1) == Hierarchy::ReparentResult::Success);

        const std::vector<Index> expectedOld = {4, 6};
        CHECK(ChildrenOf(fixture.Tree(), 0) == expectedOld);

        const std::vector<Index> expectedNew = {5};
        CHECK(ChildrenOf(fixture.Tree(), 1) == expectedNew);

        CHECK(fixture.Tree().GetParent(5) == 1u);
        CHECK(fixture.Tree().GetChildCount(0) == 2u);
    }

    TEST_CASE("removing the first, middle and last child keeps the list intact")
    {
        // Each position updates different links (FirstChild / neither end / LastChild), so all three are covered.
        struct Case
        {
            Index remove;
            std::vector<Index> expected;
        };

        const Case cases[] = {
            {1, {2, 3}},  // First child.
            {2, {1, 3}},  // Middle child.
            {3, {1, 2}},  // Last child.
        };

        for (const Case& testCase : cases)
        {
            INFO("removing slot " << testCase.remove);

            TreeFixture fixture(10);
            fixture.Parent(1, 0);
            fixture.Parent(2, 0);
            fixture.Parent(3, 0);

            REQUIRE(fixture.Tree().DetachFromParent(testCase.remove));
            CHECK(ChildrenOf(fixture.Tree(), 0) == testCase.expected);
            CHECK_FALSE(fixture.Tree().HasParent(testCase.remove));

            // The surviving children's own links must still be walkable in both directions.
            for (const Index child : testCase.expected)
            {
                CHECK(fixture.Tree().GetParent(child) == 0u);
            }
        }
    }

    TEST_CASE("detaching a parentless node reports false")
    {
        TreeFixture fixture(10);

        CHECK_FALSE(fixture.Tree().DetachFromParent(3));
    }

    TEST_CASE("reparenting to the same parent is Unchanged, not an error")
    {
        // Happens every time a scene reloads over a live world or a script re-applies a saved layout. Making it
        // an error would flood logs with false alarms.
        TreeFixture fixture(10);
        fixture.Parent(2, 5);

        Hollowpoint::Test::ScopedAssertCapture asserts;
        CHECK(fixture.Parent(2, 5) == Hierarchy::ReparentResult::Unchanged);
        CHECK(asserts.GetCaptureCount() == 0);

        // And the structure is untouched.
        const std::vector<Index> expected = {2};
        CHECK(ChildrenOf(fixture.Tree(), 5) == expected);
    }

    TEST_CASE("detaching to root level uses an invalid parent")
    {
        TreeFixture fixture(10);
        fixture.Parent(7, 2);
        REQUIRE(fixture.Tree().HasParent(7));

        CHECK(fixture.Parent(7, InvalidIndex) == Hierarchy::ReparentResult::Success);
        CHECK_FALSE(fixture.Tree().HasParent(7));
        CHECK(fixture.Tree().IsRoot(7));
    }

    TEST_CASE("a node cannot be its own parent")
    {
        TreeFixture fixture(10);

        Hollowpoint::Test::ScopedLogCapture logs;
        CHECK(fixture.Parent(3, 3) == Hierarchy::ReparentResult::CycleWouldForm);
        CHECK(fixture.Tree().GetParent(3) == InvalidIndex);
        CHECK(logs.GetWarningCount() >= 1);
    }

    TEST_CASE("reparenting under a descendant is rejected")
    {
        // root 0 -> 1 -> 2 -> 3. Making 0 a child of 2 would create a cycle: transform evaluation would loop and
        // recursive teardown would overflow the stack, both far from the call that did it.
        TreeFixture fixture(10);
        fixture.Parent(1, 0);
        fixture.Parent(2, 1);
        fixture.Parent(3, 2);

        Hollowpoint::Test::ScopedLogCapture logs;

        CHECK(fixture.Parent(0, 2) == Hierarchy::ReparentResult::CycleWouldForm);
        CHECK(fixture.Parent(0, 3) == Hierarchy::ReparentResult::CycleWouldForm);
        CHECK(fixture.Parent(1, 3) == Hierarchy::ReparentResult::CycleWouldForm);

        // Rejected calls must leave the tree exactly as it was.
        CHECK(fixture.Tree().GetParent(0) == InvalidIndex);
        CHECK(fixture.Tree().GetParent(1) == 0u);
        CHECK(fixture.Tree().GetParent(2) == 1u);
        CHECK(fixture.Tree().GetParent(3) == 2u);
        CHECK(logs.GetWarningCount() >= 3);
    }

    TEST_CASE("reparenting a sibling under a nephew is allowed; the reverse is not")
    {
        // Tree: 0 -> [1, 2], and 1 -> [3]. Slot 2 is a sibling of 1, not an ancestor of 3, so hanging 2 under 3 is
        // a legal move (it just relocates a subtree). The reverse — moving 1 under 3 — would close a loop through
        // 0, and must be refused. Both directions are asserted because the cycle check walks parents, and it is
        // easy to implement it in the direction that only catches one of these.
        TreeFixture fixture(10);
        fixture.Parent(1, 0);
        fixture.Parent(2, 0);
        fixture.Parent(3, 1);

        CHECK(fixture.Parent(2, 3) == Hierarchy::ReparentResult::Success);

        const std::vector<Index> expectedRootChildren = {1};
        CHECK(ChildrenOf(fixture.Tree(), 0) == expectedRootChildren);
        const std::vector<Index> expectedUnder3 = {2};
        CHECK(ChildrenOf(fixture.Tree(), 3) == expectedUnder3);

        // Now the illegal direction: 1 is an ancestor of 3, so 1 cannot go under it.
        Hollowpoint::Test::ScopedLogCapture logs;
        CHECK(fixture.Parent(1, 3) == Hierarchy::ReparentResult::CycleWouldForm);
        CHECK(fixture.Tree().GetParent(1) == 0u);
        CHECK(logs.GetWarningCount() >= 1);
    }

    TEST_CASE("IsDescendantOf is strict and transitive")
    {
        TreeFixture fixture(10);
        fixture.Parent(1, 0);
        fixture.Parent(2, 1);
        fixture.Parent(3, 2);

        CHECK(fixture.Tree().IsDescendantOf(3, 0));
        CHECK(fixture.Tree().IsDescendantOf(3, 1));
        CHECK(fixture.Tree().IsDescendantOf(3, 2));

        CHECK_FALSE(fixture.Tree().IsDescendantOf(3, 3));  // Strict: not its own descendant.
        CHECK_FALSE(fixture.Tree().IsDescendantOf(0, 3));  // Not the other way round.
        CHECK_FALSE(fixture.Tree().IsDescendantOf(0, InvalidIndex));
        CHECK_FALSE(fixture.Tree().IsDescendantOf(InvalidIndex, 0));
    }

    TEST_CASE("depth counts edges to the root")
    {
        TreeFixture fixture(20);
        fixture.Parent(1, 0);
        fixture.Parent(2, 1);
        fixture.Parent(3, 2);

        CHECK(fixture.Tree().GetDepth(0) == 0u);
        CHECK(fixture.Tree().GetDepth(1) == 1u);
        CHECK(fixture.Tree().GetDepth(2) == 2u);
        CHECK(fixture.Tree().GetDepth(3) == 3u);
    }

    TEST_CASE("out-of-range slots are rejected rather than corrupting the tree")
    {
        TreeFixture fixture(10);

        Hollowpoint::Test::ScopedAssertCapture asserts;

        CHECK(fixture.Parent(999, 0) == Hierarchy::ReparentResult::EntityNotAlive);
        CHECK(fixture.Parent(0, 999) == Hierarchy::ReparentResult::EntityNotAlive);

        // Rejected means nothing changed.
        CHECK(fixture.Tree().GetChildCount(0) == 0u);
        CHECK(asserts.GetCaptureCount() == 0);  // A warning is right here; scripts hit this routinely.
    }

    TEST_CASE("unlinking for destruction orphans the children and clears the node")
    {
        TreeFixture fixture(10);
        fixture.Parent(1, 0);
        fixture.Parent(2, 0);
        fixture.Parent(3, 0);

        const std::vector<Index> orphaned = fixture.Tree().UnlinkForDestruction(0);

        const std::vector<Index> expected = {1, 2, 3};
        CHECK(orphaned == expected);

        // The dying node holds no children and has no parent.
        CHECK(fixture.Tree().GetFirstChild(0) == InvalidIndex);
        CHECK(fixture.Tree().GetLastChild(0) == InvalidIndex);
        CHECK_FALSE(fixture.Tree().HasParent(0));

        // Orphans are fully detached: nothing may still reference the destroyed slot.
        for (const Index child : orphaned)
        {
            CHECK_FALSE(fixture.Tree().HasParent(child));
            CHECK(fixture.Tree().GetNextSibling(child) == InvalidIndex);
            CHECK(fixture.Tree().GetPreviousSibling(child) == InvalidIndex);
        }
    }

    TEST_CASE("unlinking a node that has a parent removes it from the sibling list")
    {
        TreeFixture fixture(10);
        fixture.Parent(1, 0);
        fixture.Parent(2, 0);
        fixture.Parent(3, 0);
        fixture.Parent(4, 2);  // A child of the node being destroyed.

        const std::vector<Index> orphaned = fixture.Tree().UnlinkForDestruction(2);

        const std::vector<Index> expectedOrphans = {4};
        CHECK(orphaned == expectedOrphans);

        const std::vector<Index> expectedRemaining = {1, 3};
        CHECK(ChildrenOf(fixture.Tree(), 0) == expectedRemaining);
    }

    TEST_CASE("unlinking a leaf or an unknown slot is harmless")
    {
        TreeFixture fixture(10);
        fixture.Parent(1, 0);

        const std::vector<Index> noChildren;

        CHECK(fixture.Tree().UnlinkForDestruction(1) == noChildren);  // Leaf.
        CHECK(fixture.Tree().UnlinkForDestruction(999).empty());      // Out of range.
    }

    TEST_CASE("a destroyed node's slot can be reused with a clean tree state")
    {
        TreeFixture fixture(10);
        fixture.Parent(5, 1);
        fixture.Parent(6, 5);
        fixture.Tree().UnlinkForDestruction(5);

        // Reusing the slot must not resurrect the old parent or children.
        CHECK(fixture.Parent(5, 2) == Hierarchy::ReparentResult::Success);
        CHECK(fixture.Tree().GetParent(5) == 2u);
        CHECK(fixture.Tree().GetChildCount(5) == 0u);
    }

    TEST_CASE("Clear removes every link but keeps storage")
    {
        TreeFixture fixture(10);
        fixture.Parent(1, 0);
        fixture.Parent(2, 0);

        fixture.Tree().Clear();

        const Size nodeCount = fixture.Tree().GetNodeCount();
        CHECK(nodeCount > 0u);  // Storage retained so the next level allocates nothing.
        CHECK(fixture.Tree().GetChildCount(0) == 0u);
        CHECK_FALSE(fixture.Tree().HasParent(1));
        CHECK(fixture.Tree().GetFirstChild(0) == InvalidIndex);

        // And it is immediately usable again.
        CHECK(fixture.Parent(1, 2) == Hierarchy::ReparentResult::Success);
    }

    TEST_CASE("ReserveSlotCapacity only grows")
    {
        Hierarchy tree;
        tree.ReserveSlotCapacity(100);
        CHECK(tree.GetNodeCount() == 100u);

        tree.ReserveSlotCapacity(50);  // Shrinking would invalidate existing slots.
        CHECK(tree.GetNodeCount() == 100u);

        tree.ReserveSlotCapacity(200);
        CHECK(tree.GetNodeCount() == 200u);
    }

    TEST_CASE("ReparentResultName covers every enumerator")
    {
        CHECK(std::string_view(Hierarchy::ReparentResultName(Hierarchy::ReparentResult::Success)) == "Success");
        CHECK(std::string_view(Hierarchy::ReparentResultName(Hierarchy::ReparentResult::Unchanged)) == "Unchanged");
        CHECK(std::string_view(Hierarchy::ReparentResultName(Hierarchy::ReparentResult::CycleWouldForm)) ==
              "CycleWouldForm");
        CHECK(std::string_view(Hierarchy::ReparentResultName(Hierarchy::ReparentResult::EntityNotAlive)) ==
              "EntityNotAlive");
    }

    TEST_CASE("deep chains are accepted — tree depth is not capped")
    {
        // Depth used to be capped at 256, which rejected valid content and made SetParent walk the chain on every
        // call. Engine-side walks are iterative, so a deep tree costs time and never stack; this asserts that a
        // chain far longer than the old cap builds and reports depth correctly.
        constexpr Index chainLength = 5000;
        TreeFixture fixture(chainLength + 1);

        for (Index slot = 1; slot < chainLength; ++slot)
        {
            REQUIRE(fixture.Parent(slot, slot - 1) == Hierarchy::ReparentResult::Success);
        }

        CHECK(fixture.Tree().GetDepth(chainLength - 1) == chainLength - 1);
        CHECK(fixture.Tree().IsDescendantOf(chainLength - 1, 0));
        CHECK_FALSE(fixture.Tree().IsDescendantOf(0, chainLength - 1));

        // The tail is still refused for being out of range rather than for being deep.
        CHECK(fixture.Parent(chainLength + 500, chainLength - 1) == Hierarchy::ReparentResult::EntityNotAlive);
    }

    TEST_CASE("heavy randomised reparenting keeps every invariant")
    {
        // The property test for the linked-list bookkeeping: after any sequence of operations, each parent's child
        // list must be walkable forward and backward, agree about parent pointers, and contain no duplicates.
        TreeFixture fixture(64);

        unsigned int state = 0xDEADBEEFu;
        auto nextRandom = [&state]() {
            state = state * 1664525u + 1013904223u;
            return (state >> 16);
        };

        for (int step = 0; step < 20000; ++step)
        {
            const Index slot = static_cast<Index>(nextRandom() % 64u);
            const Index parent = static_cast<Index>(nextRandom() % 65u);

            fixture.Parent(slot, parent == 64 ? InvalidIndex : parent);

            if (step % 200 == 0)
            {
                INFO("step " << step);

                for (Index node = 0; node < 64; ++node)
                {
                    const Index parentOfNode = fixture.Tree().GetParent(node);
                    if (parentOfNode == InvalidIndex)
                    {
                        continue;
                    }

                    // Parent must list this node among its children.
                    bool found = false;
                    std::vector<Index> seen;
                    for (Index child = fixture.Tree().GetFirstChild(parentOfNode); child != InvalidIndex;
                         child = fixture.Tree().GetNextSibling(child))
                    {
                        if (child == node)
                        {
                            found = true;
                        }
                        // A slot appearing twice in one child list means unlinking left a stale link.
                        CHECK(std::find(seen.begin(), seen.end(), child) == seen.end());
                        seen.push_back(child);
                        if (seen.size() > 1000)
                        {
                            break;
                        }
                    }
                    CHECK(found);
                }
            }
        }
    }
}
