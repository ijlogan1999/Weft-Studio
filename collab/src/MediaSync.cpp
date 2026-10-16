#include "weft/collab/MediaSync.h"

#include "weft/media/Blobs.h"
#include "weft/media/WavFile.h"
#include "weft/project/ProjectFiles.h"

#include <algorithm>
#include <limits>

namespace weft::collab
{
namespace fs = std::filesystem;

namespace
{
/** The plug-in state blobs a strip refers to (states over 64 KB, 03 §5.2). */
void stateBlobs (const model::Track& t, std::set<std::string>& out)
{
    auto add = [&] (const model::PluginInstance& p) {
        if (p.state.isString() && p.state.asString().starts_with ("b3:"))
            out.insert (p.state.asString());
    };
    if (t.strip.instrument)
        add (*t.strip.instrument);
    for (const auto& p : t.strip.inserts)
        add (p);
}
} // namespace

MediaSync::MediaSync (commands::Session& s, MediaSyncOptions o) : session (s), options (std::move (o))
{
    auto t = options.transfer;
    t.account = options.account;
    t.projectId = options.projectId;
    transfer = std::make_unique<Transfers> (std::move (t));
}

std::optional<fs::path> MediaSync::localFile (const std::string& hash, bool media) const
{
    if (! media)
    {
        const auto f = media::blobFile (blobDir(), hash);
        return fs::exists (f) ? std::optional (f) : std::nullopt;
    }
    return project::findMediaFile (project::mediaFolders (options.projectRoot), hash);
}

std::map<std::string, MediaSync::Wanted> MediaSync::referenced() const
{
    std::map<std::string, Wanted> out;
    const auto& m = session.model();
    for (const auto& [id, r] : m.regions)
    {
        if (r->isMidi || r->audio.media.empty())
            continue;
        // 05 §8.3 step 2: near any playhead first, then timeline order.
        const auto end = static_cast<Ticks> (m.regionEndTick (*r));
        std::int64_t priority = heads.empty() ? r->start : std::numeric_limits<std::int64_t>::max();
        for (const auto h : heads)
            priority = std::min<std::int64_t> (priority, h >= r->start && h < end ? 0 : std::min (std::abs (r->start - h), std::abs (end - h)));
        auto [it, added] = out.try_emplace (r->audio.media, Wanted { true, priority });
        if (! added)
            it->second.priority = std::min (it->second.priority, priority);
    }
    std::set<std::string> blobs;
    for (const auto& [id, t] : m.tracks)
        stateBlobs (*t, blobs);
    for (const auto& h : blobs)
        out.try_emplace (h, Wanted { false, std::numeric_limits<std::int64_t>::max() / 2 });
    return out;
}

void MediaSync::refresh()
{
    if (role.empty())
        return;
    const bool mayUpload = role == "owner" || role == "editor";
    const auto partDir = options.projectRoot / "Cache" / "Downloads";
    for (const auto& [hash, want] : referenced())
    {
        if (const auto file = localFile (hash, want.media))
        {
            if (mayUpload && ! uploaded.contains (hash) && ! transfer->busy (hash, true))
            {
                transfer->upload (hash, *file);
                pendingUploads.try_emplace (hash, std::pair<std::int64_t, std::int64_t> { 0, static_cast<std::int64_t> (fs::file_size (*file)) });
            }
            continue;
        }
        if (failedDownloads.contains (hash))
            continue;
        const auto dest = want.media ? mediaDir() / media::mediaFileName (hash) : media::blobFile (blobDir(), hash);
        if (transfer->busy (hash, false))
            transfer->setPriority (hash, want.priority);
        else
            transfer->download (hash, dest, partDir, want.priority);
        auto& d = pendingDownloads[hash];
        if (const auto* ref = session.model().mediaRef (hash); ref != nullptr && d.total == 0)
            d.total = ref->sizeBytes;
    }
}

void MediaSync::poll()
{
    for (auto& e : transfer->poll())
    {
        if (onTransfer)
            onTransfer (e);
        using K = TransferEvent::Kind;
        switch (e.kind)
        {
            case K::progress:
                if (e.upload)
                    pendingUploads[e.hash] = { e.done, e.total };
                else
                {
                    auto& d = pendingDownloads[e.hash];
                    d.done = e.done;
                    d.total = e.total;
                    d.waiting = false;
                    d.partFile = e.file;
                    if (onDownloadProgress)
                        onDownloadProgress (e.hash, d);
                }
                break;
            case K::waiting:
                pendingDownloads[e.hash].waiting = true;
                break;
            case K::uploaded:
                pendingUploads.erase (e.hash);
                uploaded.insert (e.hash);
                if (onUploaded)
                    onUploaded (e.hash);
                break;
            case K::downloaded:
                pendingDownloads.erase (e.hash);
                if (onDownloaded)
                    onDownloaded (e.hash, e.file);
                break;
            case K::failed:
                if (e.upload)
                {
                    pendingUploads.erase (e.hash);
                    if (onMessage)
                        onMessage ("Couldn't upload audio: " + e.error);
                }
                else
                {
                    pendingDownloads[e.hash].error = e.error;
                    failedDownloads.insert (e.hash);
                    if (onMessage)
                        onMessage ("Couldn't download audio: " + e.error);
                }
                break;
            case K::tokens:
                options.account = e.account;
                if (onAccountChange)
                    onAccountChange (e.account);
                break;
        }
    }
}

void MediaSync::blobAvailable (const std::string& hash)
{
    if (auto it = pendingDownloads.find (hash); it != pendingDownloads.end())
        it->second.waiting = false;
    transfer->blobAvailable (hash);
}

void MediaSync::setRole (const std::string& r)
{
    if (r == role)
        return;
    role = r;
    refresh();
}

void MediaSync::setRecording (bool recording) { transfer->setUploadsPaused (recording); }

void MediaSync::setAccount (const Account& account)
{
    options.account = account;
    transfer->setAccount (account);
}

void MediaSync::setPlayheads (std::vector<Ticks> playheads)
{
    std::sort (playheads.begin(), playheads.end());
    if (playheads == heads)
        return;
    heads = std::move (playheads);
    for (const auto& [hash, want] : referenced())
        if (pendingDownloads.contains (hash))
            transfer->setPriority (hash, want.priority);
}

std::string MediaSync::statusText() const
{
    const auto ups = static_cast<int> (pendingUploads.size());
    int downs = 0;
    for (const auto& [h, d] : pendingDownloads)
        if (d.error.empty())
            ++downs;
    std::string out;
    if (ups > 0)
        out += "Uploading " + std::to_string (ups);
    if (downs > 0)
        out += (out.empty() ? "" : " \xc2\xb7 ") + std::string ("Downloading ") + std::to_string (downs);
    return out;
}

std::int64_t localMediaBytes (const model::ProjectModel& model, const fs::path& projectRoot)
{
    std::set<std::string> hashes;
    for (const auto& [id, r] : model.regions)
        if (! r->isMidi && ! r->audio.media.empty())
            hashes.insert (r->audio.media);
    const auto folders = project::mediaFolders (projectRoot);
    std::int64_t total = 0;
    for (const auto& h : hashes)
        if (const auto f = project::findMediaFile (folders, h))
        {
            std::error_code ec;
            const auto n = fs::file_size (*f, ec);
            if (! ec)
                total += static_cast<std::int64_t> (n);
        }
    return total;
}
} // namespace weft::collab
