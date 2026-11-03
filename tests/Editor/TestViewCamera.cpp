#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>

#include <doctest/doctest.h>

#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Editor/EditorStyle.h"
#include "Hollowpoint/Editor/ViewCamera.h"
#include "Hollowpoint/Math/MathCore.h"

// ---------------------------------------------------------------------------
// View navigation maths and the editor style — the two new editor modules that need no window, device or ImGui.
//
// These are the behaviours that are invisible in a screenshot and obvious in use: an orbit that drifts, a dolly that
// crosses its pivot, a pan that accumulates roll, a compass whose "right" disagrees with the rendered frame. Each case
// below asserts a property (distance preserved, target fixed, basis orthonormal) rather than a magic coordinate, so a
// legitimate change of convention fails loudly instead of silently shifting every number.
// ---------------------------------------------------------------------------

namespace
{

using Hollowpoint::Editor::ComputeEditorStyle;
using Hollowpoint::Editor::DollyView;
using Hollowpoint::Editor::FlyView;
using Hollowpoint::Editor::FrameBounds;
using Hollowpoint::Editor::GetViewAngles;
using Hollowpoint::Editor::GetViewBasis;
using Hollowpoint::Editor::OrbitView;
using Hollowpoint::Editor::PanView;
using Hollowpoint::Editor::ResolveUiFontPath;
using Hollowpoint::Editor::ViewAxisInput;
using Hollowpoint::Editor::ViewNavigationSpeeds;
using Hollowpoint::Math::Float32;
using Hollowpoint::Math::Vec3;

[[nodiscard]] bool NearlyEqual(float a, float b, float tolerance = 1e-3f)
{
    return std::abs(a - b) <= tolerance;
}

const ViewNavigationSpeeds kSpeeds;

}  // namespace

TEST_SUITE("EditorViewCamera")
{
    TEST_CASE("FlyView translates eye and target together, preserving the view direction")
    {
        Vec3 eye(0.0f, 1.0f, 5.0f);
        Vec3 target(0.0f, 1.0f, 0.0f);

        ViewAxisInput input;
        input.Forward = 1.0f;
        FlyView(eye, target, input, 1.0f, kSpeeds);

        // Moved forward (towards -Z here) by exactly the configured speed for one second. The target sits *ahead* of the
        // eye along the view direction, so it is 5 m further along -Z than the eye after a full second of flight.
        CHECK(eye.Z < 5.0f);
        CHECK(NearlyEqual(eye.X, target.X));
        CHECK(NearlyEqual(target.Z, eye.Z - 5.0f));

        // Direction preserved: the vector between them is a positive scale of what it was.
        const Vec3 before(0.0f, 0.0f, -1.0f);
        const Vec3 after = (target - eye).Normalized();
        CHECK(NearlyEqual(after.Dot(before), 1.0f));
    }

    TEST_CASE("FlyView holds speed constant on a diagonal instead of adding the axes")
    {
        Vec3 eye(0.0f, 0.0f, 0.0f);
        Vec3 target(0.0f, 0.0f, -1.0f);

        ViewAxisInput straight;
        straight.Forward = 1.0f;
        const Vec3 eyeStraight = eye;
        FlyView(eye, target, straight, 1.0f, kSpeeds);
        const float straightDistance = (eye - eyeStraight).Length();

        Vec3 eyeDiag(0.0f, 0.0f, 0.0f);
        Vec3 targetDiag(0.0f, 0.0f, -1.0f);
        ViewAxisInput diagonal;
        diagonal.Forward = 1.0f;
        diagonal.Right = 1.0f;
        FlyView(eyeDiag, targetDiag, diagonal, 1.0f, kSpeeds);

        // The classic fly-camera bug is sqrt(2)x on diagonals; asserting equality of magnitude catches it while still
        // allowing the direction to differ.
        CHECK(NearlyEqual((eyeDiag - Vec3(0.0f, 0.0f, 0.0f)).Length(), straightDistance));
    }

    TEST_CASE("OrbitView preserves eye-target distance and keeps the target fixed")
    {
        Vec3 eye(0.0f, 0.0f, 4.0f);
        const Vec3 target(1.0f, 2.0f, 3.0f);
        const float distance = (eye - target).Length();

        OrbitView(eye, target, 120.0f, 40.0f, kSpeeds);

        CHECK(NearlyEqual((eye - target).Length(), distance));
    }

    TEST_CASE("OrbitView keeps pitch out of the ill-conditioned band near the poles")
    {
        // Why this test is written as a bound on the angle rather than as "further drags change nothing": OrbitView reads
        // its state through GetViewAngles, whose atan2 already caps pitch at ±90°, so the view can never invert no matter
        // what — an unclamped implementation also "saturates", and a saturation test passes either way. The explicit
        // clamp exists for the band just *below* 90°, where the right axis derived from world up shrinks towards zero and
        // the view starts to wobble numerically. So the property worth pinning is the bound itself, at the value the
        // implementation promises (±0.49π ≈ ±88.2°).
        Vec3 eye(0.0f, 0.0f, 4.0f);
        const Vec3 target(0.0f, 0.0f, 0.0f);

        float worstPitch = 0.0f;
        for (int iteration = 0; iteration < 400; ++iteration)
        {
            OrbitView(eye, target, 0.0f, 25.0f, kSpeeds);

            float yaw = 0.0f;
            float pitch = 0.0f;
            GetViewAngles(eye, target, yaw, pitch);
            worstPitch = std::max(worstPitch, std::abs(pitch));

            // Condition number check at every step: the derived right axis must stay a unit vector, which is exactly what
            // fails (numerically) as the view direction approaches world up.
            const auto basis = GetViewBasis(eye, target);
            REQUIRE(NearlyEqual(basis.Right.Length(), 1.0f));
        }

        CHECK(worstPitch > 0.5f);    // it genuinely climbed
        CHECK(worstPitch <= 1.54f);  // and stopped short of the poles (0.49π = 1.5394)
    }

    TEST_CASE("OrbitView is a no-op for a zero drag, so an exact camera survives an idle frame")
    {
        Vec3 eye(1.0f, 2.0f, 3.0f);
        const Vec3 target(0.0f, 0.0f, 0.0f);

        OrbitView(eye, target, 0.0f, 0.0f, kSpeeds);
        CHECK(NearlyEqual(eye.X, 1.0f));
        CHECK(NearlyEqual(eye.Y, 2.0f));
        CHECK(NearlyEqual(eye.Z, 3.0f));
    }

    TEST_CASE("DollyView moves along the ray, never past the target, and is reversible")
    {
        Vec3 eye(0.0f, 0.0f, 10.0f);
        const Vec3 target(0.0f, 0.0f, 0.0f);

        DollyView(eye, target, 2.0f, kSpeeds);
        CHECK((eye - target).Length() < 10.0f);

        // A huge zoom-in stops at the minimum rather than crossing the pivot and inverting the view.
        Vec3 extreme = eye;
        for (int iteration = 0; iteration < 400; ++iteration)
        {
            DollyView(extreme, target, 5.0f, kSpeeds);
        }
        CHECK((extreme - target).Length() >= kSpeeds.MinimumTargetDistance - 1e-4f);
        CHECK(extreme.Z > 0.0f);

        // Reversible: equal and opposite notches return to the same distance (not position — yaw is untouched, so a
        // round trip is a scalar property here).
        Vec3 back(0.0f, 0.0f, 10.0f);
        const float before = (back - target).Length();
        DollyView(back, target, 3.0f, kSpeeds);
        DollyView(back, target, -3.0f, kSpeeds);
        CHECK(NearlyEqual((back - target).Length(), before));
    }

    TEST_CASE("PanView translates within the view plane, so it pans rather than dollies")
    {
        // A pan must move the camera perpendicular to its view direction. This is not testable by checking that the
        // direction or distance is preserved — *any* translation of both endpoints does that, and an implementation that
        // slid the camera along its own forward axis would pass such an assertion while behaving nothing like a pan. So
        // the property under test is the direction of the displacement relative to the view, which is what makes the
        // point under the cursor stay under the cursor.
        const Vec3 eye(0.0f, 0.0f, 10.0f);
        const Vec3 target(0.0f, 0.0f, 0.0f);

        Vec3 moved = eye;
        Vec3 movedTarget = target;
        PanView(moved, movedTarget, 40.0f, 25.0f, 600.0f, 1.0f);

        const Vec3 displacement = moved - eye;
        const Vec3 viewDirection = (target - eye).Normalized();
        CHECK_FALSE(NearlyEqual(displacement.Length(), 0.0f, 1e-4f));  // it did move
        CHECK(NearlyEqual(displacement.Dot(viewDirection), 0.0f));     // and not along the view axis

        // The target moves by exactly the same vector: a pan is a rigid translation of the whole camera frame.
        CHECK(NearlyEqual((movedTarget - target).Dot(displacement), displacement.LengthSquared()));

        // Horizontal drag goes along the view's right axis, vertical along its up axis — asserted per component so a
        // swap (which reads as "panning moves diagonally") fails here rather than in a screenshot.
        Vec3 sideways = eye;
        Vec3 sidewaysTarget = target;
        PanView(sideways, sidewaysTarget, 30.0f, 0.0f, 600.0f, 1.0f);
        const auto basis = GetViewBasis(eye, target);
        CHECK(NearlyEqual((sideways - eye).Dot(basis.Up), 0.0f));
        CHECK_FALSE(NearlyEqual((sideways - eye).Dot(basis.Right), 0.0f));

        Vec3 vertical = eye;
        Vec3 verticalTarget = target;
        PanView(vertical, verticalTarget, 0.0f, 30.0f, 600.0f, 1.0f);
        CHECK(NearlyEqual((vertical - eye).Dot(basis.Right), 0.0f));
        CHECK_FALSE(NearlyEqual((vertical - eye).Dot(basis.Up), 0.0f));
    }

    TEST_CASE("PanView scales with distance and does not tilt the horizon")
    {
        const Vec3 target(0.0f, 0.0f, 0.0f);

        // Metres-per-pixel grows with distance, so a far scene pans further per pixel — the property that makes it feel
        // like grabbing the world instead of swimming through molasses at range.
        Vec3 near(0.0f, 0.0f, 5.0f);
        Vec3 nearTarget = target;
        PanView(near, nearTarget, 20.0f, 0.0f, 600.0f, 1.0f);

        Vec3 far(0.0f, 0.0f, 50.0f);
        Vec3 farTarget = target;
        PanView(far, farTarget, 20.0f, 0.0f, 600.0f, 1.0f);

        CHECK((far - Vec3(0.0f, 0.0f, 50.0f)).Length() > (near - Vec3(0.0f, 0.0f, 5.0f)).Length());

        // What is *not* asserted here, and why: "panning never accumulates roll" sounds like the property worth pinning,
        // but it is structural in this engine — the camera stores no up vector and every LookAt call passes a fixed world
        // up, so there is nothing for roll to accumulate in. Asserting it would only re-test GetViewBasis's own definition
        // (a tautology that cannot fail), which is worse than no test: it looks like coverage and protects nothing.
    }

    TEST_CASE("GetViewBasis matches LookAtRH's basis exactly")
    {
        const Vec3 eye(2.0f, 3.0f, 4.0f);
        const Vec3 target(0.0f, 0.0f, 0.0f);
        const Hollowpoint::Editor::ViewBasis basis = GetViewBasis(eye, target);

        // Mat4::LookAtRH builds right = normalize(cross(up, -forward)) and up = cross(right, forward). Asserting the
        // same here is what guarantees the compass overlay and the rendered frame cannot disagree about which way is right.
        const Vec3 forward = (target - eye).Normalized();
        const Vec3 expectedRight = Vec3(0.0f, 1.0f, 0.0f).Cross(-forward).Normalized();

        CHECK(NearlyEqual(basis.Forward.X, forward.X));
        CHECK(NearlyEqual(basis.Right.Dot(expectedRight), 1.0f));
        CHECK(NearlyEqual(basis.Right.Dot(basis.Up), 0.0f));
        CHECK(NearlyEqual(basis.Right.Length(), 1.0f));
    }

    TEST_CASE("GetViewBasis survives a degenerate eye==target without producing NaNs")
    {
        const Hollowpoint::Editor::ViewBasis basis = GetViewBasis(Vec3(1.0f, 1.0f, 1.0f), Vec3(1.0f, 1.0f, 1.0f));

        // A hand-typed `viewport.camera 1 1 1 1 1 1` is user input, not a programmer error: the fallback must be finite
        // and usable rather than poisoning every downstream matrix with NaN.
        CHECK(std::isfinite(basis.Forward.X));
        CHECK(std::isfinite(basis.Right.Y));
        CHECK(NearlyEqual(basis.Right.Length(), 1.0f));
    }

    TEST_CASE("FrameBounds frames a sphere and refuses degenerate input")
    {
        Vec3 eye(0.0f, 0.0f, 10.0f);
        Vec3 target(0.0f, 0.0f, 0.0f);

        const Vec3 originalEye = eye;

        CHECK(FrameBounds(eye, target, Vec3(0.0f, 0.0f, 0.0f), 2.0f, 1.0f));
        // Bigger radius than the previous view: the eye backs off, and the target moves to what was framed.
        CHECK((eye - target).Length() > 2.0f);
        CHECK(NearlyEqual(target.X, 0.0f));

        // Framing keeps the author on the *same side* of what they framed, looking in the same direction: only the
        // distance changes. Asserting "the eye moves backwards" is wrong (framing something you are too far from correctly
        // moves you closer — the radius here needs ~6 m and the camera was at 10), so the invariant is directional: the
        // vector from the framed centre to the eye must be unchanged in direction. Flipping the sign in FrameBounds puts
        // the eye on the far side of the object — a mirror-image view that reads as "F spun my scene around" and passes
        // every distance and centre assertion above.
        const Vec3 centre(0.0f, 0.0f, 0.0f);
        const Vec3 sideBefore = (originalEye - centre).Normalized();
        const Vec3 sideAfter = (eye - target).Normalized();
        CHECK(NearlyEqual(sideAfter.Dot(sideBefore), 1.0f));

        // The view direction is likewise preserved: we are still looking the same way, just nearer.
        const Vec3 forwardBefore = (centre - originalEye).Normalized();
        const Vec3 forwardAfter = (target - eye).Normalized();
        CHECK(NearlyEqual(forwardAfter.Dot(forwardBefore), 1.0f));

        // Each rejected input class must leave the camera exactly where it was: a framing command that half-works is
        // worse than one that does nothing, because it moves the eye somewhere unusable and reports success.
        const auto refusesUntouched = [](Float32 radius, Float32 fieldOfView) {
            Vec3 cameraEye(0.0f, 0.0f, 7.0f);
            Vec3 cameraTarget(0.0f, 0.0f, 0.0f);
            const bool accepted = FrameBounds(cameraEye, cameraTarget, Vec3(5.0f, 5.0f, 5.0f), radius, fieldOfView);
            CHECK_FALSE(accepted);
            CHECK(NearlyEqual(cameraEye.Z, 7.0f));
            CHECK(NearlyEqual(cameraTarget.Z, 0.0f));
        };

        refusesUntouched(0.0f, 1.0f);   // no drawable geometry
        refusesUntouched(-3.0f, 1.0f);  // nonsense radius
        refusesUntouched(2.0f, 0.0f);   // a projection with no field of view

        // NaN is the important one: it comes from imported geometry with corrupt vertex data, and `radius <= 0` would
        // classify it as valid (the comparison is false for NaN) and place the eye at NaN — after which every matrix in
        // the frame is NaN and the screen goes black with no error anywhere. The guard's negated predicate rejects it.
                const Float32 notANumber = std::numeric_limits<Float32>::quiet_NaN();
        refusesUntouched(notANumber, 1.0f);

        // A finite but absurd object: backing off to 1e30 metres degenerates the projection, so it is refused too.
        refusesUntouched(1.0e30f, 1.0f);
    }

    TEST_CASE("ComputeEditorStyle scales every metric together and clamps a nonsense scale")
    {
        const auto base = ComputeEditorStyle(1.0f);
        const auto larger = ComputeEditorStyle(2.0f);

        // Scaling the font alone leaves widgets their old padding, which reads as broken layout — so the invariant that
        // matters is not any particular number but that they move as a set.
        CHECK(larger.FontSizePixels > base.FontSizePixels);
        CHECK(larger.WindowPaddingX > base.WindowPaddingX);
        CHECK(larger.FramePaddingY > base.FramePaddingY);
        CHECK(larger.ItemSpacingY > base.ItemSpacingY);

        // Clamped rather than trusted: a negative or zero scale produces widgets with no hit area, and `ui.scale` is a
        // console command someone can mistype.
        const auto clamped = ComputeEditorStyle(-5.0f);
        CHECK(clamped.Scale > 0.0f);
        CHECK(clamped.WindowPaddingX > 0.0f);
        CHECK(ComputeEditorStyle(100.0f).Scale <= 3.0f);
    }

    TEST_CASE("ResolveUiFontPath returns a path that exists or nothing at all")
    {
        const std::string path = ResolveUiFontPath();
        if (path.empty())
        {
            // Legitimate on a stripped container: the caller falls back to ImGui's embedded vector font. Asserting a
            // font exists here would make the suite depend on this machine's package set.
            MESSAGE("no system UI font found; the editor uses its embedded fallback");
            return;
        }

        // The contract is "a file ImGui can open", not "a path that looks plausible" — a stale candidate list would
        // otherwise pass a string check and fail at runtime on every platform except the one it was written on.
        std::error_code error;
        CHECK(std::filesystem::exists(path, error));
        CHECK_FALSE(error);
    }
}
