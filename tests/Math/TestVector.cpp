#include "TestSupport.h"

#include <doctest/doctest.h>

#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Math/MathFunctions.h"

using namespace Hollowpoint::Math;
using Hollowpoint::Test::ScopedAssertCapture;

TEST_SUITE("Math/Vector")
{
    TEST_CASE("default constructed vectors are zero and components are initialised")
    {
        const Vec2 a;
        const Vec3 b;
        const Vec4 c;

        CHECK(a.X == 0.0f);
        CHECK(a.Y == 0.0f);
        CHECK(b == Vec3(0.0f, 0.0f, 0.0f));
        CHECK(c == Vec4(0.0f, 0.0f, 0.0f, 0.0f));
    }

    TEST_CASE("arithmetic operators produce the expected components")
    {
        const Vec3 a(1.0f, 2.0f, 3.0f);
        const Vec3 b(4.0f, 5.0f, 6.0f);

        CHECK(Vec3(a + b) == Vec3(5.0f, 7.0f, 9.0f));
        CHECK(Vec3(b - a) == Vec3(3.0f, 3.0f, 3.0f));
        CHECK(Vec3(a * 2.0f) == Vec3(2.0f, 4.0f, 6.0f));
        CHECK(Vec3(2.0f * a) == Vec3(2.0f, 4.0f, 6.0f));
        CHECK(Vec3(a * b) == Vec3(4.0f, 10.0f, 18.0f));
        CHECK(Vec3(a / 2.0f) == Vec3(0.5f, 1.0f, 1.5f));
        CHECK(Vec3(-a) == Vec3(-1.0f, -2.0f, -3.0f));

        Vec3 accumulator = Vec3::Zero();
        accumulator += a;
        accumulator += b;
        accumulator -= Vec3(1.0f);
        accumulator *= 2.0f;
        accumulator /= 2.0f;
        CHECK(accumulator == Vec3(4.0f, 6.0f, 8.0f));
    }

    TEST_CASE("length, dot and cross follow the right-handed convention")
    {
        const Vec3 x = Vec3::UnitX();
        const Vec3 y = Vec3::UnitY();
        const Vec3 z = Vec3::UnitZ();

        CHECK(Vec3(3.0f, 4.0f, 0.0f).Length() == doctest::Approx(5.0f));
        CHECK(Vec3(3.0f, 4.0f, 0.0f).LengthSquared() == doctest::Approx(25.0f));

        CHECK(x.Dot(y) == doctest::Approx(0.0f));
        CHECK(x.Dot(x) == doctest::Approx(1.0f));

        // Right-handed: X cross Y = Z.
        CHECK(ApproximatelyEqual(x.Cross(y), z));
        CHECK(ApproximatelyEqual(y.Cross(z), x));
        CHECK(ApproximatelyEqual(z.Cross(x), y));
        CHECK(ApproximatelyEqual(y.Cross(x), -z));
    }

    TEST_CASE("Forward is -Z and Up is +Y (right-handed, Y-up convention)")
    {
        CHECK(Vec3::Forward() == Vec3(0.0f, 0.0f, -1.0f));
        CHECK(Vec3::Up() == Vec3(0.0f, 1.0f, 0.0f));
        CHECK(Vec3::Right() == Vec3(1.0f, 0.0f, 0.0f));
    }

    TEST_CASE("Normalize produces unit length and never NaN for degenerate input")
    {
        const Vec3 normalized = Vec3(0.0f, 3.0f, 4.0f).Normalized();
        CHECK(normalized.Length() == doctest::Approx(1.0f));
        CHECK(ApproximatelyEqual(normalized, Vec3(0.0f, 0.6f, 0.8f)));

        // A zero-length vector has no direction; the contract is a zero vector, not NaN.
        const Vec3 degenerate = Vec3::Zero().Normalized();
        CHECK(degenerate == Vec3::Zero());
        CHECK(IsFinite(degenerate));

        const Vec2 degenerate2 = Vec2(0.0f).Normalized();
        CHECK(degenerate2 == Vec2(0.0f, 0.0f));

        const Vec4 degenerate4 = Vec4(0.0f).Normalized();
        CHECK(degenerate4 == Vec4(0.0f, 0.0f, 0.0f, 0.0f));
    }

    TEST_CASE("very small vectors normalise to zero instead of exploding")
    {
        const Vec3 tiny(1e-20f, 0.0f, 0.0f);
        CHECK(tiny.Normalized() == Vec3::Zero());

        // Just above the epsilon threshold it must still normalise properly.
        const Vec3 small(1e-3f, 0.0f, 0.0f);
        CHECK(small.Normalized().Length() == doctest::Approx(1.0f));
    }

    TEST_CASE("distance, component swizzles and min/max helpers")
    {
        CHECK(Distance(Vec3(0.0f), Vec3(3.0f, 4.0f, 0.0f)) == doctest::Approx(5.0f));
        CHECK(Vec3(1.0f, 2.0f, 3.0f).XY() == Vec2(1.0f, 2.0f));
        CHECK(Vec3(1.0f, 2.0f, 3.0f).XZ() == Vec2(1.0f, 3.0f));
        CHECK(Vec4(Vec3(1.0f, 2.0f, 3.0f), 4.0f).XYZ() == Vec3(1.0f, 2.0f, 3.0f));

        CHECK(Vec3(-1.0f, 5.0f, 2.0f).Min(Vec3(3.0f, 1.0f, 2.0f)) == Vec3(-1.0f, 1.0f, 2.0f));
        CHECK(Vec3(-1.0f, 5.0f, 2.0f).Max(Vec3(3.0f, 1.0f, 2.0f)) == Vec3(3.0f, 5.0f, 2.0f));
        CHECK(Vec3(-1.0f, 5.0f, -2.0f).Abs() == Vec3(1.0f, 5.0f, 2.0f));
    }

    TEST_CASE("component indexing is bounds-checked")
    {
        Vec3 value(1.0f, 2.0f, 3.0f);
        CHECK(value[0] == 1.0f);
        CHECK(value[2] == 3.0f);
        value[1] = 9.0f;
        CHECK(value.Y == 9.0f);

        // Indexing past the last component is reported; the accessor still returns a valid reference so
        // that a release build cannot read out of bounds.
        ScopedAssertCapture capture;
        const Float32 clamped = value[5];
        CHECK(capture.GetCaptureCount() == 1);
        // Reported and clamped to the last component rather than reading out of bounds.
        CHECK(clamped == value.Z);
    }

    TEST_CASE("Lerp reaches the endpoints exactly and interpolates linearly")
    {
        CHECK(Lerp(Vec3(0.0f), Vec3(10.0f), 0.0f) == Vec3(0.0f));
        // The from + (to-from)*t form guarantees t == 1 reproduces `to` bit-exactly.
        CHECK(Lerp(Vec3(0.0f), Vec3(10.0f), 1.0f) == Vec3(10.0f));
        CHECK(Lerp(Vec3(1.0f, 2.0f, 3.0f), Vec3(3.0f, 6.0f, 9.0f), 0.5f) == Vec3(2.0f, 4.0f, 6.0f));
        CHECK(Lerp(1.0f, 2.0f, 0.25f) == doctest::Approx(1.25f));
    }

    TEST_CASE("integer vectors compare by value")
    {
        CHECK(IVec2(1, 2) == IVec2(1, 2));
        CHECK(IVec2(1, 2) != IVec2(2, 1));
        CHECK(IVec3(1, 2, 3) == IVec3(1, 2, 3));
        CHECK(UVec2(4u, 5u) == UVec2(4u, 5u));
        CHECK(UVec3() == UVec3(0u, 0u, 0u));
    }

    TEST_CASE("vectors are trivially copyable and stay in registers")
    {
        // The renderer passes these by value in hot loops; a hidden vtable or padding would show up here.
        CHECK(std::is_trivially_copyable_v<Vec2>);
        CHECK(std::is_trivially_copyable_v<Vec3>);
        CHECK(std::is_trivially_copyable_v<Vec4>);
        CHECK(std::is_trivially_destructible_v<Vec3>);
        // Vec2 is 8 bytes (align 8): it maps directly onto GPU float2 attributes and shader vec2.
        // Vec3 is padded to 16 so that arrays of them stay 16-byte aligned for SIMD loads, matching
        // std140-style layouts the renderer relies on.
        CHECK(sizeof(Vec2) == 8);
        CHECK(alignof(Vec2) == 8);
        CHECK(sizeof(Vec3) == 16);
        CHECK(sizeof(Vec4) == 16);
    }
}
