#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"

namespace Hollowpoint::Core
{

// ---------------------------------------------------------------------------
// Platform queries.
//
// Everything here is a thin, tested wrapper over OS facilities: the rest of the engine must never
// call GetVersionEx/read /proc directly, so that porting to a new platform means implementing one
// file rather than auditing the codebase.
// ---------------------------------------------------------------------------

enum class OperatingSystem : UInt8
{
    Unknown = 0,
    Windows,
    MacOS,
    Linux,
    Android,
    IOS
};

enum class CPUArchitecture : UInt8
{
    Unknown = 0,
    x86_32,
    x86_64,
    ARM32,
    ARM64,
    RISCV64
};

[[nodiscard]] HP_CORE_API OperatingSystem GetOperatingSystem();

// Human readable OS name including version where available ("Windows 11", "Fedora Linux 44").
[[nodiscard]] HP_CORE_API std::string GetOperatingSystemName();

[[nodiscard]] HP_CORE_API CPUArchitecture GetCPUArchitecture();
[[nodiscard]] HP_CORE_API const char* CPUArchitectureName(CPUArchitecture architecture);

// Logical CPU count (includes SMT threads). Returns at least 1.
[[nodiscard]] HP_CORE_API UInt32 GetLogicalProcessorCount();

// True when the process is running under a terminal attached to stdout (used to decide whether
// colour codes and progress bars make sense).
[[nodiscard]] HP_CORE_API bool IsAttachedToTerminal();

// Environment variables ------------------------------------------------------------

// Returns the value of an environment variable, or std::nullopt when unset. An empty value is
// reported as an empty string, not nullopt — the distinction matters for PATH-like variables.
[[nodiscard]] HP_CORE_API std::optional<std::string> GetEnvironmentVariable(std::string_view name);

// Sets an environment variable for this process. Returns false on failure.
HP_CORE_API bool SetEnvironmentVariable(std::string_view name, std::string_view value);

// Paths ---------------------------------------------------------------------------

// Absolute path of the currently running executable, resolved through symlinks.
[[nodiscard]] HP_CORE_API std::string GetExecutableDirectory();

// User profile/home directory, or empty when it cannot be determined.
[[nodiscard]] HP_CORE_API std::string GetUserHomeDirectory();

// Directory for user-writable application data ("AppData/Roaming" on Windows, XDG_DATA_HOME on
// Linux, ~/Library/Application Support on macOS). `organization` may be empty.
[[nodiscard]] HP_CORE_API std::string GetApplicationDataDirectory(std::string_view organization,
                                                                  std::string_view applicationName);

// Directory for cached data that is safe to delete ("caches"). See GetApplicationDataDirectory.
[[nodiscard]] HP_CORE_API std::string GetApplicationCacheDirectory(std::string_view organization,
                                                                   std::string_view applicationName);

// Last error message from the OS (GetLastError/errno) formatted as text. Useful after a failed
// file or library operation.
[[nodiscard]] HP_CORE_API std::string GetLastSystemErrorMessage();

}  // namespace Hollowpoint::Core
