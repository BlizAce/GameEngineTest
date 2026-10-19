#pragma once

#include <string>

#include "Hollowpoint/Graphics/GraphicsApi.h"  // API macro + Core::UInt32 aliases, per module convention

// ---------------------------------------------------------------------------
// The engine's resource-binding convention, shared by C++ and GLSL.
//
// Why this exists: NVRHI's Vulkan backend assigns each descriptor a Vulkan binding number as `typeOffset + slot`,
// where the offset depends on the resource *type* (VulkanBindingOffsets). A shader that writes `layout(binding = 0)`
// for a constant buffer therefore points at nothing, and the failure mode is the worst kind available: the uniform
// reads all zeros with no validation error, no warning and no crash — indistinguishable from a shading bug. This
// machine has no Vulkan validation layer installed, so nothing would ever be reported. Finding that cost a debugging
// session; this header is what stops it costing another.
//
// The offsets are deliberately not flattened to zero: overlapping all four types breaks the moment a layout holds both
// a constant buffer and a texture at slot 0, which any real material does. Shaders use the generated macros and never
// write a raw binding number, so there is one place to change.
//
// The GLSL text is GENERATED from the constants below rather than hand-written alongside them, so C++ and shader
// cannot disagree by construction. Additionally, ShaderBindings.cpp static_asserts these values against NVRHI's own
// defaults: an upgrade that moves them fails the build loudly instead of silently zeroing every uniform at runtime.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Graphics
{

//! Vulkan binding-number offsets per resource type, mirroring NVRHI's VulkanBindingOffsets defaults.
namespace BindingOffsets
{

//! Texture/SRV-class resources (`Texture_SRV`, typed and structured buffer reads).
constexpr UInt32 ShaderResource = 0;

//! Samplers. Separate from SRVs so a texture and its sampler can share a slot index.
constexpr UInt32 Sampler = 128;

//! Constant (uniform) buffers. The one most often guessed wrong.
constexpr UInt32 ConstantBuffer = 256;

//! Read-write resources (`Texture_UAV`, structured buffer UAVs).
constexpr UInt32 UnorderedAccess = 384;

}  // namespace BindingOffsets

//! Vulkan binding number NVRHI assigns to a constant buffer at \p slot.
[[nodiscard]] constexpr UInt32 ConstantBufferBinding(UInt32 slot) noexcept
{
    return BindingOffsets::ConstantBuffer + slot;
}

//! Vulkan binding number for a sampled texture at \p slot.
[[nodiscard]] constexpr UInt32 TextureBinding(UInt32 slot) noexcept
{
    return BindingOffsets::ShaderResource + slot;
}

//! Vulkan binding number for a sampler at \p slot.
[[nodiscard]] constexpr UInt32 SamplerBinding(UInt32 slot) noexcept
{
    return BindingOffsets::Sampler + slot;
}

//! Vulkan binding number for an UAV at \p slot.
[[nodiscard]] constexpr UInt32 UnorderedAccessBinding(UInt32 slot) noexcept
{
    return BindingOffsets::UnorderedAccess + slot;
}

//! GLSL source defining `HP_BINDING_TEXTURE/SAMPLER/CONSTANT_BUFFER/UAV(slot)` macros.
//!
//! Generated from BindingOffsets, so it always agrees with the C++ side. Prepend to shader sources (before any
//! `#version`-sensitive content is not an issue: these are plain #defines and may follow the version directive).
[[nodiscard]] HP_GRAPHICS_API std::string BuildGlslBindingPrelude();

}  // namespace Hollowpoint::Graphics
