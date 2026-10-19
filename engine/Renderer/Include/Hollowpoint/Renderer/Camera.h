#pragma once

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Math/Angles.h"
#include "Hollowpoint/Math/Frustum.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Math/Transform.h"

// ---------------------------------------------------------------------------
// Camera — the render request: where the eye is, what it sees, and how that maps to the screen.
//
// Deliberately a plain component with no rendering dependencies. The renderer reads it; nothing in it knows about
// devices, swapchains or passes. That keeps scene loading, scripting and editor tools able to create and drive cameras
// without linking Graphics, and keeps the projection math testable on its own.
//
// Conventions (stated because getting them wrong is invisible for a surprising length of time):
//   * Right-handed, with the Vulkan NDC depth range [0,1] — matching Math::PerspectiveRH, which already encodes that.
//     A GL-style [-1,1] projection here would not fail loudly; it would just reject half the depth range in the
//     comparison and produce z-fighting that appears resolution-dependent.
//   * Field of view is VERTICAL and in radians. Vertical because aspect ratio varies and a horizontal FOV makes the
//     vertical extent change with window shape — which is the wrong axis to keep fixed for a human face at 16:9 vs 4:3.
//   * Near/far are distances along -Z (positive values), not clip-plane Z coordinates.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Renderer
{

using Math::Float32;
using Math::Mat4;
using Math::Vec3;

//! A perspective or orthographic camera. Exactly one mode is active; the unused parameters are still stored (rather
//! than kept in a union) so switching modes in the editor does not discard author data.
struct HP_RENDERER_API CameraComponent
{
    enum class ProjectionType : unsigned char
    {
        Perspective,
        Orthographic,
    };

    //! World-space eye position. Kept on the component as well as reachable via LocalTransform so a camera can be
    //! driven procedurally (scripted flythrough, physics follow) without touching the transform hierarchy — at the
    //! cost of one precedence rule, documented on GetEffectivePosition.
    Vec3 Position = Vec3(0.0f, 0.0f, 1.0f);

    //! Where the camera looks, in world space. A target rather than an angle triple because "look at this thing" is
    //! what callers (editor focus, scripting API, follow logic) actually have, and it makes tracking a moving object a
    //! single assignment instead of an angle solve.
    Vec3 Target = Vec3(0.0f, 0.0f, 0.0f);

    //! Up vector used to resolve the roll ambiguity of Position/Target. Must not be parallel to the view direction;
    //! see GetView for how that degenerate case is handled.
    Vec3 Up = Vec3(0.0f, 1.0f, 0.0f);

    ProjectionType Projection = ProjectionType::Perspective;

    //! Vertical field of view in radians. Default ~60 degrees: the conventional "normal lens". Wider values (75-90)
    //! are common for first-person but distort edges; narrower is for telescopic/screenshot use.
    Float32 VerticalFieldOfViewRadians = 1.0471976f;

    //! Orthographic half-height in world units (the perspective counterpart is derived from distance to Target).
    Float32 OrthographicHalfHeight = 5.0f;

    //! Distance to the near clip plane, along -Z. Must be > 0 and < FarPlane.
    //!
    //! Not smaller by default: depth precision is spent logarithmically in a standard perspective projection, so an
    //! unnecessarily tiny near plane costs precision across the entire range and shows up as z-fighting on distant
    //! geometry — the classic "far plane too close to zero" bug. Scenes needing more range should get it from a larger
    //! far plane plus (later) logarithmic or reversed-depth projection, not by shrinking this.
    Float32 NearPlane = 0.1f;

    //! Distance to the far clip plane. See NearPlane for the precision trade-off.
    Float32 FarPlane = 1000.0f;

    //! Exposure multiplier applied in linear space before tone mapping (see ColorPipeline::LinearToDisplay). Kept here
    //! rather than in a post-process component because it is a property of the eye/lens, and because per-camera
    //! exposure is what an auto-exposure pass would later write.
    Float32 Exposure = 1.0f;

    //! When false this camera is skipped by view selection but keeps its state — so switching cameras in the editor
    //! does not lose framing.
    bool Active = true;
};

//! Computed per-frame camera state: the matrices and frustum a renderer consumes.
//!
//! A value type produced on demand rather than cached on the component, for the same reason LocalTransform avoids a
//! cached world transform: caching creates a second source of truth whose staleness depends on every writer
//! remembering to invalidate. Recomputing four matrix multiplies per camera per frame is not a cost worth risking
//! that class of bug for.
struct HP_RENDERER_API CameraState
{
    Mat4 View = Mat4::Identity();
    Mat4 Projection = Mat4::Identity();
    Mat4 ViewProjection = Mat4::Identity();

    //! Inverse view-projection, provided because unprojection (mouse picking, reconstructing world position from a
    //! depth buffer) needs it and inverting per use is easy to get wrong.
    Mat4 InverseViewProjection = Mat4::Identity();

    Math::Frustum Frustum;

    //! Effective eye position after any transform-hierarchy contribution.
    Vec3 EyePosition = Vec3(0.0f);

    //! Aspect ratio actually used to build Projection, exposed so the renderer can detect that a resize requires a
    //! rebuild rather than comparing against its own window size.
    Float32 AspectRatio = 1.0f;
};

//! Builds the per-frame camera state from a component plus an explicit world transform.
//!
//! \param camera The camera component.
//! \param worldTransform World-space transform of the camera's entity, or nullptr to use only CameraComponent::Position
//!        and Target. When supplied, its translation is used as the eye position (the hierarchy owns placement), while
//!        Target still determines orientation — matching how an editor "look at selection" camera behaves.
//! \param aspectRatio Viewport width / height. Values <= 0 fall back to 1 rather than asserting: a zero-size viewport
//!        during window minimise is normal, and producing a degenerate-but-finite matrix lets the frame be skipped
//!        instead of taking the process down.
[[nodiscard]] HP_RENDERER_API CameraState ComputeCameraState(const CameraComponent& camera,
                                                             const Math::Transform* worldTransform,
                                                             Float32 aspectRatio);

//! Convenience overload using only the component's own Position/Target (no transform hierarchy).
[[nodiscard]] HP_RENDERER_API CameraState ComputeCameraState(const CameraComponent& camera, Float32 aspectRatio);

}  // namespace Hollowpoint::Renderer
