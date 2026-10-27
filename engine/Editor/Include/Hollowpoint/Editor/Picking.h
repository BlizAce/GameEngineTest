#pragma once

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/Math/MathCore.h"

// ---------------------------------------------------------------------------
// Picking — turning a viewport click into an entity.
//
// Deliberately free of ImGui, ECS iteration and the renderer: it takes rays and geometry and answers a question.
// That is what makes it testable (tests/Editor/TestPicking.cpp asserts hits, misses, ties and back faces with no
// window and no GPU), and picking is exactly the kind of code where an off-by-one in ray normalisation or a missed
// perspective divide reads as "clicks are a bit off" for a week before anyone suspects the maths.
//
// The algorithm is UE's, at this scale: broad phase against each candidate's world-space AABB, narrow phase a
// Möller–Trumbore triangle test over the mesh, nearest hit wins. A scene of built-in primitives is tens of
// thousands of triangles, so exhaustive narrow phase costs microseconds; when real assets arrive, a BVH goes
// between the two phases without changing this interface.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Editor
{

using Math::Float32;
using Math::Mat4;
using Math::Vec3;

//! A ray in world space. Origin is a point, Direction is **normalised at construction** — every test below assumes
//! it, and the hit distance `T` is meaningless otherwise (a non-unit direction returns t scaled by its length, which
//! silently breaks "nearest wins").
struct Ray
{
    Vec3 Origin{};
    Vec3 Direction = Vec3(0.0f, 0.0f, -1.0f);

    Ray() = default;
    Ray(const Vec3& origin, const Vec3& direction);

    [[nodiscard]] Vec3 PointAt(Float32 t) const
    {
        return Origin + Direction * t;
    }
};

//! One picking candidate: geometry in object space plus the transform that puts it in the world.
struct PickCandidate
{
    //! Entity this belongs to, so the caller can select it. Stored as a plain index rather than an ECS::Entity
    //! because this file must not depend on iteration order or liveness — those are the caller's business.
    Core::Index OwnerSlot = Core::InvalidIndex;

    Mat4 WorldTransform = Mat4::Identity();

    //! Object-space vertices and CCW triangle indices. Given as a base pointer plus a **stride** rather than a
    //! `Vec3[]`, so a caller hands over its own vertex format (Renderer::GeometryVertex is 32 bytes with position at
    //! offset 0) instead of copying every mesh into a scratch array on every click — which for a scene under the
    //! cursor is per-frame work in the input path.
    const void* Vertices = nullptr;  // not owned; VertexCount entries, VertexStrideBytes apart
    Core::Index VertexCount = 0;
    Core::Index VertexStrideBytes = 0;
    Core::Index PositionOffsetBytes = 0;

    const Core::UInt32* Indices = nullptr;  // not owned; length = IndexCount, multiple of 3
    Core::Index IndexCount = 0;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return Vertices != nullptr && Indices != nullptr && VertexCount > 0 && IndexCount >= 3 &&
               VertexStrideBytes >= static_cast<Core::Index>(sizeof(Vec3));
    }

    //! Object-space position of vertex `index`, read through the stride.
    [[nodiscard]] Vec3 GetVertex(Core::Index index) const noexcept;
};

//! Result of a pick: the winning candidate and where it was hit.
struct PickResult
{
    bool HasHit = false;

    //! Slot of the winning PickCandidate::OwnerSlot, or InvalidIndex. Ties (two candidates hitting at exactly the same
    //! distance) resolve to the **earliest** in the input array: deterministic, and matches draw order for the
    //! scene collector, which submits in entity order.
    Core::Index OwnerSlot = Core::InvalidIndex;

    //! Distance along the ray from Ray::Origin, in world units (metres).
    Float32 Distance = 0.0f;

    Vec3 Position{};  //!< World-space hit point.
    Vec3 Normal{};    //!< Interpolated surface normal is not available without vertex normals, so this is the
                      //!< geometric face normal of the winning triangle — enough for gizmo placement.
    //! Index into the candidate's index triples, or InvalidIndex.
    Core::Index Triangle = Core::InvalidIndex;
};

//! Ray vs axis-aligned box in ray-slab form. Returns false when there is no intersection or the box is behind the
//! origin (maxT < 0 means "the ray points away"). `outEntryT` receives the entry distance, which broad phase uses to
//! order candidates; it may be negative when the origin is inside the box, which counts as a hit.
[[nodiscard]] bool IntersectsRayBox(const Ray& ray, const Vec3& boxMin, const Vec3& boxMax,
                                    Float32& outEntryT) noexcept;

//! Ray vs one triangle (Möller–Trumbore). Back faces are **rejected** when `backfaceCulling` is true — matching what
//! an author expects from clicking a solid object, where the inside of a cube's far wall should not be selectable by
//! a ray that entered through its near wall. Degenerate triangles (zero area) never hit: the epsilon guard is what
//! stops them returning a plausible distance computed from a division by ~0.
[[nodiscard]] bool IntersectsRayTriangle(const Ray& ray, const Vec3& v0, const Vec3& v1, const Vec3& v2,
                                         bool backfaceCulling, Float32& outT) noexcept;

//! Transforms an object-space AABB by `world` and returns the enclosing world-space AABB (not an OBB — a rotated box
//! grows, which is correct for a broad phase: it may include space the object does not, never exclude space it does).
//! All eight corners are transformed, not the min point plus an extent: under rotation a corner that was not the min
//! becomes the new min, so the one-point version is wrong for every rotated instance — i.e. most of what an editor
//! scene contains.
struct WorldBounds
{
    Vec3 Min{};
    Vec3 Max{};
};

[[nodiscard]] HP_EDITOR_API WorldBounds TransformBox(const Vec3& boxMin, const Vec3& boxMax,
                                                     const Mat4& world) noexcept;

//! Picks the nearest candidate hit along `ray`. Empty input returns a miss. Candidates that fail IsValid() are
//! skipped rather than asserted on — an entity whose mesh has not loaded is a normal state (asset missing), and a
//! picker that crashes on it turns a content problem into an editor crash.
[[nodiscard]] HP_EDITOR_API PickResult PickRay(const Ray& ray, const PickCandidate* candidates, Core::Index count,
                                               bool backfaceCulling = false) noexcept;

//! Builds a world-space ray through a viewport pixel from an inverse view-projection matrix (the standard unproject:
//! NDC point at z=0 for the eye and z=1 for the far plane in Vulkan's [0,1] depth range — using GL's -1 would tilt
//! every ray slightly and be invisible until someone clicked near the horizon).
/**
 * \param ndcX,-ndcY Clip-space coordinates in [-1,1]; y is **already flipped** by the caller (screen y grows down,
 *        NDC y grows up), because who flips is exactly the kind of convention that must be stated once.
 */
[[nodiscard]] HP_EDITOR_API Ray ScreenPointToRay(Float32 ndcX, Float32 ndcY,
                                                 const Mat4& inverseViewProjection) noexcept;

}  // namespace Hollowpoint::Editor
