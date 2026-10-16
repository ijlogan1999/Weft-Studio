#pragma once

// Keeps a shared project's audio files and large plug-in states in step with the relay
// (spec 05 §8): whatever the document references that this computer has is uploaded
// (Owners and Editors only), and whatever it references that this computer lacks is
// downloaded, regions near a playhead first. Downloads arrive front to back, so the
// caller can play the part that's already here (D-COL-11). Caller's thread only.

#include "weft/collab/Transfers.h"
#include "weft/commands/Session.h"

#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace weft::collab
{
struct MediaSyncOptions
{
    Account account;
    std::string projectId;
    std::filesystem::path projectRoot; // Media/, Blobs/ and Cache/Downloads/ are under it
    TransferOptions transfer;          // limits and test hooks (account and project are filled in)
};

class MediaSync
{
public:
    MediaSync (commands::Session& session, MediaSyncOptions options);

    /** Looks at the document again: queues uploads and downloads for what's new. */
    void refresh();
    /** Handles finished transfers and progress. */
    void poll();

    void blobAvailable (const std::string& hash);
    /** The role decides whether this client uploads; nothing happens until it's known. */
    void setRole (const std::string& role);
    /** Uploads pause while recording (D-COL-10). */
    void setRecording (bool recording);
    void setAccount (const Account& account);
    /** Where playheads are (this user's and collaborators'): their regions download first. */
    void setPlayheads (std::vector<Ticks> playheads);

    struct Download
    {
        std::int64_t done = 0, total = 0;
        bool waiting = false; // not on the relay yet (still uploading)
        std::string error;
        std::filesystem::path partFile;
    };
    /** Downloads not finished yet, by hash. */
    const std::map<std::string, Download>& downloads() const noexcept { return pendingDownloads; }
    /** Uploads not finished yet, by hash: bytes sent of total. */
    const std::map<std::string, std::pair<std::int64_t, std::int64_t>>& uploads() const noexcept { return pendingUploads; }
    /** "Uploading 2 · Downloading 3", or empty when nothing is moving (04 §10). */
    std::string statusText() const;

    std::function<void (const std::string& hash, const Download&)> onDownloadProgress;
    std::function<void (const std::string& hash, const std::filesystem::path& file)> onDownloaded;
    std::function<void (const std::string& hash)> onUploaded;
    std::function<void (const std::string& message)> onMessage;
    std::function<void (const Account&)> onAccountChange;

    Transfers& transfers() noexcept { return *transfer; }
    /** Where a downloaded media file goes, and where a blob does. */
    std::filesystem::path mediaDir() const { return options.projectRoot / "Media"; }
    std::filesystem::path blobDir() const { return options.projectRoot / "Blobs"; }

private:
    struct Wanted
    {
        bool media = true; // a WAV in Media/, else a plug-in state blob in Blobs/
        std::int64_t priority = 0;
    };
    std::map<std::string, Wanted> referenced() const;
    std::optional<std::filesystem::path> localFile (const std::string& hash, bool media) const;

    commands::Session& session;
    MediaSyncOptions options;
    std::unique_ptr<Transfers> transfer;
    std::string role;
    std::vector<Ticks> heads;
    std::set<std::string> uploaded; // the relay has these
    std::set<std::string> failedDownloads;
    std::map<std::string, Download> pendingDownloads;
    std::map<std::string, std::pair<std::int64_t, std::int64_t>> pendingUploads;
};

/** Bytes of the audio the project's regions use that this computer has (the share question). */
std::int64_t localMediaBytes (const model::ProjectModel& model, const std::filesystem::path& projectRoot);
} // namespace weft::collab
