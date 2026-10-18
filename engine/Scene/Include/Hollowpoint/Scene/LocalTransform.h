#pragma once

#include "Hollowpoint/Math/Transform.h"

namespace Hollowpoint::Scene
{

// ---------------------------------------------------------------------------
// LocalTransform — the component every placeable entity carries.
//
// It stores the transform **relative to its parent** (identity for a root entity), never in world space. That is
// the only representation that survives reparenting: store world coordinates and moving an entity under a new
// parent either teleports it or forces an immediate recompute of everything below, whereas a local value simply
// changes meaning. It also means an animation authoring "rotate the turret" never has to know where the tank is.
//
// World-space values are derived by TransformHierarchy and cached there — deliberately not in this component. A
// cached world transform stored next to the local one creates two sources of truth, and keeping them consistent
// requires every write path (scripting, animation, physics, editor gizmo, scene load) to remember to invalidate.
// One of them will forget, and the symptom is an object rendering in the wrong place with correct data — a bug that
// reads as a renderer problem and costs a day. Here the local value is the only writable state and staleness is
// impossible by construction.
// ---------------------------------------------------------------------------

struct LocalTransform
{
    Math::Transform Value = Math::Transform::Identity();

    LocalTransform() = default;

    explicit LocalTransform(const Math::Transform& transform) noexcept : Value(transform) {}

    explicit LocalTransform(const Math::Vec3& position) noexcept : Value(position) {}

    // Convenience so systems and scripts read as `transform.Position` rather than `transform.Value.Position`.
    [[nodiscard]] Math::Vec3 GetPosition() const noexcept
    {
        return Value.Position;
    }
    void SetPosition(const Math::Vec3& position) noexcept
    {
        Value.Position = position;
    }

    [[nodiscard]] Math::Quat GetRotation() const noexcept
    {
        return Value.Rotation;
    }
    void SetRotation(const Math::Quat& rotation) noexcept
    {
        Value.Rotation = rotation;
    }

    [[nodiscard]] Math::Vec3 GetScale() const noexcept
    {
        return Value.Scale;
    }
    void SetScale(const Math::Vec3& scale) noexcept
    {
        Value.Scale = scale;
    }

    bool operator==(const LocalTransform&) const = default;
};

}  // namespace Hollowpoint::Scene
