#pragma once

#include "Hollowpoint/Graphics/GraphicsApi.h"  // API macro + Core integer aliases, per module convention

// ---------------------------------------------------------------------------
// Engine-level value types for GPU resources.
//
// These mirror — but never alias — the corresponding NVRHI types. The rule this module lives by is that public
// headers include neither Vulkan nor NVRHI (see GraphicsApi.h): including either would leak the RHI's vocabulary
// into every consumer and slow every dependent translation unit by megabytes of preprocessor state. The mapping
// functions that translate these to nvrhi:: types live in Src/ and are the only place the two vocabularies meet.
//
// When a value here has no engine-level meaning yet (say, an exotic blend factor), do not add it "because NVRHI
// has it" — add what the renderer actually asks for, when it asks for it. Every enum member here is a decision
// the engine has made; members copied from upstream on speculation are decisions nobody reviewed.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Graphics
{

//! Pixel formats the engine creates resources with. Values map 1:1 onto nvrhi::Format in Src/; the numeric values
//! here are engine-stable and must not be assumed to equal NVRHI's.
enum class GraphicsFormat : UInt8
{
    Unknown,
    Rgba8Unorm,   //!< Standard LDR colour (framebuffers, RGBA8 targets).
    Bgra8Unorm,   //!< Some swapchains prefer BGRA ordering; chosen per-surface during swapchain creation.
    Rgba16Float,  //!< HDR scene colour — the format every numeric shading test reads back through.
    Rg16Float,    //!< Two-channel HDR (velocity buffers, later SSAO's motion pair).
    R16Float,     //!< Single-channel HDR (luminance chains for auto-exposure).
    D32Float,     //!< Depth buffer, float — the engine default; see DepthStencil note on why not D24.
    R8Unorm,      //!< Masks, AO factors, single-byte data views.
    Rgba8Srgb,    //!< sRGB-encoded RGBA for sampled colour textures (hardware-decoded on sample).
    Rgba32Float,  //!< Full-precision HDR. Numeric CPU/GPU comparison tests use this because half-float storage adds
                  //!< ~1e-3 of its own error, forcing tolerances loose enough to hide real shading differences; the
                  //!< half-float path itself is measured by TestHdrRenderTarget.

    // Vertex-attribute formats. Separate from the render-target list above because GLSL `in vecN` requires the
    // matching 32-bit type — feeding an Rg16Float attribute to a `vec2` input reads garbage, silently.
    Rg32Float,   //!< vec2 vertex attributes (positions of full-screen quads, 2D sprites).
    Rgb32Float,  //!< vec3 vertex attributes (normals, colours) — what GLSL `in vec3` expects.
};

//! How a buffer is used, which drives both the RHI flags and its initial resource state. Composable bit flags:
//! a vertex buffer that is also shader-readable sets both.
enum class BufferUsage : UInt8
{
    None = 0,
    Vertex = 1 << 0,
    Index = 1 << 1,
    Constant = 1 << 2,
    ShaderResource = 1 << 3,
    Staging = 1 << 4,  //!< CPU-visible; combined with CpuAccess below rather than implied.
};

[[nodiscard]] inline constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) noexcept
{
    return static_cast<BufferUsage>(static_cast<UInt8>(a) | static_cast<UInt8>(b));
}

[[nodiscard]] inline constexpr bool HasFlag(BufferUsage value, BufferUsage flag) noexcept
{
    return (static_cast<UInt8>(value) & static_cast<UInt8>(flag)) != 0;
}

//! Which direction data flows between CPU and GPU for a resource. Maps to nvrhi::CpuAccessMode.
enum class CpuAccess : UInt8
{
    None,   //!< Default: device-local only.
    Read,   //!< CPU-visible readback staging.
    Write,  //!< CPU-visible upload staging (the engine's writeBuffer path uses command-list writes instead).
};

//! Primitive topology for a graphics pipeline. The renderer draws TriangleList everywhere today; the others exist
//! because debug overlays (lines) and point splats are planned, not for parity with NVRHI's list.
enum class PrimitiveTopology : UInt8
{
    TriangleList,
    LineList,
    PointList,
};

//! Triangle culling mode. Front-face convention is framebuffer-dependent under Vulkan's Y-down viewport space —
//! tests that care state it explicitly rather than trusting a default (see the draw test's note).
enum class CullMode : UInt8
{
    None,
    Back,
    Front,
};

//! Colour-target alpha blending. A property of the pipeline like depth state, deliberately not left for call sites to
//! hand-assemble through the RHI escape hatch: that is how one pass ends up premultiplied and the next straight alpha,
//! with no shader at fault and a composite that looks almost right.
//!
//! Two values rather than NVRHI's full factor/op matrix because every member of a public API is a decision this project
//! owns (see this header's file comment) and only these two have users: opaque writes, and compositing UI over a
//! scene. An additive glow pass adds its enumerator when it exists.
enum class BlendMode : UInt8
{
    Disabled,  //!< `out = src`. The forward pass; the engine's historical behaviour.
    Alpha,     //!< `out = src*srcAlpha + dst*(1-srcAlpha)` — non-premultiplied compositing, i.e. ImGui's output.
};

//! Depth comparison for the depth-stencil state.
enum class CompareFunction : UInt8
{
    Never,
    Less,
    Equal,
    LessEqual,  //!< Engine default for standard forward rendering with a [0,1] clip range.
    Greater,
    NotEqual,
    GreaterEqual,
    Always,
};

//! One vertex attribute of an input layout. Offsets and strides are in bytes.
//!
//! Attributes bind to the vertex shader **by name**: `createInputLayout` takes the compiled vertex shader and
//! matches each entry's `Name` against its reflected inputs (verified working in the draw test, where "InPosition"
//!/"InColor" match the GLSL declarations). There is no location index in this path — do not add one expecting it to
//! steer the binding.
//!
//! The stride rule is load-bearing and measured, not folklore: NVRHI defaults elementStride to 0, which makes every
//! vertex read element zero — a degenerate mesh that rasterises nothing with no error anywhere (session 12). All
//! strides are set explicitly here, and Pipeline asserts they agree across attributes sharing the same slot.
struct VertexAttribute
{
    const char* Name = "";  //!< Must match the GLSL input variable name; this is the binding mechanism, not a label.
    GraphicsFormat Format = GraphicsFormat::Unknown;
    UInt32 OffsetBytes = 0;
    UInt32 ElementStrideBytes = 0;
};

//! One descriptor slot in a pipeline's binding layout, described without RHI types.
//!
//! The engine declares what a shader binds (a constant buffer at slot N, a sampled texture at M) and Pipeline::Create
//  turns these into the RHI's layout object. That indirection exists because public headers stay free of Vulkan/NVRHI
//  (GraphicsApi.h), and because it gives every pipeline one place to state its interface — which is what makes a
//  "shader expects slot X but C++ bound Y" mismatch reviewable instead of invisible.
struct BindingSlotDesc
{
    enum class Kind : UInt8
    {
        ConstantBuffer,
        SampledTexture,
        Sampler,
    };

    Kind Type = Kind::ConstantBuffer;

    //! Slot index within the layout — the same number passed to HP_BINDING_CONSTANT_BUFFER / TEXTURE / SAMPLER in
    //  GLSL. The Vulkan binding *number* differs per type (see ShaderBindings.h: offsets 0/128/256/384); NVRHI adds
    //  those, so callers use this slot number on both sides and never compute an offset themselves.
    UInt32 Slot = 0;
};

}  // namespace Hollowpoint::Graphics
