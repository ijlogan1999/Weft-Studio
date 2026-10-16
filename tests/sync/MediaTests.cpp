// [S] audio file sync (spec 05 §8, M11): uploads, downloads, resume, verification and
// access through a real weft-relay, with each peer's MediaSync working in its own
// project folder. AT-COL-14 to AT-COL-17. (AT-COL-13 is a unit test; AT-COL-18 is an
// engine test; the relay's own side is also covered in relay/tests/relay.rs.)

#include "SyncFixtures.h"

#include "weft/collab/MediaSync.h"
#include "weft/engine/devices/NullDevice.h"
#include "weft/engine/media/MediaPool.h"
#include "weft/engine/transport/Engine.h"
#include "weft/media/Hash.h"
#include "weft/media/MediaStore.h"

#include <atomic>
#include <cmath>

using namespace weft;
using namespace weft::test;
using collab::TransferEvent;

namespace
{
constexpr double rate = 48000.0;

/** One peer's media side: its project folder and MediaSync, wired to its SyncClient. */
struct Side
{
    Peer& peer;
    TempDir root;
    collab::MediaSync media;
    std::vector<TransferEvent> events;
    std::vector<std::string> messages;

    static collab::MediaSyncOptions options (const Peer& p, const fs::path& root, collab::TransferOptions t)
    {
        collab::MediaSyncOptions o;
        o.account = p.account;
        o.projectId = p.client->projectId();
        o.projectRoot = root;
        o.transfer = std::move (t);
        return o;
    }

    Side (Peer& p, collab::TransferOptions t = {}) : peer (p), media (*p.session, options (p, root.path, std::move (t)))
    {
        p.client->onBlobAvailable = [this] (const std::string& hash, std::int64_t) { media.blobAvailable (hash); };
        p.client->onJoined = [this] { media.setRole (peer.client->role()); };
        media.onTransfer = [this] (const TransferEvent& e) { events.push_back (e); };
        media.onMessage = [this] (const std::string& m) { messages.push_back (m); };
        if (! p.client->role().empty())
            media.setRole (p.client->role());
    }

    fs::path mediaFile (const std::string& hash) const { return root.path / "Media" / media::mediaFileName (hash); }

    int count (TransferEvent::Kind kind, const std::string& hash) const
    {
        return static_cast<int> (std::count_if (events.begin(), events.end(), [&] (const TransferEvent& e) { return e.kind == kind && e.hash == hash; }));
    }
    const TransferEvent* last (TransferEvent::Kind kind, const std::string& hash) const
    {
        for (auto it = events.rbegin(); it != events.rend(); ++it)
            if (it->kind == kind && it->hash == hash)
                return &*it;
        return nullptr;
    }
};

/** Polls everyone (sync and transfers), looking at the document again every 100 ms. */
void pumpMedia (const std::vector<Side*>& sides, const std::string& what, const std::function<bool()>& done, int seconds = 60)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (seconds);
    auto nextRefresh = std::chrono::steady_clock::now();
    for (;;)
    {
        const auto now = std::chrono::steady_clock::now();
        for (auto* s : sides)
        {
            s->peer.client->poll();
            if (now >= nextRefresh)
                s->media.refresh();
            s->media.poll();
        }
        if (now >= nextRefresh)
            nextRefresh = now + std::chrono::milliseconds (100);
        if (done())
            return;
        if (std::chrono::steady_clock::now() >= deadline)
        {
            std::string state;
            for (auto* s : sides)
                state += s->peer.name + ": " + s->media.statusText() + (s->messages.empty() ? "" : " / " + s->messages.back()) + "\n";
            INFO ("waiting for " << what << "\n" << state);
            FAIL();
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
}

/** Mono audio at a constant level: easy to recognise in a render. */
media::AudioData level (std::int64_t frames, float value)
{
    media::AudioData a;
    a.sampleRate = rate;
    a.channels.assign (1, std::vector<float> (static_cast<std::size_t> (frames), value));
    // A little variation, so two recordings of the same length don't share a hash.
    for (std::size_t i = 0; i < a.channels[0].size(); i += 4801)
        a.channels[0][i] = value * 0.5f;
    return a;
}

/** What recording or importing does: the WAV in Media/, its MediaRef, and a region on a new stereo track. */
std::string addRecording (Side& s, std::int64_t frames, float value, Id* regionOut = nullptr, Ticks start = 0)
{
    const auto hash = media::storeMedia (level (frames, value), media::SampleFormat::int24, s.root.path / "Media");
    REQUIRE (hash);
    model::MediaRef ref;
    ref.hash = *hash;
    ref.originalName = "take.wav";
    ref.sampleRate = static_cast<int> (rate);
    ref.channels = 1;
    ref.lengthSamples = frames;
    ref.sizeBytes = static_cast<std::int64_t> (fs::file_size (s.mediaFile (*hash)));
    REQUIRE (s.peer.s().addMedia (ref));
    const auto track = s.peer.s().createTrack (model::TrackKind::audio, "Take", 2);
    REQUIRE (track);
    const auto region = s.peer.s().createAudioRegion (*track, start, *hash, 0, frames);
    REQUIRE (region);
    if (regionOut != nullptr)
        *regionOut = *region;
    return *hash;
}

/** An engine on a NullDevice playing a peer's document from its own media pool. */
struct Player
{
    engine::MediaPool pool;
    engine::InstrumentRack rack;
    engine::Engine engine { pool, rack };
    engine::NullDevice device;

    Player()
    {
        engine::TransportSettings quiet;
        quiet.metronomeOn = false;
        engine.setSettings (quiet);
        engine::DeviceConfig c;
        c.sampleRate = rate;
        c.maxBlockSize = 256;
        c.numInputChannels = 0;
        c.numOutputChannels = 2;
        REQUIRE (device.open (c));
        REQUIRE (device.start (engine));
    }

    /** Plays `frames` from the start; returns the left channel. */
    std::vector<float> play (const model::ProjectModel& m, std::int64_t frames)
    {
        engine.setProject (m);
        engine.play (0);
        device.setCaptureCapacity (frames);
        REQUIRE (device.render (frames));
        const auto out = device.capturedOutput (0);
        engine.stop();
        device.render (512);
        return { out.begin(), out.end() };
    }
};

float peakOf (const std::vector<float>& v, std::int64_t from, std::int64_t to)
{
    float p = 0;
    for (auto i = std::max<std::int64_t> (0, from); i < std::min<std::int64_t> (to, static_cast<std::int64_t> (v.size())); ++i)
        p = std::max (p, std::abs (v[static_cast<std::size_t> (i)]));
    return p;
}
} // namespace

TEST_CASE ("AT-COL-14 a recording reaches the other client before its audio, which then downloads, verifies and plays; the owner's next project needs no upload",
           "[S][sync][media][AT-COL-14]")
{
    RelayProcess relay;
    SharedProject project (relay);
    auto b = project.join ("Ben");

    std::atomic<bool> uploadAllowed { false };
    collab::TransferOptions held;
    held.beforeChunk = [&] (const std::string&, std::int64_t) { return uploadAllowed.load(); };
    Side a (*project.owner, held);
    Side sb (*b);

    // A records 2 s; the upload is held back, the document change isn't.
    Id region;
    const auto hash = addRecording (a, 2 * static_cast<std::int64_t> (rate), 0.5f, &region);
    pumpMedia ({ &a, &sb }, "the region on B", [&] { return b->m().region (region) != nullptr; });
    CHECK (b->m().mediaRef (hash) != nullptr);
    pumpMedia ({ &a, &sb }, "B waiting for the upload", [&] { return sb.media.downloads().contains (hash) && sb.media.downloads().at (hash).waiting; });
    CHECK_FALSE (fs::exists (sb.mediaFile (hash)));
    CHECK (sb.media.statusText() == "Downloading 1");
    CHECK (a.media.uploads().contains (hash));

    // B's engine: the region is there but silent; nothing counts as a disk problem.
    Player player;
    auto out = player.play (b->m(), static_cast<std::int64_t> (rate));
    CHECK (peakOf (out, 0, static_cast<std::int64_t> (rate)) == 0.0f);
    CHECK (player.engine.diskUnderruns() == 0);

    // The upload goes through: BLOB_AVAILABLE, then B downloads and checks the hash.
    uploadAllowed = true;
    pumpMedia ({ &a, &sb }, "the download", [&] { return sb.count (TransferEvent::Kind::downloaded, hash) == 1; });
    CHECK (a.count (TransferEvent::Kind::uploaded, hash) == 1);
    CHECK_FALSE (a.last (TransferEvent::Kind::uploaded, hash)->deduplicated);
    REQUIRE (fs::exists (sb.mediaFile (hash)));
    CHECK (media::hashFile (sb.mediaFile (hash)) == hash);
    CHECK (sb.media.downloads().empty());
    CHECK (sb.media.statusText().empty());
    CHECK (sb.peer.client->statusText() == "Synced");

    // Playback includes the audio now.
    REQUIRE (player.pool.load (sb.root.path / "Media", hash));
    out = player.play (b->m(), static_cast<std::int64_t> (rate));
    CHECK (peakOf (out, 4800, 48000) > 0.45f);
    CHECK (player.engine.diskUnderruns() == 0);

    // A second project of the same owner using the same recording: the relay already has it.
    auto song2 = Session::createNew ({}, project.ownerAccount.userId);
    REQUIRE (song2);
    const auto song2Id = song2->model().info.id;
    REQUIRE (RelayApi (project.ownerAccount).shareProject (song2Id, "Song 2"));
    Peer owner2 ("Owner (Song 2)", project.ownerAccount, std::move (*song2), song2Id);
    pumpUntil ({ &owner2 }, "song 2 synced", [&] { return owner2.synced(); });
    Side a2 (owner2);
    fs::create_directories (a2.root.path / "Media");
    fs::copy_file (a.mediaFile (hash), a2.mediaFile (hash));
    model::MediaRef ref = *project.owner->m().mediaRef (hash);
    REQUIRE (owner2.s().addMedia (ref));
    const auto t2 = owner2.s().createTrack (model::TrackKind::audio, "Take", 2);
    REQUIRE (owner2.s().createAudioRegion (*t2, 0, hash, 0, ref.lengthSamples));
    pumpMedia ({ &a2 }, "song 2's upload", [&] { return a2.count (TransferEvent::Kind::uploaded, hash) == 1; });
    CHECK (a2.last (TransferEvent::Kind::uploaded, hash)->deduplicated);
    CHECK (a2.count (TransferEvent::Kind::progress, hash) == 0); // not a byte sent
}

TEST_CASE ("AT-COL-15 an interrupted upload resumes from the last 8 MB chunk; a download that doesn't match its hash is retried once, then reported",
           "[S][sync][media][AT-COL-15]")
{
    RelayProcess relay;
    SharedProject project (relay);
    auto b = project.join ("Ben");

    // A 10 MB recording; the app "quits" just before the second chunk.
    std::atomic<bool> stopAtSecond { true };
    collab::TransferOptions quitting;
    quitting.beforeChunk = [&] (const std::string&, std::int64_t offset) { return ! (offset == (8 << 20) && stopAtSecond.exchange (false)); };
    std::atomic<std::int64_t> tamperedBytes { 0 };
    collab::TransferOptions corrupt;
    corrupt.tamper = [&] (const std::string&, collab::Bytes& data) {
        tamperedBytes += static_cast<std::int64_t> (data.size());
        if (! data.empty())
            data[data.size() / 2] ^= 0x5a;
    };
    Side a (*project.owner, quitting);
    Side sb (*b, corrupt);

    const auto frames = static_cast<std::int64_t> (73 * rate); // 73 s of 24-bit mono: just over 10 MB
    const auto hash = addRecording (a, frames, 0.25f);
    const auto size = static_cast<std::int64_t> (fs::file_size (a.mediaFile (hash)));
    REQUIRE (size > (8 << 20));
    REQUIRE (size < (16 << 20));

    // The first attempt stops after one chunk; the next resumes from 8 MB.
    pumpMedia ({ &a, &sb }, "the upload", [&] { return a.count (TransferEvent::Kind::uploaded, hash) == 1; });
    const auto* uploaded = a.last (TransferEvent::Kind::uploaded, hash);
    CHECK (uploaded->resumedFrom == (8 << 20));
    CHECK_FALSE (uploaded->deduplicated);
    CHECK (a.media.uploads().empty());

    // Every download B makes is corrupted: it tries twice in all, then says so.
    pumpMedia ({ &a, &sb }, "the failure on B", [&] { return sb.count (TransferEvent::Kind::failed, hash) == 1; });
    CHECK (tamperedBytes == 2 * size);
    CHECK (sb.count (TransferEvent::Kind::downloaded, hash) == 0);
    CHECK_FALSE (fs::exists (sb.mediaFile (hash)));
    CHECK_FALSE (fs::exists (sb.root.path / "Cache" / "Downloads" / (hash.substr (3) + ".part")));
    REQUIRE_FALSE (sb.messages.empty());
    CHECK (sb.messages.back().starts_with ("Couldn't download audio"));
    CHECK (sb.media.downloads().at (hash).error.size() > 0);
    CHECK (sb.media.statusText().empty()); // failed downloads aren't counted as moving

    // A client whose first download is corrupted gets it right on the retry.
    auto c = project.join ("Cleo");
    std::atomic<int> corrupted { 0 };
    collab::TransferOptions once;
    once.tamper = [&] (const std::string&, collab::Bytes& data) {
        if (corrupted.fetch_add (1) == 0 && ! data.empty())
            data[0] ^= 0xff;
    };
    Side sc (*c, once);
    pumpMedia ({ &a, &sc }, "C's download", [&] { return sc.count (TransferEvent::Kind::downloaded, hash) == 1; });
    CHECK (sc.count (TransferEvent::Kind::failed, hash) == 0);
    CHECK (media::hashFile (sc.mediaFile (hash)) == hash);
}

TEST_CASE ("AT-COL-16 someone who isn't a member gets 403 for a project's blob, even with a valid token", "[S][sync][media][AT-COL-16]")
{
    RelayProcess relay;
    SharedProject project (relay);
    Side a (*project.owner);
    const auto hash = addRecording (a, static_cast<std::int64_t> (rate / 2), 0.5f);
    pumpMedia ({ &a }, "the upload", [&] { return a.count (TransferEvent::Kind::uploaded, hash) == 1; });

    const auto d = signUp (relay, "Dee");
    const auto url = relay.url() + "/v1/blobs/" + hash + "?project=" + project.projectId;
    CHECK (collab::httpRequest ("GET", url, d.accessToken, doc::Value()).status == 403);
    CHECK (collab::httpRequest ("HEAD", url, d.accessToken, doc::Value()).status == 403);
    CHECK (collab::httpRequest ("POST", relay.url() + "/v1/blobs/" + hash + "/uploads?project=" + project.projectId, d.accessToken,
                                doc::Value::map().set ("size", std::int64_t { 10 }))
               .status
           == 403);
    // The owner's token is fine (the blob is there).
    CHECK (collab::httpRequest ("HEAD", url, project.owner->client->account().accessToken, doc::Value()).status == 200);

    // D's transfer engine gives up rather than retrying a refusal.
    collab::TransferOptions o;
    o.account = d;
    o.projectId = project.projectId;
    collab::Transfers transfers (o);
    TempDir dir;
    transfers.download (hash, dir.path / "x.wav", dir.path);
    std::vector<TransferEvent> seen;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (20);
    while (std::none_of (seen.begin(), seen.end(), [] (const TransferEvent& e) { return e.kind == TransferEvent::Kind::failed; }))
    {
        REQUIRE (std::chrono::steady_clock::now() < deadline);
        for (auto& e : transfers.poll())
            seen.push_back (e);
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    CHECK_FALSE (fs::exists (dir.path / "x.wav"));
}

TEST_CASE ("AT-COL-17 half a download plays its prefix and is silent after; progress and transfer counts show; uploads pause while recording",
           "[S][E][sync][media][AT-COL-17]")
{
    RelayProcess relay;
    SharedProject project (relay);
    auto b = project.join ("Ben");

    // B's downloads stop after 3 MB until the test lets them go on.
    std::atomic<std::int64_t> received { 0 };
    std::atomic<bool> release { false };
    struct Release // a failed check must not leave the download thread waiting forever
    {
        std::atomic<bool>& flag;
        ~Release() { flag = true; }
    };
    collab::TransferOptions halting;
    halting.tamper = [&] (const std::string&, collab::Bytes& data) {
        while (received.load() >= (3 << 20) && ! release.load())
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
        received += static_cast<std::int64_t> (data.size());
    };
    Side a (*project.owner);
    Side sb (*b, halting);
    const Release releaseOnExit { release };

    // Uploads wait while A records.
    a.media.setRecording (true);
    const auto frames = static_cast<std::int64_t> (40 * rate); // 40 s, about 5.8 MB
    const auto hash = addRecording (a, frames, 0.5f);
    const auto size = static_cast<std::int64_t> (fs::file_size (a.mediaFile (hash)));
    pumpMedia ({ &a, &sb }, "B waiting", [&] { return sb.media.downloads().contains (hash) && sb.media.downloads().at (hash).waiting; });
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (1500);
    pumpMedia ({ &a, &sb }, "a quiet 1.5 s", [&] { return std::chrono::steady_clock::now() >= until; });
    CHECK (a.count (TransferEvent::Kind::progress, hash) == 0);
    CHECK (a.count (TransferEvent::Kind::uploaded, hash) == 0);
    CHECK (a.media.transfers().pendingUploads() == 1);
    CHECK (a.media.statusText() == "Uploading 1");
    a.media.setRecording (false);

    // B gets the first 3 MB and plays them as they arrive.
    Player player;
    sb.media.onDownloadProgress = [&] (const std::string& h, const collab::MediaSync::Download& d) {
        player.pool.loadPartial (d.partFile, h, b->m().mediaRef (h)->lengthSamples);
    };
    pumpMedia ({ &a, &sb }, "half the download", [&] {
        const auto& downloads = sb.media.downloads();
        return downloads.contains (hash) && downloads.at (hash).done >= (3 << 20);
    });
    const auto source = player.pool.find (hash);
    REQUIRE (source != nullptr);
    CHECK (source->partial());
    CHECK_FALSE (player.pool.available (hash));
    const auto playable = source->playableFrames();
    CHECK (playable > static_cast<std::int64_t> (8 * rate));
    CHECK (playable < frames / 2 + static_cast<std::int64_t> (2 * rate));
    const auto& d = sb.media.downloads().at (hash);
    CHECK (d.total == size);
    CHECK (static_cast<double> (d.done) / static_cast<double> (d.total) > 0.4);
    CHECK (static_cast<double> (d.done) / static_cast<double> (d.total) < 0.7);
    CHECK (sb.media.statusText() == "Downloading 1");

    auto out = player.play (b->m(), frames);
    CHECK (peakOf (out, 4800, playable - 4800) > 0.45f);       // the prefix plays
    CHECK (peakOf (out, playable + 256, frames) == 0.0f);      // the rest is silent
    CHECK (player.engine.diskUnderruns() == 0);               // and that isn't a disk problem

    // The rest arrives; the file replaces the partial source.
    release = true;
    pumpMedia ({ &a, &sb }, "the whole download", [&] { return sb.count (TransferEvent::Kind::downloaded, hash) == 1; });
    REQUIRE (player.pool.completeDownload (sb.root.path / "Media", hash));
    CHECK (player.pool.available (hash));
    out = player.play (b->m(), frames);
    CHECK (peakOf (out, frames - 2 * static_cast<std::int64_t> (rate), frames - 4800) > 0.45f);
    CHECK (sb.media.statusText().empty());
}
