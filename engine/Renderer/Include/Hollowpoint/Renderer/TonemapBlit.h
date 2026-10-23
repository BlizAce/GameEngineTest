#pragma once

#include <string>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Math/MathCore.h"

// ---------------------------------------------------------------------------
// Tonemap blit: scene-linear HDR in, display-encoded colour out.
//
// The forward pass writes RGBA16_FLOAT scene radiance (values above 1 are meaningful — the sun is thousands of units).
// Nothing on screen can show that directly, so a final pass converts with the engine's one canonical order:
// exposure → ACES tone map → sRGB encode. That order lives in ColorPipeline.h's LinearToDisplay and in the generated
// shader's HpLinearToDisplay; this file is the third member of that pair, not a fourth implementation — its shader's
// entire colour maths is one call to the already-generated function, and its CPU reference calls the already-tested C++
// one. A test renders a known HDR ramp through the pass and compares against LinearToDisplay pixel by pixel, which is
// what stops the three from drifting in practice rather than in comments.
//
// Why a separate pass rather than tonemapping inside the forward pass: exposure, auto-exposure and any post chain
// operate on the whole frame after it exists, and doing it per-material would tone-map intermediate values that later
// passes still need linear. It also keeps the forward pass writing radiance, which is what every numeric shading test
// reads back — tonemapped output cannot be compared against a linear CPU reference at all.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Renderer
{

using Math::Float32;
using Math::UInt32;
using Math::Vec3;

//! CPU reference for the blit: display-encoded colour for one HDR texel.
/**
 * Exactly ColorPipeline's LinearToDisplay (exposure → ACES → sRGB), named so callers have one operation matching what
 * the shader does and the test can assert they agree instead of trusting a comment.
 *
 * \param hdrColor Scene-linear radiance; values above 1 are expected and normal (that is the point of HDR).
 * \param exposure Multiplicative exposure. Negative values behave as in ApplyExposure (clamped to black downstream),
 *        and NaN propagates — a NaN input is an upstream bug worth seeing rather than hiding.
 */
[[nodiscard]] HP_RENDERER_API Vec3 ComputeDisplayColourCpu(const Vec3& hdrColor, Float32 exposure) noexcept;

//! GLSL for the full-screen triangle that performs the blit.
/**
 * Body-only text (no `#version`): assemble as `"#version 450\n"` + Graphics::BuildGlslBindingPrelude() + this, the same
 * contract as the forward shaders — see ForwardRender.h.
 *
 * The vertex stage synthesises a triangle covering the viewport from gl_VertexIndex with no vertex buffer or index
 * buffer: three vertices, clip-space positions, UVs derived from them. A mesh would need a buffer upload, an input
 * layout and a binding for data that is a pure function of the vertex index — each one another way to configure a black
 * frame.
 *
 * The fragment stage samples the source texture and writes HpLinearToDisplay(sample.rgb, exposure). Sampling rather
 * than copying is deliberate: the same pass then rescales when a post chain renders below display resolution. The test
 * runs it 1:1 with nearest filtering, where sampling must be an exact no-op — so any filtering or offset error shows up
 * as a numeric mismatch instead of a subtly resampled image.
 */
[[nodiscard]] HP_RENDERER_API std::string BuildTonemapBlitVertexGlsl();
[[nodiscard]] HP_RENDERER_API std::string BuildTonemapBlitFragmentGlsl();

//! Binding slots used by the blit, shared between the generated GLSL and whatever binds it.
namespace TonemapBlitBindings
{
//! Sampled HDR scene colour — HP_BINDING_TEXTURE(SourceTexture).
constexpr UInt32 SourceTexture = 0;

//! Sampler for the source — HP_BINDING_SAMPLER(SourceSampler). Nearest filtering gives exact 1:1 copies; the slot
//! exists so a resampling variant can switch filters without changing layout.
constexpr UInt32 SourceSampler = 0;

//! Exposure constant buffer — HP_BINDING_CONSTANT_BUFFER(ExposureBuffer).
constexpr UInt32 ExposureBuffer = 0;
}  // namespace TonemapBlitBindings

}  // namespace Hollowpoint::Renderer
