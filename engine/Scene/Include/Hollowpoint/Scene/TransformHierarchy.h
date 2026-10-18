#pragma once

#include <vector>

#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/ECS/World.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Math/Transform.h"
#include "Hollowpoint/Scene/LocalTransform.h"

namespace Hollowpoint::Scene
{

using Core::Size;

// ---------------------------------------------------------------------------
// TransformHierarchy — evaluates world transforms from local transforms + the ECS parent/child tree.
//
// Model: LocalTransform (authored, relative to parent) is the only writable state; this system derives
// WorldTransform by composing down the tree once per frame, in **parent-before-child order**, so each entity's
// parent matrix is ready before its own is computed — one composition per entity, no repeated walking up the chain.
//
// Why a flat cache rather than computing on demand: world transforms are read by the renderer (every draw), physics
// (every body), culling and scripting. Computing on demand means either an O(depth) walk per query or a cache — and
// a cache is what this is, rebuilt in one pass so it can never be inconsistent with the local values it came from.
//
// Why parent-before-child order matters for correctness rather than speed: composing in slot order would read a
// child's parent matrix before the parent had been updated this frame, producing a one-frame lag that shows up as
// jitter on nested objects and disappears when you stop moving — exactly the kind of bug that survives review. The
// traversal is iterative (explicit worklist), so hierarchy depth costs memory in the worklist, never stack.
//
// Staleness: the cache records World::GetStructureVersion() at evaluation time, which changes on reparent, create and
// destroy. That catches the dangerous class — a cached *tree shape* that no longer matches (a reparented entity would
// otherwise keep its old world matrix indefinitely, with correct local data and no way to tell from the read).
//
// Writing a LocalTransform does **not** bump the version: component writes are invisible to the ECS, and making them
// visible would require the world to know this component type, breaking the open-type design that keeps it usable
// from plugins and scripts. So the contract is "evaluate once per frame after all writers have run" — which is what
// a game loop does anyway, and it makes staleness a non-issue in correct usage while still reporting structural
// misuse loudly.
// ---------------------------------------------------------------------------

//! A world-space transform, derived from LocalTransform + parent chain. Read-only outside this system.
struct WorldTransform
{
    Math::Mat4 Matrix = Math::Mat4::Identity();

    [[nodiscard]] Math::Vec3 GetPosition() const
    {
        return Matrix.GetTranslation();
    }

    //! Decomposed world transform. Slower than reading the matrix; intended for gizmos, physics sync and scripting,
    //! not per-draw code.
    [[nodiscard]] Math::Transform ToTransform() const;
};

class TransformHierarchy
{
public:
    //! Recomputes world transforms for every entity that has a LocalTransform. Call once per frame after all
    //! systems/scripts have written their local transforms and before anything reads a world transform.
    void Update(ECS::World& world);

    //! World matrix for an entity, or identity when it has no LocalTransform. Reports (and asserts in debug) when
    //! the cache is stale rather than returning a matrix from before the last structural change: a wrong transform
    //! renders and collides confidently, and the cause is nowhere near the read.
    [[nodiscard]] Math::Mat4 GetWorldMatrix(const ECS::World& world, const ECS::Entity& entity) const;

    [[nodiscard]] Math::Transform GetWorldTransform(const ECS::World& world, const ECS::Entity& entity) const;

    //! World -> local: converts a world-space point/direction into `entity`'s own space. Handy for placing an object
    //! relative to another (attach points, camera-relative effects) without hand-inverting matrices at each call.
    [[nodiscard]] Math::Vec3 WorldToLocalPoint(const ECS::World& world, const ECS::Entity& entity,
                                               const Math::Vec3& point) const;

    //! Local -> world for a point, without holding the matrix.
    [[nodiscard]] Math::Vec3 LocalToWorldPoint(const ECS::World& world, const ECS::Entity& entity,
                                               const Math::Vec3& point) const;

    [[nodiscard]] bool IsStale(const ECS::World& world) const noexcept;

    [[nodiscard]] Core::UInt64 GetEvaluatedStructureVersion() const noexcept
    {
        return m_EvaluatedStructureVersion;
    }

    [[nodiscard]] Size GetCachedCount() const noexcept
    {
        return m_WorldMatrices.size();
    }

    //! Drops the cache. Needed when a whole world is torn down between levels so stale entries cannot outlive it.
    void Invalidate() noexcept;

private:
    //! Cache indexed by entity slot. Slot (not handle) because it is rebuilt from scratch each frame and generations
    //! only matter for validating handles at the door, which GetWorldMatrix does via World::IsAlive.
    std::vector<Math::Mat4> m_WorldMatrices;

    //! Scratch buffer for the parent-before-child traversal, reused between frames so steady state allocates nothing.
    std::vector<ECS::Entity> m_TraversalOrder;

    //! Per-slot visit stamps for the current Update, avoiding a cleared bool array each frame (stamp comparison
    //   instead). Index is an entity slot.
    std::vector<Core::UInt64> m_InheritedStamps;
    Core::UInt64 m_VisitStamp = 0;

    Core::UInt64 m_EvaluatedStructureVersion = 0;
    bool m_HasEvaluated = false;

    [[nodiscard]] bool IsVisited(const ECS::Entity& entity) const noexcept;
    void MarkVisited(const ECS::Entity& entity) noexcept;

    //! Matrix a child should compose against: the parent's world matrix, or what the parent inherited when it has
    //! no LocalTransform. Identity for a root.
    [[nodiscard]] Math::Mat4 GetInheritedMatrix(const ECS::World& world, const ECS::Entity& entity) const noexcept;

    //! Stale-cache fallback: composes up the parent chain on demand so a query outside Update still answers.
    [[nodiscard]] Math::Mat4 ComposeAncestors(const ECS::World& world, const ECS::Entity& entity) const noexcept;
};

}  // namespace Hollowpoint::Scene
