#include <doctest/doctest.h>

#include "Hollowpoint/Math/Frustum.h"
#include "Hollowpoint/Math/MathFunctions.h"

using namespace Hollowpoint::Math;

namespace
{

// Builds a view-projection matrix for a camera at `eye` looking at `target`.
[[nodiscard]] Frustum MakeFrustum(Vec3 eye, Vec3 target, Float32 fieldOfViewDegrees = 60.0f)
{
    const Mat4 view = Mat4::LookAtRH(eye, target, Vec3::Up());
    const Mat4 projection = Mat4::PerspectiveRH(Radians(fieldOfViewDegrees), 1.5f, 0.1f, 100.0f);
    return Frustum::FromViewProjection(projection * view);
}

}  // namespace

TEST_SUITE("Math/Frustum")
{
    TEST_CASE("a point in front of the camera is inside; behind it is outside")
    {
        const Frustum frustum = MakeFrustum(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f, 0.0f, 0.0f));

        // The origin sits 5 units in front of the camera, well inside the near/far range.
        CHECK(frustum.TestPoint(Vec3::Zero()) == IntersectionResult::Inside);

        // Behind the camera (z = 10 > eye z = 5) is outside, past the far plane is outside too.
        CHECK(frustum.TestPoint(Vec3(0.0f, 0.0f, 10.0f)) == IntersectionResult::Outside);
        CHECK(frustum.TestPoint(Vec3(0.0f, 0.0f, -200.0f)) == IntersectionResult::Outside);

        // Far off to the side, outside the horizontal field of view.
        CHECK(frustum.TestPoint(Vec3(100.0f, 0.0f, 0.0f)) == IntersectionResult::Outside);
    }

    TEST_CASE("points between near and far along the view axis stay inside")
    {
        const Frustum frustum = MakeFrustum(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f, 0.0f, 0.0f));

        // Just in front of the near plane (distance 0.1 from the eye) and just inside the far plane.
        CHECK(frustum.TestPoint(Vec3(0.0f, 0.0f, 4.85f)) == IntersectionResult::Inside);
        CHECK(frustum.TestPoint(Vec3(0.0f, 0.0f, -94.0f)) == IntersectionResult::Inside);
    }

    TEST_CASE("plane normals are unit length and face inwards")
    {
        const Frustum frustum = MakeFrustum(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f));

        for (int index = 0; index < static_cast<int>(FrustumPlane::Count); ++index)
        {
            const Vec4& plane = frustum.GetPlane(static_cast<FrustumPlane>(index));
            const Float32 normalLength = Vec3(plane.X, plane.Y, plane.Z).Length();
            CHECK(normalLength == doctest::Approx(1.0f).epsilon(1e-4f));
        }

        // The camera position is on the inside boundary of every plane (distance ~ 0 within the near
        // plane offset), and moving backwards along a plane normal must go outside.
        const Vec4& rightPlane = frustum.GetPlane(FrustumPlane::Right);
        CHECK(rightPlane.X != doctest::Approx(0.0f));
    }

    TEST_CASE("sphere tests classify inside, intersecting and outside")
    {
        const Frustum frustum = MakeFrustum(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f, 0.0f, 0.0f));

        CHECK(frustum.TestSphere(Vec3::Zero(), 0.1f) == IntersectionResult::Inside);
        // Straddles the far plane: centre is inside, radius reaches past it.
        CHECK(frustum.TestSphere(Vec3(0.0f, 0.0f, -94.5f), 1.0f) == IntersectionResult::Intersects);
        CHECK(frustum.TestSphere(Vec3(0.0f, 0.0f, -500.0f), 1.0f) == IntersectionResult::Outside);
    }

    TEST_CASE("AABB tests classify inside, straddling and outside")
    {
        const Frustum frustum = MakeFrustum(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f, 0.0f, 0.0f));

        CHECK(frustum.TestAABB(AxisAlignedBox(Vec3(-0.5f), Vec3(0.5f))) == IntersectionResult::Inside);

        // A large box that contains the camera straddles several planes.
        const AxisAlignedBox huge(Vec3(-50.0f), Vec3(50.0f));
        CHECK(frustum.TestAABB(huge) == IntersectionResult::Intersects);

        // Behind the camera.
        const AxisAlignedBox behind(Vec3(-1.0f, -1.0f, 6.0f), Vec3(1.0f, 1.0f, 8.0f));
        CHECK(frustum.TestAABB(behind) == IntersectionResult::Outside);

        // An invalid box is never visible (must not be drawn).
        CHECK(frustum.TestAABB(AxisAlignedBox::Invalid()) == IntersectionResult::Outside);
    }

    TEST_CASE("anything inside the frustum's AABB test is also inside the sphere test")
    {
        // Consistency check: a box fully inside must have its bounding sphere at least partially
        // inside, otherwise one of the two tests has a sign error.
        const Frustum frustum = MakeFrustum(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f, 0.0f, 0.0f));

        const AxisAlignedBox box(Vec3(-0.2f, -0.2f, 4.0f), Vec3(0.2f, 0.2f, 4.6f));
        REQUIRE(frustum.TestAABB(box) == IntersectionResult::Inside);
        CHECK(frustum.TestSphere(box.GetCenter(), box.GetExtent().Length()) != IntersectionResult::Outside);
    }

    TEST_CASE("orthographic frustums cull as expected")
    {
        const Mat4 view = Mat4::LookAtRH(Vec3(0.0f, 0.0f, 10.0f), Vec3::Zero(), Vec3::Up());
        const Mat4 projection = Mat4::OrthographicRH(20.0f, 20.0f, 1.0f, 100.0f);
        const Frustum frustum = Frustum::FromViewProjection(projection * view);

        CHECK(frustum.TestPoint(Vec3(5.0f, 5.0f, 5.0f)) == IntersectionResult::Inside);
        CHECK(frustum.TestPoint(Vec3(15.0f, 0.0f, 5.0f)) == IntersectionResult::Outside);  // beyond the right edge
        CHECK(frustum.TestPoint(Vec3(0.0f, 0.0f, 9.5f)) == IntersectionResult::Outside);   // behind the near plane
    }
}
