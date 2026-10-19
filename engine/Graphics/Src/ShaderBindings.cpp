#include <sstream>

#include "Hollowpoint/Graphics/ShaderBindings.h"

// NVRHI is included here (and only here) to static_assert our constants against its own defaults. The public header
// stays RHI-free, per this module's rule that Vulkan/NVRHI never leak into anything consumers include.
// RhiAccess.h is this module's sanctioned entry point for NVRHI: it wraps the include in the warning suppressions the
// library needs, so nvrhi.h is never included raw (which would fail the engine's own warning set on third-party code).
#include "Hollowpoint/Graphics/RhiAccess.h"

namespace Hollowpoint::Graphics
{

// ---------------------------------------------------------------------------
// Pin our numbers to NVRHI's actual defaults.
//
// These are compile-time assertions against `nvrhi::VulkanBindingOffsets{}` — reading the library's own default member
// initialisers rather than restating them — so an NVRHI upgrade that shifts a default fails the build here with a
// message naming exactly what moved. That matters because the runtime consequence of a mismatch is not an error: the
// affected uniform silently reads zero, which looks like black lighting or a dead parameter and sends debugging toward
// the shader instead of the binding table. (This is not hypothetical — that is precisely how the convention in this
// file was discovered.)
// ---------------------------------------------------------------------------

static_assert(BindingOffsets::ShaderResource == nvrhi::VulkanBindingOffsets{}.shaderResource,
              "HP BindingOffsets::ShaderResource disagrees with NVRHI's default shaderResource offset; update "
              "ShaderBindings.h and regenerate the GLSL prelude");

static_assert(BindingOffsets::Sampler == nvrhi::VulkanBindingOffsets{}.sampler,
              "HP BindingOffsets::Sampler disagrees with NVRHI's default sampler offset");

static_assert(BindingOffsets::ConstantBuffer == nvrhi::VulkanBindingOffsets{}.constantBuffer,
              "HP BindingOffsets::ConstantBuffer disagrees with NVRHI's default constantBuffer offset — every "
              "uniform block in the engine would silently read zeros");

static_assert(BindingOffsets::UnorderedAccess == nvrhi::VulkanBindingOffsets{}.unorderedAccess,
              "HP BindingOffsets::UnorderedAccess disagrees with NVRHI's default unorderedAccess offset");

// NOTE: NVRHI maps ResourceType::PushConstants through the *constant buffer* offset as well (see
// getRegisterOffsetForResourceType in vulkan-resource-bindings.cpp). That cannot be asserted from a default-constructed
// struct — it is a property of a switch statement, not of data — so it is recorded here: push constants and constant
// buffers share one numbering space, which means a layout must not assume a push-constant range at slot N leaves
// binding (ConstantBuffer + N) free for a uniform block.

std::string BuildGlslBindingPrelude()
{
    // Generated rather than hand-written so the C++ constants and the shader macros cannot drift. The numbers appear
    // only once in this translation unit.
    std::ostringstream out;

    out << "// HP binding convention, generated from Hollowpoint/Graphics/ShaderBindings.h.\n"
           "// NVRHI's Vulkan backend numbers bindings as <type offset> + <slot>; using a raw slot number in a shader\n"
           "// silently reads zeros. Always use these macros instead of writing binding indices by hand.\n"
        << "#define HP_BINDING_TEXTURE(slot)         (" << BindingOffsets::ShaderResource << " + (slot))\n"
        << "#define HP_BINDING_SAMPLER(slot)         (" << BindingOffsets::Sampler << " + (slot))\n"
        << "#define HP_BINDING_CONSTANT_BUFFER(slot) (" << BindingOffsets::ConstantBuffer << " + (slot))\n"
        << "#define HP_BINDING_UAV(slot)             (" << BindingOffsets::UnorderedAccess << " + (slot))\n";

    return out.str();
}

}  // namespace Hollowpoint::Graphics
