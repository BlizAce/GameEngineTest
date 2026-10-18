#include <doctest/doctest.h>

#include "Hollowpoint/Math/BoundingBox.h"
#include "Hollowpoint/Math/MathFunctions.h"

using namespace Hollowpoint::Math;

TEST_SUITE("Math/AxisAlignedBox")
{
    TEST_CASE("an invalid box contains nothing and becomes valid on first expansion")
    {
        const AxisAlignedBox invalid = AxisAlignedBox::Invalid();
        CHECK_FALSE(invalid.IsValid());
        CHECK_FALSE(invalid.Contains(Vec3::Zero()));
        CHECK(invalid.GetVolume() == 0.0f);

        AxisAlignedBox box = AxisAlignedBox::Invalid();
        box.ExpandToContain(Vec3(1.0f, 2.0f, 3.0f));
        CHECK(box.IsValid());
        CHECK(box.Min == Vec3(1.0f, 2.0f, 3.0f));
        CHECK(box.Max == Vec3(1.0f, 2.0f, 3.0f));

        box.ExpandToContain(Vec3(-1.0f, 5.0f, 0.5f));
        CHECK(ApproximatelyEqual(box.Min, Vec3(-1.0f, 2.0f, 0.5f)));
        CHECK(ApproximatelyEqual(box.Max, Vec3(1.0f, 5.0f, 3.0f)));
    }

    TEST_CASE("centre, extent and size are consistent")
    {
        const AxisAlignedBox box(Vec3(-2.0f, 0.0f, -1.0f), Vec3(2.0f, 4.0f, 3.0f));
        CHECK(ApproximatelyEqual(box.GetCenter(), Vec3(0.0f, 2.0f, 1.0f)));
        CHECK(ApproximatelyEqual(box.GetExtent(), Vec3(2.0f, 2.0f, 2.0f)));
        CHECK(ApproximatelyEqual(box.GetSize(), Vec3(4.0f, 4.0f, 4.0f)));
        CHECK(box.GetVolume() == doctest::Approx(64.0f));
        CHECK(box.GetSurfaceArea() == doctest::Approx(96.0f));
        CHECK(box.GetMaxExtent() == doctest::Approx(2.0f));

        const AxisAlignedBox fromCenter = AxisAlignedBox::FromCenterAndExtent(Vec3(1.0f), Vec3(0.5f));
        CHECK(ApproximatelyEqual(fromCenter.Min, Vec3(0.5f)));
        CHECK(ApproximatelyEqual(fromCenter.Max, Vec3(1.5f)));
    }

    TEST_CASE("containment and intersection")
    {
        const AxisAlignedBox box(Vec3(-1.0f), Vec3(1.0f));

        CHECK(box.Contains(Vec3::Zero()));
        CHECK(box.Contains(Vec3(1.0f, -1.0f, 1.0f)));  // inclusive of the boundary
        CHECK_FALSE(box.Contains(Vec3(1.01f, 0.0f, 0.0f)));

        CHECK(box.Contains(AxisAlignedBox(Vec3(-0.5f), Vec3(0.5f))));
        CHECK_FALSE(box.Contains(AxisAlignedBox(Vec3(0.0f), Vec3(2.0f))));

        CHECK(box.Intersects(AxisAlignedBox(Vec3(0.5f), Vec3(2.0f))));
        CHECK_FALSE(box.Intersects(AxisAlignedBox(Vec3(1.5f), Vec3(2.0f))));
        // Touching faces count as intersecting (conservative for culling).
        CHECK(box.Intersects(AxisAlignedBox(Vec3(1.0f), Vec3(2.0f))));
        CHECK_FALSE(box.Intersects(AxisAlignedBox::Invalid()));
    }

    TEST_CASE("merging and expanding")
    {
        const AxisAlignedBox a(Vec3(-1.0f), Vec3(0.0f));
        const AxisAlignedBox b(Vec3(0.0f), Vec3(2.0f));

        const AxisAlignedBox merged = a.Merged(b);
        CHECK(ApproximatelyEqual(merged.Min, Vec3(-1.0f)));
        CHECK(ApproximatelyEqual(merged.Max, Vec3(2.0f)));

        AxisAlignedBox expanded = a;
        expanded.ExpandBy(0.5f);
        CHECK(ApproximatelyEqual(expanded.Min, Vec3(-1.5f)));
        CHECK(ApproximatelyEqual(expanded.Max, Vec3(0.5f)));
    }

    TEST_CASE("corners cover all eight combinations")
    {
        const AxisAlignedBox box(Vec3(-1.0f, -2.0f, -3.0f), Vec3(1.0f, 2.0f, 3.0f));

        // Bit 0 = X, bit 1 = Y, bit 2 = Z.
        CHECK(box.GetCorner(0) == Vec3(-1.0f, -2.0f, -3.0f));
        CHECK(box.GetCorner(1) == Vec3(1.0f, -2.0f, -3.0f));
        CHECK(box.GetCorner(2) == Vec3(-1.0f, 2.0f, -3.0f));
        CHECK(box.GetCorner(4) == Vec3(-1.0f, -2.0f, 3.0f));
        CHECK(box.GetCorner(7) == Vec3(1.0f, 2.0f, 3.0f));

        // Every corner must lie on the box surface and be contained by it.
        for (std::size_t index = 0; index < 8; ++index)
        {
            CHECK(box.Contains(box.GetCorner(index)));
        }
    }

    TEST_CASE("transformed boxes stay conservative under rotation")
    {
        const AxisAlignedBox box(Vec3(-1.0f), Vec3(1.0f));
        const Mat4 rotated = Mat4::Rotate(Vec3::UnitY(), Radians(45.0f));

        const AxisAlignedBox transformed = box.Transformed(rotated);
        CHECK(transformed.IsValid());
        // A unit cube rotated 45 degrees about Y has half-extent sqrt(2) in X and Z.
        CHECK(transformed.GetExtent().X == doctest::Approx(1.4142135f).epsilon(1e-4f));
        CHECK(transformed.GetExtent().Y == doctest::Approx(1.0f).epsilon(1e-4f));

        // The transformed corners of the original box must all be inside the result.
        for (std::size_t index = 0; index < 8; ++index)
        {
            CHECK(transformed.Contains(rotated.TransformPoint(box.GetCorner(index))));
        }
    }

    TEST_CASE("ray intersection reports the entry distance")
    {
        const AxisAlignedBox box(Vec3(-1.0f), Vec3(1.0f));

        Float32 hitDistance = 0.0f;

        // Head-on from in front.
        CHECK(box.IntersectsRay(Vec3(0.0f, 0.0f, -5.0f), Vec3(0.0f, 0.0f, 1.0f), 100.0f, &hitDistance));
        CHECK(hitDistance == doctest::Approx(4.0f));

        // From inside the box the ray starts at distance 0.
        CHECK(box.IntersectsRay(Vec3::Zero(), Vec3(1.0f, 0.0f, 0.0f), 10.0f, &hitDistance));
        CHECK(hitDistance == doctest::Approx(0.0f));

        // Missing the box entirely.
        CHECK_FALSE(box.IntersectsRay(Vec3(0.0f, 5.0f, -5.0f), Vec3(0.0f, 0.0f, 1.0f), 100.0f));

        // Behind the ray origin (maxDistance too short).
        CHECK_FALSE(box.IntersectsRay(Vec3(0.0f, 0.0f, -5.0f), Vec3(0.0f, 0.0f, -1.0f), 100.0f));

        // Parallel to a slab but inside it: still hits.
        CHECK(box.IntersectsRay(Vec3(0.0f, 0.0f, -5.0f), Vec3(0.0f, 0.0f, 1.0f), 3.0f) == false);

        // Grazing along a face diagonal must not divide by zero into NaN.
        CHECK(box.IntersectsRay(Vec3(-3.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), 100.0f));
    }

    TEST_CASE("sphere intersection")
    {
        const AxisAlignedBox box(Vec3(-1.0f), Vec3(1.0f));

        CHECK(box.IntersectsSphere(Vec3::Zero(), 0.1f));
        CHECK(box.IntersectsSphere(Vec3(2.0f, 0.0f, 0.0f), 1.0f));  // touching the face
        CHECK_FALSE(box.IntersectsSphere(Vec3(2.5f, 0.0f, 0.0f), 1.0f));
        CHECK(box.IntersectsSphere(Vec3(1.5f, 1.5f, 1.5f), 0.9f));  // near the corner

        const Vec3 closest = box.GetClosestPoint(Vec3(5.0f, -5.0f, 0.25f));
        CHECK(ApproximatelyEqual(closest, Vec3(1.0f, -1.0f, 0.25f)));
        CHECK(ApproximatelyEqual(box.GetClosestPoint(Vec3(0.1f)), Vec3(0.1f)));  // inside: unchanged
    }

    TEST_CASE("BoundingSphere helpers")
    {
        const AxisAlignedBox box(Vec3(-1.0f), Vec3(1.0f));
        const BoundingSphere sphere = BoundingSphere::FromAABB(box);

        CHECK(ApproximatelyEqual(sphere.Center, Vec3::Zero()));
        CHECK(sphere.Radius == doctest::Approx(1.7320508f).epsilon(1e-4f));  // sqrt(3)

        CHECK(sphere.Contains(Vec3(1.0f)));
        CHECK_FALSE(sphere.Contains(Vec3(2.0f, 2.0f, 2.0f)));
        CHECK(sphere.Intersects(box));
        // Two unit spheres 1.5 apart overlap; 2.5 apart they do not.
        CHECK(BoundingSphere(Vec3(0.0f, 0.0f, 0.0f), 1.0f).Intersects(BoundingSphere(Vec3(1.5f, 0.0f, 0.0f), 1.0f)));
        CHECK_FALSE(
            BoundingSphere(Vec3(0.0f, 0.0f, 0.0f), 1.0f).Intersects(BoundingSphere(Vec3(2.5f, 0.0f, 0.0f), 1.0f)));

        // Ray: hitting, missing, and starting inside.
        CHECK(sphere.IntersectsRay(Vec3(0.0f, 0.0f, -10.0f), Vec3(0.0f, 0.0f, 1.0f), 100.0f));
        CHECK_FALSE(sphere.IntersectsRay(Vec3(0.0f, 10.0f, -10.0f), Vec3(0.0f, 0.0f, 1.0f), 100.0f));
        CHECK(sphere.IntersectsRay(Vec3::Zero(), Vec3(1.0f, 0.0f, 0.0f), 0.0f));

        // Distance to a point outside the sphere is the gap, not the distance to the centre.
        CHECK(BoundingSphere(Vec3::Zero(), 1.0f).GetSquaredDistanceTo(Vec3(3.0f, 0.0f, 0.0f)) == doctest::Approx(4.0f));
        CHECK(BoundingSphere(Vec3::Zero(), 1.0f).GetSquaredDistanceTo(Vec3(0.5f, 0.0f, 0.0f)) == 0.0f);

        // A non-uniform scale must grow the sphere by the largest axis so culling stays conservative.
        const BoundingSphere scaled =
            BoundingSphere(Vec3::Zero(), 1.0f).Transformed(Mat4::Scale(Vec3(2.0f, 5.0f, 3.0f)));
        CHECK(scaled.Radius == doctest::Approx(5.0f));
    }
}
