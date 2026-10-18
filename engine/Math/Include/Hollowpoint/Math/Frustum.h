#pragma once

#include <array>

#include "Hollowpoint/Math/BoundingBox.h"
#include "Hollowpoint/Math/MathCore.h"

namespace Hollowpoint::Math
{

// ---------------------------------------------------------------------------
// Frustum — six half-space planes extracted from a view-projection matrix.
//
// Planes face inwards (the positive side is the visible side) and are normalised, so the signed plane
// distance is also the world-space distance to the plane. Extraction follows the Gribb/Hartmann method:
// for a row-major-conceptual clip volume, plane i is derived from sums/differences of the matrix rows;
// our matrices are column-major so the rows are read via Mat4::GetRow.
// ---------------------------------------------------------------------------

enum class FrustumPlane : UInt8
{
    Near = 0,
    Far,
    Left,
    Right,
    Top,
    Bottom,
    Count = 6
};

enum class IntersectionResult : UInt8
{
    Outside = 0,  //!< Fully behind at least one plane.
    Intersects,   //!< Partially inside (straddles a plane).
    Inside        //!< Fully inside all planes.
};

class HP_MATH_API Frustum
{
public:
    Frustum() = default;

    //! Rebuilds the planes from `viewProjectionMatrix` (project-from-camera to clip space).
    static Frustum FromViewProjection(const Mat4& viewProjectionMatrix);

    [[nodiscard]] const Vec4& GetPlane(FrustumPlane plane) const
    {
        return m_Planes[static_cast<Size>(plane)];
    }

    //! Signed distance from `point` to the plane; negative means outside.
    [[nodiscard]] static Float32 GetSignedDistance(const Vec4& plane, Vec3 point);

    [[nodiscard]] IntersectionResult TestPoint(Vec3 point) const;
    [[nodiscard]] IntersectionResult TestSphere(Vec3 center, Float32 radius) const;

    //! Conservative AABB test using the "p-vertex" (the corner furthest along the plane normal).
    [[nodiscard]] IntersectionResult TestAABB(const AxisAlignedBox& box) const;

private:
    std::array<Vec4, static_cast<Size>(FrustumPlane::Count)> m_Planes{};
};

}  // namespace Hollowpoint::Math
