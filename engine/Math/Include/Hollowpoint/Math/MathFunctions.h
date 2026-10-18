#pragma once

#include <algorithm>
#include <cmath>
#include <type_traits>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Math/MathCore.h"

namespace Hollowpoint::Math
{

// ---------------------------------------------------------------------------
// Scalar functions.
//
// Everything here is branch-light and works on both float and double. Functions that take an epsilon
// document whose scale it applies to; the defaults come from MathCore.h and follow the policy in
// docs/MODULE_MATH.md.
// ---------------------------------------------------------------------------

template <typename T>
[[nodiscard]] constexpr const T& Clamp(const T& value, const T& minimum, const T& maximum)
{
    // Note: assumes minimum <= maximum; passing an inverted range is a programmer error.
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

//! Clamps to [0,1]. The name says what the result means, which reads better than Clamp(x, 0, 1).
template <typename T>
[[nodiscard]] constexpr T Saturate(const T& value)
{
    return Clamp(value, T{0}, T{1});
}

[[nodiscard]] inline Float32 Lerp(Float32 from, Float32 to, Float32 t)
{
    // Written as from + (to-from)*t so that t == 1 reproduces `to` exactly (the a*(1-t)+b*t form
    // rounds differently and made interpolated positions jitter at the end of a blend).
    return from + (to - from) * t;
}

[[nodiscard]] inline Float64 Lerp(Float64 from, Float64 to, Float64 t)
{
    return from + (to - from) * t;
}

//! Where does `value` sit between `from` and `to`? Returns 0 when the range is degenerate.
[[nodiscard]] inline Float32 InverseLerp(Float32 from, Float32 to, Float32 value)
{
    const Float32 range = to - from;
    if (std::fabs(range) <= kDefaultAbsoluteEpsilon)
    {
        return 0.0f;
    }
    return (value - from) / range;
}

[[nodiscard]] inline Float32 SmoothStep(Float32 edge0, Float32 edge1, Float32 value)
{
    const Float32 t = Saturate(InverseLerp(edge0, edge1, value));
    return t * t * (3.0f - 2.0f * t);
}

[[nodiscard]] inline Float32 SmootherStep(Float32 edge0, Float32 edge1, Float32 value)
{
    const Float32 t = Saturate(InverseLerp(edge0, edge1, value));
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

[[nodiscard]] constexpr Float32 Radians(Float32 degreesValue)
{
    return degreesValue * (kPi / 180.0f);
}
[[nodiscard]] constexpr Float32 Degrees(Float32 radiansValue)
{
    return radiansValue * (180.0f / kPi);
}

//! Moves `current` towards `target`, at most `maxDelta` units. Reaches the target exactly.
[[nodiscard]] inline Float32 MoveTowards(Float32 current, Float32 target, Float32 maxDelta)
{
    if (std::fabs(target - current) <= maxDelta)
    {
        return target;
    }
    return current + std::copysign(maxDelta, target - current);
}

//! Frame-rate independent exponential smoothing. `halfLife` is the time in seconds for the distance
//! to the target to halve; 0 snaps immediately.
[[nodiscard]] inline Float32 SmoothDamp(Float32 current, Float32 target, Float32 halfLife, Float32 deltaTime)
{
    if (halfLife <= 0.0f)
    {
        return target;
    }
    const Float32 smoothing = 1.0f - std::pow(0.5f, deltaTime / halfLife);
    return Lerp(current, target, smoothing);
}

//! Wraps an angle into [-pi, pi] so that rotations compose without drift.
[[nodiscard]] HP_MATH_API Angle WrapAngle(Angle angleRadians);

//! Wraps a value into [0, range). Range must be positive.
[[nodiscard]] inline Float32 Wrap(Float32 value, Float32 range)
{
    HP_ASSERT_ARGUMENT(range > 0.0f, "Wrap requires a positive range");
    const Float32 wrapped = std::fmod(value, range);
    return wrapped < 0.0f ? wrapped + range : wrapped;
}

[[nodiscard]] inline Float32 PingPong(Float32 value, Float32 length = 1.0f)
{
    const Float32 doubledLength = length * 2.0f;
    const Float32 wrapped = Wrap(value, doubledLength);
    return length - std::fabs(wrapped - length);
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

//! Combined absolute/relative comparison — see docs/MODULE_MATH.md for the epsilon policy.
[[nodiscard]] inline bool ApproximatelyEqual(Float32 a, Float32 b, Float32 absoluteEpsilon = kDefaultAbsoluteEpsilon,
                                             Float32 relativeEpsilon = kDefaultRelativeEpsilon)
{
    if (a == b)
    {
        return true;
    }
    const Float32 difference = std::fabs(a - b);
    if (difference <= absoluteEpsilon)
    {
        return true;
    }
    const Float32 largest = std::max(std::fabs(a), std::fabs(b));
    return difference <= relativeEpsilon * largest;
}

[[nodiscard]] inline bool ApproximatelyEqual(Float64 a, Float64 b, Float64 absoluteEpsilon = 1e-12,
                                             Float64 relativeEpsilon = 1e-9)
{
    if (a == b)
    {
        return true;
    }
    const Float64 difference = std::fabs(a - b);
    if (difference <= absoluteEpsilon)
    {
        return true;
    }
    const Float64 largest = std::max(std::fabs(a), std::fabs(b));
    return difference <= relativeEpsilon * largest;
}

[[nodiscard]] inline bool IsFinite(Float32 value)
{
    return std::isfinite(value);
}
[[nodiscard]] inline bool IsFinite(Vec2 value)
{
    return std::isfinite(value.X) && std::isfinite(value.Y);
}
[[nodiscard]] inline bool IsFinite(Vec3 value)
{
    return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
}
[[nodiscard]] inline bool IsFinite(Vec4 value)
{
    return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z) && std::isfinite(value.W);
}
[[nodiscard]] HP_MATH_API bool IsFinite(const Quat& value);
[[nodiscard]] HP_MATH_API bool IsFinite(const Mat4& value);

// ---------------------------------------------------------------------------
// Vector helpers that need the epsilon policy (glm's versions do not have one).
// ---------------------------------------------------------------------------

[[nodiscard]] inline Float32 Distance(Vec2 a, Vec2 b)
{
    return (a - b).Length();
}
[[nodiscard]] inline Float32 Distance(Vec3 a, Vec3 b)
{
    return (a - b).Length();
}
[[nodiscard]] inline Float32 Dot(Vec2 a, Vec2 b)
{
    return a.Dot(b);
}
[[nodiscard]] inline Float32 Dot(Vec3 a, Vec3 b)
{
    return a.Dot(b);
}
[[nodiscard]] inline Vec3 Cross(Vec3 a, Vec3 b)
{
    return a.Cross(b);
}

[[nodiscard]] inline Vec2 Normalize(Vec2 value)
{
    return value.Normalized();
}
[[nodiscard]] inline Vec3 Normalize(Vec3 value)
{
    return value.Normalized();
}
[[nodiscard]] inline Vec4 Normalize(Vec4 value)
{
    return value.Normalized();
}

[[nodiscard]] HP_MATH_API bool ApproximatelyEqual(Vec2 a, Vec2 b, Float32 epsilon = kDefaultAbsoluteEpsilon);
[[nodiscard]] HP_MATH_API bool ApproximatelyEqual(Vec3 a, Vec3 b, Float32 epsilon = kDefaultAbsoluteEpsilon);
[[nodiscard]] HP_MATH_API bool ApproximatelyEqual(Vec4 a, Vec4 b, Float32 epsilon = kDefaultAbsoluteEpsilon);
[[nodiscard]] HP_MATH_API bool ApproximatelyEqual(const Quat& a, const Quat& b,
                                                  Float32 epsilon = kDefaultAbsoluteEpsilon);
[[nodiscard]] HP_MATH_API bool ApproximatelyEqual(const Mat4& a, const Mat4& b,
                                                  Float32 epsilon = kDefaultAbsoluteEpsilon);

//! Unsigned angle between two directions, in radians (0..pi). Safe for zero-length inputs.
[[nodiscard]] HP_MATH_API Angle AngleBetween(Vec3 a, Vec3 b);

//! Signed angle from `from` to `to` around `axis`, in (-pi, pi].
[[nodiscard]] HP_MATH_API Angle SignedAngle(Vec3 from, Vec3 to, Vec3 axis);

[[nodiscard]] inline Vec3 Lerp(Vec3 from, Vec3 to, Float32 t)
{
    return from + (to - from) * t;
}

[[nodiscard]] inline Vec2 Lerp(Vec2 from, Vec2 to, Float32 t)
{
    return from + (to - from) * t;
}

[[nodiscard]] inline Vec4 Lerp(Vec4 from, Vec4 to, Float32 t)
{
    return from + (to - from) * t;
}

[[nodiscard]] inline Quat Lerp(const Quat& from, const Quat& to, Float32 t)
{
    return Quat::NLerp(from, to, t);
}

[[nodiscard]] inline Vec3 RotateTowards(Vec3 current, Vec3 target, Float32 maxDeltaRadians)
{
    const Vec3 currentNormalized = current.Normalized();
    const Vec3 targetNormalized = target.Normalized();
    const Angle angle = AngleBetween(currentNormalized, targetNormalized);

    if (angle <= maxDeltaRadians || angle <= kDefaultAbsoluteEpsilon)
    {
        return targetNormalized;
    }
    const Vec3 axis = currentNormalized.Cross(targetNormalized).Normalized();
    return Quat::FromAxisAngle(axis, maxDeltaRadians) * currentNormalized;
}

}  // namespace Hollowpoint::Math
