#pragma once

// Blob transfers to and from the relay (spec 05 §8): uploads in 8 MB chunks that resume
// where they stopped, and downloads by HTTP range into `<downloadDir>/<hex>.part` that
// are verified against their BLAKE3 hash before they're moved into place. Background
// threads do the work (at most 2 uploads and 2 downloads at once, D-COL-10, D-COL-22);
// poll() hands what happened to the caller's thread.

#include "weft/collab/Relay.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace weft::collab
{
struct TransferOptions
{
    Account account;
    std::string projectId;
    int maxUploads = 2;
    int maxDownloads = 2;
    std::int64_t uploadChunk = 8 << 20;   // 05 §8.2
    std::int64_t downloadChunk = 1 << 20; // a range request's size
    /** Retry delays after network errors: 1, 2, 4 ... this many seconds. */
    int maxBackoffSeconds = 30;
    /** A download waiting for its blob asks again this often, in case BLOB_AVAILABLE was missed. */
    int waitingRetrySeconds = 30;

    // Test hooks -----------------------------------------------------------------------
    /** Called before each upload chunk with its offset; returning false stops the upload
        there as if the app had quit (it resumes on the next upload of that blob). */
    std::function<bool (const std::string& hash, std::int64_t offset)> beforeChunk;
    /** Called with each downloaded range before it's written (to corrupt it). */
    std::function<void (const std::string& hash, Bytes& data)> tamper;
};

struct TransferEvent
{
    enum class Kind
    {
        progress,   // bytes moved: done of total
        uploaded,   // the relay has the blob (resumed counts the bytes it already had)
        downloaded, // verified and moved to `file`
        waiting,    // the blob isn't on the relay yet
        failed,     // gave up: error says why
        tokens      // the access token was refreshed (account has the new one)
    };
    Kind kind = Kind::progress;
    bool upload = false;
    std::string hash;
    std::int64_t done = 0, total = 0;
    std::int64_t resumedFrom = 0; // uploads: where this upload started
    bool deduplicated = false;    // uploads: the relay already had it (HEAD hit)
    std::filesystem::path file;   // downloads: the final file; progress: the partial file
    std::string error;
    Account account; // tokens
};

class Transfers
{
public:
    explicit Transfers (TransferOptions options);
    ~Transfers();
    Transfers (const Transfers&) = delete;
    Transfers& operator= (const Transfers&) = delete;

    /** Makes sure the relay has the blob in `file` (HEAD first; nothing is sent when it does). */
    void upload (const std::string& hash, const std::filesystem::path& file);
    /** Downloads the blob to `dest` via `<partDir>/<hex>.part`. Lower priority goes first. */
    void download (const std::string& hash, const std::filesystem::path& dest, const std::filesystem::path& partDir, std::int64_t priority = 0);
    /** Reorders queued downloads. */
    void setPriority (const std::string& hash, std::int64_t priority);
    /** BLOB_AVAILABLE: a download waiting for this blob tries again now. */
    void blobAvailable (const std::string& hash);
    /** Uploads pause while recording (D-COL-10); a chunk in flight finishes. */
    void setUploadsPaused (bool paused);
    void setAccount (const Account& account);

    /** What happened since the last call. Caller's thread. */
    std::vector<TransferEvent> poll();

    bool busy (const std::string& hash, bool upload) const;
    int activeUploads() const;
    int activeDownloads() const;
    /** Uploads and downloads not finished (queued, running or waiting). */
    int pendingUploads() const;
    int pendingDownloads() const;

private:
    struct Job
    {
        std::string hash;
        bool upload = false;
        std::filesystem::path file, partDir;
        std::int64_t priority = 0;
        bool waiting = false;   // downloads: the relay doesn't have it yet
        std::int64_t retryAt = 0; // steady ms
        int attempts = 0;       // hash mismatches so far
        int failures = 0;       // network errors in a row
    };
    void worker (bool uploads);
    bool takeJob (bool uploads, Job& out);
    void finish (const Job& job);
    void requeue (Job job, std::int64_t delayMs);
    void emit (TransferEvent e);
    HttpResult call (std::string_view method, const std::string& path, HttpBody body);
    enum class Outcome
    {
        done,
        retry,   // network trouble: try again after a backoff
        waiting, // downloads: not on the relay yet
        stopped, // a test hook stopped it, or shutting down
        failed
    };
    Outcome runUpload (Job& job, std::string& error);
    Outcome runDownload (Job& job, std::string& error);
    std::string blobPath (const std::string& hash) const;
    bool stopping() const;

    TransferOptions options;
    mutable std::mutex lock;
    std::condition_variable wake;
    std::deque<Job> uploads, downloads;
    std::set<std::string> runningUploads, runningDownloads;
    std::vector<TransferEvent> events;
    bool paused = false;
    bool stop = false;
    std::vector<std::thread> threads;
};
} // namespace weft::collab
