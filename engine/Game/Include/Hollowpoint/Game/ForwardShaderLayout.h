#pragma once

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Renderer/ForwardRender.h"

// ---------------------------------------------------------------------------
// The byte-for-byte contract between C++ constant-buffer uploads and the generated shaders.
//
// Why this is a header rather than a detail of SceneRenderer.cpp: the upload side and the shader side are the same
// contract written in two languages with no compile-time link between them. A mismatch is invisible — Vulkan reads
// whatever bytes sit at the offset, so a reordered field delivers metallic where roughness belongs and every pixel
// stays finite and plausible. The engine's one defence is a test that parses the generated GLSL and compares it against
// these structs, and that test is only worth anything if **production writes through these same types**. When the
// layouts lived privately inside SceneRenderer.cpp and separately inside tests/Renderer/TestSceneToPixel.cpp, the test
// pinned its own copies to the shader while production held a third description that could drift from both and fail
// nothing. Single sourcing here closes that gap: one definition, used by the writer, asserted against the shader.
//
// std140 rules relied on (the reason sizes are not "whatever C++ produces"):
//   * a mat4 occupies 64 bytes at alignment 16 — three columns of vec4 plus a discarded fourth;
//   * a vec4 is 16 bytes; an array of vec4 is 16-aligned with each element taking a full 16;
//   * a scalar after an array starts a new 16-byte unit, so LightCount does NOT sit at the end of the index array;
//   * a uniform buffer's size is rounded up to its alignment, so every block here is padded to a multiple of 16.
//   Creating
//     a buffer at exactly sizeof(float) leaves the shader reading a whole unit past the end — which on most drivers
//     reads zero: exposure 0, black frame, nothing naming the cause.
// The static_asserts below are therefore load-bearing rather than tidy: they fail the build when a member is added or a
// math type's storage changes, which is precisely when an offset would otherwise move in silence.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Game
{

using Math::Mat3;
using Math::Mat4;

//! FrameParams (constant-buffer slot 0): mat4 ViewProjection; vec4 CameraPositionAndPad.
struct alignas(16) HP_GAME_API ForwardFrameBlock
{
    float ViewProjection[16] = {};
    float CameraPositionAndPad[4] = {};
};

//! DrawTransform (slot 1): mat4 ModelMatrix; mat4 NormalMatrix.
struct alignas(16) HP_GAME_API ForwardTransformBlock
{
    float ModelMatrix[16] = {};
    float NormalMatrix[16] = {};
};

//! DrawMaterial (slot 3): vec4 BaseColor; vec4 MaterialParams (x roughness, y metallic).
struct alignas(16) HP_GAME_API ForwardMaterialBlock
{
    float BaseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float MaterialParams[4] = {};
};

//! ExposureBlock for the tonemap blit: a lone float, which std140 rounds up to a 16-byte block.
struct alignas(16) HP_GAME_API TonemapExposureBlock
{
    float Exposure = 1.0f;
    float Pad[3] = {};
};

// Sizes are the GLSL blocks' sizes. Expressed exactly (not as ">= n") so a change fails loudly here rather than
// shifting a later uniform by a few bytes and producing plausible garbage on screen.
static_assert(sizeof(ForwardFrameBlock) == 80u, "FrameParams is mat4 + vec4 = 80 bytes");
static_assert(sizeof(ForwardTransformBlock) == 128u, "DrawTransform is two mat4s");
static_assert(sizeof(ForwardMaterialBlock) == 32u, "DrawMaterial is two vec4s");
static_assert(sizeof(TonemapExposureBlock) == 16u, "std140 rounds a lone float up to 16 bytes");

namespace ShaderLayout
{

[[nodiscard]] constexpr Core::Size AlignUp(Core::Size value, Core::Size alignment) noexcept
{
    return (value + alignment - 1u) / alignment * alignment;
}

//! LightArray (slot 2) size for a runtime light bound: four vec4 per light and nothing else in the block. The field
//! order is PositionAndIntensity, ColorTypeAndRange, RangeAndSpotAngles, DirectionAndPad — each field's lights
//! contiguous (SoA), which is what lets the upload walk one field at a time.
[[nodiscard]] constexpr Core::Size LightBlockBytes(Core::UInt32 maxLights) noexcept
{
    return static_cast<Core::Size>(maxLights) * 4u * 16u;
}

//! Byte offset of light `index`'s `field` (0..3) within the light block. One function so the writer and any test that
//! decodes the same buffer agree by construction rather than by each computing strides.
[[nodiscard]] constexpr Core::Size LightFieldOffset(Core::UInt32 maxLights, Core::UInt32 field,
                                                    Core::UInt32 index) noexcept
{
    return (static_cast<Core::Size>(field) * maxLights + index) * 16u;
}

//! DrawLightIndices (slot 4) size: ivec4 LightIndices[ceil(N/4)], then int LightCount at the next 16-byte unit.
[[nodiscard]] constexpr Core::Size IndexBlockBytes(Core::UInt32 maxLights) noexcept
{
    const Core::Size vectors = (maxLights + 3u) / 4u;
    return AlignUp(vectors * 16u + 4u, 16u);
}

//! Byte offset of LightCount within the index block — after the vector array's own 16-byte unit, not appended to it.
[[nodiscard]] constexpr Core::Size IndexBlockCountOffset(Core::UInt32 maxLights) noexcept
{
    return ((maxLights + 3u) / 4u) * 16u;
}

//! Column-major copy into a GLSL `mat4`'s storage. Mat4 already stores column-major (pinned by MathCoreTests), so this
//! is a straight element walk — written explicitly rather than memcpy'd so the intent survives a Mat4 layout change.
inline void StoreMat4(float (&destination)[16], const Mat4& source) noexcept
{
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            destination[column * 4 + row] = source[column][row];
        }
    }
}

//! The engine's light type as the generated shader decodes it: 0 directional, 1 point, 2 spot.
/**
 * This mapping was, until it moved here, stated only inside a test file: ForwardRender.cpp writes the field and the
 * generated GLSL compares raw integers, with no shared constant between them — so inserting an enumerator into
 * LightComponent::LightType silently reinterprets every light in every scene (a point light shading as directional,
 * with no error anywhere). Encoding lives here now, as the one production writer, and ForwardShaderLayoutTests pins
 * both halves: this function's outputs and the literal comparisons the generated shader emits.
 */
[[nodiscard]] HP_GAME_API float EncodeLightType(Renderer::LightComponent::LightType type) noexcept;

}  // namespace ShaderLayout

}  // namespace Hollowpoint::Game
