#include "RhiConversions.h"  // PipelineImpl; also brings RhiAccess.h

#include <utility>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Graphics/GraphicsDevice.h"
#include "Hollowpoint/Graphics/Pipeline.h"
#include "Hollowpoint/Graphics/RenderTarget.h"

namespace Hollowpoint::Graphics
{

namespace
{

[[nodiscard]] nvrhi::ShaderHandle CreateShaderModule(nvrhi::IDevice* rhi, const ShaderProgram& program,
                                                     nvrhi::ShaderType type, std::string_view stageName)
{
    if (program.SpirV.empty())
    {
        // An empty SPIR-V blob means the caller skipped CompileGlsl or ignored its failure. That is a programmer
        // error at the call site — logging "pipeline failed" here would hide where the shader actually broke.
        HP_ASSERT_ARGUMENT(!program.SpirV.empty(), "Pipeline::Create requires compiled shader bytecode");
        return nullptr;
    }

    nvrhi::ShaderDesc shaderDesc;
    shaderDesc.shaderType = type;
    shaderDesc.debugName = program.Name;

    nvrhi::ShaderHandle module =
        rhi->createShader(shaderDesc, program.SpirV.data(), program.SpirV.size() * sizeof(UInt32));
    if (!static_cast<bool>(module))
    {
        // A plausible runtime failure: SPIR-V that glslang accepted but the driver's compiler rejects (an
        // unsupported capability, a too-complex loop). Logged with the stage named; caller gets nullptr.
        HP_LOG_ERROR("GRAPHICS", "createShader failed for {} stage \"{}\"", stageName, program.Name);
    }
    return module;
}

[[nodiscard]] nvrhi::PrimitiveType ToNvrhi(PrimitiveTopology topology) noexcept
{
    switch (topology)
    {
        case PrimitiveTopology::TriangleList:
            return nvrhi::PrimitiveType::TriangleList;
        case PrimitiveTopology::LineList:
            return nvrhi::PrimitiveType::LineList;
        case PrimitiveTopology::PointList:
            return nvrhi::PrimitiveType::PointList;
    }
    return nvrhi::PrimitiveType::TriangleList;
}

[[nodiscard]] nvrhi::RasterCullMode ToNvrhi(CullMode mode) noexcept
{
    switch (mode)
    {
        case CullMode::None:
            return nvrhi::RasterCullMode::None;
        case CullMode::Back:
            return nvrhi::RasterCullMode::Back;
        case CullMode::Front:
            return nvrhi::RasterCullMode::Front;
    }
    return nvrhi::RasterCullMode::None;
}

[[nodiscard]] nvrhi::ComparisonFunc ToNvrhi(CompareFunction function) noexcept
{
    // NVRHI spells this ComparisonFunc with a LessOrEqual member — the engine name stays idiomatic and the mapping
    // lives here, once. Exhaustive: a new CompareFunction fails the build until its mapping is decided.
    switch (function)
    {
        case CompareFunction::Never:
            return nvrhi::ComparisonFunc::Never;
        case CompareFunction::Less:
            return nvrhi::ComparisonFunc::Less;
        case CompareFunction::Equal:
            return nvrhi::ComparisonFunc::Equal;
        case CompareFunction::LessEqual:
            return nvrhi::ComparisonFunc::LessOrEqual;
        case CompareFunction::Greater:
            return nvrhi::ComparisonFunc::Greater;
        case CompareFunction::NotEqual:
            return nvrhi::ComparisonFunc::NotEqual;
        case CompareFunction::GreaterEqual:
            return nvrhi::ComparisonFunc::GreaterOrEqual;
        case CompareFunction::Always:
            return nvrhi::ComparisonFunc::Always;
    }
    return nvrhi::ComparisonFunc::Always;
}

}  // namespace

Core::Ref<Pipeline> Pipeline::Create(GraphicsDevice& device, const PipelineDesc& desc, const RenderTarget& target)
{
    nvrhi::IDevice* rhi = device.GetNativeDevice();
    if (rhi == nullptr)
    {
        HP_LOG_ERROR("GRAPHICS", "Pipeline::Create called with a device that has no RHI object");
        return nullptr;
    }

    nvrhi::ShaderHandle vertexModule = CreateShaderModule(rhi, desc.VertexShader, nvrhi::ShaderType::Vertex, "vertex");
    if (!static_cast<bool>(vertexModule))
    {
        return nullptr;
    }

    nvrhi::ShaderHandle pixelModule = CreateShaderModule(rhi, desc.PixelShader, nvrhi::ShaderType::Pixel, "pixel");
    if (!static_cast<bool>(pixelModule))
    {
        return nullptr;
    }

    // --- Input layout -------------------------------------------------------
    // elementStride is set explicitly for every attribute. NVRHI's default of 0 makes all vertices read element
    // zero: a degenerate mesh that rasterises nothing with no error anywhere (measured, session 12). Attributes
    // sharing a buffer slot must agree on the stride; the engine uses one vertex buffer per draw today, so one
    // stride for all — and the assert below keeps that true if a second slot ever appears.
    std::vector<nvrhi::VertexAttributeDesc> attributes;
    attributes.reserve(desc.VertexAttributes.size());

    for (const VertexAttribute& attribute : desc.VertexAttributes)
    {
        HP_ASSERT_ARGUMENT(attribute.ElementStrideBytes > 0,
                           "Pipeline vertex attribute requires an explicit non-zero ElementStrideBytes");

        nvrhi::VertexAttributeDesc nvrhiAttribute;
        nvrhiAttribute.setName(attribute.Name);
        nvrhiAttribute.setFormat(Detail::ToNvrhi(attribute.Format));
        nvrhiAttribute.setOffset(attribute.OffsetBytes);
        nvrhiAttribute.setElementStride(attribute.ElementStrideBytes);
        attributes.push_back(nvrhiAttribute);
    }

    nvrhi::InputLayoutHandle inputLayout = rhi->createInputLayout(
        attributes.empty() ? nullptr : attributes.data(), static_cast<uint32_t>(attributes.size()), vertexModule);
    if (!static_cast<bool>(inputLayout))
    {
        HP_LOG_ERROR("GRAPHICS", "createInputLayout failed for pipeline \"{}\"", desc.Name);
        return nullptr;
    }

    // --- Binding layout -------------------------------------------------------
    // Built from the engine-level slot descriptions and attached to the pipeline. A shader that declares uniform
    // blocks with no layout on the pipeline crashes at bindDescriptorSets time (null dereference inside the driver,
    // SIGSEGV with nothing naming the cause) — see PipelineDesc::Bindings for the full note. An empty Bindings list
    // is legal only for shaders that bind nothing (the triangle tests), which is why this is conditional rather than
    // always creating a layout: an empty-but-attached layout works too, but attaching one the shader does not declare
    // invites validation complaints about unused sets on some drivers.
    nvrhi::BindingLayoutHandle bindingLayout;
    if (!desc.Bindings.empty())
    {
        nvrhi::ShaderType visibility = nvrhi::ShaderType::None;
        if (desc.BindingsVisibleToVertex)
        {
            visibility = visibility | nvrhi::ShaderType::Vertex;
        }
        if (desc.BindingsVisibleToPixel)
        {
            visibility = visibility | nvrhi::ShaderType::Pixel;
        }

        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = visibility;

        for (const BindingSlotDesc& slot : desc.Bindings)
        {
            switch (slot.Type)
            {
                case BindingSlotDesc::Kind::ConstantBuffer:
                    layoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(slot.Slot));
                    break;
                case BindingSlotDesc::Kind::SampledTexture:
                    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(slot.Slot));
                    break;
                case BindingSlotDesc::Kind::Sampler:
                    layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(slot.Slot));
                    break;
            }
        }

        bindingLayout = rhi->createBindingLayout(layoutDesc);
        if (!static_cast<bool>(bindingLayout))
        {
            HP_LOG_ERROR("GRAPHICS", "createBindingLayout failed for pipeline \"{}\"", desc.Name);
            return nullptr;
        }
    }

    // --- Pipeline state -----------------------------------------------------
    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.setPrimType(ToNvrhi(desc.Topology))
        .setInputLayout(inputLayout)
        .setVertexShader(vertexModule)
        .setPixelShader(pixelModule);

    if (static_cast<bool>(bindingLayout))
    {
        pipelineDesc.addBindingLayout(bindingLayout);
    }

    nvrhi::RasterState raster;
    raster.cullMode = ToNvrhi(desc.TriangleCulling);
    // Set explicitly rather than inherited: NVRHI's default (false → clockwise front) is the opposite of the
    // counter-clockwise convention this engine's geometry and glTF both use, which silently inverts what culling keeps.
    // See PipelineDesc::IsFrontFaceCounterClockwise.
    raster.frontCounterClockwise = desc.IsFrontFaceCounterClockwise;

    nvrhi::DepthStencilState depth;
    depth.depthTestEnable = desc.IsDepthEnabled;
    depth.depthWriteEnable = desc.IsDepthWriteEnabled;
    depth.depthFunc = ToNvrhi(desc.DepthFunction);

    pipelineDesc.setRenderState(nvrhi::RenderState().setRasterState(raster).setDepthStencilState(depth));

    // The framebuffer info comes from the target itself (friend access, by design — see RenderTarget.h): a
    // pipeline whose attachment formats disagree with its framebuffer is undefined behaviour Vulkan may or may not
    // report, and this machine has no validation layer. Pairing them structurally removes the class of bug.
    const nvrhi::FramebufferInfo& framebufferInfo = target.GetFramebufferInfo();

    nvrhi::GraphicsPipelineHandle nativePipeline = rhi->createGraphicsPipeline(pipelineDesc, framebufferInfo);
    if (!static_cast<bool>(nativePipeline))
    {
        HP_LOG_ERROR("GRAPHICS", "createGraphicsPipeline failed for \"{}\"", desc.Name);
        return nullptr;
    }

    auto* impl = new PipelineImpl();
    impl->Pipeline = std::move(nativePipeline);
    impl->InputLayout = std::move(inputLayout);
    impl->BindingLayout = std::move(bindingLayout);
    impl->VertexShaderModule = std::move(vertexModule);
    impl->PixelShaderModule = std::move(pixelModule);

    Core::Ref<Pipeline> pipeline = Core::Ref<Pipeline>(new Pipeline());
    pipeline->m_Impl = impl;
    return pipeline;
}

Pipeline::~Pipeline()
{
    delete m_Impl;
    m_Impl = nullptr;
}

bool Pipeline::IsValid() const noexcept
{
    return m_Impl != nullptr && static_cast<bool>(m_Impl->Pipeline);
}

nvrhi::IGraphicsPipeline* Pipeline::GetNativePipeline() const noexcept
{
    return m_Impl != nullptr ? m_Impl->Pipeline : nullptr;
}

nvrhi::IBindingLayout* Pipeline::GetBindingLayout() const noexcept
{
    return m_Impl != nullptr ? m_Impl->BindingLayout : nullptr;
}

}  // namespace Hollowpoint::Graphics
