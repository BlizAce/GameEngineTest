#include <cmath>

#include "Hollowpoint/Math/Transform.h"

namespace Hollowpoint::Math
{

Transform Transform::Identity()
{
    return Transform(Vec3(0.0f), Quat::Identity(), Vec3(1.0f));
}

Mat4 Transform::ToMatrix() const
{
    // Mat4::FromQuaternion carries only rotation; folding in scale and translation afterwards keeps
    // the matrix exactly T * R * S without a second quaternion->matrix conversion.
    Mat4 matrix = Mat4::FromQuaternion(Rotation);
    for (int column = 0; column < 3; ++column)
    {
        const Float32 axisScale = (column == 0) ? Scale.X : (column == 1) ? Scale.Y : Scale.Z;
        for (int row = 0; row < 3; ++row)
        {
            matrix[column][row] *= axisScale;
        }
    }
    matrix[3][0] = Position.X;
    matrix[3][1] = Position.Y;
    matrix[3][2] = Position.Z;
    return matrix;
}

Transform Transform::FromMatrix(const Mat4& matrix)
{
    return Decompose(matrix);
}

Transform Transform::Concatenated(const Transform& parent) const
{
    // Parent scale multiplies ours, and the parent rotates our already-scaled translation offset —
    // exactly what TRS composition does, but without building two 4x4 matrices per entity per frame.
    // (Verified against parent.ToMatrix() * child.ToMatrix() in tests/Math/TestTransform.cpp.)
    return Transform(parent.MapPoint(Position), (parent.Rotation * Rotation).Normalized(), parent.Scale * Scale);
}

Vec3 Transform::MapPoint(Vec3 point) const
{
    // T * R * S applied to a column vector: scale first, then rotate, then translate. Applying the
    // rotation before the scale would shear positions whenever the scale is non-uniform.
    return Position + Rotation.MapVector(point * Scale);
}

Vec3 Transform::MapDirection(Vec3 direction) const
{
    // Directions are not translated, but they are still scaled (a stretched parent stretches the
    // direction it carries). Callers needing a pure rotation should use MapRotation.
    return Rotation.MapVector(direction * Scale);
}

Quat Transform::MapRotation(const Quat& rotation) const
{
    return (Rotation * rotation).Normalized();
}

bool Transform::IsUniformScale(Float32 epsilon) const
{
    return ApproximatelyEqual(Scale.X, Scale.Y, epsilon) && ApproximatelyEqual(Scale.Y, Scale.Z, epsilon);
}

bool ApproximatelyEqual(const Transform& a, const Transform& b, Float32 epsilon)
{
    return Math::ApproximatelyEqual(a.Position, b.Position, epsilon) &&
           Math::ApproximatelyEqual(a.Rotation, b.Rotation, epsilon) &&
           Math::ApproximatelyEqual(a.Scale, b.Scale, epsilon);
}

Transform Decompose(const Mat4& matrix)
{
    Transform transform;
    transform.Position = matrix.GetTranslation();

    const Vec3 scale = matrix.GetScale();
    transform.Scale = scale;

    // Remove scale by normalising the basis columns; a zero column (degenerate matrix) keeps identity.
    Mat3 rotationMatrix = matrix.GetUpperLeft3();
    for (int column = 0; column < 3; ++column)
    {
        const Float32 axisScale = (column == 0) ? scale.X : (column == 1) ? scale.Y : scale.Z;
        if (std::fabs(axisScale) <= kDefaultAbsoluteEpsilon)
        {
            rotationMatrix[column][column] = 1.0f;
            continue;
        }
        for (int row = 0; row < 3; ++row)
        {
            rotationMatrix[column][row] /= axisScale;
        }
    }

    // Gram-Schmidt the columns so a slightly skewed matrix still yields a proper rotation.
    Vec3 xAxis(rotationMatrix[0][0], rotationMatrix[0][1], rotationMatrix[0][2]);
    Vec3 yAxis(rotationMatrix[1][0], rotationMatrix[1][1], rotationMatrix[1][2]);
    Vec3 zAxis(rotationMatrix[2][0], rotationMatrix[2][1], rotationMatrix[2][2]);

    xAxis = xAxis.Normalized();
    yAxis = (yAxis - xAxis * xAxis.Dot(yAxis)).Normalized();
    zAxis = xAxis.Cross(yAxis);

    Mat3 orthonormal = Mat3::FromBasisVectors(xAxis, yAxis, zAxis);

    // A negative determinant means the matrix mirrors; bake that back into the X scale so the rotation
    // stays a proper rotation and the decomposition still reproduces the input.
    if (orthonormal.Determinant() < 0.0f)
    {
        transform.Scale.X = -transform.Scale.X;
        for (int row = 0; row < 3; ++row)
        {
            orthonormal[0][row] = -orthonormal[0][row];
        }
    }

    transform.Rotation = EulerAngles::FromMatrix(Mat4(orthonormal)).ToQuaternion().Normalized();
    return transform;
}

}  // namespace Hollowpoint::Math
