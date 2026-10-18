#pragma once

#include "Hollowpoint/Math/Angles.h"
#include "Hollowpoint/Math/MathCore.h"

namespace Hollowpoint::Math
{

// ---------------------------------------------------------------------------
// Transform — the position/rotation/scale triple attached to every entity.
//
// A Transform is always expressed **relative to its parent** (identity for a root entity). World-space
// values are computed by composing up the hierarchy, never stored, so moving a parent cannot leave
// stale cached matrices behind. The ECS does that composition in one cache-friendly pass.
//
// Composition follows the engine convention T * R * S:
//
//     localToWorld = Translate(Position) * FromQuaternion(Rotation) * Scale(Scale)
// ---------------------------------------------------------------------------

struct HP_MATH_API Transform
{
    Vec3 Position = Vec3(0.0f);
    Quat Rotation = Quat::Identity();
    Vec3 Scale = Vec3(1.0f);

    constexpr Transform() = default;
    constexpr Transform(Vec3 position, Quat rotation, Vec3 scale) : Position(position), Rotation(rotation), Scale(scale)
    {}

    explicit Transform(Vec3 position) : Position(position) {}

    [[nodiscard]] static Transform Identity();

    //! Local -> world matrix (T * R * S).
    [[nodiscard]] Mat4 ToMatrix() const;

    //! Extracts a Transform from a matrix. Assumes no skew; the rotation is orthonormalised.
    [[nodiscard]] static Transform FromMatrix(const Mat4& matrix);

    //! localFromParent -> world, given the parent's own local-to-parent transform.
    [[nodiscard]] Transform Concatenated(const Transform& parent) const;

    //! Transforms a point/rotation from this transform's space into the parent's space.
    [[nodiscard]] Vec3 MapPoint(Vec3 point) const;
    [[nodiscard]] Vec3 MapDirection(Vec3 direction) const;  //!< Rotation + scale, no translation.
    [[nodiscard]] Quat MapRotation(const Quat& rotation) const;

    [[nodiscard]] EulerAngles GetEulerAngles() const
    {
        return EulerAngles::FromQuaternion(Rotation);
    }
    void SetEulerAngles(const EulerAngles& angles)
    {
        Rotation = angles.ToQuaternion();
    }

    //! True when the scale is (approximately) uniform — required for correct normal transforms.
    [[nodiscard]] bool IsUniformScale(Float32 epsilon = kDefaultAbsoluteEpsilon) const;

    Transform operator*(const Transform& other) const
    {
        return other.Concatenated(*this);
    }
    bool operator==(const Transform&) const = default;
};

[[nodiscard]] HP_MATH_API bool ApproximatelyEqual(const Transform& a, const Transform& b,
                                                  Float32 epsilon = kDefaultAbsoluteEpsilon);

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

//! Convenience: builds the T*R*S matrix without constructing a Transform.
[[nodiscard]] inline Mat4 TRSMatrix(Vec3 translation, const Quat& rotation, Vec3 scale)
{
    return Mat4::Translate(translation) * Mat4::FromQuaternion(rotation) * Mat4::Scale(scale);
}

//! Decomposes a T*R*S matrix. Skew is not representable and is silently removed by orthonormalising.
[[nodiscard]] HP_MATH_API Transform Decompose(const Mat4& matrix);

}  // namespace Hollowpoint::Math
