#include "TestSupport.h"

#include <doctest/doctest.h>

#include "Hollowpoint/Math/Transform.h"

using namespace Hollowpoint::Math;
using Hollowpoint::Test::ScopedAssertCapture;

TEST_SUITE("Math/Transform")
{
    TEST_CASE("the identity transform leaves everything unchanged")
    {
        const Transform identity = Transform::Identity();
        CHECK(identity.Position == Vec3::Zero());
        CHECK(identity.Scale == Vec3(1.0f));
        CHECK(ApproximatelyEqual(identity.ToMatrix(), Mat4::Identity(), 1e-6f));

        const Vec3 point(1.5f, -2.5f, 3.5f);
        CHECK(ApproximatelyEqual(identity.MapPoint(point), point, 1e-6f));
    }

    TEST_CASE("ToMatrix composes exactly T * R * S")
    {
        const Transform transform(Vec3(4.0f, 5.0f, 6.0f), Quat::FromAxisAngle(Vec3::UnitY(), Radians(30.0f)),
                                  Vec3(2.0f, 2.0f, 2.0f));

        const Mat4 expected = Mat4::Translate(transform.Position) * Mat4::FromQuaternion(transform.Rotation) *
                              Mat4::Scale(transform.Scale);
        CHECK(ApproximatelyEqual(transform.ToMatrix(), expected, 1e-5f));
    }

    TEST_CASE("MapPoint applies scale then rotation then translation")
    {
        const Transform transform(Vec3(10.0f, 0.0f, 0.0f), Quat::FromAxisAngle(Vec3::UnitZ(), kHalfPi),
                                  Vec3(2.0f, 1.0f, 1.0f));

        // Local +X scaled by 2 then rotated 90 degrees about Z lands on +Y, plus the translation.
        CHECK(ApproximatelyEqual(transform.MapPoint(Vec3(1.0f, 0.0f, 0.0f)), Vec3(10.0f, 2.0f, 0.0f), 1e-5f));
        // The local origin maps to the translation exactly.
        CHECK(ApproximatelyEqual(transform.MapPoint(Vec3::Zero()), transform.Position, 1e-6f));
    }

    TEST_CASE("MapDirection ignores translation but honours scale")
    {
        const Transform transform(Vec3(100.0f, 100.0f, 100.0f), Quat::Identity(), Vec3(2.0f, 3.0f, 4.0f));
        CHECK(ApproximatelyEqual(transform.MapDirection(Vec3(1.0f, 1.0f, 1.0f)), Vec3(2.0f, 3.0f, 4.0f), 1e-6f));
    }

    TEST_CASE("Concatenated matches matrix composition for a parent/child pair")
    {
        const Transform parent(Vec3(1.0f, 2.0f, 3.0f), Quat::FromAxisAngle(Vec3::UnitY(), Radians(45.0f)), Vec3(2.0f));
        const Transform child(Vec3(0.5f, 0.0f, 0.0f), Quat::FromAxisAngle(Vec3::UnitX(), Radians(30.0f)),
                              Vec3(1.5f, 1.0f, 1.0f));

        const Transform composed = child.Concatenated(parent);
        const Mat4 byMatrices = parent.ToMatrix() * child.ToMatrix();

        CHECK(ApproximatelyEqual(composed.ToMatrix(), byMatrices, 1e-4f));
    }

    TEST_CASE("nested concatenation is associative")
    {
        const Transform a(Vec3(1.0f, 0.0f, 0.0f), Quat::FromAxisAngle(Vec3::UnitZ(), Radians(10.0f)), Vec3(1.0f));
        const Transform b(Vec3(0.0f, 2.0f, 0.0f), Quat::FromAxisAngle(Vec3::UnitY(), Radians(20.0f)), Vec3(2.0f));
        const Transform c(Vec3(0.0f, 0.0f, 3.0f), Quat::FromAxisAngle(Vec3::UnitX(), Radians(30.0f)), Vec3(1.0f));

        // ((c in b) in a) must equal (c in (b in a)).
        const Transform left = c.Concatenated(b).Concatenated(a);
        const Transform right = c.Concatenated(b.Concatenated(a));
        CHECK(ApproximatelyEqual(left, right, 1e-4f));
    }

    TEST_CASE("the operator* overload reads parent-to-child")
    {
        const Transform parent(Vec3(5.0f, 0.0f, 0.0f), Quat::Identity(), Vec3(2.0f));
        const Transform child(Vec3(1.0f, 0.0f, 0.0f), Quat::Identity(), Vec3(1.0f));

        // child in parent space: position 5 + 1*2 = 7 with scale 2.
        const Transform composed = child.Concatenated(parent);
        CHECK(ApproximatelyEqual(composed.Position, Vec3(7.0f, 0.0f, 0.0f), 1e-6f));
        CHECK(ApproximatelyEqual(composed.Scale, Vec3(2.0f, 2.0f, 2.0f), 1e-6f));

        // The operator form composes the same way: parent * child == child in parent.
        const Transform viaOperator = parent * child;
        CHECK(ApproximatelyEqual(viaOperator.Position, composed.Position, 1e-5f));
    }

    TEST_CASE("decomposition round-trips a full transform")
    {
        const Transform original(Vec3(-3.5f, 12.25f, 0.125f),
                                 Quat::FromAxisAngle(Vec3(0.2f, 0.9f, -0.38f).Normalized(), Radians(71.0f)),
                                 Vec3(1.0f, 2.5f, 0.5f));

        const Transform decomposed = Decompose(original.ToMatrix());

        CHECK(ApproximatelyEqual(decomposed.Position, original.Position, 1e-4f));
        CHECK(ApproximatelyEqual(decomposed.Scale, original.Scale, 1e-4f));
        // Rotation is compared by behaviour because q and -q are the same rotation.
        CHECK(ApproximatelyEqual(decomposed.Rotation * Vec3::UnitX(), original.Rotation * Vec3::UnitX(), 1e-4f));
    }

    TEST_CASE("decomposing a matrix with a mirrored axis keeps a faithful reconstruction")
    {
        const Mat4 mirrored = Mat4::Translate(Vec3(1.0f, 2.0f, 3.0f)) * Mat4::Scale(Vec3(-2.0f, 1.0f, 1.0f));
        const Transform decomposed = Decompose(mirrored);

        CHECK(decomposed.Scale.X < 0.0f);
        CHECK(ApproximatelyEqual(decomposed.ToMatrix(), mirrored, 1e-4f));
    }

    TEST_CASE("a degenerate (zero-scale) matrix decomposes without NaN")
    {
        const Mat4 flattened = Mat4::Scale(Vec3(1.0f, 0.0f, 1.0f));
        const Transform decomposed = Decompose(flattened);

        CHECK(IsFinite(decomposed.Position));
        CHECK(IsFinite(decomposed.Scale));
        CHECK(IsFinite(decomposed.Rotation));
    }

    TEST_CASE("IsUniformScale detects non-uniform scaling")
    {
        CHECK(Transform::Identity().IsUniformScale());
        CHECK(Transform(Vec3::Zero(), Quat::Identity(), Vec3(3.0f)).IsUniformScale());
        CHECK_FALSE(Transform(Vec3::Zero(), Quat::Identity(), Vec3(1.0f, 2.0f, 1.0f)).IsUniformScale());
    }

    TEST_CASE("Euler angle accessors round-trip through the rotation")
    {
        Transform transform;
        transform.SetEulerAngles(EulerAngles(Radians(30.0f), Radians(-15.0f), Radians(60.0f)));

        const EulerAngles readBack = transform.GetEulerAngles();
        CHECK(ApproximatelyEqual(readBack, EulerAngles(Radians(30.0f), Radians(-15.0f), Radians(60.0f)), 1e-4f));
    }

    TEST_CASE("MapRotation composes rotations in the parent's frame")
    {
        const Transform parent(Vec3::Zero(), Quat::FromAxisAngle(Vec3::UnitY(), kHalfPi), Vec3(1.0f));
        const Quat local = Quat::FromAxisAngle(Vec3::UnitY(), kHalfPi);

        const Quat world = parent.MapRotation(local);
        CHECK(ApproximatelyEqual(world.GetAngle(), Radians(180.0f), 1e-4f));
    }
}
