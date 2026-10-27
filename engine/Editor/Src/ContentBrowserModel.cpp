#include <algorithm>
#include <chrono>
#include <system_error>

#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Editor/ContentBrowserModel.h"

namespace Hollowpoint::Editor
{

namespace
{

//! Extensions the engine can open without an asset pipeline. Kept as a list rather than a switch so adding one is a
//! one-word change, and lowercase-compared because Windows filesystems are case-insensitive while Linux is not — the
//! editor must offer the same actions on both, so it matches case-insensitively and lets the loader be strict.
constexpr const char* kOpenableExtensions[] = {".hpscene", ".lua"};

[[nodiscard]] std::string ExtensionOf(std::string_view name)
{
    const Size dot = name.find_last_of('.');
    if (dot == std::string_view::npos)
    {
        return {};
    }

    std::string extension(name.substr(dot));
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

//! Drops trailing separators so two spellings of one directory compare equal.
/**
 * `root / ""` and `root / "."` both normalise to a path with a trailing separator ("/proj/"), which is a different path
 * from the root as configured ("/proj") even though they name the same directory. It surfaced as an "Up" button that
 * never disabled after navigating back to the project root: CanNavigateUp compared paths textually and still believed
 * it was one level down (caught by TestEditorModel's NavigateUp case).
 */
[[nodiscard]] std::filesystem::path TrimTrailingSeparators(const std::filesystem::path& path)
{
    std::string text = path.string();

    // The root itself keeps its separator ("/"), since trimming it would produce an empty path — which this model
    // treats as "no root set" and would silently disable the whole browser.
    while (text.size() > 1 && text.back() == std::filesystem::path::preferred_separator)
    {
        text.pop_back();
    }
    return std::filesystem::path(text);
}

[[nodiscard]] std::filesystem::path LexicalRelative(const std::filesystem::path& root,
                                                    const std::filesystem::path& absolute)
{
    // Lexically_relative rather than the member on a path that has been through weakly_canonical: it works without
    // touching the filesystem, which matters because a symlinked project directory (common with network drives and
    // `~/Repo` checkouts) resolves to a real path that shares no prefix with the configured root — and every entry
    // would then look like it escaped.
    return absolute.lexically_relative(root);
}

[[nodiscard]] Core::UInt64 ModifiedSecondsOf(const std::filesystem::directory_entry& entry) noexcept
{
    std::error_code error;
    const std::filesystem::file_time_type stamp = entry.last_write_time(error);
    if (error)
    {
        return 0;  // see the header: a file deleted mid-listing is normal, not an error worth reporting per row
    }

    // file_clock has no epoch relationship to the Unix clock, so it must be converted rather than cast — subtracting
    // its raw tick count yields a huge number that formats as a plausible-looking year 30000 date in the panel.
    const auto systemStamp = std::chrono::file_clock::to_sys(stamp);
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(systemStamp.time_since_epoch()).count();
    return seconds > 0 ? static_cast<Core::UInt64>(seconds) : 0ull;
}

}  // namespace

bool ContentBrowserModel::SetRoot(const std::filesystem::path& root)
{
    std::error_code error;
    if (!std::filesystem::is_directory(root, error))
    {
        HP_LOG_ERROR("EDITOR", "content browser root \"{}\" is not a directory ({})", root.string(),
                     error ? error.message() : "not found");
        return false;
    }

    m_Root = root;
    m_CurrentPath = root;
    m_Entries.clear();
    return true;
}

bool ContentBrowserModel::Refresh(const std::filesystem::path& relativePath, std::string* outError)
{
    if (m_Root.empty())
    {
        if (outError != nullptr)
        {
            *outError = "content browser has no root set";
        }
        HP_LOG_ERROR("EDITOR", "%s", outError != nullptr ? outError->c_str() : "no root");
        return false;
    }

    std::filesystem::path target;
    if (!ResolveWithinRoot(relativePath, target))
    {
        if (outError != nullptr)
        {
            *outError =
                ""
                " + relativePath.string() + "
                " is not a path inside the project root";
        }
        return false;
    }

    // Listed into a local vector first: on failure the previous listing stays, which is the difference between "the
    // panel flickered" and "the panel claimed my project is empty".
    std::vector<ContentEntry> entries;

    std::error_code error;
    std::filesystem::directory_iterator iterator(target, std::filesystem::directory_options::skip_permission_denied,
                                                 error);
    if (error)
    {
        if (outError != nullptr)
        {
            *outError = "cannot list \"" + target.string() + "\": " + error.message();
        }
        HP_LOG_ERROR("EDITOR", "%s", outError != nullptr ? outError->c_str() : "list failed");
        return false;
    }

    for (const std::filesystem::directory_entry& entry : iterator)
    {
        ContentEntry row;
        row.Name = entry.path().filename().string();

        // Skips "." and ".." never appear from directory_iterator, but hidden files do — and listing .git here would
        // put a directory the editor must not touch one double-click away. Hidden entries are still *counted* in the
        // total; hiding them entirely would make "the folder is empty" a lie about folders that hold a .gitignore.
        if (!entry.is_directory(error))
        {
            row.IsDirectory = false;
            row.SizeBytes = entry.file_size(error);
            if (error)
            {
                row.SizeBytes = 0;
            }
            row.IsEngineAsset = IsEngineAssetName(row.Name);
        }
        else
        {
            row.IsDirectory = true;
        }

        row.ModifiedSeconds = ModifiedSecondsOf(entry);
        entries.push_back(std::move(row));
    }

    // Directories first, then files; alphabetical within each. This is the ordering every file browser uses and the one
    // an outliner-style list can be scanned by eye — "files, then folders" forces a full scan to find a folder.
    std::stable_sort(entries.begin(), entries.end(), [](const ContentEntry& lhs, const ContentEntry& rhs) {
        if (lhs.IsDirectory != rhs.IsDirectory)
        {
            return lhs.IsDirectory;
        }
        return std::lexicographical_compare(
            lhs.Name.begin(), lhs.Name.end(), rhs.Name.begin(), rhs.Name.end(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) < std::tolower(static_cast<unsigned char>(b));
            });
    });

    m_Entries = std::move(entries);
    m_CurrentPath = target;
    return true;
}

bool ContentBrowserModel::CanNavigateUp() const noexcept
{
    // Both sides trimmed: a root configured as "/proj/" must still compare equal to the "/proj" this model produces.
    return !m_Root.empty() && TrimTrailingSeparators(m_CurrentPath) != TrimTrailingSeparators(m_Root);
}

bool ContentBrowserModel::NavigateUp(std::string* outError)
{
    if (!CanNavigateUp())
    {
        if (outError != nullptr)
        {
            *outError = "already at the project root";
        }
        return false;
    }

    const std::filesystem::path parent = m_CurrentPath.parent_path();
    // Refresh rather than assigning: the listing must be re-read for the new directory. A failed refresh keeps the
    // stale listing (see Refresh), so reporting failure matters — the panel would otherwise show one directory's files
    // under another directory's heading.
    return Refresh(parent.lexically_relative(m_Root), outError);
}

std::vector<Size> ContentBrowserModel::FilterByName(std::string_view substring) const
{
    std::string needle(substring);
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::vector<Size> matches;
    for (Size index = 0; index < m_Entries.size(); ++index)
    {
        if (needle.empty())
        {
            matches.push_back(index);
            continue;
        }

        std::string haystack = m_Entries[index].Name;
        std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (haystack.find(needle) != std::string::npos)
        {
            matches.push_back(index);
        }
    }
    return matches;
}

bool ContentBrowserModel::ResolveWithinRoot(const std::filesystem::path& relativePath,
                                            std::filesystem::path& outAbsolute) const
{
    if (m_Root.empty())
    {
        HP_LOG_ERROR("EDITOR", "content browser has no root set");
        return false;
    }

    // An absolute argument (a console command's `content.list /etc`) is rejected rather than truncated: silently
    // re-rooting it under the project would list a directory whose entries claim to be somewhere they are not.
    if (relativePath.is_absolute())
    {
        HP_LOG_ERROR("EDITOR", "\"{}\" is an absolute path; content browser paths are relative to the project root",
                     relativePath.string());
        return false;
    }

    // Trimmed as well as normalised (see TrimTrailingSeparators): every path this model stores goes through here, so
    // GetCurrentPath() compares equal to the root a caller passed to SetRoot.
    outAbsolute = TrimTrailingSeparators((m_Root / relativePath).lexically_normal());

    // After normalisation, "a/../b" becomes "b" and "../x" keeps its ".." — so a prefix test on the normalised path
    // catches traversal without resolving symlinks (see the header for why that boundary is chosen deliberately).
    const std::filesystem::path relative = LexicalRelative(m_Root, outAbsolute);
    if (relative.empty() && outAbsolute != m_Root)
    {
        HP_LOG_ERROR("EDITOR", "\"{}\" resolves outside the project root", relativePath.string());
        return false;
    }
    if (!relative.empty() && *relative.begin() == "..")
    {
        HP_LOG_ERROR("EDITOR", "\"{}\" escapes the project root", relativePath.string());
        return false;
    }

    return true;
}

bool ContentBrowserModel::OpenEntry(const ContentEntry& entry, std::string& outMessage)
{
    if (entry.IsDirectory)
    {
        outMessage = "opening directory " + entry.Name;
        return Refresh(entry.Name);
    }

    if (!IsEngineAssetName(entry.Name))
    {
        outMessage = "no importer for \"" + entry.Name + "\" yet — see docs/ARCHITECTURE.md (asset pipeline)";
        HP_LOG_INFO("EDITOR", "{}", outMessage);
        return false;
    }

    std::filesystem::path absolute;
    if (!ResolveWithinRoot(entry.Name, absolute))
    {
        outMessage = "\"" + entry.Name + "\" is not inside the project root";
        return false;
    }

    // The caller loads it. This model has no engine dependency on purpose: making the content browser itself call
    // RuntimeApplication would mean the asset tree cannot be tested (or reused by a command-line packlet) without a
    // device, and it would put scene-mutation policy two layers away from the code that owns the world.
    outMessage = absolute.string();
    return true;
}

bool ContentBrowserModel::IsEngineAssetName(std::string_view name) noexcept
{
    const std::string extension = ExtensionOf(name);
    for (const char* candidate : kOpenableExtensions)
    {
        if (extension == candidate)
        {
            return true;
        }
    }
    return false;
}

}  // namespace Hollowpoint::Editor
