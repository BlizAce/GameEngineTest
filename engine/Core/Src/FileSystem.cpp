#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>

#include "Hollowpoint/Core/FileSystem.h"

// ::fileno / ::fsync for the durability flush in WriteFileAtomic. On Windows the CRT spells these _fileno/_commit;
// the engine's own header include rules (no platform headers above Platform/) do not apply here because this IS a
// thin OS wrapper — see Core/PlatformInfo.cpp for the same treatment of OS facilities.
#if defined(_WIN32)
    #include <io.h>
    #include <wchar.h>
#else
    #include <fcntl.h>
    #include <unistd.h>
#endif

#include "Hollowpoint/Core/Log.h"

// Implementation notes that are easy to get wrong and hard to spot:
//
//  * Every std::filesystem call here uses the error_code overload. The throwing ones would cross a module boundary,
//    which AGENTS.md forbids — and the throw site is usually inside a frame loop where unwinding achieves nothing
//    except losing the frame's other work.
//  * Paths convert through std::u8string so that UTF-8 engine strings survive on Windows, where path's narrow-string
//    constructor uses the ANSI code page instead (non-ASCII paths then silently miss files that exist).
//  * Reads are bounded by the reported size checked BEFORE allocating, and non-regular files (character devices,
//    FIFOs) are rejected outright — /dev/zero otherwise "reads" successfully and returns as much zero as asked for.
//    The reported size itself is treated as a hint, not a contract: procfs reports 0 for files that have content and
//    sysfs rounds up, both measured on this machine, so exact-length reads silently return nothing or fail spuriously.

namespace Hollowpoint::Core
{

namespace
{

[[nodiscard]] std::filesystem::path ToNativePath(std::string_view utf8Path)
{
    // path has a real UTF-8 constructor in C++20; going through std::u8string is what selects it. On POSIX the two
    // representations are identical, so this costs nothing there and fixes Windows.
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8Path.data()), utf8Path.size()));
}

[[nodiscard]] std::string FromNativePath(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

[[nodiscard]] FileError ClassifyError(const std::error_code& error, FileError fallback) noexcept
{
    if (!error)
    {
        return FileError::None;
    }

    // Compared against std::errc rather than raw errno numbers: mapping errno to errc is what the standard guarantees,
    // and the raw numbers differ between the platforms this engine targets. Order matters only where a platform maps
    // two names to the same value; each case below is distinct on every target.
    if (error == std::errc::no_such_file_or_directory)
    {
        return FileError::NotFound;
    }
    if (error == std::errc::permission_denied || error == std::errc::read_only_file_system)
    {
        return FileError::PermissionDenied;
    }
    if (error == std::errc::is_a_directory)
    {
        return FileError::IsDirectory;
    }
    if (error == std::errc::not_a_directory)
    {
        return FileError::IsFile;
    }
    if (error == std::errc::file_exists)
    {
        return FileError::AlreadyExists;
    }
    if (error == std::errc::directory_not_empty)
    {
        return FileError::NotEmpty;
    }
    if (error == std::errc::operation_not_supported || error == std::errc::invalid_argument ||
        error == std::errc::filename_too_long)
    {
        return FileError::NotSupported;
    }
    if (error == std::errc::no_space_on_device || error == std::errc::file_too_large)
    {
        return FileError::OutOfSpace;
    }
    if (error == std::errc::timed_out || error == std::errc::resource_unavailable_try_again)
    {
        return FileError::TimedOut;
    }

    return fallback;
}

[[nodiscard]] std::string DescribeSystemError(const std::error_code& error)
{
    const std::string text = error.message();
    return text.empty() ? std::string("error code ") + std::to_string(error.value()) : text;
}

template <typename T>
[[nodiscard]] FileResult<T> MakeFailure(FileError error, std::string message)
{
    FileResult<T> result;
    result.Succeeded = false;
    result.Error = error;
    result.Message = std::move(message);
    return result;
}

[[nodiscard]] FileOperationResult MakeOperationFailure(FileError error, std::string message)
{
    FileOperationResult result;
    result.Succeeded = false;
    result.Error = error;
    result.Message = std::move(message);
    return result;
}

[[nodiscard]] FileOperationResult MakeOperationSuccess()
{
    FileOperationResult result;
    result.Succeeded = true;
    result.Error = FileError::None;
    return result;
}

}  // namespace

const char* FileErrorName(FileError error) noexcept
{
    switch (error)
    {
        case FileError::None:
            return "none";
        case FileError::NotFound:
            return "not found";
        case FileError::PermissionDenied:
            return "permission denied";
        case FileError::IsDirectory:
            return "is a directory";
        case FileError::IsFile:
            return "is a file";
        case FileError::AlreadyExists:
            return "already exists";
        case FileError::NotEmpty:
            return "directory not empty";
        case FileError::NotSupported:
            return "not supported";
        case FileError::OutOfSpace:
            return "out of space";
        case FileError::TimedOut:
            return "timed out";
        case FileError::Unknown:
            break;
    }
    return "unknown";
}

// --- Reading ------------------------------------------------------------------------

BytesResult ReadFileBytes(std::string_view path, UInt64 maxBytes)
{
    if (path.empty())
    {
        return MakeFailure<std::vector<UInt8>>(FileError::NotSupported, "ReadFileBytes called with an empty path");
    }

    const std::filesystem::path native = ToNativePath(path);

    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(native, error);
    if (error)
    {
        return MakeFailure<std::vector<UInt8>>(
            ClassifyError(error, FileError::NotFound),
            "cannot read '" + std::string(path) + "': " + DescribeSystemError(error));
    }

    if (std::filesystem::is_directory(status))
    {
        // A real failure mode, not paranoia: a directory whose name ends in ".png" arrives from a manifest, and the
        // generic "cannot open" ifstream would report is far less actionable than naming the mistake.
        return MakeFailure<std::vector<UInt8>>(FileError::IsDirectory, "'" + std::string(path) + "' is a directory");
    }

    if (!std::filesystem::is_regular_file(status))
    {
        // Character devices, FIFOs and sockets can be opened and "read" successfully while producing an endless or
        // meaningless stream — /dev/zero would otherwise return maxBytes of zeros as a successful asset load. Measured
        // on this host: /dev/null and /dev/zero are character special files, so this gate is not hypothetical.
        return MakeFailure<std::vector<UInt8>>(FileError::NotSupported,
                                               "'" + std::string(path) + "' is not a regular file");
    }

    // Reported size drives the cap check BEFORE allocating. It cannot be trusted as the exact byte count though, and
    // both directions of distrust are real on Linux (measured here):
    //   * procfs reports 0 for files with content — /proc/uptime reports 0 and yields 21 bytes, /proc/cpuinfo reports 0
    //     and yields ~39 KB. Treating 0 as "empty file" silently returns nothing for a read that looked successful.
    //   * sysfs reports a page-rounded size — a 23-byte attribute reports 4096, so an exact-length read comes up short
    //     at a clean EOF. Reporting that as "the file changed during read" would reject every sysfs query (which is how
    //     tools read GPU and power information).
    std::error_code sizeError;
    const std::uintmax_t sizeValue = std::filesystem::file_size(native, sizeError);
    // A failed size query is not fatal here: treat it as "unknown" (0) and fall through to the chunked read, which
    // works without knowing the length. The open below still reports the real error if the file is unreadable.
    const UInt64 reportedSize = sizeError ? 0ull : static_cast<UInt64>(sizeValue);
    if (reportedSize > maxBytes)
    {
        // Cap exceeded: rejected rather than truncated. A silently shortened asset is a corrupted import nobody notices
        // until the texture looks wrong three steps later.
        return MakeFailure<std::vector<UInt8>>(FileError::OutOfSpace,
                                               "'" + std::string(path) + "' reports " + std::to_string(reportedSize) +
                                                   " bytes, above the " + std::to_string(maxBytes) + " byte limit");
    }

    // No std::ios::ate: seeking to end is unsupported on procfs and FAILS THE OPEN there (measured — the ifstream is
    // not usable, which previously surfaced as a misleading "permission denied" for /proc/uptime). The size comes from
    // file_size() above, so nothing needs a seek, and a plain binary open works on every filesystem class.
    std::ifstream stream(native, std::ios::binary);
    if (!stream)
    {
        return MakeFailure<std::vector<UInt8>>(FileError::PermissionDenied,
                                               "cannot open '" + std::string(path) + "' for reading");
    }

    if (reportedSize == 0)
    {
        // Size unknown: read in chunks to EOF, bounded by maxBytes so an unexpectedly huge pseudo-file fails with a
        // reason instead of exhausting memory. Doubling from a modest start keeps small procfs reads (the common case)
        // to one or two allocations.
        std::vector<UInt8> bytes;
        std::vector<UInt8> chunk(4096);
        for (;;)
        {
            stream.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
            const std::streamsize got = stream.gcount();
            if (got <= 0)
            {
                break;
            }

            if (bytes.size() + static_cast<std::size_t>(got) > maxBytes)
            {
                return MakeFailure<std::vector<UInt8>>(
                    FileError::OutOfSpace, "'" + std::string(path) + "' exceeds the " + std::to_string(maxBytes) +
                                               " byte limit (its size could not be determined in advance)");
            }

            bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + got);

            if (got < static_cast<std::streamsize>(chunk.size()))
            {
                break;  // Short read means EOF for this stream.
            }

            if (chunk.size() < 1024u * 1024u)
            {
                chunk.resize(chunk.size() * 2u);
            }
        }

        FileResult<std::vector<UInt8>> result;
        result.Value = std::move(bytes);
        result.Succeeded = true;
        result.Error = FileError::None;
        return result;
    }

    std::vector<UInt8> bytes(static_cast<std::size_t>(reportedSize));
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

    const std::size_t got = static_cast<std::size_t>(stream.gcount());
    if (got < bytes.size())
    {
        if (!stream.eof())
        {
            // A failed read that is not end-of-file: a real I/O error. Returning the partial buffer would hand callers
            // a corrupt asset marked as success — the worst possible combination.
            return MakeFailure<std::vector<UInt8>>(
                FileError::Unknown, "'" + std::string(path) + "' read failed after " + std::to_string(got) + " of " +
                                        std::to_string(bytes.size()) + " bytes");
        }

        // Clean EOF early: the file is smaller than its directory entry claimed (sysfs rounds up; a concurrent
        // truncator also lands here). The bytes before EOF are genuinely the whole file's content as it stood, so
        // return them rather than failing — the alternative rejects every sysfs attribute on Linux.
        bytes.resize(got);
    }

    FileResult<std::vector<UInt8>> result;
    result.Value = std::move(bytes);
    result.Succeeded = true;
    result.Error = FileError::None;
    return result;
}

TextResult ReadFileText(std::string_view path, UInt64 maxBytes)
{
    BytesResult bytes = ReadFileBytes(path, maxBytes);

    TextResult result;
    result.Succeeded = bytes.Succeeded;
    result.Error = bytes.Error;
    result.Message = std::move(bytes.Message);
    if (bytes.Succeeded)
    {
        result.Value.assign(bytes.Value.begin(), bytes.Value.end());
    }
    return result;
}

// --- Writing ------------------------------------------------------------------------

FileOperationResult WriteFileAtomic(std::string_view path, const void* data, UInt64 byteCount)
{
    if (path.empty())
    {
        return MakeOperationFailure(FileError::NotSupported, "WriteFileAtomic called with an empty path");
    }
    if (data == nullptr && byteCount > 0)
    {
        return MakeOperationFailure(FileError::NotSupported,
                                    "WriteFileAtomic called with null data and a non-zero size");
    }

    const std::filesystem::path native = ToNativePath(path);

    std::error_code error;
    if (std::filesystem::is_directory(native, error))
    {
        return MakeOperationFailure(FileError::IsDirectory, "'" + std::string(path) + "' is a directory");
    }

    // Parent first: creating the target's directory is what lets callers save into a location the user just typed or
    // picked, without a separate mkdir step they will forget.
    const std::filesystem::path parent = native.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent, error))
    {
        std::filesystem::create_directories(parent, error);
        if (error)
        {
            return MakeOperationFailure(
                ClassifyError(error, FileError::Unknown),
                "cannot create directory '" + FromNativePath(parent) + "': " + DescribeSystemError(error));
        }
    }

    // Temporary in the SAME directory, then rename. Two properties depend on this: rename is atomic within a
    // filesystem (a reader sees either the whole old file or the whole new one), and it cannot cross a device
    // boundary — a temp in /tmp would fail with EXDEV after every byte was already paid for, reporting failure for a
    // reason that names only /tmp while leaving the real file untouched.
    //
    // The suffix carries a per-process counter so two writers of the same target never share a temporary. Whichever
    // rename lands last wins — the same outcome as non-atomic writing, but never a merged file. True mutual exclusion
    // needs an advisory lock, which belongs with the asset database that owns save ordering.
    static std::atomic<UInt64> s_TempCounter{0};
    const UInt64 token = s_TempCounter.fetch_add(1, std::memory_order_relaxed);
    const std::filesystem::path temp = ToNativePath(FromNativePath(native) + ".tmp" + std::to_string(token));

    // C stdio rather than ofstream, for one reason: fsync needs a file descriptor, and the standard streams do not
    // expose one portably. Durability is the whole point of this function, so it gets the API that can provide it.
    FILE* handle = nullptr;
#if defined(_WIN32)
    // Windows' narrow fopen cannot take UTF-8 either; _wfopen with the wide native path is the correct call there.
    handle = ::_wfopen(temp.c_str(), L"wb");
#else
    handle = ::fopen(temp.c_str(), "wb");
#endif
    if (handle == nullptr)
    {
        return MakeOperationFailure(FileError::PermissionDenied,
                                    "cannot open temporary for '" + std::string(path) + "'");
    }

    bool writeFailed = false;
    if (byteCount > 0)
    {
        const std::size_t written = std::fwrite(data, 1, static_cast<std::size_t>(byteCount), handle);
        writeFailed = (written != static_cast<std::size_t>(byteCount));
    }
    if (std::fflush(handle) != 0)
    {
        writeFailed = true;
    }

    if (!writeFailed)
    {
        // Flushing the C buffer is not enough: the bytes are in the OS page cache, and a power cut there loses them
        // while the rename still happens — leaving a zero-length file where the save "succeeded". Syncing before the
        // rename is what makes success mean "on disk". Costs one fsync per save; saves are user-driven, not per-frame.
#if defined(_WIN32)
        // _fileno, not fileno: the unprefixed alias is omitted under strict ANSI mode on the MSVC CRT.
        ::_commit(::_fileno(handle));
#else
        ::fsync(::fileno(handle));
#endif
    }

    const int closeResult = std::fclose(handle);  // fclose flushes; a failure here means data never made it.
    if (writeFailed || closeResult != 0)
    {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        return MakeOperationFailure(FileError::OutOfSpace,
                                    "writing '" + std::string(path) + "' failed mid-write (disk full?)");
    }

    std::error_code renameError;
    std::filesystem::rename(temp, native, renameError);
    if (renameError)
    {
        // Clean the temp up: leaving "scene.hpscene.tmp4" behind next to a failure teaches users that saving is
        // unsafe, and accumulates garbage in their project directory on every retry.
        std::filesystem::remove(temp, renameError);
        return MakeOperationFailure(ClassifyError(renameError, FileError::Unknown),
                                    "cannot replace '" + std::string(path) + "': " + DescribeSystemError(renameError));
    }

#ifndef _WIN32
    // The rename itself is a directory entry change, and THAT can still be sitting in the page cache when we return —
    // so after a crash the file could exist with its old contents even though every byte was fsynced. Syncing the
    // parent directory closes that window. Best-effort: not every platform lets a directory be opened for reading in
    // this way, and failing a completed rename over it would be strictly worse than the theoretical loss.
    if (const std::filesystem::path parentDirectory = native.parent_path(); !parentDirectory.empty())
    {
        if (const int directoryHandle = ::open(parentDirectory.c_str(), O_RDONLY); directoryHandle >= 0)
        {
            ::fsync(directoryHandle);
            ::close(directoryHandle);
        }
    }
#endif

    return MakeOperationSuccess();
}

FileOperationResult WriteFileAtomic(std::string_view path, std::string_view text)
{
    return WriteFileAtomic(path, text.data(), text.size());
}

FileOperationResult WriteFileTruncate(std::string_view path, const void* data, UInt64 byteCount)
{
    if (path.empty())
    {
        return MakeOperationFailure(FileError::NotSupported, "WriteFileTruncate called with an empty path");
    }
    if (data == nullptr && byteCount > 0)
    {
        return MakeOperationFailure(FileError::NotSupported,
                                    "WriteFileTruncate called with null data and a non-zero size");
    }

    const std::filesystem::path native = ToNativePath(path);

    std::error_code error;
    const std::filesystem::path parent = native.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent, error))
    {
        std::filesystem::create_directories(parent, error);
        if (error)
        {
            return MakeOperationFailure(
                ClassifyError(error, FileError::Unknown),
                "cannot create directory '" + FromNativePath(parent) + "': " + DescribeSystemError(error));
        }
    }

    std::ofstream stream(native, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        return MakeOperationFailure(FileError::PermissionDenied, "cannot open '" + std::string(path) + "' for writing");
    }

    if (byteCount > 0)
    {
        stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(byteCount));
    }
    stream.flush();

    if (!stream)
    {
        return MakeOperationFailure(FileError::OutOfSpace, "writing '" + std::string(path) + "' failed mid-write");
    }

    return MakeOperationSuccess();
}

// --- Querying --------------------------------------------------------------------------

bool FileExists(std::string_view path) noexcept
{
    if (path.empty())
    {
        return false;
    }

    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(ToNativePath(path), error);
    // status() FOLLOWS symlinks, so a link to a regular file reports true (the caller can read it) and a dangling
    // link yields an error and reports false. Both are what a "should I load this?" caller wants; is_symlink would be
    // wrong here because it answers "what is this name" rather than "can I read it".
    return !error && std::filesystem::is_regular_file(status);
    return !error && std::filesystem::is_regular_file(status);
}

bool DirectoryExists(std::string_view path) noexcept
{
    if (path.empty())
    {
        return false;
    }

    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(ToNativePath(path), error);
    return !error && std::filesystem::is_directory(status);
}

Int64 GetFileSize(std::string_view path) noexcept
{
    if (path.empty())
    {
        return -1;
    }

    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(ToNativePath(path), error);
    if (error || size == static_cast<std::uintmax_t>(-1))
    {
        return -1;
    }

    // Saturating rather than wrapping: a sparse file can report a size above Int64 on some filesystems, and returning
    // a negative value there would read as "error" to every caller.
    return size > static_cast<std::uintmax_t>(std::numeric_limits<Int64>::max()) ? std::numeric_limits<Int64>::max()
                                                                                 : static_cast<Int64>(size);
}

// --- Directories -------------------------------------------------------------------------

FileOperationResult CreateDirectoryRecursive(std::string_view path)
{
    if (path.empty())
    {
        return MakeOperationFailure(FileError::NotSupported, "CreateDirectoryRecursive called with an empty path");
    }

    std::error_code error;
    std::filesystem::create_directories(ToNativePath(path), error);
    if (error)
    {
        return MakeOperationFailure(
            ClassifyError(error, FileError::Unknown),
            "cannot create directory '" + std::string(path) + "': " + DescribeSystemError(error));
    }

    // create_directories reports success when the directory already exists, so the only way to land here with it
    // absent is a race against deletion. Checking afterwards keeps the return value honest instead of optimistic.
    if (!DirectoryExists(path))
    {
        return MakeOperationFailure(FileError::Unknown, "'" + std::string(path) + "' was not created");
    }

    return MakeOperationSuccess();
}

FileOperationResult CreateParentDirectory(std::string_view filePath)
{
    if (filePath.empty())
    {
        return MakeOperationFailure(FileError::NotSupported, "CreateParentDirectory called with an empty path");
    }

    const std::filesystem::path parent = ToNativePath(filePath).parent_path();
    if (parent.empty())
    {
        // A bare filename has no parent to create — that is success ("nothing to do"), not a failure. Reporting it as
        // one would force every caller to special-case relative saves.
        return MakeOperationSuccess();
    }

    return CreateDirectoryRecursive(FromNativePath(parent));
}

std::vector<std::string> ListDirectoryEntries(std::string_view directory, bool includeDirectories)
{
    std::vector<std::string> entries;
    if (directory.empty())
    {
        HP_LOG_WARNING("CORE", "ListDirectoryEntries called with an empty path");
        return entries;
    }

    std::error_code error;
    std::filesystem::directory_iterator iterator(ToNativePath(directory),
                                                 std::filesystem::directory_options::skip_permission_denied, error);
    if (error)
    {
        HP_LOG_ERROR("CORE", "cannot list directory '{}': {}", directory, DescribeSystemError(error));
        return entries;
    }

    for (const std::filesystem::directory_entry& entry : iterator)
    {
        std::error_code statusError;
        const bool isDirectory = entry.is_directory(statusError);
        if (statusError)
        {
            continue;  // Unstatable entries are skipped rather than killing the listing.
        }
        if (isDirectory && !includeDirectories)
        {
            continue;
        }

        entries.push_back(FromNativePath(entry.path()));
    }

    // Sorted for determinism. Directory order is filesystem-dependent and differs even between two runs on the same
    // machine after a rename, so an unsorted listing makes cooked manifests and golden-file tests irreproducible —
    // AGENTS.md forbids relying on unordered iteration when producing output, and this is where that would creep in.
    std::sort(entries.begin(), entries.end());
    return entries;
}

// --- Path string helpers ---------------------------------------------------------------------

std::string GetFileName(std::string_view path)
{
    const std::string normalized = ToForwardSlashes(path);
    const std::size_t separator = normalized.find_last_of('/');
    if (separator == std::string::npos)
    {
        return normalized;
    }
    // Trailing separators mean "the directory itself", and the file name of a path ending in '/' is empty — matching
    // every filesystem's own reading, and important because GetFileExtension must not invent an extension from a
    // directory named "assets.v2/".
    if (separator + 1 >= normalized.size())
    {
        return {};
    }
    return normalized.substr(separator + 1);
}

std::string GetFileNameWithoutExtension(std::string_view path)
{
    const std::string fileName = GetFileName(path);
    const std::size_t dot = fileName.find_last_of('.');
    // A leading dot is not an extension boundary: ".gitignore" has no extension, and treating the dot as one would
    // report its name as empty — which then produces "..tmp" style filenames when anything appends to it.
    if (dot == std::string::npos || dot == 0)
    {
        return fileName;
    }
    return fileName.substr(0, dot);
}

std::string GetFileExtension(std::string_view path)
{
    const std::string fileName = GetFileName(path);
    const std::size_t dot = fileName.find_last_of('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= fileName.size())
    {
        return {};
    }

    std::string extension = fileName.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

std::string ChangeFileExtension(std::string_view path, std::string_view newExtension)
{
    const std::string base = GetFileNameWithoutExtension(path);
    // The directory part is preserved by taking the original path up to where the file name started, so a path like
    // "a.b/c.txt" keeps its dotted directory rather than having it rewritten from the stripped file name.
    const std::string normalized = ToForwardSlashes(path);
    const std::size_t fileNameStart = normalized.find_last_of('/') + 1;

    std::string result = normalized.substr(0, fileNameStart) + base;
    if (!newExtension.empty())
    {
        result += newExtension.front() == '.' ? std::string(newExtension) : "." + std::string(newExtension);
    }
    return result;
}

std::string JoinPath(std::string_view left, std::string_view right)
{
    if (left.empty())
    {
        return ToForwardSlashes(right);
    }
    if (right.empty())
    {
        return ToForwardSlashes(left);
    }

    const std::string rightText = ToForwardSlashes(right);

    // An absolute second argument wins — the rule every filesystem applies, and getting it wrong here produces the
    // classic "project//home/user/asset.png" entry in a manifest that then resolves nowhere.
    if (rightText.front() == '/')
    {
        return rightText;
#ifdef _WIN32
    }
    else if (rightText.size() >= 2 && rightText[1] == ':')
    {
        return rightText;  // Drive-absolute ("C:\\assets").
#endif
    }

    const std::string leftText = ToForwardSlashes(left);
    if (leftText.back() == '/')
    {
        return leftText + rightText;
    }
    return leftText + "/" + rightText;
}

std::string NormalizePath(std::string_view path)
{
    // Lexical normalisation via the standard's own lexically_normal, after unifying separators so Windows-style input
    // normalises the same way on POSIX. Note what this cannot do: it does not touch the disk, so it cannot resolve
    // symlinks or report that ".." went above the root. Documented in the header for the same reason.
    std::filesystem::path normalized = ToNativePath(ToForwardSlashes(path)).lexically_normal();

    std::string result = FromNativePath(normalized);
    // lexically_normal can leave "." for an all-relative collapse ("./x" → "x" but "." stays "."); and it may emit a
    // trailing separator for directory-like paths. Strip the trailing one so callers comparing two normalized paths
    // they built differently get equality.
    while (result.size() > 1 && result.back() == '/')
    {
        result.pop_back();
    }
    return result;
}

std::string ToForwardSlashes(std::string_view path)
{
    std::string result(path);
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

}  // namespace Hollowpoint::Core
