#include <string>

#include "Hollowpoint/Renderer/ColorPipeline.h"
#include "Hollowpoint/Renderer/ShadingModelGlsl.h"
#include "Hollowpoint/Renderer/TonemapBlit.h"

namespace Hollowpoint::Renderer
{

Vec3 ComputeDisplayColourCpu(const Vec3& hdrColor, Float32 exposure) noexcept
{
    // One line, deliberately: this exists so the GPU blit has a named CPU counterpart to be compared against, and any
    // logic added here would have to be mirrored in HpLinearToDisplay — the exact drift the pair prevents. If display
    // conversion grows (dithering, gamut clipping), it grows inside ColorPipeline::LinearToDisplay and both members of
    // this pair follow automatically.
    return LinearToDisplay(hdrColor, exposure);
}

std::string BuildTonemapBlitVertexGlsl()
{
    // Body-only text; see the header for the assembly-order contract.
    std::string glsl;

    // No vertex attributes at all: positions and UVs are functions of gl_VertexIndex. The consequence worth stating is
    // that PipelineDesc::VertexAttributes must be empty for this shader — an input layout declaring attributes the
    // shader does not have is rejected by createInputLayout, which at least fails loudly rather than silently.
    glsl += "// Full-screen triangle synthesised from gl_VertexIndex: no vertex buffer, no input layout, no index\n";
    glsl += "// buffer. (gl_VertexID is the OpenGL name and does not exist in Vulkan GLSL - glslang says so loudly.)\n";
    glsl += "// The classic oversized-triangle trick — (−1,−1), (3,−1), (−1,3) covers the [−1,1]² viewport exactly,\n";
    glsl += "// and the UV mapping below makes texel centres land on pixel centres at 1:1 scale.\n";
    glsl += "layout(location = 0) out vec2 OutUv;\n\n";

    glsl += "void main()\n{\n";
    // Vertex 0 → (−1,−1), 1 → (3,−1), 2 → (−1,3): arithmetic on the vertex index so there is no array to get out of
    // order.
    glsl += "    vec2 position;\n";
    glsl += "    if (gl_VertexIndex == 0) { position = vec2(-1.0, -1.0); }\n";
    glsl += "    else if (gl_VertexIndex == 1) { position = vec2(3.0, -1.0); }\n";
    glsl += "    else { position = vec2(-1.0, 3.0); }\n\n";

    // UVs: x maps straight across, y is INVERTED. This is the Vulkan framebuffer's top-left origin — NDC +y lands on
    // readback row 0 (y_f = (1 − y_ndc)/2 · height), the orientation this engine already measured and pinned in
    // TestForwardRenderGpu's probe rather than assuming it. Without the inversion a blit reproduces its input upside
    // down, and the reason that bug survives review is that a symmetric test image looks perfectly correct either way:
    // the first version of this pass was written OpenGL-style and failed only on texels off the horizontal centre,
    // which is why the comparison test uses per-texel asymmetric values rather than a symmetric gradient.
    glsl += "    OutUv = vec2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);\n";
    glsl += "    gl_Position = vec4(position, 0.0, 1.0);\n";
    glsl += "}\n";

    return glsl;
}

std::string BuildTonemapBlitFragmentGlsl()
{
    // Body-only text; see the header for the assembly-order contract.
    std::string glsl;

    // The shared shading model provides HpLinearToDisplay — and therefore the exposure/ACES/sRGB order — so this pass
    // contains no colour maths of its own. That is the whole design: one definition, three consumers (CPU reference,
    // forward shaders' optional display path, this blit).
    glsl += BuildShadingModelGlsl();
    glsl += "\n";

    glsl += "layout(location = 0) in vec2 InUv;\n";
    glsl += "layout(location = 0) out vec4 OutColor;\n\n";

    // Separate texture and sampler (texture2D + sampler), NOT a combined `sampler2D`. NVRHI's Vulkan backend maps
    // ResourceType::Texture_SRV to VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE and Sampler to VK_DESCRIPTOR_TYPE_SAMPLER with no
    // combined-image-sampler path at all (no COMBINED_IMAGE_SAMPLER anywhere in src/vulkan/), so a GLSL `sampler2D`
    // would pack its combined descriptor into the texture slot's offset and bind nothing. It also matters on this
    // host: the device is AMD RADV under lavapipe, whose VK_KHR_portability_subset requires
    // independentResolutionOfImageSamplers — combined samplers are not guaranteed to work at all here. The shader
    // compiles either way; only binding reveals which was right, so this comment records where the answer came from.
    glsl +=
        "layout(set = 0, binding = HP_BINDING_TEXTURE(" + std::to_string(TonemapBlitBindings::SourceTexture) + "))\n";
    glsl += "    uniform texture2D sourceColour;\n";
    glsl +=
        "layout(set = 0, binding = HP_BINDING_SAMPLER(" + std::to_string(TonemapBlitBindings::SourceSampler) + "))\n";
    glsl += "    uniform sampler sourceSampler;\n\n";

    // Named instance, matching the forward shaders' convention (frameParams, drawTransform). This is not cosmetic: a
    // block declared without an instance name injects its members into global scope, where `BlockName.Member` is then
    // invalid — glslang reports "cannot be used (maybe an instance name is needed)" rather than naming the real
    // mistake, and the fix is easy to miss because the block *looks* well-formed.
    glsl += "layout(set = 0, binding = HP_BINDING_CONSTANT_BUFFER(" +
            std::to_string(TonemapBlitBindings::ExposureBuffer) + ")) uniform ExposureBlock\n{\n";
    glsl += "    float Exposure;\n";
    glsl += "} exposureBlock;\n\n";

    glsl += "void main()\n{\n";
    // texelFetch-style exactness at 1:1 comes from the caller using a nearest sampler; sample() keeps the resampling
    // option open (see header). Alpha passes through: the forward pass writes 1.0 and later passes may pack data in it.
    // texture(), not sample(): with separate texture + sampler the GLSL spelling is texture(sampler2D(tex, samp), uv) —
    // constructing the combined sampler inline. There is no free function `sample` (glslang rejects it as a syntax
    // error), which is easy to get wrong coming from other APIs' naming.
    glsl += "    vec4 source = texture(sampler2D(sourceColour, sourceSampler), InUv);\n";
    glsl += "    OutColor = vec4(HpLinearToDisplay(source.rgb, exposureBlock.Exposure), source.a);\n";
    glsl += "}\n";

    return glsl;
}

}  // namespace Hollowpoint::Renderer
