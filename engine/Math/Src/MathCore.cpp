#include <algorithm>
#include <cassert>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Math/MathCore.h"

namespace Hollowpoint::Math
{

namespace
{

// Shared normalisation helper: returns the fallback instead of NaN when the vector is too short to
// define a direction. Callers that must distinguish "no direction" check for the fallback themselves.
[[nodiscard]] inline Float32 SafeReciprocalLength(Float32 lengthSquared, Float32 epsilon)
{
    if (lengthSquared <= epsilon * epsilon)
    {
        return 0.0f;
    }
    return 1.0f / std::sqrt(lengthSquared);
}

}  // namespace

// ---------------------------------------------------------------------------
// Vec2
// ---------------------------------------------------------------------------

Vec2 Vec2::Normalized() const
{
    const Float32 length = Length();
    if (length <= kDefaultAbsoluteEpsilon)
    {
        return Vec2(0.0f, 0.0f);
    }
    const Float32 inverse = 1.0f / length;
    return Vec2(X * inverse, Y * inverse);
}

// ---------------------------------------------------------------------------
// Vec3
// ---------------------------------------------------------------------------

Float32& Vec3::operator[](std::size_t index)
{
    HP_ASSERT_ARGUMENT(index < 3, "Vec3 component index out of range");
    switch (index)
    {
        case 0:
            return X;
        case 1:
            return Y;
        default:
            return Z;
    }
}

const Float32& Vec3::operator[](std::size_t index) const
{
    HP_ASSERT_ARGUMENT(index < 3, "Vec3 component index out of range");
    switch (index)
    {
        case 0:
            return X;
        case 1:
            return Y;
        default:
            return Z;
    }
}

Vec3 Vec3::Normalized(Float32 epsilon) const
{
    const Float32 factor = SafeReciprocalLength(LengthSquared(), epsilon);
    if (factor == 0.0f)
    {
        return Vec3(0.0f, 0.0f, 0.0f);
    }
    return Vec3(X * factor, Y * factor, Z * factor);
}

// ---------------------------------------------------------------------------
// Vec4
// ---------------------------------------------------------------------------

Float32& Vec4::operator[](std::size_t index)
{
    HP_ASSERT_ARGUMENT(index < 4, "Vec4 component index out of range");
    switch (index)
    {
        case 0:
            return X;
        case 1:
            return Y;
        case 2:
            return Z;
        default:
            return W;
    }
}

const Float32& Vec4::operator[](std::size_t index) const
{
    HP_ASSERT_ARGUMENT(index < 4, "Vec4 component index out of range");
    switch (index)
    {
        case 0:
            return X;
        case 1:
            return Y;
        case 2:
            return Z;
        default:
            return W;
    }
}

Vec4 Vec4::Normalized(Float32 epsilon) const
{
    const Float32 factor = SafeReciprocalLength(LengthSquared(), epsilon);
    if (factor == 0.0f)
    {
        return Vec4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    return Vec4(X * factor, Y * factor, Z * factor, W * factor);
}

// ---------------------------------------------------------------------------
// Mat3
// ---------------------------------------------------------------------------

Mat3::Mat3(Float32 diagonal)
{
    for (int column = 0; column < 3; ++column)
    {
        for (int row = 0; row < 3; ++row)
        {
            Columns[column][row] = (column == row) ? diagonal : 0.0f;
        }
    }
}

Mat3::Mat3(Float32 c0r0, Float32 c0r1, Float32 c0r2, Float32 c1r0, Float32 c1r1, Float32 c1r2, Float32 c2r0,
           Float32 c2r1, Float32 c2r2)
{
    Columns[0][0] = c0r0;
    Columns[0][1] = c0r1;
    Columns[0][2] = c0r2;
    Columns[1][0] = c1r0;
    Columns[1][1] = c1r1;
    Columns[1][2] = c1r2;
    Columns[2][0] = c2r0;
    Columns[2][1] = c2r1;
    Columns[2][2] = c2r2;
}

Mat3 Mat3::Identity()
{
    return Mat3(1.0f);
}

Mat3 Mat3::FromRotation(Float32 angleRadians)
{
    const Float32 cosine = std::cos(angleRadians);
    const Float32 sine = std::sin(angleRadians);
    return Mat3(cosine, sine, 0.0f, -sine, cosine, 0.0f, 0.0f, 0.0f, 1.0f);
}

Mat3 Mat3::FromScaling(Vec2 scale)
{
    return Mat3(scale.X, 0.0f, 0.0f, 0.0f, scale.Y, 0.0f, 0.0f, 0.0f, 1.0f);
}

Mat3 Mat3::FromBasisVectors(Vec3 xAxis, Vec3 yAxis, Vec3 zAxis)
{
    Mat3 result;
    result[0][0] = xAxis.X;
    result[0][1] = xAxis.Y;
    result[0][2] = xAxis.Z;
    result[1][0] = yAxis.X;
    result[1][1] = yAxis.Y;
    result[1][2] = yAxis.Z;
    result[2][0] = zAxis.X;
    result[2][1] = zAxis.Y;
    result[2][2] = zAxis.Z;
    return result;
}

Mat3 Mat3::operator*(const Mat3& other) const
{
    Mat3 result(0.0f);
    for (int column = 0; column < 3; ++column)
    {
        for (int row = 0; row < 3; ++row)
        {
            Float32 sum = 0.0f;
            for (int inner = 0; inner < 3; ++inner)
            {
                sum += Columns[inner][row] * other.Columns[column][inner];
            }
            result.Columns[column][row] = sum;
        }
    }
    return result;
}

Vec3 Mat3::operator*(Vec3 vector) const
{
    return Vec3(Columns[0][0] * vector.X + Columns[1][0] * vector.Y + Columns[2][0] * vector.Z,
                Columns[0][1] * vector.X + Columns[1][1] * vector.Y + Columns[2][1] * vector.Z,
                Columns[0][2] * vector.X + Columns[1][2] * vector.Y + Columns[2][2] * vector.Z);
}

Mat3 Mat3::Transposed() const
{
    Mat3 result;
    for (int column = 0; column < 3; ++column)
    {
        for (int row = 0; row < 3; ++row)
        {
            result.Columns[column][row] = Columns[row][column];
        }
    }
    return result;
}

Float32 Mat3::Determinant() const
{
    const Float32 a = Columns[0][0], b = Columns[0][1], c = Columns[0][2];
    const Float32 d = Columns[1][0], e = Columns[1][1], f = Columns[1][2];
    const Float32 g = Columns[2][0], h = Columns[2][1], i = Columns[2][2];
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
}

Mat3 Mat3::Inverse() const
{
    const Float32 determinant = Determinant();
    if (std::fabs(determinant) <= kDefaultAbsoluteEpsilon)
    {
        HP_ASSERT_MSG(false, "Mat3::Inverse called on a singular matrix");
        return Identity();
    }

    const Float32 inverseDeterminant = 1.0f / determinant;
    const Float32 a = Columns[0][0], b = Columns[0][1], c = Columns[0][2];
    const Float32 d = Columns[1][0], e = Columns[1][1], f = Columns[1][2];
    const Float32 g = Columns[2][0], h = Columns[2][1], i = Columns[2][2];

    return Mat3((e * i - f * h) * inverseDeterminant, (c * h - b * i) * inverseDeterminant,
                (b * f - c * e) * inverseDeterminant, (f * g - d * i) * inverseDeterminant,
                (a * i - c * g) * inverseDeterminant, (c * d - a * f) * inverseDeterminant,
                (d * h - e * g) * inverseDeterminant, (b * g - a * h) * inverseDeterminant,
                (a * e - b * d) * inverseDeterminant);
}

Mat3 Mat3::AbsDeterminant1() const
{
    const Float32 determinant = Determinant();
    const Float32 scale = std::cbrt(std::fabs(determinant));
    if (scale <= kDefaultAbsoluteEpsilon)
    {
        return Identity();
    }
    Mat3 result = *this;
    for (int column = 0; column < 3; ++column)
    {
        for (int row = 0; row < 3; ++row)
        {
            result.Columns[column][row] /= scale;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Mat4
// ---------------------------------------------------------------------------

Mat4::Mat4(Float32 diagonal)
{
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            Columns[column][row] = (column == row) ? diagonal : 0.0f;
        }
    }
}

Mat4::Mat4(const Mat3& upperLeft) : Mat4(1.0f)
{
    for (int column = 0; column < 3; ++column)
    {
        for (int row = 0; row < 3; ++row)
        {
            Columns[column][row] = upperLeft.Columns[column][row];
        }
    }
}

Mat4 Mat4::Identity()
{
    return Mat4(1.0f);
}

Mat4 Mat4::Translate(Vec3 translation)
{
    Mat4 result = Identity();
    result[3][0] = translation.X;
    result[3][1] = translation.Y;
    result[3][2] = translation.Z;
    return result;
}

Mat4 Mat4::Scale(Vec3 scale)
{
    Mat4 result(0.0f);
    result[0][0] = scale.X;
    result[1][1] = scale.Y;
    result[2][2] = scale.Z;
    result[3][3] = 1.0f;
    return result;
}

Mat4 Mat4::FromQuaternion(Quat rotation)
{
    const Quat q = rotation.Normalized();
    const Float32 xx = q.X * q.X, yy = q.Y * q.Y, zz = q.Z * q.Z;
    const Float32 xy = q.X * q.Y, xz = q.X * q.Z, yz = q.Y * q.Z;
    const Float32 wx = q.W * q.X, wy = q.W * q.Y, wz = q.W * q.Z;

    Mat4 result = Identity();
    result[0][0] = 1.0f - 2.0f * (yy + zz);
    result[0][1] = 2.0f * (xy + wz);
    result[0][2] = 2.0f * (xz - wy);

    result[1][0] = 2.0f * (xy - wz);
    result[1][1] = 1.0f - 2.0f * (xx + zz);
    result[1][2] = 2.0f * (yz + wx);

    result[2][0] = 2.0f * (xz + wy);
    result[2][1] = 2.0f * (yz - wx);
    result[2][2] = 1.0f - 2.0f * (xx + yy);
    return result;
}

Mat4 Mat4::Rotate(Vec3 axis, Float32 angleRadians)
{
    return FromQuaternion(Quat::FromAxisAngle(axis, angleRadians));
}

Mat4 Mat4::LookAtRH(Vec3 eye, Vec3 target, Vec3 up)
{
    const Vec3 forward = (eye - target).Normalized();  //!< Right-handed: basis Z points away from target.
    const Vec3 right = up.Cross(forward).Normalized();
    const Vec3 newUp = forward.Cross(right);

    Mat4 result = Identity();
    result[0][0] = right.X;
    result[0][1] = newUp.X;
    result[0][2] = forward.X;
    result[1][0] = right.Y;
    result[1][1] = newUp.Y;
    result[1][2] = forward.Y;
    result[2][0] = right.Z;
    result[2][1] = newUp.Z;
    result[2][2] = forward.Z;
    result[3][0] = -right.Dot(eye);
    result[3][1] = -newUp.Dot(eye);
    result[3][2] = -forward.Dot(eye);
    return result;
}

Mat4 Mat4::LookAtLH(Vec3 eye, Vec3 target, Vec3 up)
{
    const Vec3 forward = (target - eye).Normalized();
    const Vec3 right = up.Cross(forward).Normalized();
    const Vec3 newUp = forward.Cross(right);

    Mat4 result = Identity();
    result[0][0] = right.X;
    result[0][1] = newUp.X;
    result[0][2] = -forward.X;
    result[1][0] = right.Y;
    result[1][1] = newUp.Y;
    result[1][2] = -forward.Y;
    result[2][0] = right.Z;
    result[2][1] = newUp.Z;
    result[2][2] = -forward.Z;
    result[3][0] = -right.Dot(eye);
    result[3][1] = -newUp.Dot(eye);
    result[3][2] = forward.Dot(eye);
    return result;
}

Mat4 Mat4::PerspectiveRH(Float32 verticalFieldOfViewRadians, Float32 aspectRatio, Float32 nearPlane, Float32 farPlane)
{
    HP_ASSERT_ARGUMENT(verticalFieldOfViewRadians > 0.0f && verticalFieldOfViewRadians < kPi,
                       "field of view must be inside (0, pi) radians");
    HP_ASSERT_ARGUMENT(aspectRatio > 0.0f, "aspect ratio must be positive");
    HP_ASSERT_ARGUMENT(farPlane > nearPlane && nearPlane > 0.0f, "require far > near > 0");

    const Float32 cotangent = 1.0f / std::tan(verticalFieldOfViewRadians * 0.5f);
    const Float32 range = nearPlane - farPlane;

    // Vulkan clip space: z' = (far/(near-far)) * z + (near*far/(near-far)), mapped to [0,1] after the
    // perspective divide (w' = -z).
    Mat4 result(0.0f);
    result[0][0] = cotangent / aspectRatio;
    result[1][1] = cotangent;
    result[2][2] = farPlane / range;
    result[2][3] = -1.0f;
    result[3][2] = nearPlane * farPlane / range;
    return result;
}

Mat4 Mat4::OrthographicRH(Float32 width, Float32 height, Float32 nearPlane, Float32 farPlane)
{
    HP_ASSERT_ARGUMENT(width > 0.0f && height > 0.0f, "orthographic extents must be positive");
    HP_ASSERT_ARGUMENT(farPlane != nearPlane, "orthographic near and far must differ");

    // Vulkan depth range [0,1]: scale = 1/(near-far), offset = near/(near-far).
    const Float32 range = nearPlane - farPlane;
    Mat4 result = Identity();
    result[0][0] = 2.0f / width;
    result[1][1] = 2.0f / height;
    result[2][2] = 1.0f / range;
    result[3][2] = nearPlane / range;
    return result;
}

Mat4 Mat4::operator*(const Mat4& other) const
{
    Mat4 result(0.0f);
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            Float32 sum = 0.0f;
            for (int inner = 0; inner < 4; ++inner)
            {
                sum += Columns[inner][row] * other.Columns[column][inner];
            }
            result.Columns[column][row] = sum;
        }
    }
    return result;
}

Vec4 Mat4::operator*(Vec4 vector) const
{
    return Vec4(
        Columns[0][0] * vector[0] + Columns[1][0] * vector[1] + Columns[2][0] * vector[2] + Columns[3][0] * vector[3],
        Columns[0][1] * vector[0] + Columns[1][1] * vector[1] + Columns[2][1] * vector[2] + Columns[3][1] * vector[3],
        Columns[0][2] * vector[0] + Columns[1][2] * vector[1] + Columns[2][2] * vector[2] + Columns[3][2] * vector[3],
        Columns[0][3] * vector[0] + Columns[1][3] * vector[1] + Columns[2][3] * vector[2] + Columns[3][3] * vector[3]);
}

Vec3 Mat4::TransformPoint(Vec3 point) const
{
    const Float32 x = Columns[0][0] * point.X + Columns[1][0] * point.Y + Columns[2][0] * point.Z + Columns[3][0];
    const Float32 y = Columns[0][1] * point.X + Columns[1][1] * point.Y + Columns[2][1] * point.Z + Columns[3][1];
    const Float32 z = Columns[0][2] * point.X + Columns[1][2] * point.Y + Columns[2][2] * point.Z + Columns[3][2];
    const Float32 w = Columns[0][3] * point.X + Columns[1][3] * point.Y + Columns[2][3] * point.Z + Columns[3][3];

    if (std::fabs(w) <= kDefaultAbsoluteEpsilon)
    {
        // Point at infinity (degenerate projection); returning the un-divided value keeps callers from
        // producing NaN, and the assertion documents that this should not happen in normal use.
        HP_ASSERT_MSG(false, "TransformPoint encountered a zero perspective weight");
        return Vec3(x, y, z);
    }

    const Float32 inverseW = 1.0f / w;
    return Vec3(x * inverseW, y * inverseW, z * inverseW);
}

Vec3 Mat4::TransformVector(Vec3 vector) const
{
    return Vec3(Columns[0][0] * vector.X + Columns[1][0] * vector.Y + Columns[2][0] * vector.Z,
                Columns[0][1] * vector.X + Columns[1][1] * vector.Y + Columns[2][1] * vector.Z,
                Columns[0][2] * vector.X + Columns[1][2] * vector.Y + Columns[2][2] * vector.Z);
}

Mat4 Mat4::Transposed() const
{
    Mat4 result;
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            result.Columns[column][row] = Columns[row][column];
        }
    }
    return result;
}

Float32 Mat4::Determinant() const
{
    // Laplace expansion along the first row, using 2x2 minors of the remaining rows.
    const Float32* c0 = Columns[0];
    const Float32* c1 = Columns[1];
    const Float32* c2 = Columns[2];
    const Float32* c3 = Columns[3];

    const Float32 s0 = c0[0] * c1[1] - c1[0] * c0[1];
    const Float32 s1 = c0[0] * c1[2] - c1[0] * c0[2];
    const Float32 s2 = c0[0] * c1[3] - c1[0] * c0[3];
    const Float32 s3 = c0[1] * c1[2] - c1[1] * c0[2];
    const Float32 s4 = c0[1] * c1[3] - c1[1] * c0[3];
    const Float32 s5 = c0[2] * c1[3] - c1[2] * c0[3];

    const Float32 m5 = c2[2] * c3[3] - c3[2] * c2[3];
    const Float32 m4 = c2[1] * c3[3] - c3[1] * c2[3];
    const Float32 m3 = c2[1] * c3[2] - c3[1] * c2[2];
    const Float32 m2 = c2[0] * c3[3] - c3[0] * c2[3];
    const Float32 m1 = c2[0] * c3[2] - c3[0] * c2[2];
    const Float32 m0 = c2[0] * c3[1] - c3[0] * c2[1];

    return s0 * m5 - s1 * m4 + s2 * m3 + s3 * m2 - s4 * m1 + s5 * m0;
}

Mat4 Mat4::Inverse() const
{
    // Cofactor implementation (identical structure to GLM's inverse, kept self-contained so that the
    // engine does not depend on a specific glm extension being available).
    const Float32* c0 = Columns[0];
    const Float32* c1 = Columns[1];
    const Float32* c2 = Columns[2];
    const Float32* c3 = Columns[3];

    const Float32 a00 = c0[0], a01 = c0[1], a02 = c0[2], a03 = c0[3];
    const Float32 a10 = c1[0], a11 = c1[1], a12 = c1[2], a13 = c1[3];
    const Float32 a20 = c2[0], a21 = c2[1], a22 = c2[2], a23 = c2[3];
    const Float32 a30 = c3[0], a31 = c3[1], a32 = c3[2], a33 = c3[3];

    const Float32 b00 = a00 * a11 - a01 * a10;
    const Float32 b01 = a00 * a12 - a02 * a10;
    const Float32 b02 = a00 * a13 - a03 * a10;
    const Float32 b03 = a01 * a12 - a02 * a11;
    const Float32 b04 = a01 * a13 - a03 * a11;
    const Float32 b05 = a02 * a13 - a03 * a12;
    const Float32 b06 = a20 * a31 - a21 * a30;
    const Float32 b07 = a20 * a32 - a22 * a30;
    const Float32 b08 = a20 * a33 - a23 * a30;
    const Float32 b09 = a21 * a32 - a22 * a31;
    const Float32 b10 = a21 * a33 - a23 * a31;
    const Float32 b11 = a22 * a33 - a23 * a32;

    Float32 determinant = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
    if (std::fabs(determinant) <= kDefaultAbsoluteEpsilon)
    {
        HP_ASSERT_MSG(false, "Mat4::Inverse called on a singular matrix");
        return Identity();
    }

    determinant = 1.0f / determinant;
    Mat4 result(0.0f);
    result[0][0] = (a11 * b11 - a12 * b10 + a13 * b09) * determinant;
    result[0][1] = (a02 * b10 - a01 * b11 - a03 * b09) * determinant;
    result[0][2] = (a31 * b05 - a32 * b04 + a33 * b03) * determinant;
    result[0][3] = (a22 * b04 - a21 * b05 - a23 * b03) * determinant;
    result[1][0] = (a12 * b08 - a10 * b11 - a13 * b07) * determinant;
    result[1][1] = (a00 * b11 - a02 * b08 + a03 * b07) * determinant;
    result[1][2] = (a32 * b02 - a30 * b05 - a33 * b01) * determinant;
    result[1][3] = (a20 * b05 - a22 * b02 + a23 * b01) * determinant;
    result[2][0] = (a10 * b10 - a11 * b08 + a13 * b06) * determinant;
    result[2][1] = (a01 * b08 - a00 * b10 - a03 * b06) * determinant;
    result[2][2] = (a30 * b04 - a31 * b02 + a33 * b00) * determinant;
    result[2][3] = (a21 * b02 - a20 * b04 - a23 * b00) * determinant;
    result[3][0] = (a11 * b07 - a10 * b09 - a12 * b06) * determinant;
    result[3][1] = (a00 * b09 - a01 * b07 + a02 * b06) * determinant;
    result[3][2] = (a31 * b01 - a30 * b03 - a32 * b00) * determinant;
    result[3][3] = (a20 * b03 - a21 * b01 + a22 * b00) * determinant;
    return result;
}

Vec3 Mat4::GetTranslation() const
{
    return Vec3(Columns[3][0], Columns[3][1], Columns[3][2]);
}

Mat3 Mat4::GetUpperLeft3() const
{
    Mat3 result;
    for (int column = 0; column < 3; ++column)
    {
        for (int row = 0; row < 3; ++row)
        {
            result.Columns[column][row] = Columns[column][row];
        }
    }
    return result;
}

Vec3 Mat4::GetScale() const
{
    Vec3 scale(Columns[0][0] * Columns[0][0] + Columns[0][1] * Columns[0][1] + Columns[0][2] * Columns[0][2],
               Columns[1][0] * Columns[1][0] + Columns[1][1] * Columns[1][1] + Columns[1][2] * Columns[1][2],
               Columns[2][0] * Columns[2][0] + Columns[2][1] * Columns[2][1] + Columns[2][2] * Columns[2][2]);
    scale.X = std::sqrt(scale.X);
    scale.Y = std::sqrt(scale.Y);
    scale.Z = std::sqrt(scale.Z);

    // Preserve a mirroring (negative) scale on an axis: its sign shows up in the determinant.
    if (Determinant() < 0.0f)
    {
        scale.X = -scale.X;
    }
    return scale;
}

Mat3 Mat4::GetRotationMatrix() const
{
    const Vec3 scale = GetScale();
    Mat3 rotation = GetUpperLeft3();
    for (int column = 0; column < 3; ++column)
    {
        const Float32 axisScale = (column == 0) ? scale.X : (column == 1) ? scale.Y : scale.Z;
        if (std::fabs(axisScale) <= kDefaultAbsoluteEpsilon)
        {
            continue;
        }
        for (int row = 0; row < 3; ++row)
        {
            rotation[column][row] /= axisScale;
        }
    }
    return rotation;
}

Vec4 Mat4::GetRow(std::size_t rowIndex) const
{
    HP_ASSERT_ARGUMENT(rowIndex < 4, "row index out of range");

    // Clamp before indexing. Assertions continue by contract (the editor and the test runner install a
    // non-fatal handler), so reporting the bad index is not enough on its own — reading through it would
    // be undefined behaviour in exactly the build where we most want a clean, reproducible failure.
    const std::size_t row = rowIndex < 4 ? rowIndex : 3;
    return Vec4(Columns[0][row], Columns[1][row], Columns[2][row], Columns[3][row]);
}

// ---------------------------------------------------------------------------
// Quat
// ---------------------------------------------------------------------------

Quat Quat::FromAxisAngle(Vec3 axis, Float32 angleRadians)
{
    const Vec3 normalizedAxis = axis.Normalized();
    if (normalizedAxis == Vec3::Zero())
    {
        return Identity();
    }

    const Float32 halfAngle = angleRadians * 0.5f;
    const Float32 sine = std::sin(halfAngle);
    return Quat(normalizedAxis.X * sine, normalizedAxis.Y * sine, normalizedAxis.Z * sine, std::cos(halfAngle));
}

Quat Quat::FromTo(Vec3 from, Vec3 to)
{
    const Vec3 a = from.Normalized();
    const Vec3 b = to.Normalized();
    const Float32 dotProduct = a.Dot(b);

    if (dotProduct >= 1.0f - kDefaultAbsoluteEpsilon)
    {
        return Identity();
    }

    if (dotProduct <= -1.0f + kDefaultAbsoluteEpsilon)
    {
        // Opposite directions: rotate 180 degrees about any axis perpendicular to `a`. Pick the
        // perpendicular with the largest magnitude so the cross product stays numerically healthy.
        Vec3 axis = Vec3::UnitX().Cross(a);
        if (axis.LengthSquared() < 0.5f)
        {
            axis = Vec3::UnitY().Cross(a);
        }
        axis = axis.Normalized();
        return Quat(axis.X, axis.Y, axis.Z, 0.0f);
    }

    const Vec3 crossProduct = a.Cross(b);
    Quat result(crossProduct.X, crossProduct.Y, crossProduct.Z, 1.0f + dotProduct);
    return result.Normalized();
}

Quat Quat::LookRotation(Vec3 forward, Vec3 up)
{
    const Vec3 normalizedForward = forward.Normalized();
    if (normalizedForward == Vec3::Zero())
    {
        return Identity();
    }

    // Build the rotation whose local -Z axis points along `forward` (right-handed convention). The
    // image of local +Z is therefore backward = -forward; completing a right-handed orthonormal basis
    // gives right = up x backward and trueUp = backward x right. Getting either sign flipped produces a
    // reflection matrix (determinant -1), which silently mirrors everything drawn with it — the
    // determinant check below is the guard against regressing that.
    const Vec3 backward = -normalizedForward;

    Vec3 right = up.Cross(backward).Normalized();
    if (right == Vec3::Zero())
    {
        // `up` is parallel to forward; pick any perpendicular axis instead.
        right = Vec3::UnitX().Cross(backward).Normalized();
        if (right == Vec3::Zero())
        {
            right = Vec3::UnitY().Cross(backward).Normalized();
        }
    }
    const Vec3 trueUp = backward.Cross(right);

    Mat3 rotation = Mat3::FromBasisVectors(right, trueUp, backward);
    HP_ASSERT_MSG(rotation.Determinant() > 0.5f, "LookRotation produced a non-rotation basis");

    // Matrix -> quaternion using Shepperd's method. m(row, column) keeps the well-known formulas
    // readable even though Mat3 stores columns.
    const Mat3& orientation = rotation;
    auto m = [&orientation](int row, int column) { return orientation[column][row]; };
    const Float32 trace = m(0, 0) + m(1, 1) + m(2, 2);

    if (trace > 0.0f)
    {
        const Float32 scale = std::sqrt(trace + 1.0f) * 2.0f;
        return Quat((m(2, 1) - m(1, 2)) / scale, (m(0, 2) - m(2, 0)) / scale, (m(1, 0) - m(0, 1)) / scale,
                    0.25f * scale)
            .Normalized();
    }

    if (m(0, 0) > m(1, 1) && m(0, 0) > m(2, 2))
    {
        const Float32 scale = std::sqrt(1.0f + m(0, 0) - m(1, 1) - m(2, 2)) * 2.0f;
        return Quat(0.25f * scale, (m(0, 1) + m(1, 0)) / scale, (m(0, 2) + m(2, 0)) / scale,
                    (m(2, 1) - m(1, 2)) / scale)
            .Normalized();
    }

    if (m(1, 1) > m(2, 2))
    {
        const Float32 scale = std::sqrt(1.0f + m(1, 1) - m(0, 0) - m(2, 2)) * 2.0f;
        return Quat((m(0, 1) + m(1, 0)) / scale, 0.25f * scale, (m(1, 2) + m(2, 1)) / scale,
                    (m(0, 2) - m(2, 0)) / scale)
            .Normalized();
    }

    const Float32 scale = std::sqrt(1.0f + m(2, 2) - m(0, 0) - m(1, 1)) * 2.0f;
    return Quat((m(0, 2) + m(2, 0)) / scale, (m(1, 2) + m(2, 1)) / scale, 0.25f * scale, (m(1, 0) - m(0, 1)) / scale)
        .Normalized();
}

Quat Quat::Normalized() const
{
    const Float32 lengthSquared = LengthSquared();
    if (lengthSquared <= kDefaultAbsoluteEpsilon * kDefaultAbsoluteEpsilon)
    {
        return Identity();
    }
    const Float32 inverseLength = 1.0f / std::sqrt(lengthSquared);
    return Quat(X * inverseLength, Y * inverseLength, Z * inverseLength, W * inverseLength);
}

Quat Quat::Inverse() const
{
    const Float32 lengthSquared = LengthSquared();
    if (lengthSquared <= kDefaultAbsoluteEpsilon * kDefaultAbsoluteEpsilon)
    {
        HP_ASSERT_MSG(false, "Quat::Inverse called on a zero-length quaternion");
        return Identity();
    }
    const Float32 scale = 1.0f / lengthSquared;
    return Quat(-X * scale, -Y * scale, -Z * scale, W * scale);
}

Quat Quat::operator*(const Quat& other) const
{
    return Quat(
        W * other.X + X * other.W + Y * other.Z - Z * other.Y, W * other.Y + Y * other.W + Z * other.X - X * other.Z,
        W * other.Z + Z * other.W + X * other.Y - Y * other.X, W * other.W - X * other.X - Y * other.Y - Z * other.Z);
}

Vec3 Quat::operator*(Vec3 vector) const
{
    // q * v * q^-1 expanded; 15 mul + 12 add, the standard form.
    const Vec3 quaternionVector(X, Y, Z);
    const Vec3 crossPart = quaternionVector.Cross(vector);
    const Vec3 doubleCross = quaternionVector.Cross(crossPart);
    return vector + (crossPart * (2.0f * W)) + doubleCross * 2.0f;
}

Quat Quat::Slerp(Quat from, Quat to, Float32 t)
{
    // Take the short path by flipping `to` when the dot product is negative.
    if (from.Dot(to) < 0.0f)
    {
        to = Quat(-to.X, -to.Y, -to.Z, -to.W);
    }

    Float32 dotProduct = from.Dot(to);
    dotProduct = std::min(1.0f, dotProduct);

    if (dotProduct > 0.9995f)
    {
        // Angles this small are indistinguishable from linear interpolation; NLerp avoids the divide by
        // a near-zero sine.
        return NLerp(from, to, t);
    }

    const Float32 theta = std::acos(dotProduct);
    const Float32 sinTheta = std::sin(theta);
    const Float32 weightThis = std::sin((1.0f - t) * theta) / sinTheta;
    const Float32 weightOther = std::sin(t * theta) / sinTheta;

    return Quat(from.X * weightThis + to.X * weightOther, from.Y * weightThis + to.Y * weightOther,
                from.Z * weightThis + to.Z * weightOther, from.W * weightThis + to.W * weightOther);
}

Quat Quat::NLerp(Quat from, Quat to, Float32 t)
{
    if (from.Dot(to) < 0.0f)
    {
        to = Quat(-to.X, -to.Y, -to.Z, -to.W);
    }

    Quat result(from.X + (to.X - from.X) * t, from.Y + (to.Y - from.Y) * t, from.Z + (to.Z - from.Z) * t,
                from.W + (to.W - from.W) * t);
    return result.Normalized();
}

Float32 Quat::GetAngle() const
{
    const Quat normalized = Normalized();
    return 2.0f * std::acos(std::max(-1.0f, std::min(1.0f, normalized.W)));
}

Vec3 Quat::GetAxis() const
{
    const Quat normalized = Normalized();
    const Float32 scaleSquared = 1.0f - normalized.W * normalized.W;
    if (scaleSquared <= kDefaultAbsoluteEpsilon)
    {
        return Vec3(0.0f, 1.0f, 0.0f);  //!< Angle is zero; any axis is valid, +Y is the friendliest.
    }
    const Float32 scale = 1.0f / std::sqrt(scaleSquared);
    return Vec3(normalized.X * scale, normalized.Y * scale, normalized.Z * scale);
}

// ---------------------------------------------------------------------------
// Axis helpers
// ---------------------------------------------------------------------------

Vec3 ToVector(AxisDirection direction)
{
    switch (direction)
    {
        case AxisDirection::NegativeX:
            return -Vec3::UnitX();
        case AxisDirection::PositiveX:
            return Vec3::UnitX();
        case AxisDirection::NegativeY:
            return -Vec3::UnitY();
        case AxisDirection::PositiveY:
            return Vec3::UnitY();
        case AxisDirection::NegativeZ:
            return -Vec3::UnitZ();
        case AxisDirection::PositiveZ:
            return Vec3::UnitZ();
        default:
            HP_UNREACHABLE("invalid AxisDirection");
            return Vec3::Zero();
    }
}

Axis GetAxis(AxisDirection direction)
{
    return static_cast<Axis>(static_cast<UInt8>(direction) / 2);
}

bool IsPositive(AxisDirection direction)
{
    return (static_cast<UInt8>(direction) & 1u) == 1u;
}

}  // namespace Hollowpoint::Math
