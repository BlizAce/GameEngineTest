#include <format>

#include "Hollowpoint/Core/EngineVersion.h"

#if defined(__has_include)
    #if __has_include("HollowpointRevision.h")
        #include "HollowpointRevision.h"
    #endif
#endif

#ifndef HP_SOURCE_REVISION
    #define HP_SOURCE_REVISION "unknown"
#endif

namespace Hollowpoint::Core
{

std::string_view GetEngineVersionString()
{
    // Built from the compile-time constants so that the string can never drift from them.
    // A function-local static keeps this allocation to exactly one per process.
    static const std::string s_Version = Format("{}.{}.{}", VersionMajor, VersionMinor, VersionPatch);
    return s_Version;
}

std::string_view GetSourceRevision()
{
    return HP_SOURCE_REVISION;
}

std::string GetEngineBuildBanner()
{
#if defined(_WIN32)
    constexpr std::string_view platformName = "Windows";
#elif defined(__APPLE__)
    constexpr std::string_view platformName = "macOS";
#elif defined(__linux__)
    constexpr std::string_view platformName = "Linux";
#else
    constexpr std::string_view platformName = "Unknown";
#endif

#if defined(CMAKE_BUILD_TYPE)
    const std::string_view buildType = CMAKE_BUILD_TYPE;
#elif defined(NDEBUG)
    const std::string_view buildType = "Release";
#else
    const std::string_view buildType = "Debug";
#endif

    return Format("Hollowpoint {} ({}, {}, {}, build {} {})", GetEngineVersionString(), GetSourceRevision(),
                  platformName, buildType, __DATE__, __TIME__);
}

}  // namespace Hollowpoint::Core
