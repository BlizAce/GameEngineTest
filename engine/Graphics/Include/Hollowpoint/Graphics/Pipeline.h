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
class IBindingLayout;
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

    //! Winding that counts as front-facing. **Counter-clockwise**, matching glTF, gl and Vulkan's own default for a
    //! Y-down framebuffer — and the winding MakeCube/MakePlane emit (their triangles' geometric normal agrees with
    //! their stored vertex normal on all 12 cube triangles, measured).
    //!
    //! This field exists because NVRHI's RasterState defaults `frontCounterClockwise` to **false**, which asks Vulkan
    //! for a *clockwise* front face. Left at that default, back-face culling discards the faces pointing at the camera
    //! and keeps the ones pointing away — so the first mesh ever drawn with the engine's stated "back-face culling on"
    //! default rendered its own interior, shading as dot(N,V) ≈ −1 (measured: −0.9999 on a face-on cube, where +1 is
    //! correct). Disabling culling hid it, which is why every test written before this field passed: each one set
    //! TriangleCulling = None, so the winding question never came up. Same class as PipelineDesc's other explicit
    //! defaults above — a library default that reads as an obvious choice and isn't.
    bool IsFrontFaceCounterClockwise = true;

    bool IsDepthEnabled = true;
    CompareFunction DepthFunction = CompareFunction::LessEqual;
    bool IsDepthWriteEnabled = true;

    //! Descriptor slots this pipeline's shaders bind to, described in engine terms (no NVRHI types in public
    //  headers — see GraphicsApi.h). Create() builds a single binding layout from these at set 0 and attaches it.
    //!
    //! A shader that declares any uniform block or sampled texture REQUIRES its slots listed here. The Vulkan
    //  pipeline is built against the layout, and at draw time bindDescriptorSets dereferences it — with none attached
    //  that is a null-handle dereference and an immediate SIGSEGV inside the driver (measured in this suite's first
    //  run: the crash lands deep in libnvidia, with nothing naming the missing layout). Listing slots is therefore
    //  not optional bookkeeping; it is what makes drawing possible at all.
    std::vector<BindingSlotDesc> Bindings;

    //! Which stages may read the bindings above. Both stages must be listed if both declare the blocks — a slot
    //  invisible to a stage reads as zeros there, which is the same silent-uniform failure this header's binding
    //  convention exists to prevent. Two bools rather than a mask: this engine has exactly two raster stages today,
    //  and inventing a flag type for two users is speculation (compute arrival will add its own field with tests).
    bool BindingsVisibleToVertex = false;
    bool BindingsVisibleToPixel = true;
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

    //! The binding layout built from PipelineDesc::Bindings, required by createBindingSet — which dereferences its
    //  layout argument immediately (passing nullptr segfaults inside the Vulkan backend rather than requesting an
    //  inferred layout; verified against nvrhi-resource-bindings.cpp and by crashing on it).
    //
    //  Only the layout is exposed, not the set: a binding set holds per-draw resource handles that change every
    //  frame, while this layout is immutable pipeline state. Wrapping something that churns more than it is read
    //  buys nothing — the same reasoning behind leaving command recording to the documented escape hatch.
    //  Null for pipelines whose shaders bind nothing (Bindings empty).
    [[nodiscard]] nvrhi::IBindingLayout* GetBindingLayout() const noexcept;

protected:
    ~Pipeline() override;

private:
    Pipeline() = default;  // Create() only.

    // Holds the nvrhi pipeline/input-layout/shader handles. Defined in Src/RhiConversions.h (shared with no one
    // else today, but kept beside its siblings); pimpl rationale as in Buffer.h.
    PipelineImpl* m_Impl = nullptr;  //!< Owned. Null when invalid.
};

}  // namespace Hollowpoint::Graphics
