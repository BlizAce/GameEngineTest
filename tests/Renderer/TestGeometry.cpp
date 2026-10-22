#include "TestSupport.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <doctest/doctest.h>

#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Renderer/Geometry.h"

using namespace Hollowpoint;
using namespace Hollowpoint::Math;
using std::size_t;

namespace
{

constexpr Float32 kEpsilon = 1.0e-5f;

[[nodiscard]] std::string Describe(const Vec3& value)
{
    return "(" + std::to_string(value.X) + ", " + std::to_string(value.Y) + ", " + std::to_string(value.Z) + ")";
}

//! Every triangle of the mesh, checked against a predicate. Returns the first failure's description so one loop
//! covers winding, normals and index sanity without 60 near-identical CHECK lines.
template <typename TPredicate>
[[nodiscard]] std::string FirstBadTriangle(const Renderer::GeometryData& geometry, TPredicate&& predicate)
{
    for (size_t triangle = 0; triangle + 2 < geometry.Indices.size(); triangle += 3)
    {
        const Renderer::GeometryVertex& v0 = geometry.Vertices[geometry.Indices[triangle + 0]];
        const Renderer::GeometryVertex& v1 = geometry.Vertices[geometry.Indices[triangle + 1]];
        const Renderer::GeometryVertex& v2 = geometry.Vertices[geometry.Indices[triangle + 2]];

        if (std::string problem = predicate(v0, v1, v2); !problem.empty())
        {
            return "triangle at index " + std::to_string(triangle) + ": " + problem;
        }
    }
    return {};
}

}  // namespace

TEST_SUITE("Renderer/Geometry")
{
    TEST_CASE("cube has the documented vertex/index counts and a unit bounding box")
    {
        const Renderer::GeometryData cube = Renderer::MakeCube();

        // 24 vertices: four per face, not eight shared. Shared corners would average normals across faces and
        // round off what is meant to be a hard-edged cube.
        CHECK(cube.Vertices.size() == 24);
        CHECK(cube.Indices.size() == 36);

        const Math::AxisAlignedBox bounds = cube.ComputeBounds();
        CHECK(bounds.Min.X == doctest::Approx(-0.5f).epsilon(kEpsilon));
        CHECK(bounds.Min.Y == doctest::Approx(-0.5f).epsilon(kEpsilon));
        CHECK(bounds.Min.Z == doctest::Approx(-0.5f).epsilon(kEpsilon));
        CHECK(bounds.Max.X == doctest::Approx(0.5f).epsilon(kEpsilon));
        CHECK(bounds.Max.Y == doctest::Approx(0.5f).epsilon(kEpsilon));
        CHECK(bounds.Max.Z == doctest::Approx(0.5f).epsilon(kEpsilon));
    }

    TEST_CASE("cube edge length scales positions but never the normals")
    {
        const Renderer::GeometryData cube = Renderer::MakeCube(4.0f);
        const Math::AxisAlignedBox bounds = cube.ComputeBounds();
        CHECK(bounds.Max.X == doctest::Approx(2.0f).epsilon(kEpsilon));

        for (const Renderer::GeometryVertex& vertex : cube.Vertices)
        {
            const Float32 length = vertex.GetNormal().Length();
            // Scaling a normal with the geometry leaves it non-unit, which breaks every dot product downstream —
            // shading goes dark on large objects and nobody knows why.
            CHECK(length == doctest::Approx(1.0f).epsilon(kEpsilon));
        }
    }

    TEST_CASE("every cube triangle is wound so its geometric normal matches the stored normal")
    {
        const Renderer::GeometryData cube = Renderer::MakeCube();

        const std::string problem = FirstBadTriangle(
            cube,
            [](const Renderer::GeometryVertex& v0, const Renderer::GeometryVertex& v1,
               const Renderer::GeometryVertex& v2) -> std::string {
                const Vec3 geometric = (v1.GetPosition() - v0.GetPosition()).Cross(v2.GetPosition() - v0.GetPosition());
                if (geometric.Length() < kEpsilon)
                {
                    return "degenerate triangle (zero area) — corners listed in the wrong order or duplicated";
                }

                const Vec3 stored = v0.GetNormal();
                // A negative dot means this face's triangles are wound opposite to its normal: back-face culling then
                // removes exactly the faces that should be visible, and the cube looks inside-out.
                if (geometric.Normalized().Dot(stored) <= 0.0f)
                {
                    return "winding disagrees with stored normal; geometric " + Describe(geometric.Normalized()) +
                           " vs stored " + Describe(stored);
                }
                return {};
            });

        CHECK_MESSAGE(problem.empty(), problem);
    }

    TEST_CASE("every cube vertex lies on the surface and UVs span each face")
    {
        const Renderer::GeometryData cube = Renderer::MakeCube();

        Float32 minU = 1.0f, maxU = 0.0f;
        for (const Renderer::GeometryVertex& vertex : cube.Vertices)
        {
            const Vec3 position = vertex.GetPosition();
            // On the surface: exactly one coordinate sits at ±half extent (the other two vary across the face).
            Float32 maxAbs = 0.0f;
            for (const Float32 component : {position.X, position.Y, position.Z})
            {
                maxAbs = std::max(maxAbs, std::abs(component));
            }
            CHECK(maxAbs == doctest::Approx(0.5f).epsilon(kEpsilon));

            minU = std::min(minU, vertex.Uv[0]);
            maxU = std::max(maxU, vertex.Uv[0]);
        }

        // UVs covering the full 0..1 range per face is what makes a checkerboard texture land correctly — a
        // half-range mistake reads as "the texture is too small" rather than a bug in geometry.
        CHECK(minU == doctest::Approx(0.0f).epsilon(kEpsilon));
        CHECK(maxU == doctest::Approx(1.0f).epsilon(kEpsilon));
    }

    TEST_CASE("indices stay inside the vertex array")
    {
        const Renderer::GeometryData cube = Renderer::MakeCube();
        for (const Math::UInt32 index : cube.Indices)
        {
            // Out-of-range indices read whatever follows the buffer: garbage triangles, and on a real device an
            // out-of-bounds vertex fetch whose symptoms appear far from here.
            CHECK(index < cube.Vertices.size());
        }
    }

    TEST_CASE("plane is flat at y=0, faces up, and spans its half size")
    {
        const Renderer::GeometryData plane = Renderer::MakePlane(2.0f);
        CHECK(plane.Vertices.size() == 4);
        CHECK(plane.Indices.size() == 6);

        for (const Renderer::GeometryVertex& vertex : plane.Vertices)
        {
            CHECK(vertex.Position[1] == 0.0f);
            CHECK(vertex.GetNormal() == Vec3(0.0f, 1.0f, 0.0f));
        }

        const Math::AxisAlignedBox bounds = plane.ComputeBounds();
        CHECK(bounds.Min.X == doctest::Approx(-2.0f).epsilon(kEpsilon));
        CHECK(bounds.Max.X == doctest::Approx(2.0f).epsilon(kEpsilon));
        CHECK(bounds.Min.Z == doctest::Approx(-2.0f).epsilon(kEpsilon));
        CHECK(bounds.Max.Z == doctest::Approx(2.0f).epsilon(kEpsilon));

        const std::string problem = FirstBadTriangle(
            plane,
            [](const Renderer::GeometryVertex& v0, const Renderer::GeometryVertex& v1,
               const Renderer::GeometryVertex& v2) -> std::string {
                const Vec3 geometric = (v1.GetPosition() - v0.GetPosition()).Cross(v2.GetPosition() - v0.GetPosition());
                return geometric.Dot(Vec3(0.0f, 1.0f, 0.0f)) <= 0.0f ? "plane faces away from +Y" : std::string();
            });
        CHECK_MESSAGE(problem.empty(), problem);
    }

    TEST_CASE("empty geometry yields a readable degenerate box rather than an invalid one")
    {
        const Renderer::GeometryData empty = Renderer::GeometryData{};
        CHECK(empty.IsEmpty());

        // Callers frame the editor and cull with this unconditionally; AxisAlignedBox::Invalid() makes every
        // intersection test return false, which reads as "the mesh never draws" instead of "there is no mesh".
        const Math::AxisAlignedBox bounds = empty.ComputeBounds();
        CHECK(bounds.Min == Vec3(0.0f));
        CHECK(bounds.Max == Vec3(0.0f));
    }

    TEST_CASE("non-positive edge lengths produce a usable unit cube instead of a collapsed mesh")
    {
        // Editor sliders reach these values directly; an asserted crash on 0 would be worse than a sane default,
        // and a zero-extent mesh draws nothing at all — invisible and undebuggable.
        const Renderer::GeometryData degenerate = Renderer::MakeCube(0.0f);
        CHECK(degenerate.ComputeBounds().Max.X == doctest::Approx(0.5f).epsilon(kEpsilon));

        const Renderer::GeometryData negative = Renderer::MakePlane(-3.0f);
        CHECK(negative.ComputeBounds().Max.X == doctest::Approx(1.0f).epsilon(kEpsilon));
    }
}
