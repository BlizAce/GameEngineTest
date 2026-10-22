#pragma once

#include <string>
#include <vector>

#include "Hollowpoint/Core/RefCounted.h"
#include "Hollowpoint/Graphics/GraphicsApi.h"
#include "Hollowpoint/Graphics/GraphicsTypes.h"

// Forward declaration only; see Buffer.h for the rationale.
namespace nvrhi
{
class IGraphicsPipeline;
}  // namespace nvrhi

// ---------------------------------------------------------------------------
// A graphics pipeline state: shaders + vertex layout + fixed-function state, bound to one render target's format.
//
// NVRHI is a *state-based* API in this revision: there is no begin/end render pass. The pipeline object is created
// against a FramebufferInfo (the attachment formats) and everything else — framebuffer object, viewport, vertex
// buffers, bindings — is assembled into a GraphicsState immediately before draw(). This class owns the half that is
// expensive and immutable (shader modules, input layout, pipeline creation); callers assemble the cheap per-draw
// half themselves through RhiAccess.h.
//
// The pairing rule (a pipeline must match its framebuffer's formats exactly) is enforced by construction: Create
// takes a RenderTarget and reads its info internally. That is why no Pipeline overload accepts a raw format list —
// the drift it would allow caused real debugging time before this existed.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Graphics
{

class GraphicsDevice;
class PipelineImpl;
class RenderTarget;

//! One compiled shader stage handed to Pipeline::Create: SPIR-V words plus what NVRHI needs to wrap them.
struct ShaderProgram
{
    //! SPIR-V words, e.g. from CompileGlsl().CompiledShader::SpirV. Empty is invalid and asserted.
    std::vector<UInt32> SpirV;

    std::string Name;  //!< Debug name for the shader module; shows in pipeline dumps and validation messages.
};

//! Creation parameters. Fixed-function defaults match forward rendering: back-face culling on, depth tested
//! LessEqual and writable — stated explicitly rather than inherited from library defaults so a NVRHI upgrade
//  cannot silently change what "the same pipeline" means (session 12 learned this with elementStride's default).
struct PipelineDesc
{
    std::string Name;

    ShaderProgram VertexShader;
    ShaderProgram PixelShader;

    //! Vertex attributes matched to the vertex shader **by name** (see VertexAttribute). May be empty for shaders
    //! that read no vertex inputs (full-screen blits synthesize positions from vertex ID).
    std::vector<VertexAttribute> VertexAttributes;

    PrimitiveTopology Topology = PrimitiveTopology::TriangleList;

    //! Named TriangleCulling rather than CullMode to avoid shadowing the CullMode enum type inside this struct —
    //! a collision GCC rejects outright (-Wchanges-meaning), which is cheaper than discovering it in a header.
    CullMode TriangleCulling = CullMode::Back;

    bool IsDepthEnabled = true;
    CompareFunction DepthFunction = CompareFunction::LessEqual;
    bool IsDepthWriteEnabled = true;
};

//! An immutable graphics pipeline. Thread-safe for concurrent use from recording threads (immutable after create).
class HP_GRAPHICS_API Pipeline : public Core::RefCounted
{
public:
    //! Creates the pipeline against `target`'s attachment formats. Returns nullptr on failure with the reason —
    //! including shader compile-module failures, which are routine authoring errors and must not assert.
    [[nodiscard]] static Core::Ref<Pipeline> Create(GraphicsDevice& device, const PipelineDesc& desc,
                                                    const RenderTarget& target);

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    [[nodiscard]] bool IsValid() const noexcept;

    //! The NVRHI pipeline for GraphicsState::setPipeline. Never null on an instance that exists.
    [[nodiscard]] nvrhi::IGraphicsPipeline* GetNativePipeline() const noexcept;


protected:
    ~Pipeline() override;

private:
    Pipeline() = default;  // Create() only.

    // Holds the nvrhi pipeline/input-layout/shader handles. Defined in Src/RhiConversions.h (shared with no one
    // else today, but kept beside its siblings); pimpl rationale as in Buffer.h.
    PipelineImpl* m_Impl = nullptr;  //!< Owned. Null when invalid.
};

}  // namespace Hollowpoint::Graphics
