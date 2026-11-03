#pragma once

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/Math/MathCore.h"

// ---------------------------------------------------------------------------
// ViewCamera — the maths behind navigating a viewport, with no window and no ImGui.
//
// The editor's fly camera is stored as position + target (that is what CameraComponent holds and what scene files
// write), but orbiting and dollying are naturally spherical: "swing the eye around the point I'm looking at". Keeping
// both descriptions in sync by hand across six call sites is how an orbit drifts vertically or a dolly overshoots, so
// every navigation operation converts once, mutates, and writes back. These are pure Vec3 functions — testable with no
// display — because "orbit keeps the target fixed" and "dolly never crosses the pivot" are exactly the properties that
// are invisible in a screenshot and obvious in use.
//
// Conventions inherited from Renderer/Camera.h: right-handed, +Y up, metres, radians. Yaw rotates about +Y (world up),
// pitch rotates about the eye's right axis, and pitch is clamped away from the poles where the view direction becomes
// parallel to Up and LookAt's cross product degenerates — which in an orbit reads as the world snapping 180° when you
// look straight down.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Editor
{

using Math::Float32;
using Math::Vec3;

//! Navigation tuning, in units a config file or console command can set without touching the maths.
struct ViewNavigationSpeeds
{
    //! Metres per second flying with WASD/QE at 1x.
    Float32 FlyMetresPerSecond = 5.0f;

    //! Multiplier applied while Shift is held (the sprint feel every editor has).
    Float32 SprintMultiplier = 4.0f;

    //! Radians of yaw/pitch per pixel of orbit drag. ~0.006 rad/px is a half turn across ~150 px, which feels right
    //  across panel sizes because it scales with the drag rather than with an absolute speed.
    Float32 OrbitRadiansPerPixel = 0.006f;

    //! Fractional zoom per wheel notch: 0.16 moves the eye 16% closer to (or further from) the target along the ray.
    Float32 DollyPerWheelNotch = 0.16f;

    //! Smallest eye-to-target distance in metres. Stops a dolly passing through the pivot and coming out inverted on
    //  the other side — the classic "I zoomed into a mesh and lost the scene".
    Float32 MinimumTargetDistance = 0.05f;

    //! Largest eye-to-target distance, so a runaway wheel cannot put the eye where float precision makes orbiting
    //  jitter (about 1e5 metres is already far outside anything this engine renders).
    Float32 MaximumTargetDistance = 100000.0f;
};

//! One frame's worth of movement intent, as axis values in -1..1 (time and speed are applied by FlyView).
struct ViewAxisInput
{
    Float32 Forward = 0.0f;
    Float32 Right = 0.0f;
    Float32 Up = 0.0f;
    bool Sprint = false;
};

//! An orthonormal view basis: Forward = normalise(target - eye), Right/Up matching Mat4::LookAtRH.
/**
 * Exported because the viewport's corner axis indicator projects world axes through exactly this basis, and having the
 * overlay compute its own would let the gizmo disagree with the camera it is describing — a gizmo pointing 5° off the
 * view you are looking through is worse than no gizmo, because it reads as a projection bug.
 */
struct ViewBasis
{
    Vec3 Forward = Vec3(0.0f, 0.0f, -1.0f);
    Vec3 Right = Vec3(1.0f, 0.0f, 0.0f);
    Vec3 Up = Vec3(0.0f, 1.0f, 0.0f);
};

//! The basis of the current view. Degenerate input (eye == target) yields a stable fallback rather than NaNs.
[[nodiscard]] HP_EDITOR_API ViewBasis GetViewBasis(const Vec3& eye, const Vec3& target) noexcept;

//! Moves eye and target together, preserving the view direction exactly (a translation: this is "move through the
//  world", not "look somewhere").
/**
 * Forward/back uses the full view direction; strafe and up use the horizontal plane and world up respectively. The
 * split matters: moving along the raw forward vector while looking down buries the camera in the floor within a second,
 * which is the failure mode every hand-written fly camera has before someone notices.
 */
HP_EDITOR_API void FlyView(Vec3& ioEye, Vec3& ioTarget, const ViewAxisInput& input, Float32 deltaSeconds,
                           const ViewNavigationSpeeds& speeds) noexcept;

//! Orbits the eye around `target` by a pixel drag (dx yaws, dy pitches), preserving eye-to-target distance.
/**
 * A zero-length drag leaves Position untouched rather than re-deriving it from spherical coordinates, so a camera set
 * exactly by `viewport.camera 1 2 3` survives an idle frame — which is what makes that command assertable.
 */
HP_EDITOR_API void OrbitView(Vec3& ioEye, const Vec3& target, Float32 dragPixelsX, Float32 dragPixelsY,
                             const ViewNavigationSpeeds& speeds) noexcept;

//! Moves the eye along the view ray by wheel notches (positive = towards the target), target fixed.
/**
 * Multiplicative rather than additive: an additive step feels sluggish when you are far away and uncontrollable close
 * up, and it is what makes "one notch = one notch" true at every distance. Clamped to [MinimumTargetDistance,
 * MaximumTargetDistance] so the eye can neither cross the pivot nor escape to imprecision.
 */
HP_EDITOR_API void DollyView(Vec3& ioEye, const Vec3& target, Float32 wheelNotches,
                             const ViewNavigationSpeeds& speeds) noexcept;

//! Pans eye and target along the view's right/up axes by a pixel drag.
/**
 * Metres-per-pixel is derived from the distance to the target and the vertical field of view, so the scene under the
 * cursor tracks the cursor — the property that makes a pan feel like grabbing the world. A fixed-metre pan crawls at
 * arm's length and launches the scene off-screen when zoomed out.
 */
HP_EDITOR_API void PanView(Vec3& ioEye, Vec3& ioTarget, Float32 dragPixelsX, Float32 dragPixelsY, Float32 viewHeightPixels,
                           Float32 verticalFieldOfViewRadians) noexcept;

//! Places the eye so a bounding sphere fits the frame along the current view direction ("frame selection", F).
/**
 * `frameFraction` is how much of the viewport height the sphere should occupy (0.7 leaves margin). Returns false,
 * leaving the camera untouched, when the radius or field of view makes the distance meaningless — a framing command
 * that teleports the eye to infinity is worse than one that does nothing and reports why.
 */
[[nodiscard]] HP_EDITOR_API bool FrameBounds(Vec3& ioEye, Vec3& ioTarget, const Vec3& boundsCentre,
                                             Float32 boundsRadius, Float32 verticalFieldOfViewRadians,
                                             float frameFraction = 0.7f) noexcept;

//! The yaw/pitch (radians) of the current eye→target direction. For reporting, and the basis OrbitView is tested on.
HP_EDITOR_API void GetViewAngles(const Vec3& eye, const Vec3& target, Float32& outYawRadians,
                                               Float32& outPitchRadians) noexcept;

}  // namespace Hollowpoint::Editor
