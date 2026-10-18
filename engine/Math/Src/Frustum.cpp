#include <cmath>

#include "Hollowpoint/Math/Frustum.h"

namespace Hollowpoint::Math
{

namespace
{

//! Normalises a plane so that (X,Y,Z) is unit length and W is the signed distance from the origin.
Vec4 NormalizePlane(Vec4 plane)
{
    const Vec3 normal(plane.X, plane.Y, plane.Z);
    const Float32 length = normal.Length();
    if (length <= kDefaultAbsoluteEpsilon)
    {
        return Vec4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    const Float32 inverseLength = 1.0f / length;
    return plane * inverseLength;
}

}  // namespace

Frustum Frustum::FromViewProjection(const Mat4& viewProjection)
{
    const Vec4 row0 = viewProjection.GetRow(0);
    const Vec4 row1 = viewProjection.GetRow(1);
    const Vec4 row2 = viewProjection.GetRow(2);
    const Vec4 row3 = viewProjection.GetRow(3);

    // A point is inside the clip volume when -w <= x <= w, -w <= y <= w and 0 <= z <= w (Vulkan depth
    // range). Each inequality rearranges to a plane whose normal points inwards:
    Frustum frustum;
    frustum.m_Planes[static_cast<Size>(FrustumPlane::Left)] = NormalizePlane(row3 + row0);
    frustum.m_Planes[static_cast<Size>(FrustumPlane::Right)] = NormalizePlane(row3 - row0);
    frustum.m_Planes[static_cast<Size>(FrustumPlane::Bottom)] = NormalizePlane(row3 + row1);
    frustum.m_Planes[static_cast<Size>(FrustumPlane::Top)] = NormalizePlane(row3 - row1);
    frustum.m_Planes[static_cast<Size>(FrustumPlane::Near)] = NormalizePlane(row2);
    frustum.m_Planes[static_cast<Size>(FrustumPlane::Far)] = NormalizePlane(row3 - row2);
    return frustum;
}

Float32 Frustum::GetSignedDistance(const Vec4& plane, Vec3 point)
{
    return plane.X * point.X + plane.Y * point.Y + plane.Z * point.Z + plane.W;
}

IntersectionResult Frustum::TestPoint(Vec3 point) const
{
    for (const Vec4& plane : m_Planes)
    {
        if (GetSignedDistance(plane, point) < 0.0f)
        {
            return IntersectionResult::Outside;
        }
    }
    return IntersectionResult::Inside;
}

IntersectionResult Frustum::TestSphere(Vec3 center, Float32 radius) const
{
    IntersectionResult result = IntersectionResult::Inside;
    for (const Vec4& plane : m_Planes)
    {
        const Float32 distance = GetSignedDistance(plane, center);
        if (distance < -radius)
        {
            return IntersectionResult::Outside;
        }
        // Either the centre sits outside the plane, or it is inside but closer than the radius — both
        // mean the sphere straddles this plane.
        if (distance < radius)
        {
            result = IntersectionResult::Intersects;
        }
    }
    return result;
}

IntersectionResult Frustum::TestAABB(const AxisAlignedBox& box) const
{
    if (!box.IsValid())
    {
        return IntersectionResult::Outside;
    }

    IntersectionResult result = IntersectionResult::Inside;
    for (const Vec4& plane : m_Planes)
    {
        // The "p-vertex" is the box corner furthest in the direction of the plane normal; if that one
        // is behind the plane, the whole box is.
        const Vec3 pVertex((plane.X >= 0.0f) ? box.Max.X : box.Min.X, (plane.Y >= 0.0f) ? box.Max.Y : box.Min.Y,
                           (plane.Z >= 0.0f) ? box.Max.Z : box.Min.Z);

        const Float32 distance = GetSignedDistance(plane, pVertex);
        if (distance < 0.0f)
        {
            return IntersectionResult::Outside;
        }

        // The opposite corner (n-vertex) tells us whether the box straddles the plane.
        const Vec3 nVertex((plane.X >= 0.0f) ? box.Min.X : box.Max.X, (plane.Y >= 0.0f) ? box.Min.Y : box.Max.Y,
                           (plane.Z >= 0.0f) ? box.Min.Z : box.Max.Z);
        if (GetSignedDistance(plane, nVertex) < 0.0f)
        {
            result = IntersectionResult::Intersects;
        }
    }
    return result;
}

}  // namespace Hollowpoint::Math
