#include <cmath>

#include "Hollowpoint/Math/MathFunctions.h"

namespace Hollowpoint::Math
{

Angle WrapAngle(Angle angleRadians)
{
    // fmod into (-2pi, 2pi), then shift into [-pi, pi]. Two branches instead of a loop: cheaper and
    // exact for the ranges gameplay produces.
    Float32 wrapped = std::fmod(angleRadians, kTwoPi);
    if (wrapped > kPi)
    {
        wrapped -= kTwoPi;
    }
    else if (wrapped < -kPi)
    {
        wrapped += kTwoPi;
    }
    return wrapped;
}

bool IsFinite(const Quat& value)
{
    return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z) && std::isfinite(value.W);
}

bool IsFinite(const Mat4& value)
{
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            if (!std::isfinite(value[column][row]))
            {
                return false;
            }
        }
    }
    return true;
}

bool ApproximatelyEqual(Vec2 a, Vec2 b, Float32 epsilon)
{
    return ApproximatelyEqual(a.X, b.X, epsilon) && ApproximatelyEqual(a.Y, b.Y, epsilon);
}

bool ApproximatelyEqual(Vec3 a, Vec3 b, Float32 epsilon)
{
    return ApproximatelyEqual(a.X, b.X, epsilon) && ApproximatelyEqual(a.Y, b.Y, epsilon) &&
           ApproximatelyEqual(a.Z, b.Z, epsilon);
}

bool ApproximatelyEqual(Vec4 a, Vec4 b, Float32 epsilon)
{
    return ApproximatelyEqual(a.X, b.X, epsilon) && ApproximatelyEqual(a.Y, b.Y, epsilon) &&
           ApproximatelyEqual(a.Z, b.Z, epsilon) && ApproximatelyEqual(a.W, b.W, epsilon);
}

bool ApproximatelyEqual(const Quat& a, const Quat& b, Float32 epsilon)
{
    // q and -q describe the same rotation, so compare the absolute dot product: 1 means equal.
    const Float32 dotProduct = std::fabs(a.Dot(b));
    return ApproximatelyEqual(dotProduct, 1.0f, epsilon, epsilon);
}

bool ApproximatelyEqual(const Mat4& a, const Mat4& b, Float32 epsilon)
{
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            if (!ApproximatelyEqual(a[column][row], b[column][row], epsilon))
            {
                return false;
            }
        }
    }
    return true;
}

Angle AngleBetween(Vec3 a, Vec3 b)
{
    const Vec3 normalizedA = a.Normalized();
    const Vec3 normalizedB = b.Normalized();
    if (normalizedA == Vec3::Zero() || normalizedB == Vec3::Zero())
    {
        return 0.0f;
    }

    // atan2(|a x b|, a . b) stays accurate near 0 and pi, unlike acos of a clamped dot product.
    const Float32 crossLength = normalizedA.Cross(normalizedB).Length();
    return std::atan2(crossLength, normalizedA.Dot(normalizedB));
}

Angle SignedAngle(Vec3 from, Vec3 to, Vec3 axis)
{
    const Angle unsignedAngle = AngleBetween(from, to);
    const Vec3 crossProduct = from.Normalized().Cross(to.Normalized());
    const Float32 sign = crossProduct.Dot(axis.Normalized()) >= 0.0f ? 1.0f : -1.0f;
    return unsignedAngle * sign;
}

}  // namespace Hollowpoint::Math
