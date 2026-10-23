#pragma once

#include <string>
#include <vector>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Renderer/Geometry.h"
#include "Hollowpoint/Renderer/Lights.h"
#include "Hollowpoint/Renderer/PbrShading.h"

// ---------------------------------------------------------------------------
// Forward render pass: per-draw and per-frame data, the generated shader source, and the CPU reference that defines
// what the GPU must produce for the same inputs.
//
// This is M3's core: a cube, lit by the engine's lights, shaded by the engine's BRDF, written to an HDR target — and
// verified numerically against CPU code rather than by looking at it. "Looks right" cannot be regression-tested;
// agreement with EvaluateDirectLight can, and it catches errors that read as art notes (a cosine applied twice, a
// flipped normal, attenuation multiplied in twice).
//
// Why the shader is generated rather than checked-in .vert/.frag files: the shading model already lives in
// BuildShadingModelGlsl(), emitted from the same constants the CPU reference uses. Hand-writing lighting GLSL next
// to it reintroduces exactly the drift that generation was created to prevent. This pass contributes the transform
// and the light loop; every shading expression is a call to an Hp* function.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Renderer
{

using Math::Float32;
using Math::Mat3;
using Math::Mat4;
using Math::UInt32;
using Math::Vec3;

//! A light plus where it is in the world.
//!
//! LightComponent deliberately carries no position — like every other component, an entity's Transform says where it
//! is (see Lights.h). The forward pass needs positions to compute distances and spot axes, so collection pairs the
//! two up here rather than adding a field that would duplicate Transform and drift from it.
struct ForwardLight
{
    LightComponent Light;

    //! World-space position for Point/Spot lights; ignored for Directional (a light at infinity has no parallax).
    Vec3 WorldPosition{};

    //! World-space direction the light points, for Directional and Spot. Copied from the component after any
    //! rotation-from-Transform is applied by the collector, so this layer never re-derives it from a matrix.
    Vec3 WorldDirection = Vec3(0.0f, -1.0f, 0.0f);
};

//! One instance submitted to the forward pass. Plain data, no GPU handles: the renderer that consumes these owns
//! buffer upload, so this stays testable (and usable by a CPU-only tool) without an RHI.
struct ForwardDrawItem
{
    //! Model matrix (local → world), column-major as Mat4 stores it. Composed from an entity's Transform before it
    //! gets here; this layer takes no opinion on where matrices come from, only that the shader applies
    //! gl_Position = Projection * View * Model * position.
    Mat4 ModelMatrix = Mat4::Identity();

    PbrMaterialParameters Material{};

    //! Indices into ForwardPassParams::Lights. Indices rather than a copy: the light list is uploaded once per frame
    //! and shared by every draw, so a per-draw copy would be both bandwidth and a chance for the two to disagree.
    std::vector<UInt32> LightIndices;

    //! Vertex/index data. Pointer, not ownership — geometry lives in an asset layer (later) or a static (now), and
    //! the renderer reads it during submission only.
    const GeometryData* Geometry = nullptr;  // not owned

    [[nodiscard]] bool IsDrawable() const noexcept
    {
        return Geometry != nullptr && !Geometry->IsEmpty();
    }
};

//! Frame-level parameters shared by all draws: camera and the light list.
struct ForwardPassParams
{
    Mat4 ViewProjection = Mat4::Identity();

    //! World-space eye point, passed separately rather than extracted from an inverse view matrix — deriving it here
    //! invites a sign error that reads as "specular on the wrong side", which is miserable to debug.
    Vec3 CameraPosition{};

    //! The light list. ForwardDrawItem::LightIndices indexes this array.
    std::vector<ForwardLight> Lights;
};

//! CPU reference: scene-linear HDR radiance at one point on a draw's surface, using that triangle's plane normal.
//!
//! Deliberately narrow — it evaluates lighting at a supplied position and does **not** rasterise or interpolate.
//! Rasterisation is Vulkan's job; reimplementing it here would mean testing against my own guess of coverage rules
//! instead of against the shading model actually in question. Flat faces make this exact: for an axis-aligned cube
//! viewed face-on, the plane normal *is* every fragment's interpolated normal on that face, so CPU and GPU agree to
//  rounding rather than approximately. Curved surfaces need per-vertex normal interpolation, which arrives with the
//  renderer pass that has real fragment positions; until then this contract is "flat faces only".
//!
//! Lighting sums over draw.LightIndices: per light, radiance from Evaluate{Point,Spot,Directional}LightRadiance
//! times the BRDF result from EvaluateDirectLight (which already carries one geometric cosine — see its contract;
//  applying a second here would quarter the brightness, and ForwardRenderTests pins that).
[[nodiscard]] HP_RENDERER_API Vec3 ComputeForwardShadingCpu(const ForwardPassParams& params,
                                                            const ForwardDrawItem& draw, const Vec3& worldPosition,
                                                            const Vec3& localGeometricNormal);

//! The same shading, given the surface NORMAL ALREADY IN WORLD SPACE — the CPU mirror of what the fragment shader
//! actually receives.
/**
 * Split out because a rasterised surface does not have a geometric normal, it has an INTERPOLATED one: the vertex
 * stage transforms per-vertex normals and the rasteriser blends them across the triangle. Passing a local geometric
 * normal through ComputeForwardShadingCpu can only ever describe a flat face (its documented contract), which is fine
 * for a cube but cannot describe a sphere — and, more to the point for tests, cannot verify that the GPU interpolated
 * what the CPU expected. This entry point takes the interpolated value directly so both sides evaluate identical
 * inputs, isolating interpolation from shading.
 *
 * `worldNormal` is normalised internally (see the comment in the definition) exactly as the shader does, so callers
 * hand over whatever the interpolation produced rather than pre-normalising it — pre-normalising on the CPU and
 * normalising again on neither side would differ in float rounding from a GPU that normalises once.
 */
[[nodiscard]] HP_RENDERER_API Vec3 ShadeWorldPointCpu(const ForwardPassParams& params, const ForwardDrawItem& draw,
                                                      const Vec3& worldPosition, const Vec3& worldNormal);

//! The normal matrix for a model basis: inverse transpose of a 3x3 transform, computed scale-robustly.
//!
//! This is what gets uploaded to a shader's NormalMatrix uniform. Exposed because the CPU reference and every GPU
//! submission need the SAME matrix — a test or renderer that inverts by hand (Mat3::Inverse().Transposed()) drifts
//! from this function the moment either changes, and the symptom is lighting that differs between the CPU check and
//! the rendered image. Derivation and scale-robustness reasoning: see CofactorMatrix in the .cpp; degeneracy handling:
//! HasUsableOrientation there too (a collapsed or non-finite basis asserts and returns identity — call
//! ComputeWorldNormal instead when a partial direction is worth salvaging).
[[nodiscard]] HP_RENDERER_API Mat3 ComputeNormalMatrix(const Mat3& modelBasis);

//! The world-space shading normal for a triangle's geometric normal under a model matrix.
//!
//! Normals transform by the inverse transpose of the upper-left 3x3 — not by the model matrix itself. For the
//  uniform-scale transforms the engine generates today the two coincide, which is precisely why doing it correctly
//  here matters: the bug would only appear on a non-uniformly-scaled object, far from here and looking like a
//  lighting problem. Asserts when the matrix has no inverse (a zero-scale axis means there is no surface).
[[nodiscard]] HP_RENDERER_API Vec3 ComputeWorldNormal(const Mat4& modelMatrix, const Vec3& localGeometricNormal);

// Both generators emit **body-only** GLSL: no `#version` line. The caller assembles in the order GLSL requires —
// `"#version 450\n"` + `Graphics::BuildGlslBindingPrelude()` + this text — because the binding macros must be defined
// before any block uses them and `#version` must precede everything. Both halves are mandatory: skipping the prelude
// leaves HP_BINDING_* undefined, which glslang rejects loudly (good), while a hand-written numeric binding instead
// reads all-zero uniforms silently (session 12) — hence the macros and this one documented order. Same convention as
// BuildShadingModelGlsl. (These notes sit above both declarations rather than attached to one: clang's
// -Wdocumentation rejects an @param naming a parameter the adjacent declaration does not have.)

//! GLSL for the forward vertex stage. See the assembly-order note above.
[[nodiscard]] HP_RENDERER_API std::string BuildForwardVertexGlsl();

//! GLSL for the forward fragment stage. See the assembly-order note above.
//!
//! @param maxLightsPerDraw  Fixed size of the light array in the uniform block. A runtime-sized array would need
//!         bindless infrastructure the engine does not have yet; a fixed bound also lets the test assert that every
//!         index a draw references exists in the frame's light list.
[[nodiscard]] HP_RENDERER_API std::string BuildForwardFragmentGlsl(UInt32 maxLightsPerDraw);

}  // namespace Hollowpoint::Renderer
