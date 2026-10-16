#pragma once

// Project files beyond the document (spec 02 W-10): where media is found, Save As with
// or without copying media, Locate… for missing media, the media index sidecar and the
// recent projects list. No UI and no JUCE: the app's dialogs call these.

#include "weft/doc/Document.h"
#include "weft/model/ProjectModel.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace weft::project
{
namespace fs = std::filesystem;

/** The project folder for a path the user picked: the folder itself, or its project.automerge. */
fs::path projectRootFor (const fs::path& picked);
/** Whether a folder holds a project document. */
bool isProjectFolder (const fs::path& root);

/**
    Folders searched for media, in order: the project's own Media/, then the folders listed
    in `media-links.txt` (one absolute UTF-8 path per line). Save As without copying media
    writes that file so the new project uses the original files (D-W10-5).
*/
std::vector<fs::path> mediaFolders (const fs::path& root);
/** `<folder>/<hex>.wav` in the first media folder that has it. */
std::optional<fs::path> findMediaFile (const std::vector<fs::path>& folders, const std::string& hash);

/** Hashes regions reference that no media folder has, sorted. */
std::vector<std::string> missingMedia (const model::ProjectModel& model, const std::vector<fs::path>& folders);

/**
    Locate… (F-PRJ-5): hashes the files under `searchFolder` (recursively, any name) and
    copies each one whose hash is in `wanted` to `<mediaDir>/<hex>.wav`. Files whose size
    differs from a known size (model MediaRef sizeBytes, when > 0) are skipped unhashed.
    Returns the hashes it restored.
*/
std::vector<std::string> locateMedia (const fs::path& searchFolder, const std::map<std::string, std::int64_t>& wanted, const fs::path& mediaDir);

/** `Media/media-index.json`: hash -> original file name, for people browsing the folder. */
bool writeMediaIndex (const model::ProjectModel& model, const fs::path& mediaDir, std::string* error = nullptr);

struct SaveAsOptions
{
    /** Copy every referenced media file into the new Media/ (default on, W-10 Save step 3);
        off: the new project links to the original media folders. */
    bool copyMedia = true;
};

/**
    Save As (F-PRJ-4): writes the document to a new project folder `to`, which must not exist
    yet. Plug-in state blobs and bounces are copied; Cache/ and Recovery/ start empty. The
    original folder is not changed.
*/
bool saveProjectAs (doc::Document& document, const model::ProjectModel& model, const fs::path& from, const fs::path& to,
                    const SaveAsOptions& options, std::string* error = nullptr);

/** File › Open Recent (F-PRJ-6): project folders, newest first, at most ten. */
class RecentProjects
{
public:
    static constexpr std::size_t maxItems = 10;

    explicit RecentProjects (fs::path storeFile = {}) : file (std::move (storeFile)) {}

    /** Reads the store file (one UTF-8 path per line); a missing file is an empty list. */
    void load();
    bool save() const;

    /** Moves a project to the top, dropping the oldest past ten. Saves. */
    void add (const fs::path& root);
    /** Removes a project (a deleted one, when clicked). Saves. */
    void remove (const fs::path& root);
    const std::vector<fs::path>& items() const noexcept { return list; }
    /** False for a project that's been deleted or moved. */
    static bool available (const fs::path& root) { return isProjectFolder (root); }

private:
    fs::path file;
    std::vector<fs::path> list;
};

/** UTF-8 text of a path and back (paths are std::u8string-based under C++20). */
std::string utf8 (const fs::path& p);
fs::path fromUtf8 (const std::string& s);
} // namespace weft::project
