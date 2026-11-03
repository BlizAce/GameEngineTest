#include <algorithm>
#include <cmath>

#include "Hollowpoint/Editor/ViewCamera.h"

// ---------------------------------------------------------------------------
// See the header for the contract and the conventions this inherits.
//
// Implementation note worth stating once: orbit/dolly/pan all work against the eye-target pair directly rather than
// maintaining a spherical state struct, because the pair *is* the editor's camera state (CameraComponent stores it and
// scene files write it) — a second representation would need synchronising in both directions. The conversions are
// therefore local and cheap: build a basis, rotate or scale, write back.
//
// The basis is derived to match Mat4::LookAtRH exactly (engine/Math/Src/MathCore.cpp): with view direction v =
// normalise(target - eye), its right axis is up × (-v) == v × up and its up axis is right × v. Both are written that way
// below, so a navigation vector and the rendered frame cannot disagree about which way "right" is — the kind of mismatch
// that shows up as panning diagonally on a correctly-configured camera.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Editor
{

namespace
{

constexpr Float32 kPi = 3.14159265358979323846f;
const Vec3 kWorldUp(0.0f, 1.0f, 0.0f);

//! Pitch limit short of the poles. At exactly ±90° the view direction is parallel to Up and the right axis derived from
//  their cross product has no defined direction (Camera.h documents the degenerate handling); an orbit reaching it would
//  flip the scene's roll, so navigation stops just short instead.
constexpr Float32 kMaxPitchRadians = kPi * 0.49f;

[[nodiscard]] Vec3 SafeNormalized(const Vec3& value, const Vec3& fallback) noexcept
{
    // A zero-length direction means eye == target, which `viewport.camera 0 0 0 0 0 0` (user input) can certainly
    // produce. The fallback keeps every caller's arithmetic finite; asserting would take the editor down over a camera
    // value, which is data rather than a programmer error.
    return value.LengthSquared() > 1e-12f ? value.Normalized() : fallback;
}

//! Orthonormal basis of the current view: forward = normalise(target - eye), right and up matching LookAtRH.
[[nodiscard]] ViewBasis MakeBasis(const Vec3& eye, const Vec3& target) noexcept
{
    ViewBasis basis;
    basis.Forward = SafeNormalized(target - eye, basis.Forward);

    // Right from *world* up, not the camera's stored up: deriving it from the camera's own up lets each pan tilt the
    // horizon a little further, and after a minute of panning the scene is visibly slanted with nothing to explain it.
    Vec3 right = basis.Forward.Cross(kWorldUp);
    if (right.LengthSquared() <= 1e-12f)
    {
        // Looking straight up or down: any perpendicular is valid, so take a stable one rather than the zero vector.
        right = Vec3(1.0f, 0.0f, 0.0f);
    }

    basis.Right = right.Normalized();
    basis.Up = basis.Right.Cross(basis.Forward).Normalized();
    return basis;
}

[[nodiscard]] Float32 ClampDistance(Float32 distance, const ViewNavigationSpeeds& speeds) noexcept
{
    return std::clamp(distance, speeds.MinimumTargetDistance, speeds.MaximumTargetDistance);
}

}  // namespace

ViewBasis GetViewBasis(const Vec3& eye, const Vec3& target) noexcept
{
    return MakeBasis(eye, target);
}

void FlyView(Vec3& ioEye, Vec3& ioTarget, const ViewAxisInput& input, Float32 deltaSeconds,
             const ViewNavigationSpeeds& speeds) noexcept
{
    if (std::abs(input.Forward) < 1e-6f && std::abs(input.Right) < 1e-6f && std::abs(input.Up) < 1e-6f)
    {
        return;
    }

    const ViewBasis basis = MakeBasis(ioEye, ioTarget);

    // Forward is the full view direction (W flies where you look, including uphill); strafe stays horizontal and "up"
    // is world up, so looking down and pressing W does not bury the camera in the floor. See the header.
    const Vec3 horizontalForward = SafeNormalized(Vec3(basis.Forward.X, 0.0f, basis.Forward.Z), basis.Right);

    const Float32 metres = speeds.FlyMetresPerSecond * (input.Sprint ? speeds.SprintMultiplier : 1.0f) *
                           std::max(0.0f, deltaSeconds);

    // Normalised so holding W+D does not move sqrt(2)x faster than W alone — the diagonal-speed bug that makes a fly
    // camera slippery exactly where precision matters.
    Vec3 direction = basis.Forward * input.Forward + horizontalForward.Cross(kWorldUp) * input.Right +
                     kWorldUp * input.Up;
    if (direction.LengthSquared() > 1e-12f)
    {
        direction = direction.Normalized();
    }

    const Vec3 offset = direction * metres;
    ioEye = ioEye + offset;
    ioTarget = ioTarget + offset;
}

void OrbitView(Vec3& ioEye, const Vec3& target, Float32 dragPixelsX, Float32 dragPixelsY,
               const ViewNavigationSpeeds& speeds) noexcept
{
    if (std::abs(dragPixelsX) < 1e-6f && std::abs(dragPixelsY) < 1e-6f)
    {
        return;
    }

    const Vec3 offset = ioEye - target;
    const Float32 distance = ClampDistance(offset.Length(), speeds);
    if (distance <= 0.0f)
    {
        return;
    }

    Float32 yaw = 0.0f;
    Float32 pitch = 0.0f;
    GetViewAngles(ioEye, target, yaw, pitch);

    // Sign convention: the drag *grabs the world*. Dragging right moves the eye left around the target (yaw down) so
    // the scene follows the cursor; dragging down raises the eye (pitch up) for the same reason. Getting either sign
    // backwards is invisible in a screenshot and maddening in use, which is why it is stated here and asserted in tests.
    yaw -= dragPixelsX * speeds.OrbitRadiansPerPixel;
    pitch = std::clamp(pitch + dragPixelsY * speeds.OrbitRadiansPerPixel, -kMaxPitchRadians, kMaxPitchRadians);

    // Rebuilt from spherical coordinates about the target: cos(pitch) scales the horizontal ring, sin(pitch) is the
    // height. Swapping those two produces an orbit that rolls instead of tilting.
    const Float32 horizontal = std::cos(pitch) * distance;
    const Vec3 newOffset(horizontal * std::sin(yaw), std::sin(pitch) * distance, horizontal * std::cos(yaw));
    ioEye = target + newOffset;
}

void DollyView(Vec3& ioEye, const Vec3& target, Float32 wheelNotches, const ViewNavigationSpeeds& speeds) noexcept
{
    if (std::abs(wheelNotches) < 1e-6f)
    {
        return;
    }

    const Vec3 offset = ioEye - target;
    const Float32 distance = offset.Length();
    if (distance <= 0.0f)
    {
        return;
    }

    // Multiplicative, so one notch feels like one notch at every distance (an additive step is sluggish far away and
    // uncontrollable close up). pow() with a positive base keeps the sign right: positive notches = towards the target.
    const Float32 scale = std::pow(1.0f - speeds.DollyPerWheelNotch, wheelNotches);
    const Float32 newDistance = ClampDistance(distance * scale, speeds);

    ioEye = target + offset.Normalized() * newDistance;
}

void PanView(Vec3& ioEye, Vec3& ioTarget, Float32 dragPixelsX, Float32 dragPixelsY, Float32 viewHeightPixels,
             Float32 verticalFieldOfViewRadians) noexcept
{
    if (std::abs(dragPixelsX) < 1e-6f && std::abs(dragPixelsY) < 1e-6f)
    {
        return;
    }

    const ViewBasis basis = MakeBasis(ioEye, ioTarget);
    const Float32 distance = std::max(0.01f, (ioEye - ioTarget).Length());

    // World height of one screen pixel at the target's distance, from the vertical FOV: the drag then moves the world by
    // exactly the pixels it covered, which is what makes the point under the cursor stay under the cursor. A fixed
    // metre-per-pixel crawls when zoomed in and throws the scene off-screen when zoomed out.
    const Float32 safeHeight = std::max(1.0f, viewHeightPixels);
    const Float32 tanHalfFov = std::tan(std::max(0.01f, verticalFieldOfViewRadians) * 0.5f);
    const Float32 metresPerPixel = 2.0f * distance * tanHalfFov / safeHeight;

    // ImGui's mouse coordinates have y increasing downwards, so a downward drag (positive dy) moves the camera up in
    // world space — the grab-the-world convention, matching OrbitView above.
    const Vec3 offset = basis.Right * (-dragPixelsX * metresPerPixel) + basis.Up * (dragPixelsY * metresPerPixel);
    ioEye = ioEye + offset;
    ioTarget = ioTarget + offset;
}

bool FrameBounds(Vec3& ioEye, Vec3& ioTarget, const Vec3& boundsCentre, Float32 boundsRadius,
                 Float32 verticalFieldOfViewRadians, float frameFraction) noexcept
{
    // Validation lives on the *computed distance*, not on each input. An earlier version also guarded boundsRadius and
    // verticalFieldOfViewRadians separately here, and mutation-testing it (delete the guard, re-run the suite) showed no
    // test could tell — because every class that guard rejected produces a distance that is non-positive, NaN or infinite
    // and is caught below anyway. Guard code that cannot fail is worse than none: it reads as coverage while the next
    // reader cannot tell which checks are load-bearing.
    //
    // So one predicate rejects all of them, written as the negation of the valid case because `x <= 0` is false for NaN
    // while `!(x > 0)` is true — the idiom that stops a NaN from corrupt imported vertex data being treated as a real
    // radius (which would put the eye at NaN and black out every matrix in the frame with no error anywhere):
    //   !(distance > 0.0f)   — zero or negative radius, NaN, or a zero field of view's division by sin(0).
    //   !(distance < 1e9f)   — finite inputs whose distance overflows: framing an object 1e30 metres across parks the eye
    //                          where float32 spacing exceeds a pixel and the projection degenerates.
    const float fraction = std::clamp(frameFraction, 0.05f, 1.0f);
    const Float32 distance = boundsRadius / std::sin(verticalFieldOfViewRadians * 0.5f) / fraction;
    if (!(distance > 0.0f) || !(distance < 1e9f))
    {
        return false;
    }

    // Keep the current viewing direction, but frame the *bounds centre* rather than wherever the target happened to be.
    const ViewBasis basis = MakeBasis(ioEye, ioTarget);
    ioTarget = boundsCentre;
    ioEye = boundsCentre - basis.Forward * distance;
    return true;
}

void GetViewAngles(const Vec3& eye, const Vec3& target, Float32& outYawRadians, Float32& outPitchRadians) noexcept
{
    // Yaw from +Z towards +X (atan2 of the eye's offset about the target), pitch = elevation of the eye above the
    // target. Together they are a complete, non-degenerate description of the orbit state except at the poles, which
    // OrbitView clamps away from.
    const Vec3 offset = eye - target;
    outYawRadians = std::atan2(offset.X, offset.Z);
    const Float32 horizontal = std::sqrt(offset.X * offset.X + offset.Z * offset.Z);
    outPitchRadians = std::atan2(offset.Y, horizontal);
}

}  // namespace Hollowpoint::Editor
