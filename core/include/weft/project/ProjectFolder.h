#pragma once

// Local persistence (spec 02 W-10, W-11): the project folder, atomic save and
// the crash-recovery change log.
//
//   <Name>.weft/
//     project.automerge     saved document (written atomically)
//     Media/ Bounces/ Cache/
//     Recovery/changes.log  every local change since the last save
//     Recovery/archive/     discarded logs
//     Peers/                only in a shared folder (D-COL-45): <device>.automerge per computer

#include "weft/doc/Document.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace weft::project
{
namespace fs = std::filesystem;

/** Fault injection for tests: called before each chunk is written with (bytesWritten, total);
    returning false aborts the write as if the process died there. */
using WriteFaultHook = std::function<bool (std::size_t written, std::size_t total)>;

/** Writes bytes to a temporary file in chunks, flushes it to disk, then renames it over the target. */
bool writeFileAtomic (const fs::path& target, std::span<const std::uint8_t> bytes, const WriteFaultHook& fault = {}, std::string* error = nullptr);

std::optional<std::vector<std::uint8_t>> readFile (const fs::path& file);

/**
    Append-only log of local changes: "WEFTLOG1", then records of
    u32 length | u32 crc32 | i64 timeMs | change bytes (little endian).
    A torn or corrupt tail (crash mid-append) ends the log at the last good record.
*/
class RecoveryLog
{
public:
    struct Entry
    {
        std::int64_t timeMs = 0;
        std::vector<std::uint8_t> change;
    };

    explicit RecoveryLog (fs::path file) : path (std::move (file)) {}

    bool append (std::span<const std::uint8_t> change, std::int64_t timeMs);
    std::vector<Entry> read() const;
    bool hasEntries() const;
    /** Empties the log (after a successful save). */
    bool clear();
    /** Moves the log into Recovery/archive/ (Discard). */
    bool archive (std::int64_t timeMs);
    const fs::path& file() const noexcept { return path; }

private:
    fs::path path;
};

std::uint32_t crc32 (std::span<const std::uint8_t> bytes) noexcept;

/**
    A project folder on local disk. It can also be a shared folder (D-COL-45): one that a sync
    app such as Google Drive, Dropbox or OneDrive copies between computers. A sync app makes a
    "conflicted copy" when two computers write one file, so in a shared folder each computer
    writes only its own Peers/<device>.automerge (and its own recovery log), and every computer
    merges all of them. project.automerge stays as the state the folder was shared with.
*/
class ProjectFolder
{
public:
    /** `deviceId` names this computer's files in a shared folder; it is unused otherwise. */
    explicit ProjectFolder (fs::path folder, std::string deviceId = {});

    /** <parent>/<name>.weft */
    static fs::path folderFor (const fs::path& parent, const std::string& projectName);

    const fs::path& root() const noexcept { return dir; }
    fs::path documentFile() const { return dir / "project.automerge"; }
    fs::path mediaDir() const { return dir / "Media"; }
    RecoveryLog& log() noexcept { return recovery; }

    /** Creates the folder layout. */
    bool create (std::string* error = nullptr);

    /** Saves the document atomically, then clears the recovery log. */
    bool save (doc::Document& document, const WriteFaultHook& fault = {}, std::string* error = nullptr);

    struct Opened
    {
        std::optional<doc::Document> document;  // the saved state
        std::optional<doc::Document> recovered; // saved state + recovery log, when it differs
        std::string error;                      // set when document is empty
        bool recoveryAvailable() const noexcept { return recovered.has_value(); }
    };

    /** Opens without modifying anything on disk (AT-PRJ-9). */
    Opened open() const;

    /** Recover: the saved state + log becomes the document, is saved, and the log cleared. */
    bool recover (Opened& opened, std::string* error = nullptr);
    /** Discard: keeps the saved state and archives the log (AT-PRJ-12). */
    bool discard (Opened& opened, std::int64_t timeMs);

    // Shared folder (D-COL-45) ---------------------------------------------------------
    bool shared() const;
    /** Turns this into a shared folder and saves the document as this computer's file. */
    bool makeShared (doc::Document& document, std::string* error = nullptr);
    /** This computer's file in Peers/. */
    fs::path ownPeerFile() const { return peersDir() / (device + ".automerge"); }
    /**
        The other computers' files in Peers/ that are new or changed since the last call, as
        saved-document bytes for Document::apply. Any *.automerge counts, so a sync app's
        conflicted copy is merged too. A file still being written fails to load and is
        retried on the next call.
    */
    std::vector<std::vector<std::uint8_t>> pollPeers();

private:
    fs::path dir;
    std::string device;
    RecoveryLog recovery;
    struct Stamp
    {
        fs::file_time_type time {};
        std::uintmax_t size = 0;
        bool operator== (const Stamp&) const = default;
    };
    std::map<fs::path, Stamp> seenPeers;

    fs::path peersDir() const { return dir / "Peers"; }
    bool usesPeerFile() const { return ! device.empty() && shared(); }
    std::vector<fs::path> peerFiles() const;
};

/** Returns an error message if the document's schemaVersion is newer than this build supports. */
std::optional<std::string> checkSchemaVersion (const doc::Document& document);
} // namespace weft::project
