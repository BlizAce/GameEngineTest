// Tests for Core::FileSystem — the engine's only route to disk.
//
// Everything runs under a fresh directory inside std::filesystem::temp_directory_path() (AGENTS.md §5: no filesystem
// outside temp), created per test case so ordering never matters and one failure cannot poison another.
//
// The interesting cases are the failure paths. A file API that works when everything is present is worth little; the
// reason this module exists rather than callers using std::fstream is that failures must be classified (missing vs
// permission vs directory), bounded (a corrupt size must not allocate forever) and non-destructive (a failed save must
// leave the previous file intact). Those properties are what the tests below pin, and each one would fail against the
// obvious shortcut implementation.

#include "TestSupport.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>     // ::open, probing whether directory permissions are enforced
#include <sys/stat.h>  // ::chmod for the read-only-directory durability case

#if !defined(_WIN32)
    #include <unistd.h>  // ::getpid for unique scratch directory names.
#endif

#include <doctest/doctest.h>

#include "Hollowpoint/Core/FileSystem.h"

namespace fs = std::filesystem;

using namespace Hollowpoint::Core;

namespace
{

//! Creates a unique scratch directory and removes it on scope exit, including when an assertion throws mid-test —
//! leaked temp directories from failed runs are how a test suite becomes the reason /tmp fills up.
class ScopedTempDirectory
{
public:
    explicit ScopedTempDirectory(std::string_view label)
    {
        // Name must be unique per test case AND per concurrent process: ctest runs test binaries in parallel, and a
        // shared scratch directory means one test's cleanup deletes another's fixtures — producing failures that only
        // appear under -j and point at whichever test lost the race.
        //
        // random_device rather than fs::unique_path (which does exactly this job but is deprecated in C++20) or a
        // pid+counter pair (unique per process, but two runs of the same binary reuse names if they interleave). The
        // counter still separates cases within one process, so a weak RNG cannot collide inside a single run.
        static std::atomic<UInt64> s_Counter{0};
        const UInt64 token = s_Counter.fetch_add(1);

        std::random_device entropy;
        const UInt64 randomPart = (static_cast<UInt64>(entropy()) << 32) ^ static_cast<UInt64>(entropy());

        Path = fs::temp_directory_path() /
               ("hp_filesystem_" + std::string(label) + "_" + std::to_string(randomPart) + "_" + std::to_string(token));

        std::error_code ignored;
        fs::remove_all(Path, ignored);
        fs::create_directories(Path, ignored);
    }

    ~ScopedTempDirectory()
    {
        std::error_code ignored;
        fs::remove_all(Path, ignored);
    }

    ScopedTempDirectory(const ScopedTempDirectory&) = delete;
    ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;

    [[nodiscard]] std::string File(std::string_view name) const
    {
        return (Path / std::string(name)).string();
    }

    [[nodiscard]] std::string Directory(std::string_view name) const
    {
        return (Path / std::string(name)).string();
    }

    std::filesystem::path Path;
};

void WriteRaw(const std::string& path, std::string_view contents)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

}  // namespace

TEST_SUITE("Core/FileSystem")
{
    TEST_CASE("bytes round-trip exactly, including embedded nulls")
    {
        ScopedTempDirectory temp("bytes");

        // Binary content with NULs and high bytes: a text-oriented implementation (getline, istream_iterator, or a
        // std::string that assumed termination) survives this only by accident.
        std::vector<UInt8> payload;
        for (UInt32 i = 0; i < 777u; ++i)
        {
            payload.push_back(static_cast<UInt8>((i * 37u) ^ 0xA5u));
        }

        const std::string path = temp.File("payload.bin");
        REQUIRE(WriteFileAtomic(path, payload.data(), payload.size()).Succeeded);

        const BytesResult read = ReadFileBytes(path);
        REQUIRE(read.Succeeded);
        CHECK(read.Error == FileError::None);
        CHECK(read.Value == payload);
    }

    TEST_CASE("an empty file reads as a successful empty result, not an error")
    {
        ScopedTempDirectory temp("empty");
        const std::string path = temp.File("empty.bin");
        WriteRaw(path, "");

        const BytesResult read = ReadFileBytes(path);
        INFO(read.Message);
        // An empty .hpscene is a valid empty scene; treating zero length as failure would reject it before parsing.
        CHECK(read.Succeeded);
        CHECK(read.Value.empty());

        const TextResult text = ReadFileText(path);
        CHECK(text.Succeeded);
        CHECK(text.Value.empty());
    }

    TEST_CASE("reading a missing file reports NotFound rather than an empty success")
    {
        ScopedTempDirectory temp("missing");

        const BytesResult read = ReadFileBytes(temp.File("nope.bin"));
        CHECK_FALSE(read.Succeeded);
        CHECK(read.Error == FileError::NotFound);
        CHECK_FALSE(read.Message.empty());  // A failure with no explanation is the support ticket that takes a day.
        CHECK(std::string(FileErrorName(read.Error)) == "not found");
    }

    TEST_CASE("reading a directory as a file reports IsDirectory")
    {
        ScopedTempDirectory temp("isdir");
        const std::string directory = temp.Directory("subfolder");
        REQUIRE(CreateDirectoryRecursive(directory).Succeeded);

        const BytesResult read = ReadFileBytes(directory);
        CHECK_FALSE(read.Succeeded);
        // Without this check the failure comes back from ifstream as a generic "cannot open", which sends debugging
        // toward permissions when the actual mistake is a manifest pointing at a folder.
        CHECK(read.Error == FileError::IsDirectory);
    }

    TEST_CASE("a file larger than the cap fails instead of allocating or truncating")
    {
        ScopedTempDirectory temp("cap");
        const std::string path = temp.File("big.bin");
        WriteRaw(path, std::string(4096u, 'x'));

        // Cap BELOW the real size: the point is that the limit is checked before allocating, so the read fails with a
        // reason rather than either exhausting memory on a bogus reported size or silently returning half an asset.
        const BytesResult read = ReadFileBytes(path, 1024ull);
        CHECK_FALSE(read.Succeeded);
        CHECK(read.Error == FileError::OutOfSpace);
        CHECK(read.Message.find("limit") != std::string::npos);

        // Cap exactly equal to the size must succeed — an off-by-one here rejects every file that hits its own size.
        CHECK(ReadFileBytes(path, 4096ull).Succeeded);
    }

    TEST_CASE("atomic writes replace contents and never leave a temporary behind")
    {
        ScopedTempDirectory temp("atomic");
        const std::string path = temp.File("scene.hpscene");

        REQUIRE(WriteFileAtomic(path, "first").Succeeded);
        CHECK(ReadFileText(path).Value == "first");

        REQUIRE(WriteFileAtomic(path, "second").Succeeded);
        CHECK(ReadFileText(path).Value == "second");

        // Leftover "*.tmp<N>" files are the signature of a save path that renames without cleaning up: harmless on
        // success and accumulated garbage after failures, which is what users actually notice.
        for (const auto& entry : fs::directory_iterator(temp.Path))
        {
            CHECK(entry.path().string().find(".tmp") == std::string::npos);
        }
    }

    TEST_CASE("an atomic write creates missing parent directories")
    {
        ScopedTempDirectory temp("parents");
        const std::string nested = temp.File("a/b/c/deep.txt");

        REQUIRE(WriteFileAtomic(nested, "deep").Succeeded);
        CHECK(fs::exists(nested));
        CHECK(ReadFileText(nested).Value == "deep");
    }

    TEST_CASE("a failed write leaves the previous contents intact")
    {
        ScopedTempDirectory temp("torn");
        const std::string path = temp.File("important.txt");
        REQUIRE(WriteFileAtomic(path, "original level data").Succeeded);

        // Null data with a non-zero size is a caller bug; the contract is that it fails WITHOUT touching the target.
        // This is the property that separates WriteFileAtomic from truncate-then-write: the naive implementation has
        // already destroyed the file by the time it notices the argument is bad, which is exactly how "the editor ate
        // my level" happens.
        {
            Hollowpoint::Test::ScopedLogCapture logs;
            const FileOperationResult result = WriteFileAtomic(path, nullptr, 100);
            CHECK_FALSE(result.Succeeded);
            CHECK(result.Error == FileError::NotSupported);
        }
        CHECK(ReadFileText(path).Value == "original level data");

        // Writing onto a directory must fail the same way rather than creating something beside it.
        const std::string directory = temp.Directory("folder");
        REQUIRE(CreateDirectoryRecursive(directory).Succeeded);
        {
            Hollowpoint::Test::ScopedLogCapture logs;
            CHECK_FALSE(WriteFileAtomic(directory, "x").Succeeded);
        }
    }

    TEST_CASE("truncate write overwrites and creates parents")
    {
        ScopedTempDirectory temp("truncate");
        const std::string path = temp.File("x/y.txt");

        REQUIRE(WriteFileTruncate(path, "one", 3).Succeeded);
        CHECK(ReadFileText(path).Value == "one");
        REQUIRE(WriteFileTruncate(path, "two", 3).Succeeded);
        CHECK(ReadFileText(path).Value == "two");

        // Zero bytes is a valid write (an empty file), not an error — WriteFileAtomic treats the same input as valid.
        CHECK(WriteFileTruncate(temp.File("zero.txt"), "", 0).Succeeded);
    }

    TEST_CASE("existence queries distinguish files, directories and absent paths")
    {
        ScopedTempDirectory temp("exists");
        const std::string file = temp.File("thing.txt");
        const std::string directory = temp.Directory("folder");
        REQUIRE(CreateDirectoryRecursive(directory).Succeeded);
        WriteRaw(file, "x");

        CHECK(FileExists(file));
        CHECK_FALSE(FileExists(directory));  // A directory is not a file; callers use this to decide whether to load.
        CHECK(DirectoryExists(directory));
        CHECK_FALSE(DirectoryExists(file));
        CHECK_FALSE(FileExists(temp.File("absent")));
        CHECK_FALSE(FileExists(""));  // Empty path is never a real file, and must not mean "the working directory".

        // A dangling symlink: status() fails, so FileExists reports false rather than promising something unreadable.
        std::error_code ignored;
        fs::create_symlink(temp.File("target-that-does-not-exist"), temp.File("dangling"), ignored);
        CHECK_FALSE(FileExists(temp.File("dangling")));

        // A symlink to a real file reads fine, so it must report true — the question callers ask is "can I load this",
        // not "what kind of directory entry is this".
        fs::create_symlink(file, temp.File("good-link"), ignored);
        CHECK(FileExists(temp.File("good-link")));
    }

    TEST_CASE("file size reports -1 for absent paths and 0 for empty files")
    {
        ScopedTempDirectory temp("size");
        const std::string file = temp.File("sized.bin");

        CHECK(GetFileSize(file) == -1);  // -1, not 0: an empty file is real and must stay distinguishable.

        WriteRaw(file, "");
        CHECK(GetFileSize(file) == 0);

        WriteRaw(file, "12345");
        CHECK(GetFileSize(file) == 5);

        CHECK(GetFileSize(temp.Directory("nope")) == -1);
    }

    TEST_CASE("directory listing is sorted and can exclude directories")
    {
        ScopedTempDirectory temp("list");
        WriteRaw(temp.File("zebra.txt"), "z");
        WriteRaw(temp.File("apple.txt"), "a");
        WriteRaw(temp.File("Mango.txt"), "m");
        REQUIRE(CreateDirectoryRecursive(temp.Directory("sub")).Succeeded);

        const std::vector<std::string> all = ListDirectoryEntries(temp.Path.string(), true);
        REQUIRE(all.size() == 4u);

        // Sorted, and sorted by raw byte order (capital 'M' before lowercase letters) — the point is a deterministic
        // order any caller can rely on, not a locale-dependent one that varies between machines. AGENTS.md forbids
        // depending on unordered iteration when producing output, and a cook manifest built from an unsorted listing is
        // exactly how two identical builds produce different hashes.
        CHECK(all[0] == (temp.Path / "Mango.txt").string());
        CHECK(all[1] == (temp.Path / "apple.txt").string());
        CHECK(all[2] == (temp.Path / "sub").string());
        CHECK(all[3] == (temp.Path / "zebra.txt").string());

        const std::vector<std::string> filesOnly = ListDirectoryEntries(temp.Path.string(), false);
        REQUIRE(filesOnly.size() == 3u);
        for (const std::string& entry : filesOnly)
        {
            CHECK(entry.find("sub") == std::string::npos);
        }
    }

    TEST_CASE("listing an absent directory returns empty with a logged reason rather than throwing")
    {
        ScopedTempDirectory temp("listabsent");

        // Throwing here would be the default behaviour of the non-error_code overload, and a directory scan in an
        // asset watcher must not unwind through a frame loop because one folder vanished.
        Hollowpoint::Test::ScopedLogCapture logs;
        const std::vector<std::string> entries = ListDirectoryEntries(temp.File("never-existed"));
        CHECK(entries.empty());
        CHECK(logs.GetTotalCount() >= 1);

        Hollowpoint::Test::ScopedLogCapture emptyLogs;
        CHECK(ListDirectoryEntries("").empty());
    }

    TEST_CASE("CreateDirectoryRecursive is idempotent and reports what it did")
    {
        ScopedTempDirectory temp("mkdir");
        const std::string nested = temp.File("one/two/three");

        REQUIRE(CreateDirectoryRecursive(nested).Succeeded);
        CHECK(DirectoryExists(nested));

        // Second call must succeed too — "already exists" as an error here would make every save path need a pre-check.
        CHECK(CreateDirectoryRecursive(nested).Succeeded);

        {
            Hollowpoint::Test::ScopedLogCapture logs;
            CHECK_FALSE(CreateDirectoryRecursive("").Succeeded);
        }
    }

    TEST_CASE("CreateParentDirectory creates the containing directory and treats bare names as success")
    {
        ScopedTempDirectory temp("mkparent");

        const std::string filePath = temp.File("new/dir/file.txt");
        REQUIRE_FALSE(DirectoryExists((temp.Path / "new").string()));
        REQUIRE(CreateParentDirectory(filePath).Succeeded);
        CHECK(DirectoryExists((temp.Path / "new" / "dir").string()));

        // A bare filename has no parent to create: success, not failure. Reporting failure would force every caller to
        // special-case saving into the current directory.
        CHECK(CreateParentDirectory("just-a-name.txt").Succeeded);
    }

    TEST_CASE("path helpers split names and extensions")
    {
        CHECK(GetFileName("a/b/c/file.png") == "file.png");
        CHECK(GetFileName("file.png") == "file.png");
        CHECK(GetFileName(R"(a\b\file.png)") == "file.png");  // Windows separators on any host.
        CHECK(GetFileName("a/b/") == "");                     // A trailing separator means the directory itself.

        CHECK(GetFileNameWithoutExtension("file.png") == "file");
        CHECK(GetFileNameWithoutExtension("archive.tar.gz") == "archive.tar");  // Only the LAST dot is a boundary.
        CHECK(GetFileNameWithoutExtension("noext") == "noext");
        CHECK(GetFileNameWithoutExtension(".gitignore") == ".gitignore");  // A leading dot is not an extension.

        CHECK(GetFileExtension("file.png") == "png");
        CHECK(GetFileExtension("Texture.PNG") == "png");  // Lower-cased so callers can compare directly.
        CHECK(GetFileExtension("archive.tar.gz") == "gz");
        CHECK(GetFileExtension("noext") == "");
        CHECK(GetFileExtension(".gitignore") == "");
        CHECK(GetFileExtension("trailingdot.") == "");
        CHECK(GetFileExtension("a/b/") == "");  // Must not invent an extension from a directory named "assets.v2/".
    }

    TEST_CASE("ChangeFileExtension keeps the directory even when it contains dots")
    {
        CHECK(ChangeFileExtension("a/file.png", "jpg") == "a/file.jpg");
        CHECK(ChangeFileExtension("file.png", ".exr") == "file.exr");  // Leading dot accepted, not doubled.
        CHECK(ChangeFileExtension("noext", "png") == "noext.png");

        // The regression this exists for: stripping the extension by string surgery on the WHOLE path turns a dotted
        // directory into mangled output ("my.dir/file.png" → "my.jpg"). Only the file name may be touched.
        CHECK(ChangeFileExtension("my.dir/file.png", "jpg") == "my.dir/file.jpg");
        CHECK(ChangeFileExtension("a.b/c.d/e.tga", "png") == "a.b/c.d/e.png");

        // Empty extension drops it entirely rather than leaving a dangling dot.
        CHECK(ChangeFileExtension("file.png", "") == "file");
    }

    TEST_CASE("JoinPath adds exactly one separator and honours absolute right-hand paths")
    {
        CHECK(JoinPath("a", "b") == "a/b");
        CHECK(JoinPath("a/", "b") == "a/b");  // No double separator.
        CHECK(JoinPath(R"(a\)", "b") == "a/b");
        CHECK(JoinPath("", "b") == "b");
        CHECK(JoinPath("a", "") == "a");

        // An absolute second argument wins — every filesystem's own rule. Concatenating instead yields the
        // "project//home/user/asset.png" entries that make a manifest resolve nowhere.
        CHECK(JoinPath("project/root", "/etc/passwd") == "/etc/passwd");

        // Mixed separators on input normalise to one style.
        CHECK(JoinPath(R"(a\b)", R"(c\d)") == "a/b/c/d");
    }

    TEST_CASE("NormalizePath collapses dots and duplicate separators lexically")
    {
        CHECK(NormalizePath("a//b") == "a/b");
        CHECK(NormalizePath("./a/b") == "a/b");
        CHECK(NormalizePath("a/b/../c") == "a/c");
        CHECK(NormalizePath("a/./b") == "a/b");
        CHECK(NormalizePath(R"(a\b\c)") == "a/b/c");
        CHECK(NormalizePath("/a/b/") == "/a/b");  // Trailing separator stripped so two callers agree.

        // Two paths built differently normalize identically — what makes path comparison usable for deduplication.
        CHECK(NormalizePath("assets/../assets/scene.hpscene") == NormalizePath("assets/scene.hpscene"));
    }

    TEST_CASE("ToForwardSlashes converts only separators and leaves content alone")
    {
        CHECK(ToForwardSlashes(R"(a\b\c)") == "a/b/c");
        CHECK(ToForwardSlashes("already/unix") == "already/unix");
        CHECK(ToForwardSlashes("") == "");
        // Backslashes inside a file NAME are content on POSIX, and this function is documented as separator-only —
        // pinning that so nobody later "fixes" it into a path parser.
        CHECK(ToForwardSlashes(R"(we\ird)") == "we/ird");
    }

    TEST_CASE("empty paths are rejected with NotSupported rather than touching the working directory")
    {
        ScopedTempDirectory temp("emptypaths");
        Hollowpoint::Test::ScopedLogCapture logs;

        const BytesResult read = ReadFileBytes("");
        CHECK_FALSE(read.Succeeded);
        CHECK(read.Error == FileError::NotSupported);

        CHECK_FALSE(WriteFileAtomic("", "x").Succeeded);
        CHECK_FALSE(WriteFileTruncate("", "x", 1).Succeeded);
        CHECK_FALSE(CreateDirectoryRecursive("").Succeeded);
        CHECK_FALSE(CreateParentDirectory("").Succeeded);
    }

    TEST_CASE("concurrent atomic writes never produce a merged or empty file")
    {
        ScopedTempDirectory temp("concurrent");
        const std::string path = temp.File("contested.txt");
        REQUIRE(WriteFileAtomic(path, "seed").Succeeded);

        // Each writer produces a self-consistent payload (every byte identical within one write). After N concurrent
        // saves the file must equal exactly ONE of them — never an interleaving and never empty. That is what the
        // same-directory temp + rename buys: without it, two writers truncating a shared target can leave bytes from
        // both, which for a scene file means an unopenable save with no trace of why.
        constexpr int kWriters = 6;
        // Every writer is expected to succeed: each writes its own temporary and renames, so no two contend for the
        // same file. A failure would mean the temp-name uniqueness or the parent-directory creation is broken, which
        // the final-content check below cannot see (one good write hides five bad ones).
        std::atomic<int> successfulWrites{0};
        std::vector<std::thread> writers;
        writers.reserve(kWriters);
        for (int writer = 0; writer < kWriters; ++writer)
        {
            writers.emplace_back([&path, &successfulWrites, writer]() {
                const std::string payload(2048u, static_cast<char>('A' + writer));
                const FileOperationResult result = WriteFileAtomic(path, payload);
                if (result.Succeeded)
                {
                    successfulWrites.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (std::thread& thread : writers)
        {
            thread.join();
        }

        CHECK(successfulWrites.load() == kWriters);

        const TextResult finalText = ReadFileText(path);
        REQUIRE(finalText.Succeeded);
        REQUIRE_FALSE(finalText.Value.empty());

        // Every byte the same character, and that character belongs to one of the writers.
        const char marker = finalText.Value.front();
        CHECK(marker >= 'A');
        CHECK(marker < static_cast<char>('A' + kWriters));
        for (char c : finalText.Value)
        {
            REQUIRE(c == marker);
        }

        // No temporaries left behind by losing writers.
        int leftovers = 0;
        for (const auto& entry : fs::directory_iterator(temp.Path))
        {
            if (entry.path().string().find(".tmp") != std::string::npos)
            {
                ++leftovers;
            }
        }
        CHECK(leftovers == 0);
    }

    TEST_CASE("pseudo-files that report a misleading size still read correctly")
    {
        // Regression guard for two real bugs found while writing this module, both on Linux pseudo-filesystems:
        //   * procfs reports size 0 for files that have content (/proc/uptime → 21 bytes, /proc/cpuinfo → ~39 KB). An
        //     exact-length read returns nothing and looks like a successful empty-file read.
        //   * sysfs rounds the reported size up to a page (a 23-byte attribute reports 4096), so an exact-length read
        //     comes up short at a clean EOF — which a strict "gcount must equal size" check rejects as corruption.
        // Both are how tools read GPU, power and kernel information, so an engine that grows either feature needs the
        // path to work rather than to fail in a way that names /proc as the problem.
        //
        // Guarded on availability and asserting only robust properties (succeeded, plausible content), never exact
        // bytes — the value is regression protection for the read path, not a test of the kernel's output.
        struct Probe
        {
            const char* Path;
            bool RequireNonEmpty;
        };

        const Probe probes[] = {
            {"/proc/uptime", true},
            {"/sys/kernel/mm/transparent_hugepage/enabled", true},
        };

        int probed = 0;
        for (const Probe& probe : probes)
        {
            if (!FileExists(probe.Path))
            {
                continue;  // Not Linux, or a container without these mounted: nothing to check.
            }

            ++probed;
            INFO(probe.Path);
            const BytesResult read = ReadFileBytes(probe.Path, 1ull << 20);
            CHECK(read.Succeeded);
            if (probe.RequireNonEmpty)
            {
                // The specific failure this guards: Succeeded with zero bytes, which every caller reads as "the file
                // is empty" and never questions.
                CHECK_FALSE(read.Value.empty());
            }
        }

        if (probed == 0)
        {
            WARN("no procfs/sysfs probe paths available on this host; pseudo-file read path untested here");
        }

        // Non-regular files must be rejected rather than "read" successfully: /dev/zero would otherwise hand back as
        // many zero bytes as the caller asked for, which for an asset loader is a corrupt asset with no error.
        //
        // Note the guard uses std::filesystem::exists, NOT FileExists — the latter requires a regular file and so
        // returns false for exactly the character devices being tested here. Written with FileExists first, this block
        // silently skipped on every run while reporting green: a test whose own precondition excludes its subject is
        // the most expensive kind of passing test.
        if (fs::exists("/dev/null"))
        {
            const BytesResult nullRead = ReadFileBytes("/dev/null", 16);
            CHECK_FALSE(nullRead.Succeeded);
            CHECK(nullRead.Error == FileError::NotSupported);
            // And the existence query agrees it is not a loadable file, which is what keeps callers out of the read.
            CHECK_FALSE(FileExists("/dev/null"));
        }
        if (fs::exists("/dev/zero"))
        {
            const BytesResult zeroRead = ReadFileBytes("/dev/zero", 4096);
            CHECK_FALSE(zeroRead.Succeeded);
            CHECK(zeroRead.Error == FileError::NotSupported);
        }
    }

    TEST_CASE("atomic writing refuses rather than destroying when it cannot write atomically")
    {
        // THE durability test, and the contract it pins: WriteFileAtomic guarantees a reader never sees a partial file
        // and a failed save never loses the previous one. When atomicity is impossible the operation must therefore
        // FAIL — a caller who wants best-effort has WriteFileTruncate. Asserting that unconditionally is what makes
        // this test able to fail: an earlier version checked "if it failed, the original survived", which a
        // truncate-then-write implementation passes by simply succeeding instead. Verified: replacing temp+rename with
        // a direct ofstream left that version green. A conditional expectation on the outcome you hope for is not a
        // test.
        //
        // Why a read-only DIRECTORY (with a writable FILE) is the right lever: every failure reachable from arguments
        // (empty path, null data) is rejected before either implementation touches bytes, and a read-only FILE fails
        // both too — opening an existing file for writing does not need directory write permission, which is precisely
        // why the truncate version slips through. Only "cannot create the temporary" separates them.
        ScopedTempDirectory temp("durability");
        const std::string directory = temp.Directory("readonly_dir");
        REQUIRE(CreateDirectoryRecursive(directory).Succeeded);

        const std::string path = JoinPath(directory, "precious.txt");
        REQUIRE(WriteFileAtomic(path, "the only copy").Succeeded);
        REQUIRE(ReadFileText(path).Value == "the only copy");

        // Drop write permission on the directory while leaving the file itself writable.
        const bool madeReadOnly = chmod(directory.c_str(), 0500) == 0;
        if (!madeReadOnly)
        {
            WARN("could not make the scratch directory read-only; durability path untested here");
            return;
        }

        // Probe whether this environment actually enforces directory permissions before trusting the outcome: running
        // as root (or on a filesystem without permission bits) lets file creation through regardless, and reporting
        // success there would be a lie about having tested anything. The probe is the real syscall the implementation
        // uses, so it cannot disagree with it.
        const std::string probePath = JoinPath(directory, "probe.txt");
        const bool permissionsEnforced = (::open(probePath.c_str(), O_CREAT | O_WRONLY, 0600) == -1);
        std::error_code ignored;
        std::filesystem::remove(probePath, ignored);

        if (!permissionsEnforced)
        {
            chmod(directory.c_str(), 0700);
            WARN(
                "this environment does not enforce directory write permissions (running as root?); durability path "
                "untested here");
            return;
        }

        FileOperationResult result;
        {
            Hollowpoint::Test::ScopedLogCapture logs;
            result = WriteFileAtomic(path, "replacement that must never land");
        }

        chmod(directory.c_str(), 0700);  // Restore before asserting, so a failure cannot leak an unlistable directory.

        CHECK_FALSE(result.Succeeded);  // The contract: no atomicity, no write.
        CHECK(ReadFileText(path).Value == "the only copy");

        // No temporary left behind in the directory that just refused one (it may be unwritable, and a stale temp there
        // would then be permanently undeletable).
        CHECK(ListDirectoryEntries(directory, true).size() == 1u);
    }

    TEST_CASE("FileErrorName covers every enumerator without falling through to blank")
    {
        // A missing case would return "unknown" for a real error and make a log line look like a bug in the logging.
        const FileError all[] = {
            FileError::None,       FileError::NotFound,      FileError::PermissionDenied, FileError::IsDirectory,
            FileError::IsFile,     FileError::AlreadyExists, FileError::NotEmpty,         FileError::NotSupported,
            FileError::OutOfSpace, FileError::TimedOut,      FileError::Unknown,
        };
        for (const FileError error : all)
        {
            const std::string name = FileErrorName(error);
            CHECK_FALSE(name.empty());
            if (error != FileError::Unknown)
            {
                CHECK(name != "unknown");
            }
        }
    }
}
