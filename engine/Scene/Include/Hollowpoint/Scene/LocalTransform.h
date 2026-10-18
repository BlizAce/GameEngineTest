#pragma once

#include "Hollowpoint/Math/Transform.h"

// Deliberately no Reflection include: this header is included by anything that places things, and reflection is a
// concern of the serialisation layer. Registration lives in exactly one translation unit (SceneIO.cpp), which is all
// HP_REGISTER_TYPE requires.

namespace Hollowpoint::Math
{
class Vec3;
class Quat;
}  // namespace Hollowpoint::Math

namespace Hollowpoint::Scene
{

// ---------------------------------------------------------------------------
// LocalTransform — the component every placeable entity carries.
//
// It stores the transform **relative to its parent** (identity for a root entity), never in world space. That is the
// only representation that survives reparenting: store world coordinates and moving an entity under a new parent
// either teleports it or forces an immediate recompute of everything below, whereas a local value simply changes
// meaning. It also means an animator authoring "rotate the turret" never has to know where the tank is.
//
// World-space values are derived by TransformHierarchy and cached there — deliberately not in this component. A cached
// world transform stored next to the local one creates two sources of truth, and keeping them consistent requires every
// write path (scripting, animation, physics, editor gizmo, scene load) to remember to invalidate. One of them will
// forget, and the symptom is an object rendering in the wrong place with correct data — a bug that reads as a renderer
// problem and costs a day. Here the local value is the only writable state and staleness is impossible by construction.
//
// Why three members rather than one Math::Transform: Reflection binds fields through C++ member pointers, which cannot
// reach *through* a nested struct — `&LocalTransform::Value` would serialise as {"value": {...}} instead of the flat
// {"position": …, "rotation": …, "scale": …} that scene files want and that the inspector wants to draw. Storing the
// triple directly makes every consumer (serializer, inspector, scripting) see three editable fields, and
// Math::Transform is still what composition APIs take and return via GetTransform/SetTransform.
// ---------------------------------------------------------------------------

struct LocalTransform
{
    Math::Vec3 Position = Math::Vec3(0.0f);
    Math::Quat Rotation = Math::Quat::Identity();
    Math::Vec3 Scale = Math::Vec3(1.0f);

    LocalTransform() = default;

    LocalTransform(const Math::Vec3& position, const Math::Quat& rotation, const Math::Vec3& scale) noexcept
        : Position(position), Rotation(rotation), Scale(scale)
    {}

    explicit LocalTransform(const Math::Vec3& position) noexcept : Position(position) {}

    //! Builds from the value-type form used by composition APIs and tests.
    static LocalTransform FromTransform(const Math::Transform& transform) noexcept
    {
        return LocalTransform(transform.Position, transform.Rotation, transform.Scale);
    }

    [[nodiscard]] Math::Transform ToTransform() const noexcept
    {
        return Math::Transform(Position, Rotation, Scale);
    }

    void SetFrom(const Math::Transform& transform) noexcept
    {
        Position = transform.Position;
        Rotation = transform.Rotation;
        Scale = transform.Scale;
    }

    bool operator==(const LocalTransform&) const = default;
};

}  // namespace Hollowpoint::Scene
