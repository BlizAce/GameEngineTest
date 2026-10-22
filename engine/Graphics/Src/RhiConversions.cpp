#include "RhiConversions.h"

#include "Hollowpoint/Core/Assert.h"

namespace Hollowpoint::Graphics::Detail
{

nvrhi::Format ToNvrhi(GraphicsFormat format) noexcept
{
    // Exhaustive: a new GraphicsFormat without a case here fails the build (the trailing assert is belt and braces
    // for compilers that let fallthrough slide). Assuming numeric equality with nvrhi::Format instead would be a
    // silent wrong-format bug the first time either enum gains a member.
    switch (format)
    {
        case GraphicsFormat::Rgba8Unorm:
            return nvrhi::Format::RGBA8_UNORM;
        case GraphicsFormat::Bgra8Unorm:
            return nvrhi::Format::BGRA8_UNORM;
        case GraphicsFormat::Rgba16Float:
            return nvrhi::Format::RGBA16_FLOAT;
        case GraphicsFormat::Rg16Float:
            return nvrhi::Format::RG16_FLOAT;
        case GraphicsFormat::R16Float:
            return nvrhi::Format::R16_FLOAT;
        case GraphicsFormat::D32Float:
            return nvrhi::Format::D32;  // NVRHI spells depth formats without the width suffix
        case GraphicsFormat::R8Unorm:
            return nvrhi::Format::R8_UNORM;
        case GraphicsFormat::Rgba8Srgb:
            return nvrhi::Format::SRGBA8_UNORM;
        case GraphicsFormat::Rgba32Float:
            return nvrhi::Format::RGBA32_FLOAT;
        case GraphicsFormat::Rg32Float:
            return nvrhi::Format::RG32_FLOAT;
        case GraphicsFormat::Rgb32Float:
            return nvrhi::Format::RGB32_FLOAT;
        case GraphicsFormat::Unknown:
            break;
    }

    HP_ASSERT_MSG(false, "GraphicsFormat::Unknown reached the RHI conversion — resources must name a real format");
    return nvrhi::Format::UNKNOWN;
}

nvrhi::CpuAccessMode ToNvrhi(CpuAccess access) noexcept
{
    switch (access)
    {
        case CpuAccess::None:
            return nvrhi::CpuAccessMode::None;
        case CpuAccess::Read:
            return nvrhi::CpuAccessMode::Read;
        case CpuAccess::Write:
            return nvrhi::CpuAccessMode::Write;
    }
    return nvrhi::CpuAccessMode::None;
}

}  // namespace Hollowpoint::Graphics::Detail
