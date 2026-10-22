#pragma once

#include <vector>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Math/BoundingBox.h"  // Math::AxisAlignedBox
#include "Hollowpoint/Math/MathCore.h"     // Vec2, Vec3, Float32

// ---------------------------------------------------------------------------
// Procedural geometry: the primitive meshes the engine builds in code.
//
// Why this exists before any importer: every stage of the renderer — vertex transform, depth behaviour, lighting,
// shadow maps — needs a mesh to look at, and depending on glTF import for that couples "the pipeline works" to
// "the asset pipeline works". A cube built here answers the first question alone. It is also what the editor draws
// for a new entity before anything is imported, and what debug/placeholder views render.
//
// Conventions (fixed once, matching the engine's math module): right-handed, +Y up, column-major matrices applied
// as M * v. Positions are in local space centred on the origin; normals are unit length and face outward; UVs have
// (0,0) at the first listed corner of each face as documented per primitive. Tangents are deliberately absent —
// normal mapping needs them and will add a generation step here with its own tests, rather than shipping an
// unvalidated basis now.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Renderer
{

using Math::Float32;
using Math::UInt32;
using Math::Vec2;
using Math::Vec3;

//! One vertex of a triangle-list mesh — the upload format for GPU vertex buffers.
//!
//! Plain float arrays, deliberately **not** Math::Vec3 fields: Vec3 is alignas(16) (size 16, not 12), so using it
//! here would pad the struct to 48 bytes with silent holes and make the shader-side layout disagree with the CPU
//! side — a mismatch that renders garbage with no error anywhere. This type is a wire format, not an API type; the
//! accessors below are where it converts to math types. The static_asserts in the .cpp pin size 32 / offsets
//! 0-12-24, matching GLSL's `layout(location) in vec3/vec3/vec2` expectations exactly.
struct GeometryVertex
{
    float Position[3] = {0.0f, 0.0f, 0.0f};
    float Normal[3] = {0.0f, 0.0f, 0.0f};
    float Uv[2] = {0.0f, 0.0f};

    // Construct-from-math-types rather than member-wise: keeps the packing guarantee while call sites stay typed.
    GeometryVertex() = default;
    GeometryVertex(const Vec3& position, const Vec3& normal, const Vec2& uv);

    [[nodiscard]] Vec3 GetPosition() const noexcept
    {
        return Vec3(Position[0], Position[1], Position[2]);
    }
    [[nodiscard]] Vec3 GetNormal() const noexcept
    {
        return Vec3(Normal[0], Normal[1], Normal[2]);
    }
    [[nodiscard]] Vec2 GetUv() const noexcept
    {
        return Vec2(Uv[0], Uv[1]);
    }
};

//! A triangle-list mesh: vertices plus 32-bit indices. The engine's only mesh representation until an importer
//! produces it from files; the data layout is exactly what a vertex buffer upload consumes.
struct GeometryData
{
    std::vector<GeometryVertex> Vertices;
    std::vector<UInt32> Indices;

    [[nodiscard]] bool IsEmpty() const noexcept
    {
        return Vertices.empty();
    }

    //! Axis-aligned bounds of the positions, for culling and editor framing. Returns a degenerate box at the
    //! origin for empty geometry rather than an invalid box, so callers can always read Min/Max without a check.
    [[nodiscard]] Math::AxisAlignedBox ComputeBounds() const;
};

//! A cube centred at the origin with edge length `edgeLength` (default 1: the engine's unit cube).
//!
//! 24 vertices — four per face, not eight shared — because a hard-edged cube needs one normal per face direction;
//! sharing corners would average normals and round the lighting. UVs run 0..1 across each face with (0,0) at the
//! corner nearest (-x/+y relative to that face's own frame, consistent clockwise when viewed from outside).
[[nodiscard]] HP_RENDERER_API GeometryData MakeCube(Float32 edgeLength = 1.0f);

//! A unit plane on XZ at y = 0 spanning [-halfSize, +halfSize], normal +Y. Four vertices, two triangles.
[[nodiscard]] HP_RENDERER_API GeometryData MakePlane(Float32 halfSize = 1.0f);

}  // namespace Hollowpoint::Renderer
