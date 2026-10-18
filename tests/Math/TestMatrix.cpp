#include "TestSupport.h"

#include <doctest/doctest.h>

#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Math/MathFunctions.h"

using namespace Hollowpoint::Math;
using Hollowpoint::Test::ScopedAssertCapture;

namespace
{

// Reads a matrix element in (row, column) notation, which keeps the expectations below readable even
// though Mat4 stores columns.
[[nodiscard]] Float32 At(const Mat4& matrix, int row, int column)
{
    return matrix[column][row];
}
[[nodiscard]] Float32 At(const Mat3& matrix, int row, int column)
{
    return matrix[column][row];
}

}  // namespace

TEST_SUITE("Math/Matrix")
{
    TEST_CASE("default constructed matrices are the identity")
    {
        const Mat4 m;
        for (int column = 0; column < 4; ++column)
        {
            for (int row = 0; row < 4; ++row)
            {
                CHECK(At(m, row, column) == (row == column ? 1.0f : 0.0f));
            }
        }

        const Mat3 m3;
        CHECK(At(m3, 0, 0) == 1.0f);
        CHECK(At(m3, 0, 1) == 0.0f);
    }

    TEST_CASE("storage is column-major: element (row, column) lives at Columns[column][row]")
    {
        Mat4 m;
        m[2][1] = 7.0f;  // column 2, row 1
        CHECK(At(m, 1, 2) == 7.0f);
        CHECK(At(m, 2, 1) == 0.0f);
    }

    TEST_CASE("matrix-vector multiplication applies columns as basis vectors")
    {
        Mat4 m = Mat4::Translate(Vec3(10.0f, 20.0f, 30.0f));
        const Vec4 result = m * Vec4(1.0f, 0.0f, 0.0f, 1.0f);
        CHECK(ApproximatelyEqual(result, Vec4(11.0f, 20.0f, 30.0f, 1.0f)));

        // w = 0 ignores translation.
        const Vec4 direction = m * Vec4(1.0f, 0.0f, 0.0f, 0.0f);
        CHECK(ApproximatelyEqual(direction, Vec4(1.0f, 0.0f, 0.0f, 0.0f)));
    }

    TEST_CASE("matrix multiplication is associative but not commutative")
    {
        const Mat4 translation = Mat4::Translate(Vec3(1.0f, 2.0f, 3.0f));
        const Mat4 scaling = Mat4::Scale(Vec3(2.0f, 2.0f, 2.0f));

        // Translate-then-scale differs from scale-then-translate.
        CHECK((translation * scaling) != (scaling * translation));

        const Mat4 rotation = Mat4::Rotate(Vec3::UnitY(), Radians(37.0f));
        const Vec4 point(1.5f, -2.5f, 6.0f, 1.0f);

        // (A*B)*v must equal A*(B*v) up to floating point rounding.
        const Vec4 left = ((translation * scaling) * rotation) * point;
        const Vec4 right = translation * (scaling * (rotation * point));
        CHECK(ApproximatelyEqual(left, right, 1e-4f));
    }

    TEST_CASE("T * R * S composition matches the documented transform order")
    {
        const Vec3 translation(5.0f, 6.0f, 7.0f);
        const Quat rotation = Quat::FromAxisAngle(Vec3::UnitZ(), kHalfPi);
        const Vec3 scale(2.0f, 3.0f, 4.0f);

        const Mat4 trs = Mat4::Translate(translation) * Mat4::FromQuaternion(rotation) * Mat4::Scale(scale);

        // The local +X axis is scaled by 2 and rotated 90 degrees about Z, landing on +Y.
        const Vec3 imageOfX = trs.TransformPoint(Vec3(1.0f, 0.0f, 0.0f)) - translation;
        CHECK(ApproximatelyEqual(imageOfX, Vec3(0.0f, 2.0f, 0.0f), 1e-5f));

        // A point at the local origin maps exactly onto the translation.
        CHECK(ApproximatelyEqual(trs.TransformPoint(Vec3::Zero()), translation, 1e-6f));
    }

    TEST_CASE("translation, scale and rotation accessors round-trip")
    {
        const Vec3 translation(1.25f, -2.5f, 9.75f);
        const Vec3 scale(1.0f, 2.0f, 3.0f);
        const Mat4 matrix = Mat4::Translate(translation) * Mat4::Scale(scale);

        CHECK(ApproximatelyEqual(matrix.GetTranslation(), translation));
        CHECK(ApproximatelyEqual(matrix.GetScale(), scale, 1e-6f));
    }

    TEST_CASE("mirrored scale keeps a negative sign so decomposition stays faithful")
    {
        const Mat4 mirrored = Mat4::Scale(Vec3(-2.0f, 1.0f, 1.0f));
        CHECK(mirrored.GetScale().X < 0.0f);
        CHECK(ApproximatelyEqual(mirrored.GetScale(), Vec3(-2.0f, 1.0f, 1.0f), 1e-6f));
    }

    TEST_CASE("transpose flips across the diagonal and is an involution")
    {
        // Set values by (column, row) — m[col][row] — and check they land at the mirrored position.
        Mat4 m;
        m[1][3] = 5.0f;   // column 1, row 3
        m[2][0] = -2.0f;  // column 2, row 0

        const Mat4 transposed = m.Transposed();
        CHECK(At(transposed, 1, 3) == 5.0f);   // was (row 3, col 1) -> now (row 1, col 3)
        CHECK(At(transposed, 2, 0) == -2.0f);  // was (row 0, col 2) -> now (row 2, col 0)
        CHECK(At(m, 3, 1) == 5.0f);            // sanity: the original is unchanged
        CHECK(m.Transposed().Transposed() == m);
    }

    TEST_CASE("inverse reproduces the identity for transforms")
    {
        const Mat4 matrix = Mat4::Translate(Vec3(3.0f, -1.0f, 2.0f)) *
                            Mat4::Rotate(Vec3(0.39f, 0.72f, -0.57f), Radians(41.0f)) *
                            Mat4::Scale(Vec3(1.0f, 2.0f, 0.5f));

        const Mat4 inverse = matrix.Inverse();

        // Both orders must produce the identity: a cofactor transpose mistake only shows up in one of
        // them, so checking a single direction is not enough.
        const Mat4 product = matrix * inverse;
        const Mat4 reverseProduct = inverse * matrix;
        for (int column = 0; column < 4; ++column)
        {
            for (int row = 0; row < 4; ++row)
            {
                const float expected = (row == column) ? 1.0f : 0.0f;
                CHECK(At(product, row, column) == doctest::Approx(expected).epsilon(1e-4f));
                CHECK(At(reverseProduct, row, column) == doctest::Approx(expected).epsilon(1e-4f));
            }
        }

        // The determinant of a TRS matrix is the product of its axis scales.
        CHECK(matrix.Determinant() == doctest::Approx(1.0f).epsilon(1e-4f));
    }

    TEST_CASE("inverse of a pure rotation equals its transpose")
    {
        const Mat4 rotation = Mat4::Rotate(Vec3::UnitY(), Radians(63.0f));
        const Mat4 inverse = rotation.Inverse();
        const Mat4 transposed = rotation.Transposed();

        for (int column = 0; column < 3; ++column)
        {
            for (int row = 0; row < 3; ++row)
            {
                CHECK(At(inverse, row, column) == doctest::Approx(At(transposed, row, column)).epsilon(1e-5f));
            }
        }
    }

    TEST_CASE("singular matrices are reported and yield the identity instead of NaN")
    {
        ScopedAssertCapture capture;

        Mat4 singular = Mat4::Scale(Vec3(1.0f, 0.0f, 1.0f));  // zero Y scale -> not invertible
        const Mat4 inverse = singular.Inverse();
        CHECK(capture.GetCaptureCount() == 1);
        CHECK(inverse == Mat4::Identity());
        CHECK(IsFinite(inverse));

        const Mat3 singular3(0.0f);
        CHECK(singular3.Inverse() == Mat3::Identity());
        CHECK(capture.GetCaptureCount() == 2);
    }

    TEST_CASE("determinant: identity, scale and mirroring")
    {
        CHECK(Mat4::Identity().Determinant() == doctest::Approx(1.0f));
        CHECK(Mat4::Scale(Vec3(2.0f, 3.0f, 4.0f)).Determinant() == doctest::Approx(24.0f));
        CHECK(Mat4::Scale(Vec3(-2.0f, 3.0f, 4.0f)).Determinant() == doctest::Approx(-24.0f));
        CHECK(Mat4::Rotate(Vec3::UnitX(), Radians(50.0f)).Determinant() == doctest::Approx(1.0f).epsilon(1e-5f));
        CHECK(Mat4::Translate(Vec3(9.0f, 9.0f, 9.0f)).Determinant() == doctest::Approx(1.0f));
    }

    TEST_CASE("Mat3 rotation and scaling")
    {
        const Mat3 rotation = Mat3::FromRotation(Radians(90.0f));
        CHECK(ApproximatelyEqual(rotation * Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), 1e-6f));

        const Mat3 scaling = Mat3::FromScaling(Vec2(2.0f, 3.0f));
        CHECK(ApproximatelyEqual(scaling * Vec3(1.0f, 1.0f, 1.0f), Vec3(2.0f, 3.0f, 1.0f)));

        const Mat3 basis = Mat3::FromBasisVectors(Vec3::UnitY(), Vec3::UnitZ(), Vec3::UnitX());
        CHECK(ApproximatelyEqual(basis * Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f)));
    }

    TEST_CASE("Mat3 inverse and determinant")
    {
        const Mat3 scaling(2.0f, 0.0f, 0.0f, 0.0f, 4.0f, 0.0f, 0.0f, 0.0f, 8.0f);
        CHECK(scaling.Determinant() == doctest::Approx(64.0f));

        const Mat3 inverse = scaling.Inverse();
        CHECK(inverse[0][0] == doctest::Approx(0.5f));
        CHECK(inverse[1][1] == doctest::Approx(0.25f));
        CHECK(inverse[2][2] == doctest::Approx(0.125f));
    }

    TEST_CASE("perspective projection follows the right-handed Vulkan convention")
    {
        const Mat4 projection = Mat4::PerspectiveRH(Radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);

        // A point on the near plane maps to z' = 0; the far plane maps to z' = 1 (after the divide).
        const Vec3 nearClip = projection.TransformPoint(Vec3(0.0f, 0.0f, -0.1f));
        const Vec3 farClip = projection.TransformPoint(Vec3(0.0f, 0.0f, -1000.0f));
        CHECK(nearClip.Z == doctest::Approx(0.0f).epsilon(1e-4f).scale(1.0f));
        CHECK(farClip.Z == doctest::Approx(1.0f).epsilon(1e-4f).scale(1.0f));

        // Points between the planes land strictly inside (0,1) and increase monotonically with depth.
        const Vec3 middleClip = projection.TransformPoint(Vec3(0.0f, 0.0f, -10.0f));
        CHECK(middleClip.Z > 0.0f);
        CHECK(middleClip.Z < 1.0f);

        // Looking down -Z at the centre of the screen keeps x and y at zero.
        CHECK(ApproximatelyEqual(nearClip.XY(), Vec2(0.0f, 0.0f), 1e-5f));
    }

    TEST_CASE("perspective projects the frustum edges to clip-space bounds")
    {
        const Float32 aspect = 2.0f;
        const Mat4 projection = Mat4::PerspectiveRH(Radians(90.0f), aspect, 1.0f, 100.0f);

        // At z = -1 with a 90 degree vertical FOV the visible half-height is 1, so a point at y = 1
        // projects to NDC y = +1 and x = aspect maps to NDC x = +1.
        const Vec3 topEdge = projection.TransformPoint(Vec3(0.0f, 1.0f, -1.0f));
        CHECK(topEdge.Y == doctest::Approx(1.0f).epsilon(1e-4f).scale(1.0f));

        const Vec3 rightEdge = projection.TransformPoint(Vec3(aspect, 0.0f, -1.0f));
        CHECK(rightEdge.X == doctest::Approx(1.0f).epsilon(1e-4f).scale(1.0f));
    }

    TEST_CASE("perspective rejects degenerate parameters")
    {
        ScopedAssertCapture capture;
        const Mat4 bad = Mat4::PerspectiveRH(0.0f, 1.0f, 0.1f, 100.0f);
        HP_UNUSED(bad);
        CHECK(capture.GetCaptureCount() == 1);

        const Mat4 badAspect = Mat4::PerspectiveRH(Radians(60.0f), 0.0f, 0.1f, 100.0f);
        HP_UNUSED(badAspect);
        CHECK(capture.GetCaptureCount() == 2);

        const Mat4 badPlanes = Mat4::PerspectiveRH(Radians(60.0f), 1.0f, 100.0f, 0.1f);
        HP_UNUSED(badPlanes);
        CHECK(capture.GetCaptureCount() == 3);
    }

    TEST_CASE("orthographic projection maps the volume to the clip cube")
    {
        const Mat4 projection = Mat4::OrthographicRH(8.0f, 4.0f, 0.5f, 50.0f);

        const Vec3 left = projection.TransformPoint(Vec3(-4.0f, 0.0f, 0.0f));
        CHECK(left.X == doctest::Approx(-1.0f).epsilon(1e-5f).scale(1.0f));
        CHECK(left.Y == doctest::Approx(0.0f));

        const Vec3 top = projection.TransformPoint(Vec3(0.0f, 2.0f, 0.0f));
        CHECK(top.Y == doctest::Approx(1.0f).epsilon(1e-5f).scale(1.0f));

        // Depth: near -> 0, far -> 1 (Vulkan range), and it is linear in an orthographic projection.
        const Vec3 nearPlane = projection.TransformPoint(Vec3(0.0f, 0.0f, -0.5f));
        const Vec3 farPlane = projection.TransformPoint(Vec3(0.0f, 0.0f, -50.0f));
        CHECK(nearPlane.Z == doctest::Approx(0.0f).epsilon(1e-5f).scale(1.0f));
        CHECK(farPlane.Z == doctest::Approx(1.0f).epsilon(1e-5f).scale(1.0f));
    }

    TEST_CASE("LookAtRH places the camera at the eye looking down -Z")
    {
        const Mat4 view = Mat4::LookAtRH(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f, 0.0f, 0.0f), Vec3::Up());

        // The eye maps to the view-space origin; a point in front of the camera gets a negative Z.
        CHECK(ApproximatelyEqual(view.TransformPoint(Vec3(0.0f, 0.0f, 5.0f)), Vec3(0.0f, 0.0f, 0.0f), 1e-6f));
        const Vec3 inFront = view.TransformPoint(Vec3(0.0f, 0.0f, 0.0f));
        CHECK(inFront.Z < 0.0f);

        // World +Y stays up, and the transform is a rigid motion (no scale).
        CHECK(ApproximatelyEqual(view.TransformPoint(Vec3(0.0f, 1.0f, 5.0f)), Vec3(0.0f, 1.0f, 0.0f), 1e-6f));
        CHECK(view.GetScale() == Vec3(1.0f, 1.0f, 1.0f));
    }

    TEST_CASE("row extraction reads across the stored columns")
    {
        Mat4 m;
        m[0][0] = 1.0f;
        m[1][0] = 2.0f;
        m[2][0] = 3.0f;
        m[3][0] = 4.0f;  // row 0
        const Vec4 row0 = m.GetRow(0);
        CHECK(row0 == Vec4(1.0f, 2.0f, 3.0f, 4.0f));

        // An out-of-range row is reported *and* clamped: assertions continue by contract, so the
        // accessor must never read past the array (ASan would flag it as a stack-buffer-overflow).
        ScopedAssertCapture capture;
        const Vec4 outOfRange = m.GetRow(9);
        CHECK(capture.GetCaptureCount() == 1);
        CHECK(outOfRange == m.GetRow(3));
    }

    TEST_CASE("matrices are trivially copyable and 64 bytes")
    {
        CHECK(std::is_trivially_copyable_v<Mat4>);
        CHECK(sizeof(Mat4) == 64);
        CHECK(sizeof(Mat3) == 48);
    }
}
