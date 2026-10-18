#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Scene/TransformHierarchy.h"

namespace Hollowpoint::Scene
{

using Core::Size;
using ECS::Entity;
using ECS::World;
using Math::Mat4;
using Math::Transform;
using Math::Vec3;

namespace
{

//! Determinant magnitude below which a matrix is treated as non-invertible. A zero-scale entity (a hidden object in
//! an editor, a collider being faded out) is legitimate content, so "assert on singular" would crash on data the
//! engine deliberately allows; the value is reported and the caller gets an unchanged point instead.
constexpr float kSingularDeterminantEpsilon = 1.0e-9f;

}  // namespace

Math::Transform WorldTransform::ToTransform() const
{
    return Transform::FromMatrix(Matrix);
}

void TransformHierarchy::Update(World& world)
{
    const Size slotCapacity = world.GetSlotCapacity();

    if (m_WorldMatrices.size() < slotCapacity)
    {
        // Identity rather than zero: an entity that is skipped for any reason then composes against a neutral
        // transform instead of collapsing its subtree to the origin.
        m_WorldMatrices.resize(slotCapacity, Mat4::Identity());
    }
    if (m_InheritedStamps.size() < slotCapacity)
    {
        m_InheritedStamps.assign(slotCapacity, 0);
    }

    ++m_VisitStamp;
    m_TraversalOrder.clear();

    // ---- Phase 1: build a parent-before-child order.
    // Roots in slot order (deterministic — the same scene must evaluate identically every run), then each root's
    // descendants. Index-based iteration with push_back is deliberate: it turns the vector into a worklist where an
    // entity is only ever appended after its parent, which is the one property the compose pass depends on. Depth-
    // first vs breadth-first does not matter for correctness, and neither needs recursion, so hierarchy depth costs
    // worklist memory rather than stack.
    world.ForEachEntity([&world, this](const Entity& entity) {
        if (world.HasParent(entity))
        {
            return;
        }

        m_TraversalOrder.push_back(entity);
        MarkVisited(entity);
    });

    for (Size cursor = 0; cursor < m_TraversalOrder.size(); ++cursor)
    {
        const Entity entity = m_TraversalOrder[cursor];  // Copy: push_back below may reallocate.

        for (Entity child = world.GetFirstChild(entity); !child.IsNull(); child = world.GetNextSibling(child))
        {
            if (IsVisited(child))
            {
                // A node reachable twice means the sibling lists contain a loop. Hierarchy rejects cycles, so this
                // is corruption from elsewhere; appending it again would compose its subtree repeatedly.
                HP_LOG_ERROR("Scene", "Hierarchy reached entity {} twice during transform traversal", entity.Slot);
                continue;
            }

            m_TraversalOrder.push_back(child);
            MarkVisited(child);
        }
    }

    // ---- Phase 2: any transformed entity not reachable from a root is composed against identity.
    // Without this, an orphan (its parent slot was recycled underneath it, or the tree was built by a bug) keeps a
    // stale matrix forever and reads as "object frozen in place" — a symptom that points at the renderer. Reported
    // because it should not happen, then handled so one broken link cannot freeze part of the level.
    const Size reachableCount = m_TraversalOrder.size();
    world.ForEachEntity([this, slotCapacity](const Entity& entity) {
        if (entity.Slot < slotCapacity && !IsVisited(entity))
        {
            m_TraversalOrder.push_back(entity);
            MarkVisited(entity);
        }
    });

    if (m_TraversalOrder.size() != reachableCount)
    {
        HP_LOG_ERROR("Scene",
                     "{} transformed entities were unreachable from any hierarchy root and were treated as roots; "
                     "the scene tree has a broken link",
                     m_TraversalOrder.size() - reachableCount);
    }

    // ---- Phase 3: compose in that order. One matrix multiply per entity, against an already-updated parent.
    for (const Entity& entity : m_TraversalOrder)
    {
        if (entity.Slot >= m_WorldMatrices.size())
        {
            continue;  // Slot grew during Update (a callback created entities); recomputed next frame.
        }

        const Mat4 inherited = GetInheritedMatrix(world, entity);

        const LocalTransform* local = world.TryGetComponent<LocalTransform>(entity);
        if (local == nullptr)
        {
            // A transform-less entity is a logic-only holder: it has no world matrix of its own, but its children
            // must still compose against what it inherited. Storing the inherited value here is what lets a chain
            // like light > logicGroup > mesh compose through the middle node instead of silently resetting the mesh
            // to world origin — the behaviour users expect and the one that never needs explaining.
            m_WorldMatrices[entity.Slot] = inherited;
            continue;
        }

        // parentWorld * local, matching Transform::Concatenated (verified against it in tests). Composing matrices
        // rather than Transforms keeps one code path for the renderer's output and the gizmo/physics read-back, so
        // the two cannot drift apart.
        m_WorldMatrices[entity.Slot] = inherited * local->ToTransform().ToMatrix();
    }

    m_EvaluatedStructureVersion = world.GetStructureVersion();
    m_HasEvaluated = true;
}

Mat4 TransformHierarchy::GetWorldMatrix(const World& world, const Entity& entity) const
{
    if (!world.IsAlive(entity))
    {
        return Mat4::Identity();
    }

    if (const LocalTransform* local = world.TryGetComponent<LocalTransform>(entity); local == nullptr)
    {
        return Mat4::Identity();  // Not a placeable entity.
    }

    if (IsStale(world))
    {
        // Compose on demand instead of returning a matrix from before the last structural change or crashing. The
        // realistic case is spawn → parent → query-position within one frame, which is normal code, not a bug; and a
        // stale cached matrix would answer it confidently wrong. O(depth), only paid when the caller skipped Update.
        return ComposeAncestors(world, entity);
    }

    if (entity.Slot >= m_WorldMatrices.size())
    {
        return Mat4::Identity();
    }

    return m_WorldMatrices[entity.Slot];
}

Transform TransformHierarchy::GetWorldTransform(const World& world, const Entity& entity) const
{
    return Transform::FromMatrix(GetWorldMatrix(world, entity));
}

Vec3 TransformHierarchy::WorldToLocalPoint(const World& world, const Entity& entity, const Vec3& point) const
{
    const Mat4 worldMatrix = GetWorldMatrix(world, entity);

    if (std::fabs(worldMatrix.Determinant()) <= kSingularDeterminantEpsilon)
    {
        // Zero or near-zero scale: the inverse does not exist. Returning the point unchanged is arbitrary but safe;
        // asserting would crash on legitimate content (a hidden entity has scale 0).
        HP_LOG_WARNING("Scene", "WorldToLocalPoint on entity {} with a non-invertible transform (zero scale?)",
                       entity.Slot);
        return point;
    }

    return worldMatrix.Inverse().TransformPoint(point);
}

Vec3 TransformHierarchy::LocalToWorldPoint(const World& world, const Entity& entity, const Vec3& point) const
{
    return GetWorldMatrix(world, entity).TransformPoint(point);
}

bool TransformHierarchy::IsStale(const World& world) const noexcept
{
    return !m_HasEvaluated || m_EvaluatedStructureVersion != world.GetStructureVersion();
}

void TransformHierarchy::Invalidate() noexcept
{
    // Entries are dropped rather than zeroed: a fresh world's slot 0 must not inherit the previous level's matrix,
    // and IsStale already forces a rebuild before anything reads.
    m_WorldMatrices.clear();
    m_InheritedStamps.clear();
    m_TraversalOrder.clear();
    m_HasEvaluated = false;
    m_EvaluatedStructureVersion = 0;
}

// ---------------------------------------------------------------------------
// Internals.
// ---------------------------------------------------------------------------

bool TransformHierarchy::IsVisited(const Entity& entity) const noexcept
{
    return entity.Slot < m_InheritedStamps.size() && m_InheritedStamps[entity.Slot] == m_VisitStamp;
}

void TransformHierarchy::MarkVisited(const Entity& entity) noexcept
{
    if (entity.Slot < m_InheritedStamps.size())
    {
        m_InheritedStamps[entity.Slot] = m_VisitStamp;
    }
}

Mat4 TransformHierarchy::GetInheritedMatrix(const World& world, const Entity& entity) const noexcept
{
    const Entity parent = world.GetParent(entity);
    if (parent.IsNull() || parent.Slot >= m_WorldMatrices.size())
    {
        return Mat4::Identity();
    }

    // m_WorldMatrices[parent] holds the parent's world matrix when it has a LocalTransform, or the value it
    // inherited when it does not — see Update. Either way this is the matrix the child should compose against.
    return m_WorldMatrices[parent.Slot];
}

Mat4 TransformHierarchy::ComposeAncestors(const World& world, const Entity& entity) const noexcept
{
    // Stale-cache fallback: walk up and compose, so a query made outside the once-per-frame Update still answers
    // correctly. Bounded by the entity count rather than trusting the tree to be acyclic — a cycle here would hang
    // the frame instead of reporting, and hangs are far harder to diagnose from a crash report.
    const LocalTransform* local = world.TryGetComponent<LocalTransform>(entity);
    Mat4 result = local != nullptr ? local->ToTransform().ToMatrix() : Mat4::Identity();

    const Size budget = world.GetSlotCapacity() + 8;
    Size steps = 0;

    Entity current = world.GetParent(entity);
    while (!current.IsNull())
    {
        if (++steps > budget)
        {
            HP_LOG_ERROR("Scene", "Transform fallback walk exceeded the entity count; the hierarchy contains a cycle");
            break;
        }

        const LocalTransform* parentLocal = world.TryGetComponent<LocalTransform>(current);
        if (parentLocal != nullptr)
        {
            result = parentLocal->ToTransform().ToMatrix() * result;
        }

        current = world.GetParent(current);
    }

    return result;
}

}  // namespace Hollowpoint::Scene
