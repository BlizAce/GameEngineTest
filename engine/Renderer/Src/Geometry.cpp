#include <cstddef>

#include "Hollowpoint/Renderer/Geometry.h"

namespace Hollowpoint::Renderer
{

// Layout pinned because the GPU input layout declares these offsets by hand. Measured, not assumed: Math::Vec3 is
// alignas(16), so storing math types here pads the vertex to 48 bytes with silent holes and desynchronises CPU data
// from the shader's vec3 reads — which is why GeometryVertex stores plain float arrays (see its header comment).
static_assert(sizeof(GeometryVertex) == 32, "GeometryVertex must stay 32 bytes: it is the declared vertex stride");
static_assert(offsetof(GeometryVertex, Position) == 0, "Position offset is declared to the input layout as 0");
static_assert(offsetof(GeometryVertex, Normal) == 12, "Normal offset is declared to the input layout as 12");
static_assert(offsetof(GeometryVertex, Uv) == 24, "Uv offset is declared to the input layout as 24");

GeometryVertex::GeometryVertex(const Vec3& position, const Vec3& normal, const Vec2& uv)
{
    Position[0] = position.X;
    Position[1] = position.Y;
    Position[2] = position.Z;
    Normal[0] = normal.X;
    Normal[1] = normal.Y;
    Normal[2] = normal.Z;
    Uv[0] = uv.X;
    Uv[1] = uv.Y;
}

namespace
{

//! One face of a cube: outward normal plus its four corners and their UVs.
struct CubeFace
{
    Vec3 Normal;
    Vec3 Corners[4];
    Vec2 Uvs[4];
};

// Half extent for the unit cube; scaled by edgeLength at build time.
constexpr Float32 kHalf = 0.5f;

//! The six faces, each with corners ordered counter-clockwise **as seen from outside**, which is the engine's
//! winding convention: for every generated triangle, cross(v1 - v0, v2 - v0) points along the face normal (asserted
//! per-face in the tests, so a typo in this table cannot flip one face silently). Whether that maps to front-facing
//! on screen is a pipeline/cull-state question — Vulkan's framebuffer Y axis inverts it relative to right-handed
//! view space — and the renderer settles it in one documented place rather than by twisting this table.
constexpr CubeFace kFaces[6] = {
    // +X: looking down -X from outside, corners go (front, bottom-front...) counter-clockwise.
    {Vec3(1.0f, 0.0f, 0.0f),
     {Vec3(kHalf, -kHalf, kHalf), Vec3(kHalf, -kHalf, -kHalf), Vec3(kHalf, kHalf, -kHalf), Vec3(kHalf, kHalf, kHalf)},
     {Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f), Vec2(0.0f, 1.0f)}},
    // -X
    {Vec3(-1.0f, 0.0f, 0.0f),
     {Vec3(-kHalf, -kHalf, -kHalf), Vec3(-kHalf, -kHalf, kHalf), Vec3(-kHalf, kHalf, kHalf),
      Vec3(-kHalf, kHalf, -kHalf)},
     {Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f), Vec2(0.0f, 1.0f)}},
    // +Y
    {Vec3(0.0f, 1.0f, 0.0f),
     {Vec3(-kHalf, kHalf, kHalf), Vec3(kHalf, kHalf, kHalf), Vec3(kHalf, kHalf, -kHalf), Vec3(-kHalf, kHalf, -kHalf)},
     {Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f), Vec2(0.0f, 1.0f)}},
    // -Y
    {Vec3(0.0f, -1.0f, 0.0f),
     {Vec3(-kHalf, -kHalf, -kHalf), Vec3(kHalf, -kHalf, -kHalf), Vec3(kHalf, -kHalf, kHalf),
      Vec3(-kHalf, -kHalf, kHalf)},
     {Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f), Vec2(0.0f, 1.0f)}},
    // +Z
    {Vec3(0.0f, 0.0f, 1.0f),
     {Vec3(-kHalf, -kHalf, kHalf), Vec3(kHalf, -kHalf, kHalf), Vec3(kHalf, kHalf, kHalf), Vec3(-kHalf, kHalf, kHalf)},
     {Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f), Vec2(0.0f, 1.0f)}},
    // -Z
    {Vec3(0.0f, 0.0f, -1.0f),
     {Vec3(kHalf, -kHalf, -kHalf), Vec3(-kHalf, -kHalf, -kHalf), Vec3(-kHalf, kHalf, -kHalf),
      Vec3(kHalf, kHalf, -kHalf)},
     {Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f), Vec2(0.0f, 1.0f)}},
};

[[nodiscard]] Float32 ScaleFactor(Float32 edgeLength)
{
    // Negative edge lengths are an authoring mistake; zero produces a collapsed mesh that draws nothing. Clamped to
    // a positive floor rather than asserted because geometry helpers run on editor slider values.
    return edgeLength > 0.0f ? edgeLength : 1.0f;
}

}  // namespace

Math::AxisAlignedBox GeometryData::ComputeBounds() const
{
    if (Vertices.empty())
    {
        // Degenerate box at the origin, deliberately not AxisAlignedBox::Invalid(): callers frame the editor or
        // cull with this unconditionally, and an invalid box makes every intersection test return false — a mesh
        // that silently never draws is worse than one drawn at the origin.
        return Math::AxisAlignedBox(Vec3(0.0f));
    }

    Math::AxisAlignedBox bounds = Math::AxisAlignedBox::Invalid();
    for (const GeometryVertex& vertex : Vertices)
    {
        bounds.ExpandToContain(vertex.GetPosition());
    }
    return bounds;
}

GeometryData MakeCube(Float32 edgeLength)
{
    const Float32 scale = ScaleFactor(edgeLength);

    GeometryData geometry;
    geometry.Vertices.reserve(24);
    geometry.Indices.reserve(36);

    for (const CubeFace& face : kFaces)
    {
        const UInt32 baseIndex = static_cast<UInt32>(geometry.Vertices.size());

        for (int corner = 0; corner < 4; ++corner)
        {
            // Unit normals as-is: the six axis directions need no scaling, and scaling them with the edge length
            // would leave them non-unit for any cube that is not 1x1x1.
            geometry.Vertices.emplace_back(face.Corners[corner] * scale, face.Normal, face.Uvs[corner]);
        }

        // Two triangles per quad, wound consistently with the corner order so both inherit the face's outward
        // orientation. Fan from corner 0 rather than a diagonal split that mixes winding.
        geometry.Indices.insert(geometry.Indices.end(), {baseIndex + 0u, baseIndex + 1u, baseIndex + 2u, baseIndex + 0u,
                                                         baseIndex + 2u, baseIndex + 3u});
    }

    return geometry;
}

GeometryData MakePlane(Float32 halfSize)
{
    const Float32 extent = ScaleFactor(halfSize);

    GeometryData geometry;

    const Vec2 corners[4] = {
        Vec2(-extent, extent),
        Vec2(extent, extent),
        Vec2(extent, -extent),
        Vec2(-extent, -extent),
    };
    const Vec2 uvs[4] = {Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f), Vec2(0.0f, 1.0f)};

    for (int corner = 0; corner < 4; ++corner)
    {
        geometry.Vertices.emplace_back(Vec3(corners[corner].X, 0.0f, corners[corner].Y), Vec3(0.0f, 1.0f, 0.0f),
                                       uvs[corner]);
    }

    geometry.Indices = {0u, 1u, 2u, 0u, 2u, 3u};
    return geometry;
}

}  // namespace Hollowpoint::Renderer
