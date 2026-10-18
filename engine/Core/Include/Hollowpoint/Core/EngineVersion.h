#pragma once

#include <string_view>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"

namespace Hollowpoint::Core
{

// ---------------------------------------------------------------------------
// Engine version and build identity.
//
// The values come from CMake (project() version + the git describe of the checkout at configure
// time) so that any crash report or log file can be traced back to an exact source revision.
// ---------------------------------------------------------------------------

inline constexpr Int32 VersionMajor = 0;
inline constexpr Int32 VersionMinor = 1;
inline constexpr Int32 VersionPatch = 0;

// "0.1.0"
[[nodiscard]] HP_CORE_API std::string_view GetEngineVersionString();

// Git revision of the engine sources, e.g. "a1b2c3d" or "unknown" for a non-git build.
[[nodiscard]] HP_CORE_API std::string_view GetSourceRevision();

// One-line banner used at startup and in crash dumps:
//   "Hollowpoint 0.1.0 (build Jun  1 2026, 12:34:56) — x86-64 Linux, RelWithDebInfo"
[[nodiscard]] HP_CORE_API std::string GetEngineBuildBanner();

}  // namespace Hollowpoint::Core
