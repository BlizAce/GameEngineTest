#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <thread>

#include "Hollowpoint/Core/Format.h"
#include "Hollowpoint/Core/PlatformInfo.h"

#if defined(_WIN32)
    #include <direct.h>
    #include <shlobj.h>
    #include <windows.h>
#elif defined(__APPLE__)
    #include <mach-o/dyld.h>
    #include <sys/utsname.h>
    #include <unistd.h>
#else
    #include <sys/utsname.h>
    #include <unistd.h>
#endif

namespace Hollowpoint::Core
{

OperatingSystem GetOperatingSystem()
{
#if defined(_WIN32)
    return OperatingSystem::Windows;
#elif defined(__APPLE__)
    #if TARGET_OS_IPHONE
    return OperatingSystem::IOS;
    #else
    return OperatingSystem::MacOS;
    #endif
#elif defined(__ANDROID__)
    return OperatingSystem::Android;
#elif defined(__linux__)
    return OperatingSystem::Linux;
#else
    return OperatingSystem::Unknown;
#endif
}

namespace
{

// Reads the first line of a Linux distribution release file (/etc/os-release style "NAME=..." entries).
std::string ReadLinuxReleaseName()
{
    std::FILE* file = std::fopen("/etc/os-release", "re");
    if (file == nullptr)
    {
        return {};
    }

    std::array<char, 512> line{};
    std::string result;
    while (std::fgets(line.data(), static_cast<int>(line.size()), file) != nullptr)
    {
        const std::string_view text(line.data());
        std::string_view value;
        if (text.starts_with("PRETTY_NAME="))
        {
            value = text.substr(12);
        }
        else if (text.starts_with("NAME="))
        {
            value = text.substr(5);
        }
        else
        {
            continue;
        }

        // Strip surrounding quotes and the trailing newline.
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
        {
            value.remove_suffix(1);
        }
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        {
            value = value.substr(1, value.size() - 2);
        }
        result = std::string(value);
        break;
    }

    std::fclose(file);
    return result;
}

#if defined(__linux__) && !defined(__ANDROID__)
std::string ReadLinuxKernelRelease()
{
    struct utsname name{};
    if (uname(&name) == 0)
    {
        return std::string(name.release);
    }
    return {};
}
#endif

#if defined(_WIN32)
std::string ReadWindowsProductName()
{
    // RtlGetVersion is the only API that reports the true version when the manifest does not claim
    // a newer Windows 10/11 compatibility GUID. Loaded dynamically to avoid a link-time dependency.
    using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOEXW*);

    HMODULE module = ::GetModuleHandleW(L"ntdll.dll");
    if (module == nullptr)
    {
        return {};
    }

    auto rtlGetVersion = reinterpret_cast<RtlGetVersionFunction>(
        reinterpret_cast<std::uintptr_t>(::GetProcAddress(module, "RtlGetVersion")));
    if (rtlGetVersion == nullptr)
    {
        return {};
    }

    OSVERSIONINFOEXW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtlGetVersion(&info) != 0)
    {
        return {};
    }

    // Windows 11 is reported as major version 10 with build >= 22000.
    if (info.dwMajorVersion == 10)
    {
        return info.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10";
    }
    return Format("Windows {}.{}", static_cast<unsigned>(info.dwMajorVersion),
                  static_cast<unsigned>(info.dwMinorVersion));
}
#endif

}  // namespace

std::string GetOperatingSystemName()
{
    switch (GetOperatingSystem())
    {
        case OperatingSystem::Windows:
#if defined(_WIN32)
            return ReadWindowsProductName();
#else
            return "Windows";
#endif
        case OperatingSystem::MacOS:
            return "macOS";
        case OperatingSystem::IOS:
            return "iOS";
        case OperatingSystem::Android:
            return "Android";
        case OperatingSystem::Linux:
        {
#if defined(__linux__) && !defined(__ANDROID__)
            const std::string distribution = ReadLinuxReleaseName();
            const std::string kernel = ReadLinuxKernelRelease();
            if (distribution.empty())
            {
                return kernel.empty() ? "Linux" : Format("Linux {}", kernel);
            }
            return distribution;
#else
            return "Linux";
#endif
        }
        default:
            return "Unknown";
    }
}

CPUArchitecture GetCPUArchitecture()
{
#if defined(_M_X64) || defined(__x86_64__)
    return CPUArchitecture::x86_64;
#elif defined(_M_IX86) || defined(__i386__)
    return CPUArchitecture::x86_32;
#elif defined(_M_ARM64) || defined(__aarch64__)
    return CPUArchitecture::ARM64;
#elif defined(_M_ARM) || defined(__arm__)
    return CPUArchitecture::ARM32;
#elif defined(__riscv) && __riscv_xlen == 64
    return CPUArchitecture::RISCV64;
#else
    return CPUArchitecture::Unknown;
#endif
}

const char* CPUArchitectureName(CPUArchitecture architecture)
{
    switch (architecture)
    {
        case CPUArchitecture::x86_32:
            return "x86 (32-bit)";
        case CPUArchitecture::x86_64:
            return "x86-64";
        case CPUArchitecture::ARM32:
            return "ARM (32-bit)";
        case CPUArchitecture::ARM64:
            return "ARM 64-bit";
        case CPUArchitecture::RISCV64:
            return "RISC-V 64";
        default:
            return "Unknown";
    }
}

UInt32 GetLogicalProcessorCount()
{
    unsigned int count = std::thread::hardware_concurrency();
    if (count == 0)
    {
        // std::thread::hardware_concurrency is allowed to report 0 when it cannot determine the
        // value; a single core is always a safe assumption.
        count = 1;
    }
    return static_cast<UInt32>(count);
}

bool IsAttachedToTerminal()
{
#if defined(_WIN32)
    return ::_isatty(_fileno(stdout)) != 0;
#else
    return ::isatty(STDOUT_FILENO) != 0;
#endif
}

std::optional<std::string> GetEnvironmentVariable(std::string_view name)
{
    if (name.empty())
    {
        return std::nullopt;
    }

    const std::string nameString(name);

#if defined(_WIN32)
    // Two-phase read: first call asks for the required size.
    const DWORD required = ::GetEnvironmentVariableA(nameString.c_str(), nullptr, 0);
    if (required == 0)
    {
        return std::nullopt;
    }
    std::string value;
    value.resize(required);
    const DWORD copied = ::GetEnvironmentVariableA(nameString.c_str(), value.data(), required);
    if (copied == 0 || copied >= required)
    {
        return std::nullopt;
    }
    value.resize(copied);
    return value;
#else
    // getenv returns a pointer into the process environment; copy it immediately because a later
    // setenv() would invalidate it.
    const char* value = ::getenv(nameString.c_str());
    if (value == nullptr)
    {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

bool SetEnvironmentVariable(std::string_view name, std::string_view value)
{
    if (name.empty())
    {
        return false;
    }

    const std::string nameString(name);
    const std::string valueString(value);

#if defined(_WIN32)
    return ::SetEnvironmentVariableA(nameString.c_str(), valueString.c_str()) != 0;
#else
    return ::setenv(nameString.c_str(), valueString.c_str(), 1) == 0;
#endif
}

std::string GetExecutableDirectory()
{
#if defined(_WIN32)
    std::array<char, 4096> buffer{};
    const DWORD length = ::GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
    {
        return {};
    }
    std::string path(buffer.data(), length);
#elif defined(__APPLE__)
    std::array<char, 4096> buffer{};
    uint32_t size = static_cast<uint32_t>(buffer.size());
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
    {
        return {};
    }
    std::string path(buffer.data());
    char resolved[4096];
    if (::realpath(path.c_str(), resolved) != nullptr)
    {
        path = resolved;
    }
#else
    std::array<char, 4096> buffer{};
    const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0)
    {
        return {};
    }
    std::string path(buffer.data(), static_cast<std::size_t>(length));
#endif

    const std::size_t separator = path.find_last_of("/\\");
    return separator == std::string::npos ? std::string(".") : path.substr(0, separator);
}

std::string GetUserHomeDirectory()
{
#if defined(_WIN32)
    PWSTR roamingPath = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &roamingPath)) && roamingPath != nullptr)
    {
        // Convert UTF-16 to UTF-8.
        const int byteCount = ::WideCharToMultiByte(CP_UTF8, 0, roamingPath, -1, nullptr, 0, nullptr, nullptr);
        std::string result;
        if (byteCount > 1)
        {
            result.resize(static_cast<std::size_t>(byteCount) - 1);
            ::WideCharToMultiByte(CP_UTF8, 0, roamingPath, -1, result.data(), byteCount, nullptr, nullptr);
        }
        ::CoTaskMemFree(roamingPath);
        return result;
    }
    return {};
#else
    const char* home = ::getenv("HOME");
    return home != nullptr ? std::string(home) : std::string();
#endif
}

std::string GetApplicationDataDirectory(std::string_view organization, std::string_view applicationName)
{
#if defined(_WIN32)
    PWSTR knownPath = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &knownPath)) || knownPath == nullptr)
    {
        return {};
    }
    const int byteCount = ::WideCharToMultiByte(CP_UTF8, 0, knownPath, -1, nullptr, 0, nullptr, nullptr);
    std::string base;
    if (byteCount > 1)
    {
        base.resize(static_cast<std::size_t>(byteCount) - 1);
        ::WideCharToMultiByte(CP_UTF8, 0, knownPath, -1, base.data(), byteCount, nullptr, nullptr);
    }
    ::CoTaskMemFree(knownPath);
#else
    std::string base;
    if (const std::optional<std::string> xdgData = GetEnvironmentVariable("XDG_DATA_HOME"))
    {
        base = *xdgData;
    }
    else
    {
        const std::string home = GetUserHomeDirectory();
        base = home.empty() ? std::string() : home + "/.local/share";
    }
#endif

    if (base.empty())
    {
        return {};
    }
    if (!organization.empty())
    {
        base += "/";
        base += organization;
    }
    if (!applicationName.empty())
    {
        base += "/";
        base += applicationName;
    }
    return base;
}

std::string GetApplicationCacheDirectory(std::string_view organization, std::string_view applicationName)
{
#if defined(_WIN32)
    PWSTR knownPath = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &knownPath)) || knownPath == nullptr)
    {
        return {};
    }
    const int byteCount = ::WideCharToMultiByte(CP_UTF8, 0, knownPath, -1, nullptr, 0, nullptr, nullptr);
    std::string base;
    if (byteCount > 1)
    {
        base.resize(static_cast<std::size_t>(byteCount) - 1);
        ::WideCharToMultiByte(CP_UTF8, 0, knownPath, -1, base.data(), byteCount, nullptr, nullptr);
    }
    ::CoTaskMemFree(knownPath);
#elif defined(__APPLE__)
    const std::string home = GetUserHomeDirectory();
    std::string base = home.empty() ? std::string() : home + "/Library/Caches";
#else
    std::string base;
    if (const std::optional<std::string> xdgCache = GetEnvironmentVariable("XDG_CACHE_HOME"))
    {
        base = *xdgCache;
    }
    else
    {
        const std::string home = GetUserHomeDirectory();
        base = home.empty() ? std::string() : home + "/.cache";
    }
#endif

    if (base.empty())
    {
        return {};
    }
    if (!organization.empty())
    {
        base += "/";
        base += organization;
    }
    if (!applicationName.empty())
    {
        base += "/";
        base += applicationName;
    }
    return base;
}

std::string GetLastSystemErrorMessage()
{
#if defined(_WIN32)
    const DWORD errorCode = ::GetLastError();
    LPWSTR messageBuffer = nullptr;
    const DWORD size =
        ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                         nullptr, errorCode, 0, reinterpret_cast<LPWSTR>(&messageBuffer), 0, nullptr);
    std::string result = Format("System error {}", errorCode);
    if (size > 0 && messageBuffer != nullptr)
    {
        const int byteCount =
            ::WideCharToMultiByte(CP_UTF8, 0, messageBuffer, static_cast<int>(size), nullptr, 0, nullptr, nullptr);
        if (byteCount > 0)
        {
            std::string wide(byteCount, '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, messageBuffer, static_cast<int>(size), wide.data(), byteCount, nullptr,
                                  nullptr);
            while (!wide.empty() && (wide.back() == '\n' || wide.back() == '\r'))
            {
                wide.pop_back();
            }
            result = Format("System error {}: {}", errorCode, wide);
        }
        ::LocalFree(messageBuffer);
    }
    return result;
#else
    const int errorCode = errno;
    return Format("System error {}: {}", errorCode, std::strerror(errorCode));
#endif
}

}  // namespace Hollowpoint::Core
