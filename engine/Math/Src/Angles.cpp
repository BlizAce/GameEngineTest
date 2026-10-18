#include <cmath>

#include "Hollowpoint/Math/Angles.h"

namespace Hollowpoint::Math
{

namespace
{

//! Element access in (row, column) notation for a column-major matrix.
[[nodiscard]] inline Float32 Element(const Mat4& matrix, int row, int column)
{
    return matrix[column][row];
}

}  // namespace

Quat EulerAngles::ToQuaternion() const
{
    // Yaw about +Y, then pitch about +X, then roll about +Z (intrinsic Y-X-Z). Multiplying in that
    // order with column vectors applies the rightmost rotation first in world space.
    const Quat yawRotation = Quat::FromAxisAngle(Vec3::UnitY(), Yaw);
    const Quat pitchRotation = Quat::FromAxisAngle(Vec3::UnitX(), Pitch);
    const Quat rollRotation = Quat::FromAxisAngle(Vec3::UnitZ(), Roll);
    return yawRotation * pitchRotation * rollRotation;
}

Mat4 EulerAngles::ToMatrix() const
{
    return Mat4::Rotate(Vec3::UnitY(), Yaw) * Mat4::Rotate(Vec3::UnitX(), Pitch) * Mat4::Rotate(Vec3::UnitZ(), Roll);
}

EulerAngles EulerAngles::FromMatrix(const Mat4& matrix)
{
    // Inverse of R = Ry(yaw) * Rx(pitch) * Rz(roll):
    //   R[row1][col2] = -sin(pitch), and the remaining rows give yaw/roll via atan2.
    const Float32 sinPitch = -Element(matrix, 1, 2);

    if (sinPitch >= 1.0f - kDefaultAbsoluteEpsilon || sinPitch <= -1.0f + kDefaultAbsoluteEpsilon)
    {
        // Gimbal lock: pitch is +/-90 degrees and only the difference of yaw and roll is defined. We
        // fold everything into yaw and report roll as zero, which round-trips to the same rotation.
        EulerAngles result;
        result.Pitch = std::copysign(kHalfPi, sinPitch);
        result.Yaw = std::atan2(-Element(matrix, 2, 0), Element(matrix, 0, 0));
        result.Roll = 0.0f;
        return result;
    }

    EulerAngles result;
    result.Pitch = std::asin(sinPitch);
    result.Yaw = std::atan2(Element(matrix, 0, 2), Element(matrix, 2, 2));
    result.Roll = std::atan2(Element(matrix, 1, 0), Element(matrix, 1, 1));
    return result;
}

bool ApproximatelyEqual(const EulerAngles& a, const EulerAngles& b, Float32 epsilon)
{
    // Compare the resulting rotations rather than the raw angles: different angle triples can describe
    // the same orientation (yaw+2pi, or the gimbal-lock fold), and callers care about equivalence.
    return ApproximatelyEqual(a.ToQuaternion(), b.ToQuaternion(), epsilon);
}

}  // namespace Hollowpoint::Math
