#pragma once

#include "Hollowpoint/Math/MathCore.h"

namespace Hollowpoint::Math
{

// ---------------------------------------------------------------------------
// AxisAlignedBox — the workhorse bounding volume for culling and broad-phase queries.
//
// An "empty" box has Min > Max in at least one component; Invalid() is that state, and it is a valid
// starting point for accumulating points (any point swallowed by an empty box yields a box containing
// just that point). Extent is the half-size, matching the convention used by the renderer.
// ---------------------------------------------------------------------------

class HP_MATH_API AxisAlignedBox
{
public:
    Vec3 Min = Vec3(0.0f);
    Vec3 Max = Vec3(0.0f);

    //! Creates a zero-size box at `center`.
    explicit AxisAlignedBox(Vec3 center) : Min(center), Max(center) {}
    AxisAlignedBox(Vec3 minimum, Vec3 maximum) : Min(minimum), Max(maximum) {}

    //! A box that contains nothing; the identity for Merge/ExpandToContain.
    [[nodiscard]] static AxisAlignedBox Invalid();

    //! Unit cube centred on the origin — the default bounds for a mesh with no vertex data.
    [[nodiscard]] static AxisAlignedBox UnitCube();

    //! Box from a centre and half-extents (the form glTF and most engines store).
    [[nodiscard]] static AxisAlignedBox FromCenterAndExtent(Vec3 center, Vec3 extent);

    [[nodiscard]] bool IsValid() const;
    [[nodiscard]] Vec3 GetCenter() const;
    [[nodiscard]] Vec3 GetExtent() const;  //!< Half-size along each axis.
    [[nodiscard]] Vec3 GetSize() const;    //!< Full size along each axis.
    [[nodiscard]] Float32 GetVolume() const;
    [[nodiscard]] Float32 GetSurfaceArea() const;
    [[nodiscard]] Float32 GetMaxExtent() const;

    //! Expands the box to include `point`. Also makes an invalid box valid.
    void ExpandToContain(Vec3 point);
    void ExpandToContain(const AxisAlignedBox& other);
    void ExpandBy(Float32 amount);

    [[nodiscard]] AxisAlignedBox Merged(const AxisAlignedBox& other) const;
    [[nodiscard]] AxisAlignedBox Transformed(const Mat4& matrix) const;  //!< Re-derived AABB (6-plane).

    [[nodiscard]] bool Contains(Vec3 point) const;
    [[nodiscard]] bool Contains(const AxisAlignedBox& other) const;
    [[nodiscard]] bool Intersects(const AxisAlignedBox& other) const;

    //! Slab test. Returns false for an invalid box. `direction` must be normalised.
    [[nodiscard]] bool IntersectsRay(Vec3 rayOrigin, Vec3 rayDirectionNormalized, Float32 maxDistance,
                                     Float32* outHitDistance = nullptr) const;

    [[nodiscard]] bool IntersectsSphere(Vec3 center, Float32 radius) const;

    //! Closest point inside the box to `point` (returns `point` when it is already inside).
    [[nodiscard]] Vec3 GetClosestPoint(Vec3 point) const;

    //! Corner vertex by index: bit 0 = X, bit 1 = Y, bit 2 = Z (0 = min, 1 = max).
    [[nodiscard]] Vec3 GetCorner(std::size_t cornerIndex) const;

    bool operator==(const AxisAlignedBox&) const = default;
};

// ---------------------------------------------------------------------------
// BoundingSphere — cheap conservative volume for distance-based culling and LOD selection.
// ---------------------------------------------------------------------------

class HP_MATH_API BoundingSphere
{
public:
    Vec3 Center = Vec3(0.0f);
    Float32 Radius = 0.0f;

    BoundingSphere() = default;
    BoundingSphere(Vec3 center, Float32 radius) : Center(center), Radius(radius) {}

    [[nodiscard]] static BoundingSphere FromAABB(const AxisAlignedBox& box);

    [[nodiscard]] bool Contains(Vec3 point) const;
    [[nodiscard]] bool Intersects(const BoundingSphere& other) const;
    [[nodiscard]] bool Intersects(const AxisAlignedBox& box) const;
    [[nodiscard]] bool IntersectsRay(Vec3 rayOrigin, Vec3 rayDirectionNormalized, Float32 maxDistance) const;

    [[nodiscard]] BoundingSphere Transformed(const Mat4& matrix) const;  //!< Scales by the largest axis scale.
    [[nodiscard]] Float32 GetSquaredDistanceTo(Vec3 point) const;        //!< Signed: 0 when inside.

    bool operator==(const BoundingSphere&) const = default;
};

}  // namespace Hollowpoint::Math
