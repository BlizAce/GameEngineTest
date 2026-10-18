#include "TestSupport.h"

#include <doctest/doctest.h>

#include "Hollowpoint/Math/Angles.h"
#include "Hollowpoint/Math/MathFunctions.h"

using namespace Hollowpoint::Math;

TEST_SUITE("Math/Angles")
{
    TEST_CASE("zero Euler angles produce the identity rotation")
    {
        const EulerAngles none = EulerAngles::Zero();
        CHECK(ApproximatelyEqual(none.ToQuaternion(), Quat::Identity(), 1e-6f));
        CHECK(ApproximatelyEqual(none.ToMatrix(), Mat4::Identity(), 1e-6f));
    }

    TEST_CASE("single-axis Euler angles rotate about the right axis")
    {
        // Yaw about +Y: +X goes to -Z.
        const EulerAngles yaw(Radians(90.0f), 0.0f, 0.0f);
        CHECK(ApproximatelyEqual(yaw.ToQuaternion() * Vec3::UnitX(), -Vec3::UnitZ(), 1e-6f));

        // Pitch about +X: +Y goes to +Z.
        const EulerAngles pitch(0.0f, Radians(90.0f), 0.0f);
        CHECK(ApproximatelyEqual(pitch.ToQuaternion() * Vec3::UnitY(), Vec3::UnitZ(), 1e-6f));

        // Roll about +Z: +X goes to +Y.
        const EulerAngles roll(0.0f, 0.0f, Radians(90.0f));
        CHECK(ApproximatelyEqual(roll.ToQuaternion() * Vec3::UnitX(), Vec3::UnitY(), 1e-6f));
    }

    TEST_CASE("ToMatrix and ToQuaternion agree")
    {
        const EulerAngles angles(Radians(23.0f), Radians(-47.0f), Radians(81.0f));
        const Vec3 point(1.0f, 2.0f, 3.0f);

        const Vec3 byQuaternion = angles.ToQuaternion() * point;
        const Vec3 byMatrix = angles.ToMatrix().TransformPoint(point);
        CHECK(ApproximatelyEqual(byQuaternion, byMatrix, 1e-5f));
    }

    TEST_CASE("matrix round-trip recovers the original angles")
    {
        const EulerAngles original(Radians(35.0f), Radians(-20.0f), Radians(50.0f));
        const EulerAngles recovered = EulerAngles::FromMatrix(original.ToMatrix());

        CHECK(ApproximatelyEqual(recovered.Yaw, original.Yaw, 1e-4f));
        CHECK(ApproximatelyEqual(recovered.Pitch, original.Pitch, 1e-4f));
        CHECK(ApproximatelyEqual(recovered.Roll, original.Roll, 1e-4f));
    }

    TEST_CASE("gimbal lock still reconstructs the same rotation")
    {
        // At pitch = +/-90 degrees only (yaw - roll) is defined; the decomposition folds everything
        // into yaw. The rotation must still round-trip even though the angles do not.
        const EulerAngles locked(Radians(40.0f), Radians(90.0f), Radians(15.0f));
        const EulerAngles recovered = EulerAngles::FromMatrix(locked.ToMatrix());

        CHECK(ApproximatelyEqual(recovered.Pitch, Radians(90.0f), 1e-4f));
        CHECK(ApproximatelyEqual(recovered.ToQuaternion(), locked.ToQuaternion(), 1e-4f));
    }

    TEST_CASE("FromQuaternion agrees with FromMatrix")
    {
        const Quat rotation = Quat::FromAxisAngle(Vec3(0.1f, 0.95f, -0.3f).Normalized(), Radians(57.0f));
        const EulerAngles fromQuaternion = EulerAngles::FromQuaternion(rotation);
        const EulerAngles fromMatrix = EulerAngles::FromMatrix(Mat4::FromQuaternion(rotation));

        CHECK(ApproximatelyEqual(fromQuaternion.Yaw, fromMatrix.Yaw, 1e-4f));
        CHECK(ApproximatelyEqual(fromQuaternion.Pitch, fromMatrix.Pitch, 1e-4f));
        CHECK(ApproximatelyEqual(fromQuaternion.Roll, fromMatrix.Roll, 1e-4f));
    }

    TEST_CASE("equivalence compares rotations, not raw angle triples")
    {
        const EulerAngles a(Radians(30.0f), Radians(10.0f), 0.0f);
        // One extra full turn of yaw (390 degrees) is the same orientation.
        const EulerAngles sameButRotated(Radians(390.0f), Radians(10.0f), 0.0f);
        CHECK(ApproximatelyEqual(a, sameButRotated, 1e-4f));

        const EulerAngles different(Radians(90.0f), Radians(10.0f), 0.0f);
        CHECK_FALSE(ApproximatelyEqual(a, different, 1e-4f));
    }
}
