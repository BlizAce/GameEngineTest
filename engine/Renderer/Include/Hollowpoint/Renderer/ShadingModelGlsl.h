#pragma once

#include <string>

#include "Hollowpoint/Core/API.h"

// ---------------------------------------------------------------------------
// Generates the GLSL implementation of the shading model from the same constants the CPU reference uses.
//
// This is what makes CPU/GPU equivalence a *test* rather than a hope. The alternative — typing the GGX/ACES formulas
// into a GLSL string by hand — means the two implementations can differ in any constant, and the failure appears as an
// image that "looks slightly wrong" or an image test nobody can explain. Generating the shader text from
// ShadingConstants.h removes that entire class of divergence: what remains to test is arithmetic and ordering, which is
// exactly what ShadingModelEquivalenceTests measures.
//
// The generated code is a header prepended to engine shaders. It is intentionally plain GLSL 450 with no engine types:
// the same text is compiled by glslang at load time and could equally be baked, so nothing here depends on the RHI.
//
// Maintenance rule: any constant used in this source must come from ShadingConstants.h / AceFilmicCoefficients. A
// literal typed into the string below is a bug waiting to drift — ShadingModelGlslTests greps for suspicious literals
// as a backstop (see that file).
// ---------------------------------------------------------------------------

namespace Hollowpoint::Renderer
{

//! GLSL 450 source defining the engine's shading functions:
//!   `float HpPerceptualRoughnessToAlpha(float)`
//!   `float HpDistributionGGX(vec3, vec3, float)`
//!   `float HpGeometrySchlickGGX(float, float)`
//!   `float HpGeometrySmith(vec3, vec3, vec3, float)`
//!   `vec3  HpFresnelSchlick(float, vec3)`
//!   `float HpDistanceAttenuation(float, float)`
//!   `float HpSpotAngleAttenuation(float, float, float)`
//!   `vec3  HpToneMapAceFilmic(vec3)`
//!   `vec3  HpLinearToSrgb(vec3)`
//! and `HpShadeDirect(...)` combining them into the diffuse+specular result.
//!
//! Pure function of the shared constants — no I/O, no global state — so it can be called per shader compile without
//! caching concerns (glslang's own parse dominates the cost).
[[nodiscard]] HP_RENDERER_API std::string BuildShadingModelGlsl();

}  // namespace Hollowpoint::Renderer
