#include <cmath>
#include <cstring>
#include <limits>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Editor/Picking.h"

namespace Hollowpoint::Editor
{

using Core::Index;
using Math::Vec4;

namespace
{

//! Relative epsilon for the ray-triangle determinant. Scaled by edge lengths at the call site (see below); this is
//! the constant that separates "a very small triangle" from "a degenerate one".
constexpr Float32 kDeterminantEpsilon = 1e-8f;

[[nodiscard]] Vec3 ReadPosition(const PickCandidate& candidate, Index index) noexcept
{
    const auto* base = static_cast<const unsigned char*>(candidate.Vertices);
    const auto* element = base + candidate.VertexStrideBytes * index + candidate.PositionOffsetBytes;

    Vec3 position{};
    std::memcpy(&position, element, sizeof(Vec3));
    return position;
}

}  // namespace

Ray::Ray(const Vec3& origin, const Vec3& direction) : Origin(origin), Direction(direction)
{
    // Normalised here rather than at each use: every hit distance this type produces is "metres along the ray", and
    // a non-unit direction makes t a scaled quantity. That does not break a single-ray test — it breaks *nearest
    // wins* between two rays, which is exactly what picking is, and it would be wrong in a way no individual test
    // notices. A zero-length direction cannot be normalised; it falls back to -Z (the default) so the ray still
    // produces defined results rather than NaNs that compare false against everything.
    const Float32 length = Direction.Length();
    if (length > 1e-8f)
    {
        Direction = Direction * (1.0f / length);
    }
    else
    {
        Direction = Vec3(0.0f, 0.0f, -1.0f);
    }
}

Vec3 PickCandidate::GetVertex(Index index) const noexcept
{
    return ReadPosition(*this, index);
}

bool IntersectsRayBox(const Ray& ray, const Vec3& boxMin, const Vec3& boxMax, Float32& outEntryT) noexcept
{
    // The slab method. Dividing by zero is *wanted* here — an axis-aligned ray against a slab gives ±inf bounds, and
    // IEEE semantics produce the correct accept/reject without a branch per axis. Hence no epsilon on the divisor;
    // the guard that matters is the per-axis "parallel and outside" test below.
    Float32 entry = -std::numeric_limits<Float32>::infinity();
    Float32 exit = std::numeric_limits<Float32>::infinity();

    const Float32 origin[3] = {ray.Origin.X, ray.Origin.Y, ray.Origin.Z};
    const Float32 direction[3] = {ray.Direction.X, ray.Direction.Y, ray.Direction.Z};
    const Float32 low[3] = {boxMin.X, boxMin.Y, boxMin.Z};
    const Float32 high[3] = {boxMax.X, boxMax.Y, boxMax.Z};

    for (int axis = 0; axis < 3; ++axis)
    {
        if (std::fabs(direction[axis]) < 1e-9f)
        {
            // Parallel to this slab: inside only if the origin is between the planes; otherwise no hit at all.
            if (origin[axis] < low[axis] || origin[axis] > high[axis])
            {
                return false;
            }
            continue;
        }

        const Float32 inversed = 1.0f / direction[axis];
        Float32 tNear = (low[axis] - origin[axis]) * inversed;
        Float32 tFar = (high[axis] - origin[axis]) * inversed;
        if (tNear > tFar)
        {
            std::swap(tNear, tFar);
        }

        entry = tNear > entry ? tNear : entry;
        exit = tFar < exit ? tFar : exit;

        if (entry > exit)
        {
            return false;
        }
    }

    // A box entirely behind the origin still satisfies the slab overlap; the distance check is what rejects it.
    // entry < 0 with exit >= 0 means the origin is inside, which counts as a hit (a camera inside a big mesh can
    // still select it — UE behaves the same way, and refusing would make large rooms unclickable from within).
    if (exit < 0.0f)
    {
        return false;
    }

    outEntryT = entry;
    return true;
}

bool IntersectsRayTriangle(const Ray& ray, const Vec3& v0, const Vec3& v1, const Vec3& v2, bool backfaceCulling,
                           Float32& outT) noexcept
{
    // Möller–Trumbore with the edge vectors in world space (the caller transforms vertices before calling).
    const Vec3 edge1 = v1 - v0;
    const Vec3 edge2 = v2 - v0;
    const Vec3 pvec = ray.Direction.Cross(edge2);
    const Float32 determinant = edge1.Dot(pvec);

    if (backfaceCulling)
    {
        // A negative determinant is a back face; ~zero is degenerate. Both are rejects, and the same comparison does
        // both because "smaller than epsilon" covers "negative" — spelled out because the non-culling branch below
        // needs the two-sided version and reading it as one condition hides why they differ.
        if (determinant < kDeterminantEpsilon)
        {
            return false;
        }
    }
    else if (std::fabs(determinant) < kDeterminantEpsilon)
    {
        return false;
    }

    const Float32 inversedDeterminant = 1.0f / determinant;
    const Vec3 tvec = ray.Origin - v0;
    const Float32 u = tvec.Dot(pvec) * inversedDeterminant;
    if (u < 0.0f || u > 1.0f)
    {
        return false;
    }

    const Vec3 qvec = tvec.Cross(edge1);
    const Float32 v = ray.Direction.Dot(qvec) * inversedDeterminant;
    if (v < 0.0f || u + v > 1.0f)
    {
        return false;
    }

    const Float32 t = edge2.Dot(qvec) * inversedDeterminant;
    if (t <= 0.0f)
    {
        // Zero would mean the origin is exactly on the triangle; treating it as a miss avoids selecting geometry the
        // camera has clipped into, which no author wants and nothing can render.
        return false;
    }

    outT = t;
    return true;
}

WorldBounds TransformBox(const Vec3& boxMin, const Vec3& boxMax, const Mat4& world) noexcept
{
    // All eight corners, not the min point plus an extent: under rotation a corner that was not the min becomes the
    // new min, so transforming one point and offsetting by the size is wrong for every rotated instance — which is
    // most of what an editor scene contains.
    WorldBounds bounds;
    const Float32 xs[2] = {boxMin.X, boxMax.X};
    const Float32 ys[2] = {boxMin.Y, boxMax.Y};
    const Float32 zs[2] = {boxMin.Z, boxMax.Z};

    bounds.Min = world.TransformPoint(boxMin);
    bounds.Max = bounds.Min;

    for (const float x : xs)
    {
        for (const float y : ys)
        {
            for (const float z : zs)
            {
                const Vec3 corner = world.TransformPoint(Vec3(x, y, z));
                bounds.Min.X = corner.X < bounds.Min.X ? corner.X : bounds.Min.X;
                bounds.Min.Y = corner.Y < bounds.Min.Y ? corner.Y : bounds.Min.Y;
                bounds.Min.Z = corner.Z < bounds.Min.Z ? corner.Z : bounds.Min.Z;
                bounds.Max.X = corner.X > bounds.Max.X ? corner.X : bounds.Max.X;
                bounds.Max.Y = corner.Y > bounds.Max.Y ? corner.Y : bounds.Max.Y;
                bounds.Max.Z = corner.Z > bounds.Max.Z ? corner.Z : bounds.Max.Z;
            }
        }
    }
    return bounds;
}

PickResult PickRay(const Ray& ray, const PickCandidate* candidates, Index count, bool backfaceCulling) noexcept
{
    PickResult best;
    Float32 bestDistance = std::numeric_limits<Float32>::infinity();

    for (Index candidateIndex = 0; candidateIndex < count; ++candidateIndex)
    {
        const PickCandidate& candidate = candidates[candidateIndex];
        if (!candidate.IsValid())
        {
            continue;
        }

        // Broad phase: the object-space bounds of the mesh transformed to world. The caller supplies them implicitly
        // through the vertex list, so they are computed per pick here rather than cached — a pick happens on click,
        // not per frame, and caching would mean a cache invalidation rule (mesh changed? transform changed?) whose
        // bugs are far worse than an O(vertices) loop on a mouse press.
        if (candidate.VertexCount > 0)
        {
            Vec3 boxMin = candidate.GetVertex(0);
            Vec3 boxMax = boxMin;
            for (Index v = 1; v < candidate.VertexCount; ++v)
            {
                const Vec3 position = candidate.GetVertex(v);
                boxMin.X = position.X < boxMin.X ? position.X : boxMin.X;
                boxMin.Y = position.Y < boxMin.Y ? position.Y : boxMin.Y;
                boxMin.Z = position.Z < boxMin.Z ? position.Z : boxMin.Z;
                boxMax.X = position.X > boxMax.X ? position.X : boxMax.X;
                boxMax.Y = position.Y > boxMax.Y ? position.Y : boxMax.Y;
                boxMax.Z = position.Z > boxMax.Z ? position.Z : boxMax.Z;
            }

            Float32 entryT = 0.0f;
            const WorldBounds worldBounds = TransformBox(boxMin, boxMax, candidate.WorldTransform);
            if (!IntersectsRayBox(ray, worldBounds.Min, worldBounds.Max, entryT))
            {
                continue;
            }
        }

        // Vertices are transformed per triangle rather than the ray being transformed by the inverse: an inverse exists
        // only for a non-singular matrix, and a scaled-to-zero instance (a hidden entity authored that way) is exactly
        // the case that must still be pickable-or-not without asserting. TransformBox above already accepts eight
        // transforms per pick, so this is the same cost class and needs no precondition on the transform at all.
        const Mat4 world = candidate.WorldTransform;

        for (Index triangle = 0; triangle + 2 < candidate.IndexCount; triangle += 3)
        {
            const Vec3 a = world.TransformPoint(candidate.GetVertex(static_cast<Index>(candidate.Indices[triangle])));
            const Vec3 b =
                world.TransformPoint(candidate.GetVertex(static_cast<Index>(candidate.Indices[triangle + 1])));
            const Vec3 c =
                world.TransformPoint(candidate.GetVertex(static_cast<Index>(candidate.Indices[triangle + 2])));

            Float32 t = 0.0f;
            if (!IntersectsRayTriangle(ray, a, b, c, backfaceCulling, t))
            {
                continue;
            }

            // Strictly-less: an earlier candidate at the same distance wins, which is the documented tie rule.
            if (t < bestDistance)
            {
                bestDistance = t;
                best.HasHit = true;
                best.OwnerSlot = candidate.OwnerSlot;
                best.Distance = t;
                best.Position = ray.PointAt(t);
                best.Triangle = triangle / 3;

                Vec3 edge1 = b - a;
                Vec3 edge2 = c - a;
                Vec3 faceNormal = edge1.Cross(edge2);
                const Float32 normalLength = faceNormal.Length();
                // A degenerate triangle was already rejected by the determinant guard, so a zero-length cross product
                // here means the two edges were collinear but the ray still "hit" — report a normal pointing back at
                // the ray rather than a zero vector, which would make gizmo orientation divide by zero downstream.
                best.Normal = normalLength > 1e-8f ? faceNormal * (1.0f / normalLength) : ray.Direction * -1.0f;
            }
        }
    }

    return best;
}

Ray ScreenPointToRay(Float32 ndcX, Float32 ndcY, const Mat4& inverseViewProjection) noexcept
{
    // Two unprojections: a point on the near plane and one on the far plane, difference = view direction. Vulkan's
    // depth range is [0,1], so z=0 is near and z=1 is far; feeding GL's -1 here produces a ray that points slightly
    // off-axis in a way that looks like a projection bug and only shows up when clicking near the horizon.
    constexpr Float32 kNearDepth = 0.0f;
    constexpr Float32 kFarDepth = 1.0f;

    const Vec4 nearClip(ndcX, ndcY, kNearDepth, 1.0f);
    const Vec4 farClip(ndcX, ndcY, kFarDepth, 1.0f);

    const Vec4 nearWorld = inverseViewProjection * nearClip;
    const Vec4 farWorld = inverseViewProjection * farClip;

    // Perspective divide. A w of zero would mean the point lies on the plane at infinity (an eye position exactly on
    // the camera plane), which cannot come from a viewport click; asserting documents it as a caller error and the
    // guard keeps a bad matrix from producing NaNs that silently select nothing forever.
    if (std::fabs(nearWorld.W) < 1e-9f || std::fabs(farWorld.W) < 1e-9f)
    {
        HP_ASSERT_MSG(false, "ScreenPointToRay received an inverse matrix producing a zero clip w");
        return Ray(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f));
    }

    const Vec3 near(nearWorld.X / nearWorld.W, nearWorld.Y / nearWorld.W, nearWorld.Z / nearWorld.W);
    const Vec3 far(farWorld.X / farWorld.W, farWorld.Y / farWorld.W, farWorld.Z / farWorld.W);

    // The constructor normalises, so the caller's matrix scale cannot leak into hit distances.
    return Ray(near, far - near);
}

}  // namespace Hollowpoint::Editor
