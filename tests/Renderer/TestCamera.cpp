#include <cmath>
#include <string>

#include <doctest/doctest.h>

#include "Hollowpoint/Math/Frustum.h"
#include "Hollowpoint/Renderer/Camera.h"

using namespace Hollowpoint;
using namespace Hollowpoint::Renderer;
using Math::Float32;
using Math::Mat4;
using Math::Vec3;
using Math::Vec4;

namespace
{

[[nodiscard]] bool Approximately(const Vec3& a, const Vec3& b, Float32 tolerance = 1e-4f)
{
    return std::fabs(a.X - b.X) <= tolerance && std::fabs(a.Y - b.Y) <= tolerance && std::fabs(a.Z - b.Z) <= tolerance;
}

[[nodiscard]] std::string ToString(const Vec3& v)
{
    return "(" + std::to_string(v.X) + ", " + std::to_string(v.Y) + ", " + std::to_string(v.Z) + ")";
}

[[nodiscard]] bool AllFinite(const Mat4& m)
{
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            if (!std::isfinite(m.Columns[column][row]))
            {
                return false;
            }
        }
    }

    return true;
}

[[nodiscard]] CameraComponent MakeDefaultCamera()
{
    CameraComponent camera;
    camera.Position = Vec3(0.0f, 0.0f, 5.0f);
    camera.Target = Vec3(0.0f, 0.0f, 0.0f);
    camera.Up = Vec3(0.0f, 1.0f, 0.0f);
    camera.NearPlane = 0.1f;
    camera.FarPlane = 100.0f;

    return camera;
}

}  // namespace

TEST_SUITE("Renderer/Camera")
{
    TEST_CASE("view matrix places the eye at the origin and looks down -Z")
    {
        const CameraComponent camera = MakeDefaultCamera();
        const CameraState state = ComputeCameraState(camera, 1.0f);

        // The eye must map to the view-space origin: that is the definition of a view matrix. Computed independently
        // of the implementation (matrix times point by hand for this configuration).
        const Vec3 viewSpaceEye = state.View.TransformPoint(camera.Position);
        INFO(ToString(viewSpaceEye));
        CHECK(Approximately(viewSpaceEye, Vec3(0.0f, 0.0f, 0.0f)));

        // A point in front of the camera (toward the target) must land at negative view-space Z (right-handed).
        const Vec3 inFront = state.View.TransformPoint(Vec3(0.0f, 0.0f, 0.0f));
        CHECK(inFront.Z < 0.0f);

        // And its distance equals the authored distance, i.e. the view matrix is rigid (no accidental scale). A view
        // matrix that scaled would silently break every depth and culling test built on it.
        CHECK(std::fabs(-inFront.Z - 5.0f) < 1e-4f);

        // Behind the camera => positive view Z.
        const Vec3 behind = state.View.TransformPoint(Vec3(0.0f, 0.0f, 10.0f));
        CHECK(behind.Z > 0.0f);
    }

    TEST_CASE("the view matrix preserves handedness and does not mirror")
    {
        const CameraComponent camera = MakeDefaultCamera();
        const CameraState state = ComputeCameraState(camera, 16.0f / 9.0f);

        // A right-handed view basis has determinant +1. A mirrored basis (det -1) would flip triangle winding and
        // cull every front face — visible as an inside-out scene, but only once culling is enabled, which is exactly
        // when it becomes expensive to debug.
        const Math::Mat3 rotation = state.View.GetUpperLeft3();
        const Float32 determinant = rotation.Determinant();

        INFO("determinant " << determinant);
        CHECK(determinant == doctest::Approx(1.0f).epsilon(1e-4));
    }

    TEST_CASE("up vector maps to view-space +Y")
    {
        const CameraComponent camera = MakeDefaultCamera();
        const CameraState state = ComputeCameraState(camera, 1.0f);

        // A point offset purely along world +Y from the eye must appear at +Y in view space with zero X: this pins
        // roll, which nothing else constrains. A swapped X/Y basis shows up as a 90-degree-rotated image.
        const Vec3 above = state.View.TransformPoint(camera.Position + Vec3(0.0f, 2.0f, 0.0f));

        INFO(ToString(above));
        CHECK(above.X == doctest::Approx(0.0f).epsilon(1e-3));
        CHECK(above.Y == doctest::Approx(2.0f).epsilon(1e-4));
    }

    TEST_CASE("perspective projection maps the frustum bounds to NDC correctly for Vulkan")
    {
        const CameraComponent camera = MakeDefaultCamera();
        const CameraState state = ComputeCameraState(camera, 1.0f);

        // Depth is a function of distance from the eye, not of world Z: this camera sits at z = +5 looking at the
        // origin, so a point d in front of it is at world z = 5 - d. (An earlier version passed world z straight in and
        // "failed" — correctly so: the implementation matched the closed form exactly, and the test was describing a
        // different camera.)
        const auto depthAtDistance = [&](Float32 distance) {
            return state.ViewProjection.TransformPoint(Vec3(0.0f, 0.0f, camera.Position.Z - distance)).Z;
        };

        // Closed form: ndc.z = (far/(near-far)*vz + near*far/(near-far)) / (-vz), with vz = -distance.
        // At d = near that is exactly 0; at d = far it is exactly 1 — Vulkan's [0,1] depth range. A GL-style [-1,1]
        // projection would put the near plane at -1 and silently reject half the depth range in the comparison.
        CHECK(depthAtDistance(camera.NearPlane) == doctest::Approx(0.0f).epsilon(1e-4));
        CHECK(depthAtDistance(camera.FarPlane) == doctest::Approx(1.0f).epsilon(1e-3));

        // Depth must increase monotonically with view distance: the ordering every depth test depends on.
        Float32 previousDepth = -1.0f;
        bool monotonic = true;
        for (Float32 distance = camera.NearPlane; distance <= camera.FarPlane; distance += 5.0f)
        {
            const Float32 depth = depthAtDistance(distance);
            if (depth < previousDepth)
            {
                monotonic = false;
            }
            previousDepth = depth;
        }
        CHECK(monotonic);

        // And the midpoint of the depth range sits at the **harmonic** mean of near and far, 2*n*f/(n+f) ~= 0.1998
        // here — solving ndc.z = 0.5 gives exactly that. (I first guessed the geometric mean sqrt(n*f) = 3.16; it is
        // not: that depth is 0.969.) The quantitative statement of why the near plane dominates: half of all depth
        // precision lands between near and the harmonic mean — with near 0.1 and far 100, that is everything within 0.2
        // units of the eye, i.e. 0.2% of the range. A needlessly tiny near plane therefore burns most of the precision
        // on geometry already sub-pixel close to the camera.
        const Float32 harmonicMeanDepth =
            2.0f * camera.NearPlane * camera.FarPlane / (camera.NearPlane + camera.FarPlane);
        const Float32 depthAtMidRange = depthAtDistance(harmonicMeanDepth);

        INFO("harmonic mean " << harmonicMeanDepth << " depth " << depthAtMidRange);
        CHECK(depthAtMidRange == doctest::Approx(0.5f).epsilon(1e-3));
    }

    TEST_CASE("a point at the centre of the view projects to the middle of NDC")
    {
        const CameraComponent camera = MakeDefaultCamera();
        const CameraState state = ComputeCameraState(camera, 1.0f);

        // The look-at target is on the view axis, so it must project to (0,0) regardless of FOV or aspect.
        const Vec3 centreNdc = state.ViewProjection.TransformPoint(camera.Target);

        INFO(ToString(centreNdc));
        CHECK(std::fabs(centreNdc.X) < 1e-4f);
        CHECK(std::fabs(centreNdc.Y) < 1e-4f);
    }

    TEST_CASE("wider field of view puts a fixed world offset further from screen edge")
    {
        // Two cameras, same pose; the wider FOV sees more of the world, so an off-axis point sits closer to the
        // centre in NDC (its angular size relative to the frustum is smaller).
        CameraComponent narrow = MakeDefaultCamera();
        narrow.VerticalFieldOfViewRadians = 0.5f;

        CameraComponent wide = MakeDefaultCamera();
        wide.VerticalFieldOfViewRadians = 1.5f;

        const Vec3 offAxis(0.0f, 1.0f, 0.0f);

        const Float32 narrowY = ComputeCameraState(narrow, 1.0f).ViewProjection.TransformPoint(offAxis).Y;
        const Float32 wideY = ComputeCameraState(wide, 1.0f).ViewProjection.TransformPoint(offAxis).Y;

        INFO("narrow " << narrowY << " wide " << wideY);
        CHECK(std::fabs(narrowY) > std::fabs(wideY));

        // Aspect ratio must affect X and not Y: swapping them stretches the image, which is a common but easily
        // missed bug because it looks like "just a slightly wrong window size".
        const Float32 wideAspectX =
            ComputeCameraState(narrow, 2.0f).ViewProjection.TransformPoint(Vec3(1.0f, 0.0f, 0.0f)).X;
        const Float32 narrowAspectX =
            ComputeCameraState(narrow, 0.5f).ViewProjection.TransformPoint(Vec3(1.0f, 0.0f, 0.0f)).X;
        CHECK(std::fabs(wideAspectX) < std::fabs(narrowAspectX));

        const Float32 yAtWideAspect =
            ComputeCameraState(narrow, 2.0f).ViewProjection.TransformPoint(Vec3(0.0f, 1.0f, 0.0f)).Y;
        const Float32 yAtNarrowAspect =
            ComputeCameraState(narrow, 0.5f).ViewProjection.TransformPoint(Vec3(0.0f, 1.0f, 0.0f)).Y;
        CHECK(yAtWideAspect == doctest::Approx(yAtNarrowAspect).epsilon(1e-5));
    }

    TEST_CASE("frustum agrees with the projected points it claims to contain")
    {
        const CameraComponent camera = MakeDefaultCamera();
        const CameraState state = ComputeCameraState(camera, 1.0f);

        // The frustum is derived from the same matrix, so this is not a tautology: it checks that Frustum's plane
        // extraction (Gribb-Hartmann) and its inside-sign convention agree with actually projecting points through
        // ViewProjection. A sign error in either shows up as visible geometry reported outside, or invisible geometry
        // reported inside — culling bugs that are otherwise found by staring at disappearing objects.
        CHECK(state.Frustum.TestPoint(Vec3(0.0f, 0.0f, 0.0f)) == Math::IntersectionResult::Inside);

        // Far behind the camera: outside.
        CHECK(state.Frustum.TestPoint(Vec3(0.0f, 0.0f, 50.0f)) == Math::IntersectionResult::Outside);

        // In front but off to the side beyond the horizontal FOV at that depth: outside. At z=1 with a 60-degree
        // vertical FOV and aspect 1, the half-width is tan(30 deg) ~= 0.577, so x = 5 is well outside.
        CHECK(state.Frustum.TestPoint(Vec3(5.0f, 0.0f, 1.0f)) == Math::IntersectionResult::Outside);

        // Just inside that boundary: inside. Using 0.4 (against a half-width of ~0.577 at unit distance) leaves a
        // margin so the test does not straddle the plane and depend on an epsilon.
        const Math::IntersectionResult justInside = state.Frustum.TestPoint(Vec3(0.4f, 0.0f, 1.0f));
        CHECK(justInside != Math::IntersectionResult::Outside);

        // Between near and far along the axis: inside. In front of the near plane (closer than 0.1): outside — this is
        // what makes the near plane meaningful rather than only a precision knob.
        CHECK(state.Frustum.TestPoint(Vec3(0.0f, 0.0f, 0.5f)) == Math::IntersectionResult::Inside);

        // In front of the near plane means closer to the eye than NearPlane — and the eye is at z = +5, so the world
        // coordinate is 5 - 0.05 = 4.95, not 0.05. (The first version used 0.05, which is 4.95 units in front of the
        // camera and correctly Inside; a wrong test, not a wrong frustum.)
        CHECK(state.Frustum.TestPoint(Vec3(0.0f, 0.0f, camera.Position.Z - 0.05f)) != Math::IntersectionResult::Inside);
    }

    TEST_CASE("inverse view-projection round-trips a point through NDC")
    {
        const CameraComponent camera = MakeDefaultCamera();
        const CameraState state = ComputeCameraState(camera, 16.0f / 9.0f);

        // Forward then back must recover the original. This catches an Inverse() that is not the true inverse (e.g. a
        // transposed or adjoint-only result), which would break mouse picking and depth reconstruction — both of which
        // use exactly this matrix.
        const Vec3 worldPoint(1.5f, -0.75f, 2.0f);

        const Vec3 projected = state.ViewProjection.TransformPoint(worldPoint);
        const Vec4 ndc(projected.X, projected.Y, projected.Z, 1.0f);
        const Vec4 recovered = state.InverseViewProjection * ndc;

        INFO("projected " << ToString(projected) << " recovered (" << recovered.X / recovered.W << ", "
                          << recovered.Y / recovered.W << ", " << recovered.Z / recovered.W << ")");

        CHECK(recovered.X / recovered.W == doctest::Approx(worldPoint.X).epsilon(1e-3));
        CHECK(recovered.Y / recovered.W == doctest::Approx(worldPoint.Y).epsilon(1e-3));
        CHECK(recovered.Z / recovered.W == doctest::Approx(worldPoint.Z).epsilon(1e-3));
    }

    TEST_CASE("the transform hierarchy owns the eye position when supplied")
    {
        CameraComponent camera = MakeDefaultCamera();
        camera.Position = Vec3(99.0f, 99.0f, 99.0f);  // Must be ignored when a world transform is given.

        const Math::Transform hierarchyTransform(Vec3(2.0f, 3.0f, 4.0f));
        const CameraState state = ComputeCameraState(camera, &hierarchyTransform, 1.0f);

        // Precedence rule from the header: hierarchy translation wins. Asserted because two writable positions with an
        // undocumented winner is the bug class LocalTransform.h explicitly avoids.
        CHECK(Approximately(state.EyePosition, Vec3(2.0f, 3.0f, 4.0f)));

        // And the view matrix really uses it: the eye maps to the origin.
        CHECK(Approximately(state.View.TransformPoint(Vec3(2.0f, 3.0f, 4.0f)), Vec3(0.0f, 0.0f, 0.0f)));

        // Without a hierarchy transform the component's own position is used.
        const CameraState fallback = ComputeCameraState(camera, 1.0f);
        CHECK(Approximately(fallback.EyePosition, Vec3(99.0f, 99.0f, 99.0f)));
    }

    TEST_CASE("degenerate cameras produce finite matrices, not NaN")
    {
        // Every case below is reachable from an editor slider, a scripted animation or a scene file typo. A NaN that
        // reaches ViewProjection makes every frustum test report "outside", so the entire scene vanishes with no error
        // message anywhere — the worst possible failure mode for a renderer, hence tested explicitly.
        struct Case
        {
            const char* Name;
            Vec3 Position;
            Vec3 Target;
            Vec3 Up;
            Float32 Aspect;
            Float32 Near;
            Float32 Far;
            Float32 FieldOfView;
        };

        const Case cases[] = {
            {"target equals position", Vec3(0.0f), Vec3(0.0f), Vec3(0.0f, 1.0f, 0.0f), 1.0f, 0.1f, 100.0f, 1.0f},
            {"zero up vector", Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), Vec3(0.0f, 0.0f, 0.0f), 1.0f, 0.1f, 100.0f, 1.0f},
            {"up parallel to view", Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), Vec3(0.0f, 0.0f, 1.0f), 1.0f, 0.1f, 100.0f,
             1.0f},
            {"zero aspect (minimised)", Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), Vec3(0.0f, 1.0f, 0.0f), 0.0f, 0.1f, 100.0f,
             1.0f},
            {"negative aspect", Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), Vec3(0.0f, 1.0f, 0.0f), -2.0f, 0.1f, 100.0f, 1.0f},
            {"zero near plane", Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), Vec3(0.0f, 1.0f, 0.0f), 1.0f, 0.0f, 100.0f, 1.0f},
            {"near greater than far", Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), Vec3(0.0f, 1.0f, 0.0f), 1.0f, 200.0f, 50.0f,
             1.0f},
            {"zero field of view", Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), Vec3(0.0f, 1.0f, 0.0f), 1.0f, 0.1f, 100.0f,
             0.0f},
            {"field of view pi", Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), Vec3(0.0f, 1.0f, 0.0f), 1.0f, 0.1f, 100.0f,
             3.14159265f},
        };

        for (const Case& testCase : cases)
        {
            CameraComponent camera;
            camera.Position = testCase.Position;
            camera.Target = testCase.Target;
            camera.Up = testCase.Up;
            camera.NearPlane = testCase.Near;
            camera.FarPlane = testCase.Far;
            camera.VerticalFieldOfViewRadians = testCase.FieldOfView;

            const CameraState state = ComputeCameraState(camera, testCase.Aspect);

            INFO(testCase.Name);
            CHECK(AllFinite(state.View));
            CHECK(AllFinite(state.Projection));
            CHECK(AllFinite(state.ViewProjection));

            // Aspect ratio must have been replaced by the fallback rather than propagated.
            CHECK(state.AspectRatio > 0.0f);
        }
    }

    TEST_CASE("orthographic projection keeps size constant with distance")
    {
        CameraComponent camera = MakeDefaultCamera();
        camera.Projection = CameraComponent::ProjectionType::Orthographic;
        camera.OrthographicHalfHeight = 10.0f;

        const CameraState state = ComputeCameraState(camera, 1.0f);

        // The defining property of an orthographic projection: the same world-space offset projects to the same NDC
        // offset at any depth. A perspective matrix used here would make objects shrink with distance — visible, but
        // only once someone actually switches mode.
        const Float32 nearOffsetY = state.ViewProjection.TransformPoint(Vec3(0.0f, 5.0f, 1.0f)).Y;
        const Float32 farOffsetY = state.ViewProjection.TransformPoint(Vec3(0.0f, 5.0f, 80.0f)).Y;

        INFO("near " << nearOffsetY << " far " << farOffsetY);
        CHECK(nearOffsetY == doctest::Approx(farOffsetY).epsilon(1e-4));

        // Half-height of 10 means y = +10 sits at the top edge (NDC +1) and y = -10 at the bottom.
        const Float32 topEdge = state.ViewProjection.TransformPoint(Vec3(0.0f, 10.0f, 1.0f)).Y;
        const Float32 bottomEdge = state.ViewProjection.TransformPoint(Vec3(0.0f, -10.0f, 1.0f)).Y;

        INFO("top " << topEdge << " bottom " << bottomEdge);
        CHECK(topEdge == doctest::Approx(1.0f).epsilon(1e-3));
        CHECK(bottomEdge == doctest::Approx(-1.0f).epsilon(1e-3));
    }

    TEST_CASE("camera state is recomputed rather than cached between calls")
    {
        // Guards the design decision in the header: no cached state means moving a camera takes effect immediately. A
        // future "optimisation" that memoises would break this, and the symptom (camera lagging one frame behind, or
        // not moving at all) is expensive to trace back to a cache.
        CameraComponent camera = MakeDefaultCamera();

        const CameraState first = ComputeCameraState(camera, 1.0f);

        camera.Position = Vec3(10.0f, 0.0f, 5.0f);
        const CameraState second = ComputeCameraState(camera, 1.0f);

        CHECK(first.EyePosition.X != second.EyePosition.X);
        CHECK(second.EyePosition.X == doctest::Approx(10.0f));

        // The view matrices must differ too — eye position alone could be a stored field.
        //
        // Compared across all sixteen elements rather than at one index, because the index that changes is not obvious:
        // measured, Columns[3][0] is 0 for both poses (looking straight down -Z gives right = (1,0,0), so its row
        // contribution -right·eye is zero regardless of where the eye sits on X), while the movement appears in
        // Columns[3][2] (-5 -> -11.18). The first version compared [3][0] and "failed" against a correct
        // implementation.
        Float32 largestDifference = 0.0f;
        for (int column = 0; column < 4; ++column)
        {
            for (int row = 0; row < 4; ++row)
            {
                largestDifference = std::max(
                    largestDifference, std::fabs(first.View.Columns[column][row] - second.View.Columns[column][row]));
            }
        }

        INFO("largest view difference " << largestDifference);
        CHECK(largestDifference > 1e-4f);
    }
}
