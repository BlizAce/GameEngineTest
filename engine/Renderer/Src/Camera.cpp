#include <cmath>

#include "Hollowpoint/Renderer/Camera.h"

namespace Hollowpoint::Renderer
{

namespace
{

//! Fallback aspect ratio when the viewport has no area (minimised window, zero-size render target).
constexpr Float32 kFallbackAspectRatio = 1.0f;

[[nodiscard]] Float32 SafeAspectRatio(Float32 aspectRatio)
{
    // A non-positive or non-finite aspect would reach PerspectiveRH, which asserts on it — correct for a programming
    // error, wrong for "window is currently minimised", which is a normal state to survive. Falling back to 1 keeps
    // the matrices finite so the frame can simply be skipped.
    if (!std::isfinite(aspectRatio) || aspectRatio <= 0.0f)
    {
        return kFallbackAspectRatio;
    }

    return aspectRatio;
}

[[nodiscard]] Vec3 ResolveEyePosition(const CameraComponent& camera, const Math::Transform* worldTransform)
{
    // Precedence: the transform hierarchy owns placement when it is supplied. Two writable eye positions with no
    // stated winner is exactly the two-sources-of-truth problem LocalTransform.h describes, so the rule is explicit
    // and tested — component Position is the fallback for cameras not driven by the hierarchy.
    if (worldTransform != nullptr)
    {
        return worldTransform->Position;
    }

    return camera.Position;
}

[[nodiscard]] Vec3 ResolveUpVector(const CameraComponent& camera, const Vec3& forward)
{
    // LookAt is undefined when up is parallel to the view direction (or has zero length): the cross product vanishes
    // and the basis collapses to NaN, which then reaches every culling test and makes the whole scene disappear with
    // no error. Rather than assert — a camera passing straight over the world's pole is a legitimate animation, and a
    // zero-length Up is one bad deserialisation away — pick the world axis least aligned with forward. That is
    // continuous except at the exact singularity and never returns a zero-length vector.
    const Vec3 normalizedForward = forward.Normalized();

    // A degenerate configured Up (zero, non-finite) is treated as "no usable preference" and replaced outright.
    const Vec3 configuredUp = camera.Up.Normalized();
    if (configuredUp.LengthSquared() <= 0.0f)
    {
        return std::fabs(normalizedForward.X) < std::fabs(normalizedForward.Y) ? Vec3(1.0f, 0.0f, 0.0f)
                                                                               : Vec3(0.0f, 1.0f, 0.0f);
    }

    if (std::fabs(configuredUp.Dot(normalizedForward)) < 0.9999f)
    {
        return camera.Up;
    }

    // Parallel to the view direction: choose X or Y, whichever is further from forward.
    const Vec3 worldX(1.0f, 0.0f, 0.0f);
    const Vec3 worldY(0.0f, 1.0f, 0.0f);

    return std::fabs(worldX.Dot(normalizedForward)) < std::fabs(worldY.Dot(normalizedForward)) ? worldX : worldY;
}

}  // namespace

CameraState ComputeCameraState(const CameraComponent& camera, const Math::Transform* worldTransform,
                               Float32 aspectRatio)
{
    CameraState state;
    state.EyePosition = ResolveEyePosition(camera, worldTransform);
    state.AspectRatio = SafeAspectRatio(aspectRatio);

    // LookAtRH normalises internally, but a Position exactly equal to Target produces a zero forward vector there.
    // Nudging is preferable to propagating a NaN through every subsequent matrix multiply: once NaN enters View it
    // reaches ViewProjection, the frustum, and every culling test reports "outside", so the whole scene vanishes with
    // no error — the hardest kind of blank-screen bug to trace.
    Vec3 target = camera.Target;
    const Vec3 offset = target - state.EyePosition;
    if (offset.LengthSquared() <= 1e-12f)
    {
        target = state.EyePosition + Vec3(0.0f, 0.0f, -1.0f);
    }

    const Vec3 forward = state.EyePosition - target;
    const Vec3 up = ResolveUpVector(camera, forward);

    state.View = Mat4::LookAtRH(state.EyePosition, target, up);

    if (camera.Projection == CameraComponent::ProjectionType::Perspective)
    {
        // Near/far are validated rather than trusted: the component is public data written by scripting, the editor
        // and deserialisation, and PerspectiveRH asserts on far <= near > 0. A scene file with a typo should not abort
        // the engine, so an invalid pair falls back to sane defaults (and is therefore visibly wrong rather than
        // subtly wrong).
        const bool projectionValid = camera.NearPlane > 0.0f && camera.FarPlane > camera.NearPlane &&
                                     std::isfinite(camera.NearPlane) && std::isfinite(camera.FarPlane);

        const Float32 nearPlane = projectionValid ? camera.NearPlane : 0.1f;
        const Float32 farPlane = projectionValid ? camera.FarPlane : 1000.0f;

        // FOV is clamped away from both ends: at 0 the cotangent diverges, and at pi the projection inverts. Both are
        // reachable from an editor slider or a scripted animation.
        const Float32 fieldOfView = std::isfinite(camera.VerticalFieldOfViewRadians)
                                        ? std::clamp(camera.VerticalFieldOfViewRadians, 0.01f, 3.1215927f)
                                        : 1.0471976f;

        state.Projection = Mat4::PerspectiveRH(fieldOfView, state.AspectRatio, nearPlane, farPlane);
    }
    else
    {
        // Orthographic width follows the aspect ratio from the same half-height so the scene does not stretch; height
        // is the authored dimension (2 * half height).
        const Float32 halfHeight = std::isfinite(camera.OrthographicHalfHeight) && camera.OrthographicHalfHeight > 0.0f
                                       ? camera.OrthographicHalfHeight
                                       : 5.0f;

        const bool planesValid = camera.NearPlane > 0.0f && camera.FarPlane > camera.NearPlane;
        const Float32 nearPlane = planesValid ? camera.NearPlane : 0.1f;
        const Float32 farPlane = planesValid ? camera.FarPlane : 1000.0f;

        state.Projection =
            Mat4::OrthographicRH(2.0f * halfHeight * state.AspectRatio, 2.0f * halfHeight, nearPlane, farPlane);
    }

    state.ViewProjection = state.Projection * state.View;
    state.InverseViewProjection = state.ViewProjection.Inverse();
    state.Frustum = Math::Frustum::FromViewProjection(state.ViewProjection);

    return state;
}

CameraState ComputeCameraState(const CameraComponent& camera, Float32 aspectRatio)
{
    return ComputeCameraState(camera, nullptr, aspectRatio);
}

}  // namespace Hollowpoint::Renderer
