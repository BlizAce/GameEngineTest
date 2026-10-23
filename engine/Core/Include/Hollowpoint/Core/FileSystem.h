#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"

// ---------------------------------------------------------------------------
// File and directory access for the whole engine.
//
// Why this exists instead of callers using std::fstream directly: three problems recur at every file touch point and
// each one is silent when got wrong.
//
//   1. Exceptions. std::filesystem's default overloads THROW, and AGENTS.md forbids exceptions crossing a module
//      boundary. Every call here uses the error_code overloads and converts them into a FileError, so a permission-
//      denied directory listing returns a failure the caller can act on rather than unwinding through a frame loop.
//   2. Encoding. On Windows, std::filesystem::path constructed from a std::string uses the native narrow (ANSI) code
//      page, NOT UTF-8 — so a path containing non-ASCII characters works on Linux and silently misses the file on
//      Windows. FromUtf8Path/ToUtf8Path centralise the correct conversion (path has a real UTF-8 constructor in
//      C++20) so engine code can pass UTF-8 std::strings everywhere, which is also what serialised data and hashes
//      need. This is the one place that decision lives.
//   3. Torn writes. Saving a scene or an asset with a plain truncate-and-write destroys the existing file if the
//      process dies halfway — the failure mode users report as "the editor ate my level". WriteFileAtomic writes a
//      temporary in the SAME directory and renames over the target, which is atomic within a filesystem. Same
//      directory is not an implementation detail: it is what guarantees the rename cannot cross a device boundary
//      and fail after the bytes were already paid for.
//
// Paths in this API are UTF-8 strings (not std::filesystem::path) so they serialize, hash and log like every other
// engine string. Absolute-versus-relative is the caller's choice; nothing here resolves against a working directory
// implicitly, because an asset database that depends on CWD cannot be relocated.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Core
{

//! What went wrong in a file operation. Distinguishing these matters: "not found" is routine (an optional config
//! file), "permission denied" is a broken install, and "is a directory" is a caller bug — the same response to all
//! three means a shipping game cannot tell a missing DLC pack from a corrupted one.
enum class FileError : UInt8
{
    None = 0,
    NotFound,
    PermissionDenied,
    IsDirectory,
    IsFile,
    AlreadyExists,
    NotEmpty,
    NotSupported,  //!< Path syntax the platform rejects, or an operation it cannot perform (e.g. rename across
                   //!< devices).
    OutOfSpace,    //!< The target filesystem refused the write, or a read exceeded its caller-supplied cap.
    TimedOut,  //!< The operation would block (ETIMEDOUT/EAGAIN) — reachable on network mounts and non-blocking handles,
               //!< not only in an async layer.
    Unknown
};

//! Human-readable reason plus the OS detail where one exists, formatted as "message (system text)".
[[nodiscard]] HP_CORE_API const char* FileErrorName(FileError error) noexcept;

//! Result carrying either the data or a classified failure. Explicitly not std::optional: an optional cannot say WHY
//! loading failed, and "the scene file could not be read" without a reason is the support ticket that takes a day.
template <typename T>
struct FileResult
{
    T Value{};  //!< Valid only when Succeeded. Default-constructed (empty) otherwise.
    bool Succeeded = false;
    FileError Error = FileError::Unknown;
    std::string Message;  //!< Empty on success; "what failed: path (system reason)" on failure.

    [[nodiscard]] bool HasValue() const noexcept
    {
        return Succeeded;
    }
};

using BytesResult = FileResult<std::vector<UInt8>>;
using TextResult = FileResult<std::string>;

//! Plain success/failure for operations with no payload, carrying the same classified reason.
struct FileOperationResult
{
    bool Succeeded = false;
    FileError Error = FileError::Unknown;
    std::string Message;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return Succeeded;
    }
};

// --- Reading -------------------------------------------------------------------

//! Reads a whole file. `maxBytes` bounds the read BEFORE allocating: a file whose reported size is wrong (a growing
//! log, a device node like /dev/zero, or plain corruption in a directory entry) would otherwise be allocated in full
//! and exhaust memory before the first byte was inspected. Defaults to 2 GiB — generous for any asset an editor loads
//! in one piece, small enough that a runaway read fails rather than swapping the machine. A file larger than the cap
//! fails with OutOfSpace rather than being silently truncated.
[[nodiscard]] HP_CORE_API BytesResult ReadFileBytes(std::string_view path, UInt64 maxBytes = 2147483648ull);

//! Reads a UTF-8 text file. Does NOT validate that the bytes are valid UTF-8 — several callers legitimately read
//! binary-ish or Latin-1 content (glTF embeds base64, some textures carry comments), and rejecting them here would
//! make this unusable as the engine's one read path. Validation belongs to whichever parser defines the contract.
[[nodiscard]] HP_CORE_API TextResult ReadFileText(std::string_view path, UInt64 maxBytes = 2147483648ull);

// --- Writing -------------------------------------------------------------------

//! Writes bytes, creating parent directories as needed, ATOMICALLY with respect to readers (see file header). The
//! previous contents survive until the new ones are fully on disk. Returns failure without touching the target if the
//! temporary cannot be written.
[[nodiscard]] HP_CORE_API FileOperationResult WriteFileAtomic(std::string_view path, const void* data,
                                                              UInt64 byteCount);
[[nodiscard]] HP_CORE_API FileOperationResult WriteFileAtomic(std::string_view path, std::string_view text);

//! Non-atomic write: truncates in place. Exists for log appending and scratch files where a torn result is harmless;
//! anything a user would lose sleep over wants WriteFileAtomic. The two names being different is the point — a
//! default-to-destructive API eventually gets called by the one code path that needed durability.
[[nodiscard]] HP_CORE_API FileOperationResult WriteFileTruncate(std::string_view path, const void* data,
                                                                UInt64 byteCount);

//! Text convenience overload, mirroring WriteFileAtomic — without it every text call site spells out a length.
[[nodiscard]] HP_CORE_API FileOperationResult WriteFileTruncate(std::string_view path, std::string_view text);

// --- Querying --------------------------------------------------------------------

[[nodiscard]] HP_CORE_API bool FileExists(std::string_view path) noexcept;
[[nodiscard]] HP_CORE_API bool DirectoryExists(std::string_view path) noexcept;

//! Size in bytes, or -1 when unreadable. -1 rather than 0 because a zero-length file is real and common (an empty
//! .hpscene is a valid empty scene), so 0 cannot double as "missing".
[[nodiscard]] HP_CORE_API Int64 GetFileSize(std::string_view path) noexcept;

// --- Directories -----------------------------------------------------------------

[[nodiscard]] HP_CORE_API FileOperationResult CreateDirectoryRecursive(std::string_view path);

//! Creates every directory of `filePath` except the final component — what a writer needs before opening a file whose
//! location the user chose. Taking the FILE path rather than its parent is deliberate: callers hold file paths, and
//! asking them to strip a filename correctly (against both separators, on a name that may contain dots) is exactly the
//! string surgery this module exists to keep in one place.
[[nodiscard]] HP_CORE_API FileOperationResult CreateParentDirectory(std::string_view filePath);

//! Immediate children of `directory`, sorted by name for determinism — AGENTS.md forbids iterating unordered
//! containers when producing output, and a directory listing that varies between runs makes asset manifests, cook
//! logs and golden-file tests non-reproducible. Returns an empty list on failure (with the reason logged) rather than
//! throwing on an unreadable directory.
[[nodiscard]] HP_CORE_API std::vector<std::string> ListDirectoryEntries(std::string_view directory,
                                                                        bool includeDirectories = true);

// --- Path string helpers -------------------------------------------------------------
// Lexical only: none of these touch the disk, so they are safe on paths that do not exist yet (the common case when
// constructing an output filename). Trailing separators are treated as absent.

//! File name including extension ("normal.rgba.png" → "normal.rgba.png").
[[nodiscard]] HP_CORE_API std::string GetFileName(std::string_view path);

//! File name without its final extension ("normal.rgba.png" → "normal.rgba"). Only the LAST dot is a boundary:
//! treating every dot as one turns "archive.tar.gz" into "archive", which is correct for nobody.
[[nodiscard]] HP_CORE_API std::string GetFileNameWithoutExtension(std::string_view path);

//! Final extension WITHOUT the leading dot, lower-cased ("Texture.PNG" → "png"). Lower-casing is what lets callers
//! compare extensions directly instead of repeating a case-insensitive comparison at every call site — file systems
//! on all three target platforms hand back whatever case the author saved. Returns empty when there is no extension.
[[nodiscard]] HP_CORE_API std::string GetFileExtension(std::string_view path);

[[nodiscard]] HP_CORE_API std::string ChangeFileExtension(std::string_view path, std::string_view newExtension);

//! Joins with exactly one separator, handling both '/' and '\\' on input and emitting the platform's native one.
//! `right` may be absolute in which case it wins (matching every filesystem's own rule) — silently concatenating an
//! absolute second argument is how "project//home/user/x.png" appears in a manifest.
[[nodiscard]] HP_CORE_API std::string JoinPath(std::string_view left, std::string_view right);

//! Collapses ".", "..", duplicate and mixed separators lexically (no disk access). Note this cannot resolve symlinks,
//! so two paths that normalize identically are not guaranteed to be the same file — use file identity when an asset
//! database needs true uniqueness.
[[nodiscard]] HP_CORE_API std::string NormalizePath(std::string_view path);

[[nodiscard]] HP_CORE_API std::string ToForwardSlashes(std::string_view path);

}  // namespace Hollowpoint::Core
