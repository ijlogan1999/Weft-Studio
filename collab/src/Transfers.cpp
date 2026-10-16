#include "weft/collab/Transfers.h"

#include "weft/media/Hash.h"

#include <algorithm>
#include <chrono>
#include <fstream>

namespace weft::collab
{
namespace fs = std::filesystem;

namespace
{
std::int64_t steadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string hexOf (const std::string& hash) { return hash.starts_with ("b3:") ? hash.substr (3) : hash; }

std::int64_t fileSize (const fs::path& p)
{
    std::error_code ec;
    const auto n = fs::file_size (p, ec);
    return ec ? -1 : static_cast<std::int64_t> (n);
}

std::optional<Bytes> readSpan (const fs::path& p, std::int64_t offset, std::int64_t len)
{
    std::ifstream f (p, std::ios::binary);
    if (! f)
        return std::nullopt;
    f.seekg (offset);
    Bytes out (static_cast<std::size_t> (len));
    if (! f.read (reinterpret_cast<char*> (out.data()), len))
        return std::nullopt;
    return out;
}

/** "bytes a-b/total" -> total. */
std::int64_t rangeTotal (const std::string& contentRange)
{
    const auto slash = contentRange.rfind ('/');
    if (slash == std::string::npos)
        return -1;
    try
    {
        return std::stoll (contentRange.substr (slash + 1));
    }
    catch (...)
    {
        return -1;
    }
}
} // namespace

Transfers::Transfers (TransferOptions o) : options (std::move (o))
{
    for (int i = 0; i < std::max (1, options.maxUploads); ++i)
        threads.emplace_back ([this] { worker (true); });
    for (int i = 0; i < std::max (1, options.maxDownloads); ++i)
        threads.emplace_back ([this] { worker (false); });
}

Transfers::~Transfers()
{
    {
        const std::scoped_lock l (lock);
        stop = true;
    }
    wake.notify_all();
    for (auto& t : threads)
        t.join();
}

bool Transfers::stopping() const
{
    const std::scoped_lock l (lock);
    return stop;
}

void Transfers::upload (const std::string& hash, const fs::path& file)
{
    {
        const std::scoped_lock l (lock);
        if (runningUploads.contains (hash) || std::any_of (uploads.begin(), uploads.end(), [&] (const Job& j) { return j.hash == hash; }))
            return;
        Job j;
        j.hash = hash;
        j.upload = true;
        j.file = file;
        uploads.push_back (std::move (j));
    }
    wake.notify_all();
}

void Transfers::download (const std::string& hash, const fs::path& dest, const fs::path& partDir, std::int64_t priority)
{
    {
        const std::scoped_lock l (lock);
        if (runningDownloads.contains (hash))
            return;
        for (auto& j : downloads)
            if (j.hash == hash)
            {
                j.priority = priority;
                return;
            }
        Job j;
        j.hash = hash;
        j.file = dest;
        j.partDir = partDir;
        j.priority = priority;
        downloads.push_back (std::move (j));
    }
    wake.notify_all();
}

void Transfers::setPriority (const std::string& hash, std::int64_t priority)
{
    const std::scoped_lock l (lock);
    for (auto& j : downloads)
        if (j.hash == hash)
            j.priority = priority;
}

void Transfers::blobAvailable (const std::string& hash)
{
    {
        const std::scoped_lock l (lock);
        for (auto& j : downloads)
            if (j.hash == hash)
            {
                j.waiting = false;
                j.retryAt = 0;
            }
    }
    wake.notify_all();
}

void Transfers::setUploadsPaused (bool p)
{
    {
        const std::scoped_lock l (lock);
        paused = p;
    }
    wake.notify_all();
}

void Transfers::setAccount (const Account& account)
{
    const std::scoped_lock l (lock);
    options.account = account;
}

std::vector<TransferEvent> Transfers::poll()
{
    const std::scoped_lock l (lock);
    return std::exchange (events, {});
}

bool Transfers::busy (const std::string& hash, bool upload) const
{
    const std::scoped_lock l (lock);
    const auto& running = upload ? runningUploads : runningDownloads;
    const auto& queue = upload ? uploads : downloads;
    return running.contains (hash) || std::any_of (queue.begin(), queue.end(), [&] (const Job& j) { return j.hash == hash; });
}

int Transfers::activeUploads() const
{
    const std::scoped_lock l (lock);
    return static_cast<int> (runningUploads.size());
}

int Transfers::activeDownloads() const
{
    const std::scoped_lock l (lock);
    return static_cast<int> (runningDownloads.size());
}

int Transfers::pendingUploads() const
{
    const std::scoped_lock l (lock);
    return static_cast<int> (runningUploads.size() + uploads.size());
}

int Transfers::pendingDownloads() const
{
    const std::scoped_lock l (lock);
    return static_cast<int> (runningDownloads.size() + downloads.size());
}

void Transfers::emit (TransferEvent e)
{
    const std::scoped_lock l (lock);
    // Progress replaces the previous progress of the same transfer, so a slow caller isn't flooded.
    if (e.kind == TransferEvent::Kind::progress)
        for (auto& old : events)
            if (old.kind == TransferEvent::Kind::progress && old.hash == e.hash && old.upload == e.upload)
            {
                old = std::move (e);
                return;
            }
    events.push_back (std::move (e));
}

bool Transfers::takeJob (bool up, Job& out)
{
    std::unique_lock l (lock);
    for (;;)
    {
        if (stop)
            return false;
        auto& queue = up ? uploads : downloads;
        const auto now = steadyMs();
        std::int64_t nextRetry = INT64_MAX;
        if (! (up && paused))
        {
            auto best = queue.end();
            for (auto it = queue.begin(); it != queue.end(); ++it)
            {
                if (it->retryAt > now)
                {
                    nextRetry = std::min (nextRetry, it->retryAt);
                    continue;
                }
                if (best == queue.end() || it->priority < best->priority)
                    best = it;
            }
            if (best != queue.end())
            {
                out = std::move (*best);
                queue.erase (best);
                (up ? runningUploads : runningDownloads).insert (out.hash);
                return true;
            }
        }
        if (nextRetry == INT64_MAX)
            wake.wait (l);
        else
            wake.wait_for (l, std::chrono::milliseconds (nextRetry - now));
    }
}

void Transfers::finish (const Job& job)
{
    const std::scoped_lock l (lock);
    (job.upload ? runningUploads : runningDownloads).erase (job.hash);
}

void Transfers::requeue (Job job, std::int64_t delayMs)
{
    {
        const std::scoped_lock l (lock);
        (job.upload ? runningUploads : runningDownloads).erase (job.hash);
        job.retryAt = steadyMs() + delayMs;
        (job.upload ? uploads : downloads).push_back (std::move (job));
    }
    wake.notify_all();
}

void Transfers::worker (bool up)
{
    Job job;
    while (takeJob (up, job))
    {
        std::string error;
        const auto outcome = up ? runUpload (job, error) : runDownload (job, error);
        switch (outcome)
        {
            case Outcome::done:
            case Outcome::stopped:
                finish (job);
                break;
            case Outcome::retry:
            {
                ++job.failures;
                const auto seconds = std::min (options.maxBackoffSeconds, 1 << std::min (job.failures - 1, 10));
                requeue (std::move (job), seconds * 1000LL);
                break;
            }
            case Outcome::waiting:
            {
                job.waiting = true;
                job.failures = 0;
                TransferEvent e;
                e.kind = TransferEvent::Kind::waiting;
                e.hash = job.hash;
                emit (std::move (e));
                requeue (std::move (job), options.waitingRetrySeconds * 1000LL);
                break;
            }
            case Outcome::failed:
            {
                finish (job);
                TransferEvent e;
                e.kind = TransferEvent::Kind::failed;
                e.upload = up;
                e.hash = job.hash;
                e.error = error;
                emit (std::move (e));
                break;
            }
        }
    }
}

std::string Transfers::blobPath (const std::string& hash) const { return "/v1/blobs/" + hash; }

HttpResult Transfers::call (std::string_view method, const std::string& path, HttpBody body)
{
    Account account;
    {
        const std::scoped_lock l (lock);
        account = options.account;
    }
    const auto sep = path.find ('?') == std::string::npos ? "?" : "&";
    const auto url = account.relayUrl + path + sep + "project=" + options.projectId;
    auto r = httpRequest (method, url, account.accessToken, body);
    if (r.status == 401)
    {
        RelayApi api (account);
        if (api.refresh())
        {
            {
                const std::scoped_lock l (lock);
                options.account = account;
            }
            TransferEvent e;
            e.kind = TransferEvent::Kind::tokens;
            e.account = account;
            emit (std::move (e));
            r = httpRequest (method, url, account.accessToken, body);
        }
    }
    return r;
}

Transfers::Outcome Transfers::runUpload (Job& job, std::string& error)
{
    const auto size = fileSize (job.file);
    if (size < 0)
    {
        error = "The audio file to upload is missing.";
        return Outcome::failed;
    }
    auto done = [&] (std::int64_t resumedFrom, bool dedup) {
        TransferEvent e;
        e.kind = TransferEvent::Kind::uploaded;
        e.upload = true;
        e.hash = job.hash;
        e.done = e.total = size;
        e.resumedFrom = resumedFrom;
        e.deduplicated = dedup;
        emit (std::move (e));
        return Outcome::done;
    };
    auto trouble = [&] (const HttpResult& r) {
        if (r.status == 0 || r.status >= 500)
            return Outcome::retry;
        error = r.error.empty() ? "The relay refused the upload." : r.error;
        return Outcome::failed;
    };

    // 05 §8.2 step 2: nothing to send when the relay has it already.
    const auto head = call ("HEAD", blobPath (job.hash), {});
    if (head.status == 200)
        return done (0, true);
    if (head.status != 404)
        return trouble (head);

    HttpBody start;
    start.json = Value::map().set ("size", size);
    const auto started = call ("POST", blobPath (job.hash) + "/uploads", start);
    if (! started.ok())
        return trouble (started);
    if (started.json["complete"].asBool())
        return done (0, true);
    const auto id = started.json["uploadId"].asString();
    auto offset = started.json["offset"].asInt();
    const auto resumedFrom = offset;
    while (offset < size)
    {
        {
            std::unique_lock l (lock);
            wake.wait (l, [&] { return stop || ! paused; });
            if (stop)
                return Outcome::stopped;
        }
        if (options.beforeChunk && ! options.beforeChunk (job.hash, offset))
            return Outcome::stopped;
        const auto n = std::min (options.uploadChunk, size - offset);
        auto chunk = readSpan (job.file, offset, n);
        if (! chunk)
        {
            error = "The audio file couldn't be read for uploading.";
            return Outcome::failed;
        }
        HttpBody body;
        body.bytes = std::move (*chunk);
        body.timeoutMs = 120000;
        const auto put = call ("PUT", blobPath (job.hash) + "/uploads/" + id + "?offset=" + std::to_string (offset), std::move (body));
        if (put.status == 409 || put.ok())
            offset = put.json["offset"].asInt (offset);
        else
            return trouble (put);
        job.failures = 0;
        TransferEvent e;
        e.kind = TransferEvent::Kind::progress;
        e.upload = true;
        e.hash = job.hash;
        e.done = offset;
        e.total = size;
        e.resumedFrom = resumedFrom;
        emit (std::move (e));
    }
    HttpBody body;
    body.timeoutMs = 120000;
    const auto completed = call ("POST", blobPath (job.hash) + "/uploads/" + id + "/complete", std::move (body));
    if (! completed.ok())
        return trouble (completed);
    return done (resumedFrom, false);
}

Transfers::Outcome Transfers::runDownload (Job& job, std::string& error)
{
    std::error_code ec;
    fs::create_directories (job.partDir, ec);
    fs::create_directories (job.file.parent_path(), ec);
    const auto part = job.partDir / (hexOf (job.hash) + ".part");
    auto offset = std::max<std::int64_t> (0, fileSize (part));
    std::int64_t total = -1;
    for (;;)
    {
        if (stopping())
            return Outcome::stopped;
        HttpBody body;
        body.headers.emplace_back ("range", "bytes=" + std::to_string (offset) + "-" + std::to_string (offset + options.downloadChunk - 1));
        body.timeoutMs = 120000;
        auto r = call ("GET", blobPath (job.hash), std::move (body));
        if (r.status == 404)
            return Outcome::waiting;
        if (r.status == 416)
        {
            total = rangeTotal (r.header ("content-range"));
            if (total >= 0 && offset >= total)
            {
                if (offset > total)
                    fs::resize_file (part, static_cast<std::uintmax_t> (total), ec);
                break;
            }
            // The partial file doesn't fit the blob: start over.
            fs::remove (part, ec);
            offset = 0;
            continue;
        }
        if (r.status != 206 && r.status != 200)
        {
            if (r.status == 0 || r.status >= 500)
                return Outcome::retry;
            error = r.error.empty() ? "The relay refused the download." : r.error;
            return Outcome::failed;
        }
        total = r.status == 206 ? rangeTotal (r.header ("content-range")) : static_cast<std::int64_t> (r.bytes.size());
        if (r.status == 200)
            offset = 0; // the whole file
        if (options.tamper)
            options.tamper (job.hash, r.bytes);
        {
            std::ofstream f (part, std::ios::binary | (offset == 0 ? std::ios::trunc : std::ios::app));
            f.write (reinterpret_cast<const char*> (r.bytes.data()), static_cast<std::streamsize> (r.bytes.size()));
            if (! f)
            {
                error = "Couldn't write the downloaded audio to disk.";
                return Outcome::failed;
            }
        }
        offset += static_cast<std::int64_t> (r.bytes.size());
        job.failures = 0;
        TransferEvent e;
        e.kind = TransferEvent::Kind::progress;
        e.hash = job.hash;
        e.done = offset;
        e.total = total;
        e.file = part;
        emit (std::move (e));
        if (total >= 0 && offset >= total)
            break;
        if (r.bytes.empty())
            return Outcome::retry;
    }

    // 05 §8.3 step 4: verify, else discard and retry once, then report.
    const auto actual = media::hashFile (part);
    if (! actual || *actual != job.hash)
    {
        fs::remove (part, ec);
        if (++job.attempts < 2)
            return runDownload (job, error);
        error = "The downloaded audio didn't match its hash, twice.";
        return Outcome::failed;
    }
    fs::rename (part, job.file, ec);
    if (ec)
    {
        fs::copy_file (part, job.file, fs::copy_options::overwrite_existing, ec);
        fs::remove (part, ec);
    }
    TransferEvent e;
    e.kind = TransferEvent::Kind::downloaded;
    e.hash = job.hash;
    e.done = e.total = total;
    e.file = job.file;
    emit (std::move (e));
    return Outcome::done;
}
} // namespace weft::collab
