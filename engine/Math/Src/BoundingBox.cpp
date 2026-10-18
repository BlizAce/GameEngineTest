#include <algorithm>
#include <cmath>
#include <limits>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Math/BoundingBox.h"

namespace Hollowpoint::Math
{

// ---------------------------------------------------------------------------
// AxisAlignedBox
// ---------------------------------------------------------------------------

AxisAlignedBox AxisAlignedBox::Invalid()
{
    // Min > Max in every component: Contains() is false for everything and ExpandToContain() works.
    constexpr Float32 largest = std::numeric_limits<Float32>::max();
    return AxisAlignedBox(Vec3(largest), Vec3(-largest));
}

AxisAlignedBox AxisAlignedBox::UnitCube()
{
    return AxisAlignedBox(Vec3(-0.5f, -0.5f, -0.5f), Vec3(0.5f, 0.5f, 0.5f));
}

AxisAlignedBox AxisAlignedBox::FromCenterAndExtent(Vec3 center, Vec3 extent)
{
    return AxisAlignedBox(center - extent.Abs(), center + extent.Abs());
}

bool AxisAlignedBox::IsValid() const
{
    return Min.X <= Max.X && Min.Y <= Max.Y && Min.Z <= Max.Z;
}

Vec3 AxisAlignedBox::GetCenter() const
{
    return (Min + Max) * 0.5f;
}

Vec3 AxisAlignedBox::GetExtent() const
{
    return (Max - Min) * 0.5f;
}

Vec3 AxisAlignedBox::GetSize() const
{
    return Max - Min;
}

Float32 AxisAlignedBox::GetVolume() const
{
    if (!IsValid())
    {
        return 0.0f;
    }
    const Vec3 size = GetSize();
    return size.X * size.Y * size.Z;
}

Float32 AxisAlignedBox::GetSurfaceArea() const
{
    if (!IsValid())
    {
        return 0.0f;
    }
    const Vec3 size = GetSize();
    return 2.0f * (size.X * size.Y + size.X * size.Z + size.Y * size.Z);
}

Float32 AxisAlignedBox::GetMaxExtent() const
{
    if (!IsValid())
    {
        return 0.0f;
    }
    return std::max({GetExtent().X, GetExtent().Y, GetExtent().Z});
}

void AxisAlignedBox::ExpandToContain(Vec3 point)
{
    Min = Min.Min(point);
    Max = Max.Max(point);
}

void AxisAlignedBox::ExpandToContain(const AxisAlignedBox& other)
{
    if (!other.IsValid())
    {
        return;
    }
    ExpandToContain(other.Min);
    ExpandToContain(other.Max);
}

void AxisAlignedBox::ExpandBy(Float32 amount)
{
    if (!IsValid())
    {
        return;
    }
    Min -= Vec3(amount);
    Max += Vec3(amount);
}

AxisAlignedBox AxisAlignedBox::Merged(const AxisAlignedBox& other) const
{
    AxisAlignedBox result = *this;
    result.ExpandToContain(other);
    return result;
}

AxisAlignedBox AxisAlignedBox::Transformed(const Mat4& matrix) const
{
    if (!IsValid())
    {
        return Invalid();
    }

    // Transforming all eight corners and re-deriving the AABB is exact for an axis-aligned box under
    // an arbitrary affine transform, and cheap enough that the "transform basis vectors" optimisation
    // is not worth the complexity here.
    AxisAlignedBox result = Invalid();
    for (std::size_t cornerIndex = 0; cornerIndex < 8; ++cornerIndex)
    {
        result.ExpandToContain(matrix.TransformPoint(GetCorner(cornerIndex)));
    }
    return result;
}

bool AxisAlignedBox::Contains(Vec3 point) const
{
    return IsValid() && point.X >= Min.X && point.X <= Max.X && point.Y >= Min.Y && point.Y <= Max.Y &&
           point.Z >= Min.Z && point.Z <= Max.Z;
}

bool AxisAlignedBox::Contains(const AxisAlignedBox& other) const
{
    if (!other.IsValid())
    {
        return true;  // The empty set is contained in every box.
    }
    return IsValid() && other.Min.X >= Min.X && other.Max.X <= Max.X && other.Min.Y >= Min.Y && other.Max.Y <= Max.Y &&
           other.Min.Z >= Min.Z && other.Max.Z <= Max.Z;
}

bool AxisAlignedBox::Intersects(const AxisAlignedBox& other) const
{
    if (!IsValid() || !other.IsValid())
    {
        return false;
    }
    return Min.X <= other.Max.X && Max.X >= other.Min.X && Min.Y <= other.Max.Y && Max.Y >= other.Min.Y &&
           Min.Z <= other.Max.Z && Max.Z >= other.Min.Z;
}

bool AxisAlignedBox::IntersectsRay(Vec3 rayOrigin, Vec3 rayDirectionNormalized, Float32 maxDistance,
                                   Float32* outHitDistance) const
{
    if (outHitDistance != nullptr)
    {
        *outHitDistance = 0.0f;
    }
    if (!IsValid())
    {
        return false;
    }

    // Classic slab test. Divisions by zero produce +/-inf, which the min/max bookkeeping handles
    // correctly for rays parallel to a slab — that is why no special case is needed here.
    Float32 tMinimum = 0.0f;
    Float32 tMaximum = maxDistance;

    const Float32 origin[3] = {rayOrigin.X, rayOrigin.Y, rayOrigin.Z};
    const Float32 direction[3] = {rayDirectionNormalized.X, rayDirectionNormalized.Y, rayDirectionNormalized.Z};
    const Float32 minimum[3] = {Min.X, Min.Y, Min.Z};
    const Float32 maximum[3] = {Max.X, Max.Y, Max.Z};

    for (int axis = 0; axis < 3; ++axis)
    {
        if (std::fabs(direction[axis]) <= kDefaultAbsoluteEpsilon)
        {
            // Ray is parallel to this slab: it hits only if the origin is inside the slab.
            if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis])
            {
                return false;
            }
            continue;
        }

        const Float32 inverseDirection = 1.0f / direction[axis];
        Float32 tNear = (minimum[axis] - origin[axis]) * inverseDirection;
        Float32 tFar = (maximum[axis] - origin[axis]) * inverseDirection;
        if (tNear > tFar)
        {
            std::swap(tNear, tFar);
        }

        tMinimum = tNear > tMinimum ? tNear : tMinimum;
        tMaximum = tFar < tMaximum ? tFar : tMaximum;

        if (tMinimum > tMaximum)
        {
            return false;
        }
    }

    if (outHitDistance != nullptr)
    {
        *outHitDistance = tMinimum;
    }
    return true;
}

bool AxisAlignedBox::IntersectsSphere(Vec3 center, Float32 radius) const
{
    if (!IsValid())
    {
        return false;
    }
    const Vec3 closest = GetClosestPoint(center);
    const Float32 squaredDistance = (closest - center).LengthSquared();
    return squaredDistance <= radius * radius;
}

Vec3 AxisAlignedBox::GetClosestPoint(Vec3 point) const
{
    if (!IsValid())
    {
        return point;
    }
    return Vec3(std::max(Min.X, std::min(point.X, Max.X)), std::max(Min.Y, std::min(point.Y, Max.Y)),
                std::max(Min.Z, std::min(point.Z, Max.Z)));
}

Vec3 AxisAlignedBox::GetCorner(std::size_t cornerIndex) const
{
    HP_ASSERT_ARGUMENT(cornerIndex < 8, "corner index must be in [0,8)");
    return Vec3((cornerIndex & 1u) ? Max.X : Min.X, (cornerIndex & 2u) ? Max.Y : Min.Y,
                (cornerIndex & 4u) ? Max.Z : Min.Z);
}

// ---------------------------------------------------------------------------
// BoundingSphere
// ---------------------------------------------------------------------------

BoundingSphere BoundingSphere::FromAABB(const AxisAlignedBox& box)
{
    if (!box.IsValid())
    {
        return BoundingSphere(Vec3(0.0f), 0.0f);
    }
    const Vec3 center = box.GetCenter();
    return BoundingSphere(center, (box.Max - center).Length());
}

bool BoundingSphere::Contains(Vec3 point) const
{
    return (point - Center).LengthSquared() <= Radius * Radius;
}

bool BoundingSphere::Intersects(const BoundingSphere& other) const
{
    const Float32 radiusSum = Radius + other.Radius;
    return (Center - other.Center).LengthSquared() <= radiusSum * radiusSum;
}

bool BoundingSphere::Intersects(const AxisAlignedBox& box) const
{
    return box.IntersectsSphere(Center, Radius);
}

bool BoundingSphere::IntersectsRay(Vec3 rayOrigin, Vec3 rayDirectionNormalized, Float32 maxDistance) const
{
    // Solve |origin + t*direction - center|^2 = r^2 for the smallest non-negative t.
    const Vec3 toCenter = rayOrigin - Center;
    const Float32 b = toCenter.Dot(rayDirectionNormalized);
    const Float32 c = toCenter.LengthSquared() - Radius * Radius;
    const Float32 discriminant = b * b - c;

    if (discriminant < 0.0f)
    {
        return false;
    }

    const Float32 squareRoot = std::sqrt(discriminant);
    const Float32 tNear = -b - squareRoot;
    const Float32 tFar = -b + squareRoot;

    // A hit counts if the ray is outside and enters within range, or if it starts inside the sphere.
    if (tFar < 0.0f)
    {
        return false;
    }
    const Float32 tHit = tNear >= 0.0f ? tNear : 0.0f;
    return tHit <= maxDistance;
}

BoundingSphere BoundingSphere::Transformed(const Mat4& matrix) const
{
    const Vec3 transformedCenter = matrix.TransformPoint(Center);

    // Uniform scale is exact; for non-uniform scale we must take the largest axis scale so the sphere
    // stays conservative (under-estimating would cull visible geometry).
    const Vec3 scale = matrix.GetScale();
    const Float32 largestScale = std::max({std::fabs(scale.X), std::fabs(scale.Y), std::fabs(scale.Z)});
    return BoundingSphere(transformedCenter, Radius * largestScale);
}

Float32 BoundingSphere::GetSquaredDistanceTo(Vec3 point) const
{
    const Float32 distance = (point - Center).Length() - Radius;
    return distance > 0.0f ? distance * distance : 0.0f;
}

}  // namespace Hollowpoint::Math
