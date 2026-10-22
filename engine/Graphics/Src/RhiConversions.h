#pragma once

// Internal to the Graphics module: the single point where engine resource types meet NVRHI's.
//
// Engine public headers include neither Vulkan nor NVRHI (GraphicsApi.h explains why). Every conversion and every
// pimpl therefore has to live somewhere central — scattering them through call sites recreates exactly the drift
// that separation prevents (especially for Format: assuming engine enum values match nvrhi::Format numerically is
// silently wrong the moment either list changes). The switches below are exhaustive; adding a value to
// GraphicsTypes.h without extending its switch fails at compile time, not as a wrong format at runtime.
//
// Nothing outside Src/ may include this file; it is not installed and CMake does not export it.

#include "Hollowpoint/Graphics/Buffer.h"
#include "Hollowpoint/Graphics/GraphicsTypes.h"
#include "Hollowpoint/Graphics/RenderTarget.h"
#include "Hollowpoint/Graphics/RhiAccess.h"
#include "Hollowpoint/Graphics/Texture.h"

namespace Hollowpoint::Graphics
{

// The pimpl types behind the resource wrappers, defined once here so sibling translation units (Pipeline reading a
// RenderTarget's info, say) can reach native handles without widening public API for one internal caller. Each
// wrapper's .cpp includes this file and constructs its own impl; these definitions are the shared layout contract.

class BufferImpl
{
public:
    nvrhi::BufferHandle Handle;
};

class TextureImpl
{
public:
    nvrhi::TextureHandle Handle;
};

class RenderTargetImpl
{
public:
    nvrhi::FramebufferHandle Framebuffer;

    // Built from the same attachments as Framebuffer — which is the entire reason this class exists (see
    // RenderTarget.h): a pipeline created against anything else is an unreportable mismatch.
    nvrhi::FramebufferInfo Info;

    //! Keeping these alive through the framebuffer's lifetime means a caller dropping its own Ref<Texture> cannot
    //  strand a framebuffer pointing at freed attachments.
    Core::Ref<Texture> ColourAttachment;
    Core::Ref<Texture> DepthAttachment;
};

class PipelineImpl
{
public:
    nvrhi::GraphicsPipelineHandle Pipeline;
    nvrhi::InputLayoutHandle InputLayout;
    nvrhi::ShaderHandle VertexShaderModule;
    nvrhi::ShaderHandle PixelShaderModule;
};

namespace Detail
{

[[nodiscard]] nvrhi::Format ToNvrhi(GraphicsFormat format) noexcept;

[[nodiscard]] nvrhi::CpuAccessMode ToNvrhi(CpuAccess access) noexcept;

[[nodiscard]] constexpr bool IsDepthFormat(GraphicsFormat format) noexcept
{
    return format == GraphicsFormat::D32Float;
}

}  // namespace Detail

}  // namespace Hollowpoint::Graphics
