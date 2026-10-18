#include "TestSupport.h"

#include <cmath>
#include <set>

#include <doctest/doctest.h>

#include "Hollowpoint/Math/MathFunctions.h"
#include "Hollowpoint/Math/Random.h"

using namespace Hollowpoint::Math;
using Hollowpoint::Test::ScopedAssertCapture;

TEST_SUITE("Math/Functions")
{
    TEST_CASE("Clamp and Saturate")
    {
        CHECK(Clamp(5, 0, 10) == 5);
        CHECK(Clamp(-5, 0, 10) == 0);
        CHECK(Clamp(50, 0, 10) == 10);
        CHECK(Saturate(-1.0f) == 0.0f);
        CHECK(Saturate(0.25f) == doctest::Approx(0.25f));
        CHECK(Saturate(2.0f) == 1.0f);
    }

    TEST_CASE("Lerp endpoints are exact and the midpoint is linear")
    {
        CHECK(Lerp(0.0f, 10.0f, 0.0f) == 0.0f);
        // The from + (to-from)*t form must reproduce `to` bit-exactly at t = 1; the symmetric form
        // rounds and made blend-out animations jitter on the last frame.
        CHECK(Lerp(0.0f, 10.0f, 1.0f) == 10.0f);
        CHECK(Lerp(-5.0f, 5.0f, 0.5f) == doctest::Approx(0.0f));
        CHECK(Lerp(1.0, 2.0, 0.75) == doctest::Approx(1.75));
    }

    TEST_CASE("InverseLerp inverts Lerp and survives a degenerate range")
    {
        CHECK(InverseLerp(0.0f, 10.0f, 2.5f) == doctest::Approx(0.25f));
        CHECK(InverseLerp(-2.0f, 4.0f, Lerp(-2.0f, 4.0f, 0.3f)) == doctest::Approx(0.3f).epsilon(1e-5f));

        // A zero-length range has no meaningful position; returning 0 avoids a divide by zero.
        CHECK(InverseLerp(1.0f, 1.0f, 5.0f) == 0.0f);
    }

    TEST_CASE("SmoothStep and SmootherStep have the documented shape")
    {
        CHECK(SmoothStep(0.0f, 1.0f, 0.0f) == doctest::Approx(0.0f));
        CHECK(SmoothStep(0.0f, 1.0f, 1.0f) == doctest::Approx(1.0f));
        CHECK(SmoothStep(0.0f, 1.0f, 0.5f) == doctest::Approx(0.5f));
        CHECK(SmoothStep(0.0f, 1.0f, -3.0f) == 0.0f);  // clamped
        CHECK(SmoothStep(0.0f, 1.0f, 3.0f) == 1.0f);

        CHECK(SmootherStep(0.0f, 1.0f, 0.5f) == doctest::Approx(0.5f));
        // Below the midpoint smoothstep is above the diagonal, which is what makes it look eased.
        CHECK(SmoothStep(0.0f, 1.0f, 0.25f) < 0.25f);
        CHECK(SmootherStep(0.0f, 1.0f, 0.25f) < SmoothStep(0.0f, 1.0f, 0.25f));
    }

    TEST_CASE("Radians and Degrees round-trip")
    {
        CHECK(Radians(180.0f) == doctest::Approx(kPi).epsilon(1e-6f));
        CHECK(Degrees(kPi) == doctest::Approx(180.0f).epsilon(1e-5f));
        CHECK(Degrees(Radians(37.5f)) == doctest::Approx(37.5f).epsilon(1e-5f));
        CHECK(Radians(0.0f) == 0.0f);
    }

    TEST_CASE("WrapAngle folds any angle into [-pi, pi]")
    {
        CHECK(WrapAngle(0.0f) == doctest::Approx(0.0f));
        CHECK(WrapAngle(Radians(190.0f)) == doctest::Approx(Radians(-170.0f)).epsilon(1e-5f));
        CHECK(WrapAngle(Radians(-190.0f)) == doctest::Approx(Radians(170.0f)).epsilon(1e-5f));
        CHECK(WrapAngle(Radians(360.0f)) == doctest::Approx(0.0f).epsilon(1e-4f).scale(1.0f));
        CHECK(WrapAngle(Radians(720.0f + 45.0f)) == doctest::Approx(Radians(45.0f)).epsilon(1e-4f));

        // Whatever the magnitude, the result stays inside the range.
        for (float degreesValue = -2000.0f; degreesValue <= 2000.0f; degreesValue += 7.0f)
        {
            const Angle wrapped = WrapAngle(Radians(degreesValue));
            CHECK(wrapped >= -kPi - 1e-5f);
            CHECK(wrapped <= kPi + 1e-5f);
        }
    }

    TEST_CASE("Wrap keeps values in [0, range)")
    {
        CHECK(Wrap(0.5f, 1.0f) == doctest::Approx(0.5f));
        CHECK(Wrap(1.5f, 1.0f) == doctest::Approx(0.5f));
        CHECK(Wrap(-0.25f, 1.0f) == doctest::Approx(0.75f));
        CHECK(Wrap(3.0f, 3.0f) == doctest::Approx(0.0f).epsilon(1e-4f).scale(1.0f));

        ScopedAssertCapture capture;
        const Float32 bad = Wrap(1.0f, 0.0f);
        HP_UNUSED(bad);
        CHECK(capture.GetCaptureCount() == 1);
    }

    TEST_CASE("PingPong oscillates between 0 and length")
    {
        CHECK(PingPong(0.0f, 1.0f) == doctest::Approx(0.0f));
        CHECK(PingPong(0.5f, 1.0f) == doctest::Approx(0.5f));
        CHECK(PingPong(1.0f, 1.0f) == doctest::Approx(1.0f));
        CHECK(PingPong(1.5f, 1.0f) == doctest::Approx(0.5f));
        CHECK(PingPong(2.0f, 1.0f) == doctest::Approx(0.0f).epsilon(1e-4f).scale(1.0f));
        // The triangle wave restarts every 2*length: 2.75 is 0.75 into the second cycle and still on
        // the way up, so it maps to 0.75; 3.25 mirrors back down to the same value.
        CHECK(PingPong(2.75f, 1.0f) == doctest::Approx(0.75f).epsilon(1e-4f));
        CHECK(PingPong(3.25f, 1.0f) == doctest::Approx(0.75f).epsilon(1e-4f));
    }

    TEST_CASE("MoveTowards reaches the target exactly and never overshoots")
    {
        CHECK(MoveTowards(0.0f, 10.0f, 3.0f) == doctest::Approx(3.0f));
        CHECK(MoveTowards(9.0f, 10.0f, 3.0f) == 10.0f);  // clamped to the target
        CHECK(MoveTowards(0.0f, -10.0f, 3.0f) == doctest::Approx(-3.0f));
        CHECK(MoveTowards(5.0f, 5.0f, 0.0f) == 5.0f);

        // Repeated steps must land exactly on the target rather than oscillating around it.
        Float32 value = 0.0f;
        for (int step = 0; step < 100; ++step)
        {
            value = MoveTowards(value, 1.0f, 0.1f);
        }
        CHECK(value == 1.0f);
    }

    TEST_CASE("SmoothDamp is frame-rate independent")
    {
        // Two frames of 1/60 s must cover the same distance as one frame of 2/60 s, which is the whole
        // point of the exponential form (a naive lerp factor is not).
        constexpr Float32 halfLife = 0.2f;
        const Float32 deltaTime = 1.0f / 60.0f;

        Float32 twoSteps = 0.0f;
        twoSteps = SmoothDamp(twoSteps, 1.0f, halfLife, deltaTime);
        twoSteps = SmoothDamp(twoSteps, 1.0f, halfLife, deltaTime);

        const Float32 oneBigStep = SmoothDamp(0.0f, 1.0f, halfLife, deltaTime * 2.0f);

        CHECK(twoSteps == doctest::Approx(oneBigStep).epsilon(1e-4f));
        CHECK(SmoothDamp(0.5f, 1.0f, 0.0f, 0.016f) == 1.0f);  // zero half life snaps
    }

    TEST_CASE("ApproximatelyEqual combines absolute and relative tolerance")
    {
        CHECK(ApproximatelyEqual(0.0f, 0.0f));
        CHECK(ApproximatelyEqual(1.0f, 1.0f + 1e-9f));
        CHECK_FALSE(ApproximatelyEqual(1.0f, 1.01f));

        // Large magnitudes use the relative term: a 0.5 absolute difference at 100 000 is fine.
        CHECK(ApproximatelyEqual(100000.0f, 100000.4f));
        CHECK_FALSE(ApproximatelyEqual(100000.0f, 100100.0f));

        // Near zero the absolute term dominates so tiny values still compare equal.
        CHECK(ApproximatelyEqual(1e-9f, -1e-9f));

        CHECK(ApproximatelyEqual(1.0, 1.0 + 1e-13));
        CHECK_FALSE(ApproximatelyEqual(1.0, 1.001));
    }

    TEST_CASE("vector and quaternion approximate comparisons")
    {
        CHECK(ApproximatelyEqual(Vec3(1.0f, 2.0f, 3.0f), Vec3(1.0f, 2.0f, 3.0f + 1e-9f)));
        CHECK_FALSE(ApproximatelyEqual(Vec3(1.0f, 2.0f, 3.0f), Vec3(1.0f, 2.0f, 3.5f)));
        CHECK(ApproximatelyEqual(Vec2(1.0f, 2.0f), Vec2(1.0f, 2.0f)));
        CHECK(ApproximatelyEqual(Vec4(1.0f, 2.0f, 3.0f, 4.0f), Vec4(1.0f, 2.0f, 3.0f, 4.0f)));

        // q and -q are the same rotation and must compare equal.
        const Quat rotation = Quat::FromAxisAngle(Vec3::UnitY(), Radians(40.0f));
        const Quat negated(-rotation.X, -rotation.Y, -rotation.Z, -rotation.W);
        CHECK(ApproximatelyEqual(rotation, negated, 1e-5f));
    }

    TEST_CASE("IsFinite rejects NaN and infinity in every type")
    {
        const Float32 nan = std::numeric_limits<Float32>::quiet_NaN();
        const Float32 infinity = std::numeric_limits<Float32>::infinity();

        CHECK_FALSE(IsFinite(nan));
        CHECK_FALSE(IsFinite(infinity));
        CHECK(IsFinite(1.0f));

        CHECK_FALSE(IsFinite(Vec3(1.0f, nan, 0.0f)));
        CHECK_FALSE(IsFinite(Vec2(0.0f, infinity)));
        CHECK_FALSE(IsFinite(Vec4(0.0f, 0.0f, 0.0f, nan)));
        CHECK_FALSE(IsFinite(Quat(0.0f, 0.0f, nan, 1.0f)));
        CHECK_FALSE(IsFinite(Mat4::Translate(Vec3(nan, 0.0f, 0.0f))));
        CHECK(IsFinite(Mat4::Identity()));
    }

    TEST_CASE("AngleBetween and SignedAngle")
    {
        CHECK(AngleBetween(Vec3::UnitX(), Vec3::UnitX()) == doctest::Approx(0.0f).epsilon(1e-5f).scale(1.0f));
        CHECK(AngleBetween(Vec3::UnitX(), Vec3::UnitY()) == doctest::Approx(kHalfPi).epsilon(1e-5f));
        CHECK(AngleBetween(Vec3::UnitX(), -Vec3::UnitX()) == doctest::Approx(kPi).epsilon(1e-4f));

        // Degenerate input must not produce NaN.
        CHECK(AngleBetween(Vec3::Zero(), Vec3::UnitX()) == 0.0f);

        // Signed angle around +Y: X -> Z is -90 degrees, Z -> X is +90 degrees.
        CHECK(SignedAngle(Vec3::UnitX(), Vec3::UnitZ(), Vec3::UnitY()) == doctest::Approx(-kHalfPi).epsilon(1e-5f));
        CHECK(SignedAngle(Vec3::UnitZ(), Vec3::UnitX(), Vec3::UnitY()) == doctest::Approx(kHalfPi).epsilon(1e-5f));
    }

    TEST_CASE("RotateTowards moves at most the allowed angle")
    {
        const Vec3 rotated = RotateTowards(Vec3::UnitX(), Vec3::UnitY(), Radians(30.0f));
        CHECK(rotated.Length() == doctest::Approx(1.0f).epsilon(1e-5f));
        CHECK(AngleBetween(Vec3::UnitX(), rotated) == doctest::Approx(Radians(30.0f)).epsilon(1e-4f));

        // A large step reaches the target exactly.
        CHECK(ApproximatelyEqual(RotateTowards(Vec3::UnitX(), Vec3::UnitY(), Radians(95.0f)), Vec3::UnitY(), 1e-5f));
    }

    TEST_CASE("Normalize helpers never return NaN")
    {
        CHECK(Normalize(Vec2(3.0f, 4.0f)).Length() == doctest::Approx(1.0f).epsilon(1e-5f));
        CHECK(Normalize(Vec3::Zero()) == Vec3::Zero());
        CHECK(Normalize(Vec4(0.0f)) == Vec4(0.0f, 0.0f, 0.0f, 0.0f));
    }
}
