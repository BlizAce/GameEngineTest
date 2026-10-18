#pragma once

#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Math/MathFunctions.h"

namespace Hollowpoint::Math
{

// ---------------------------------------------------------------------------
// EulerAngles — three rotations in radians, applied in **Y -> X -> Z** order (yaw, pitch, roll), which
// is what the editor's transform gizmo and most DCC tools produce.
//
// The order matters: `Quat::FromEulerAngles(Yaw, Pitch, 0) * Quat::FromEulerAngles(0, Pitch, Roll)` is
// not the same rotation as a single XYZ composition. Both conversion directions are covered by tests
// against the matrix path so an accidental change of convention fails loudly.
// ---------------------------------------------------------------------------

struct HP_MATH_API EulerAngles
{
    Angle Yaw = 0.0f;    //!< Rotation about +Y.
    Angle Pitch = 0.0f;  //!< Rotation about +X.
    Angle Roll = 0.0f;   //!< Rotation about +Z.

    constexpr EulerAngles() = default;
    constexpr EulerAngles(Angle yaw, Angle pitch, Angle roll) : Yaw(yaw), Pitch(pitch), Roll(roll) {}

    static EulerAngles Zero()
    {
        return EulerAngles(0.0f, 0.0f, 0.0f);
    }

    [[nodiscard]] HP_MATH_API Quat ToQuaternion() const;
    [[nodiscard]] HP_MATH_API Mat4 ToMatrix() const;

    //! Builds Euler angles from a rotation matrix (inverse of ToMatrix).
    [[nodiscard]] HP_MATH_API static EulerAngles FromMatrix(const Mat4& matrix);
    [[nodiscard]] static EulerAngles FromQuaternion(const Quat& quaternion)
    {
        return FromMatrix(Mat4::FromQuaternion(quaternion));
    }

    bool operator==(const EulerAngles&) const = default;
};

[[nodiscard]] HP_MATH_API bool ApproximatelyEqual(const EulerAngles& a, const EulerAngles& b,
                                                  Float32 epsilon = kDefaultAbsoluteEpsilon);

}  // namespace Hollowpoint::Math
