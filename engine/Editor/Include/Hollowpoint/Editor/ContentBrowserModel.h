#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"

// ---------------------------------------------------------------------------
// ContentBrowserModel — the data behind the editor's asset/project tree.
//
// A model, not a widget: it answers "what is in this folder, what can I do to this file" and knows nothing about
// ImGui. That is what makes the project explorer testable (a temp directory on disk plus assertions — no window, no
// GPU), and it is also the seam where an asset database replaces raw files later without touching any UI code: the
// model's contract is entries and actions, not directory handles.
//
// What it does NOT do, stated because a content browser invites feature creep and AGENTS.md forbids stubs:
//   * Importing. Files are listed and opened; nothing is converted into an engine asset, because no asset database
//     exists yet (docs/ARCHITECTURE.md lists it as the next milestone). Double-clicking a .glb says "no importer", not
//     nothing — see OpenEntry.
//   * Watching for changes. A manual Refresh plus `content.refresh` is honest; a watcher that misses events is worse.
//   * Thumbnails. Same reason: they require the import path.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Editor
{

using Core::Size;

//! One row in the tree.
struct ContentEntry
{
    std::string Name;  //!< File/folder name, no path.
    bool IsDirectory = false;
    Core::UInt64 SizeBytes = 0;
    //! Seconds since epoch of last modification, or 0 when the filesystem refused to say (a file deleted between the
    //  directory listing and the stat — normal in a project folder an editor and an IDE both write to).
    Core::UInt64 ModifiedSeconds = 0;

    //! True for files the engine can open today: scenes (.hpscene) and scripts (.lua). Everything else lists fine but
    //  opens as "no importer" — reported rather than hidden, because a content browser that hides half the project is
    //  worse than one that says "not yet".
    bool IsEngineAsset = false;
};

//! Lists and acts on files under one root directory.
class HP_EDITOR_API ContentBrowserModel
{
public:
    //! Sets the root and forgets the cached listing. Returns false when `root` does not exist or is not a directory —
    //  reported, because "the content browser is empty" caused by a wrong --project path is a startup misconfiguration
    //  the user needs to hear about once, loudly, rather than discover by staring at an empty panel.
    bool SetRoot(const std::filesystem::path& root);

    [[nodiscard]] const std::filesystem::path& GetRoot() const noexcept
    {
        return m_Root;
    }

    //! Re-lists `relativePath` (empty = the root) into the model's cached entries: directories first, then files, each
    //  alphabetical. Returns false when the directory cannot be opened (deleted since it was listed, permissions); the
    //  previous listing is kept in that case rather than cleared — an empty panel after a transient error reads as
    //  "the project lost its files".
    bool Refresh(const std::filesystem::path& relativePath = {}, std::string* outError = nullptr);

    //! The listing produced by the last successful Refresh, which does not change until Refresh succeeds.
    [[nodiscard]] const std::vector<ContentEntry>& GetEntries() const noexcept
    {
        return m_Entries;
    }

    [[nodiscard]] const std::filesystem::path& GetCurrentPath() const noexcept
    {
        return m_CurrentPath;
    }

    //! True when the current path is deeper than the root. Back navigation goes up one level via Refresh's parent.
    bool CanNavigateUp() const noexcept;

    //! Re-lists the parent directory. `outError` receives the reason on failure (a directory deleted between the
    //  listing that offered "up" and the click).
    bool NavigateUp(std::string* outError = nullptr);

    //! Filters GetEntries by a case-insensitive substring of the name (the content browser's search box). Returns
    //  indices into GetEntries rather than copies, so the widget can keep stable row identity while filtering.
    [[nodiscard]] std::vector<Size> FilterByName(std::string_view substring) const;

    //! Resolves a listing entry to an absolute path under the root. Rejects anything that escapes the root: `..` in a
    //  name, an absolute path passed by a console command, or a symlink whose target is outside (checked lexically —
    //  enough for a tool reading its own project directory, and stated rather than oversold as sandboxing).
    [[nodiscard]] bool ResolveWithinRoot(const std::filesystem::path& relativePath,
                                         std::filesystem::path& outAbsolute) const;

    //! Opens an entry: directories navigate, .hpscene/.lua files return true with `outMessage` naming what should load
    //  them (the caller — the editor application — performs the load, so this model keeps no engine dependencies), and
    //  anything else reports "no importer" and returns false.
    bool OpenEntry(const ContentEntry& entry, std::string& outMessage);

    //! True for names the engine loads directly. Exposed for tests and for the console's `content.open`.
    [[nodiscard]] static bool IsEngineAssetName(std::string_view name) noexcept;

private:
    std::filesystem::path m_Root;
    std::filesystem::path m_CurrentPath;  //!< Absolute; inside m_Root.
    std::vector<ContentEntry> m_Entries;
};

}  // namespace Hollowpoint::Editor
