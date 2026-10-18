#include "TestSupport.h"

#include <doctest/doctest.h>

#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Math/MathFunctions.h"

using namespace Hollowpoint::Math;
using Hollowpoint::Test::ScopedAssertCapture;

TEST_SUITE("Math/Quaternion")
{
    TEST_CASE("the identity quaternion rotates nothing")
    {
        const Quat identity = Quat::Identity();
        CHECK(identity.X == 0.0f);
        CHECK(identity.W == 1.0f);
        CHECK(ApproximatelyEqual(identity * Vec3(1.5f, -2.5f, 3.0f), Vec3(1.5f, -2.5f, 3.0f)));
    }

    TEST_CASE("axis-angle rotations match the right-hand rule")
    {
        const Quat quarterTurnZ = Quat::FromAxisAngle(Vec3::UnitZ(), kHalfPi);
        CHECK(ApproximatelyEqual(quarterTurnZ * Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), 1e-6f));

        const Quat quarterTurnY = Quat::FromAxisAngle(Vec3::UnitY(), kHalfPi);
        // +X rotated 90 degrees about +Y goes to -Z (right-handed).
        CHECK(ApproximatelyEqual(quarterTurnY * Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, -1.0f), 1e-6f));

        const Quat halfTurnX = Quat::FromAxisAngle(Vec3::UnitX(), kPi);
        CHECK(ApproximatelyEqual(halfTurnX * Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 1e-6f));
    }

    TEST_CASE("FromAxisAngle normalises a non-unit axis and handles a zero axis")
    {
        const Quat fromLongAxis = Quat::FromAxisAngle(Vec3(0.0f, 5.0f, 0.0f), Radians(30.0f));
        const Quat fromUnitAxis = Quat::FromAxisAngle(Vec3::UnitY(), Radians(30.0f));
        CHECK(ApproximatelyEqual(fromLongAxis, fromUnitAxis, 1e-6f));

        // A zero axis defines no rotation: identity, not NaN.
        const Quat degenerate = Quat::FromAxisAngle(Vec3::Zero(), Radians(45.0f));
        CHECK(ApproximatelyEqual(degenerate, Quat::Identity()));
        CHECK(IsFinite(degenerate));
    }

    TEST_CASE("composition applies the rightmost rotation first")
    {
        const Quat rotateX = Quat::FromAxisAngle(Vec3::UnitX(), kHalfPi);
        const Quat rotateY = Quat::FromAxisAngle(Vec3::UnitY(), kHalfPi);
        const Vec3 point(1.0f, 0.0f, 0.0f);

        // qParent * qChild applies the child first: (Y then X) vs (X then Y) differ.
        const Vec3 xy = (rotateY * rotateX) * point;
        const Vec3 yx = (rotateX * rotateY) * point;
        CHECK_FALSE(ApproximatelyEqual(xy, yx, 1e-4f));

        // Explicit check of the documented order: rotateX first moves +X to itself...
        CHECK(ApproximatelyEqual(xy, rotateY * (rotateX * point), 1e-6f));
    }

    TEST_CASE("conjugate and inverse undo a rotation")
    {
        const Quat rotation = Quat::FromAxisAngle(Vec3(0.3f, 0.6f, -0.74f).Normalized(), Radians(53.0f));
        const Vec3 point(2.0f, -3.0f, 4.5f);

        const Vec3 rotated = rotation * point;
        CHECK(ApproximatelyEqual(rotation.Conjugated() * rotated, point, 1e-5f));
        CHECK(ApproximatelyEqual(rotation.Inverse() * rotated, point, 1e-5f));
    }

    TEST_CASE("normalisation keeps unit length and handles the zero quaternion")
    {
        const Quat scaled(0.0f, 0.0f, 0.0f, 2.0f);
        CHECK(scaled.Normalized().Length() == doctest::Approx(1.0f));

        const Quat zero(0.0f, 0.0f, 0.0f, 0.0f);
        CHECK(ApproximatelyEqual(zero.Normalized(), Quat::Identity()));
        CHECK(IsFinite(zero.Normalized()));
    }

    TEST_CASE("Slerp interpolates along the shortest arc")
    {
        const Quat from = Quat::Identity();
        const Quat to = Quat::FromAxisAngle(Vec3::UnitY(), Radians(90.0f));

        CHECK(ApproximatelyEqual(Quat::Slerp(from, to, 0.0f) * Vec3::UnitX(), from * Vec3::UnitX(), 1e-6f));
        CHECK(ApproximatelyEqual(Quat::Slerp(from, to, 1.0f) * Vec3::UnitX(), to * Vec3::UnitX(), 1e-6f));

        // Halfway between 0 and 90 degrees must be 45 degrees.
        const Quat half = Quat::Slerp(from, to, 0.5f);
        CHECK(half.GetAngle() == doctest::Approx(Radians(45.0f)).epsilon(1e-4f));

        // The short path must be taken even when the quaternion is negated (q and -q are the same
        // rotation, but a naive slerp would spin the long way round).
        const Quat negatedTo(-to.X, -to.Y, -to.Z, -to.W);
        const Vec3 viaPositive = Quat::Slerp(from, to, 0.5f) * Vec3::UnitX();
        const Vec3 viaNegated = Quat::Slerp(from, negatedTo, 0.5f) * Vec3::UnitX();
        CHECK(ApproximatelyEqual(viaPositive, viaNegated, 1e-5f));
    }

    TEST_CASE("Slerp degrades gracefully to NLerp for nearly identical rotations")
    {
        const Quat from = Quat::Identity();
        const Quat to = Quat::FromAxisAngle(Vec3::UnitY(), Radians(0.01f));

        // Without the NLerp fallback this divides by sin(theta) ~ 0 and yields NaN.
        const Quat result = Quat::Slerp(from, to, 0.5f);
        CHECK(IsFinite(result));
        CHECK(result.Length() == doctest::Approx(1.0f).epsilon(1e-4f));
    }

    TEST_CASE("NLerp stays on the unit sphere")
    {
        const Quat from = Quat::Identity();
        const Quat to = Quat::FromAxisAngle(Vec3::UnitZ(), Radians(60.0f));
        for (float t = 0.0f; t <= 1.0f; t += 0.1f)
        {
            CHECK(Quat::NLerp(from, to, t).Length() == doctest::Approx(1.0f).epsilon(1e-5f));
        }
    }

    TEST_CASE("FromTo produces the shortest rotation between two directions")
    {
        const Quat q = Quat::FromTo(Vec3::UnitX(), Vec3::UnitY());
        CHECK(ApproximatelyEqual(q * Vec3::UnitX(), Vec3::UnitY(), 1e-6f));

        // Identical directions give the identity.
        CHECK(ApproximatelyEqual(Quat::FromTo(Vec3::UnitZ(), Vec3::UnitZ()) * Vec3::UnitX(), Vec3::UnitX(), 1e-6f));

        // Opposite directions must still produce a valid 180 degree rotation about some perpendicular.
        const Quat flipped = Quat::FromTo(Vec3::UnitX(), -Vec3::UnitX());
        CHECK(IsFinite(flipped));
        CHECK(ApproximatelyEqual(flipped * Vec3::UnitX(), -Vec3::UnitX(), 1e-5f));
    }

    TEST_CASE("LookRotation aims local -Z along the forward direction")
    {
        // Right-handed convention: a camera looks down its local -Z axis.
        const Quat lookAtMinusZ = Quat::LookRotation(Vec3(0.0f, 0.0f, -1.0f));
        CHECK(ApproximatelyEqual(lookAtMinusZ * Vec3::Forward(), Vec3(0.0f, 0.0f, -1.0f), 1e-6f));

        const Vec3 forward = Vec3(0.0f, 0.0f, 1.0f);
        const Quat lookAtPlusZ = Quat::LookRotation(forward);
        CHECK(ApproximatelyEqual(lookAtPlusZ * Vec3::Forward(), forward, 1e-6f));

        // A diagonal direction, checked against the axis it must map to.
        const Vec3 diagonal = Vec3(1.0f, 1.0f, -1.0f).Normalized();
        const Quat lookDiagonal = Quat::LookRotation(diagonal);
        CHECK(ApproximatelyEqual(lookDiagonal * Vec3::Forward(), diagonal, 1e-5f));

        // Degenerate forward yields identity rather than NaN.
        CHECK(ApproximatelyEqual(Quat::LookRotation(Vec3::Zero()), Quat::Identity(), 1e-6f));

        // Forward parallel to up must not produce NaN (a classic camera bug).
        const Quat degenerateUp = Quat::LookRotation(Vec3::UnitY(), Vec3::UnitY());
        CHECK(IsFinite(degenerateUp));
    }

    TEST_CASE("GetAngle and GetAxis round-trip with FromAxisAngle")
    {
        const Vec3 axis = Vec3(0.2f, 0.9f, -0.38f).Normalized();
        const Angle angle = Radians(64.0f);
        const Quat rotation = Quat::FromAxisAngle(axis, angle);

        CHECK(rotation.GetAngle() == doctest::Approx(angle).epsilon(1e-4f));
        CHECK(ApproximatelyEqual(rotation.GetAxis(), axis, 1e-5f));
    }

    TEST_CASE("quaternion and matrix rotations agree")
    {
        const Vec3 axis = Vec3(-0.31f, 0.74f, 0.59f).Normalized();
        const Angle angle = Radians(38.0f);
        const Quat rotation = Quat::FromAxisAngle(axis, angle);

        const Vec3 point(1.0f, 2.0f, 3.0f);
        const Vec3 byQuaternion = rotation * point;
        const Vec3 byMatrix = Mat4::FromQuaternion(rotation).TransformPoint(point);

        CHECK(ApproximatelyEqual(byQuaternion, byMatrix, 1e-5f));
    }

    TEST_CASE("quaternions are trivially copyable and 16 bytes")
    {
        CHECK(std::is_trivially_copyable_v<Quat>);
        CHECK(sizeof(Quat) == 16);
    }
}
