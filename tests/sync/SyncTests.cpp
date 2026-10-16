// [S] sync tests (spec 06 §COL): clients with real Sessions sync through a real
// weft-relay process on localhost. AT-COL-2 to AT-COL-9, AT-TRK-9, AT-MIX-10 and
// AT-TMP-7. (The relay's own side of AT-COL-3, 4 and 9 is also covered at the
// protocol level in relay/tests/relay.rs.)

#include "SyncFixtures.h"

#include "weft/engine/arrange/OfflineRender.h"
#include "weft/project/ProjectFolder.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

using namespace weft;
using namespace weft::test;
using model::TrackKind;

namespace
{
model::Note note (Ticks pos, int pitch, int velocity = 100)
{
    model::Note n;
    n.pos = pos;
    n.length = 480;
    n.pitch = pitch;
    n.velocity = velocity;
    return n;
}

/** The invariants of spec 03 §10.4 that read-time repair guarantees. */
void checkInvariants (const model::ProjectModel& m)
{
    const std::set<Id> order (m.trackOrder.begin(), m.trackOrder.end());
    CHECK (order.size() == m.trackOrder.size());
    for (const auto& id : m.trackOrder)
        CHECK (m.track (id) != nullptr);
    for (const auto& [id, t] : m.tracks)
        if (t->kind != TrackKind::master)
            CHECK (order.contains (id));
    for (const auto& [id, r] : m.regions)
        CHECK (m.track (m.effectiveTrackId (*r)) != nullptr);
    std::set<Ticks> tempoPositions;
    for (const auto& p : m.tempo.points())
        CHECK (tempoPositions.insert (p.pos).second);
    // The routing graph without disabled edges has no cycle.
    std::map<Id, std::vector<Id>> out;
    for (const auto& e : m.routingEdges())
        if (! m.disabledEdges.contains (e.key()))
            out[e.fromTrackId].push_back (e.toTrackId);
    std::map<Id, int> state; // 1 visiting, 2 done
    std::function<bool (const Id&)> cyclic = [&] (const Id& n) {
        if (state[n] == 1)
            return true;
        if (state[n] == 2)
            return false;
        state[n] = 1;
        for (const auto& next : out[n])
            if (cyclic (next))
                return true;
        state[n] = 2;
        return false;
    };
    for (const auto& [n, _] : out)
        CHECK_FALSE (cyclic (n));
}

/** One random edit from the edit vocabulary (tracks, regions, notes, mixer, tempo, markers). */
void randomEdit (Session& s, std::mt19937& rng)
{
    auto pick = [&] (const auto& container) {
        auto it = container.begin();
        std::advance (it, std::uniform_int_distribution<std::size_t> (0, container.size() - 1) (rng));
        return it;
    };
    auto chance = [&] (int n) { return std::uniform_int_distribution<int> (0, n - 1) (rng) == 0; };
    auto ticks = [&] { return static_cast<Ticks> (std::uniform_int_distribution<int> (0, 63) (rng)) * 960; };
    const auto& m = s.model();
    const auto tracks = m.trackOrder;
    std::vector<Id> regions;
    for (const auto& [id, r] : m.regions)
        regions.push_back (id);

    switch (std::uniform_int_distribution<int> (0, 15) (rng))
    {
        case 0:
            if (tracks.size() < 12)
                s.createTrack (chance (3) ? TrackKind::bus : TrackKind::instrument, "T" + std::to_string (rng() % 100));
            break;
        case 1:
            if (tracks.size() > 2 && chance (3))
                s.deleteTrack (*pick (tracks));
            break;
        case 2:
            if (! tracks.empty())
                s.renameTrack (*pick (tracks), "N" + std::to_string (rng() % 1000));
            break;
        case 3:
            if (! tracks.empty())
                s.setTrackVolume (*pick (tracks), -static_cast<double> (rng() % 40));
            break;
        case 4:
            if (! tracks.empty())
                s.setTrackMute (*pick (tracks), chance (2));
            break;
        case 5:
            if (! tracks.empty())
                s.createMidiRegion (*pick (tracks), ticks(), barTicks);
            break;
        case 6:
            if (! regions.empty() && ! tracks.empty())
                s.moveRegion (*pick (regions), ticks(), chance (2) ? std::optional<Id> (*pick (tracks)) : std::nullopt);
            break;
        case 7:
            if (! regions.empty() && chance (2))
                s.deleteRegions ({ *pick (regions) });
            break;
        case 8:
            if (! regions.empty())
            {
                const auto* r = m.region (*pick (regions));
                if (r != nullptr && r->isMidi)
                    s.addNote (r->id, note (static_cast<Ticks> (rng() % 3840), 40 + static_cast<int> (rng() % 40)));
            }
            break;
        case 9:
            if (! regions.empty())
            {
                const auto* r = m.region (*pick (regions));
                const auto* seq = r != nullptr && r->isMidi ? m.sequence (r->midi.sequenceId) : nullptr;
                if (seq != nullptr && ! seq->notes.empty())
                {
                    auto n = pick (seq->notes)->second;
                    if (chance (2))
                        n.pitch = 30 + static_cast<int> (rng() % 60);
                    else
                        n.velocity = 1 + static_cast<int> (rng() % 127);
                    s.updateNote (r->id, n);
                }
            }
            break;
        case 10:
            s.addTempoPoint (static_cast<Ticks> (1 + rng() % 8) * barTicks, 60.0 + static_cast<double> (rng() % 120));
            break;
        case 11:
            s.addMarker (ticks(), "M" + std::to_string (rng() % 100));
            break;
        case 12:
        {
            std::vector<Id> buses;
            for (const auto& id : tracks)
                if (m.track (id)->kind == TrackKind::bus)
                    buses.push_back (id);
            if (! buses.empty() && ! tracks.empty())
            {
                const auto from = *pick (tracks), to = *pick (buses);
                if (! s.wouldCycle (from, to))
                    s.setTrackOutput (from, to);
            }
            break;
        }
        case 13:
            if (! tracks.empty())
            {
                model::PluginInstance p;
                p.pluginUid = "weft.gain";
                p.name = "Weft Gain";
                s.addInsert (*pick (tracks), p, 0);
            }
            break;
        case 14:
            if (! tracks.empty())
                s.moveTrack (*pick (tracks), rng() % tracks.size());
            break;
        default:
            if (! tracks.empty())
                s.setTrackPan (*pick (tracks), static_cast<double> (static_cast<int> (rng() % 201) - 100) / 100.0);
            break;
    }
}

/** A short MIDI pattern on an instrument track; returns (track, region). */
std::pair<Id, Id> addInstrumentWithNotes (Session& s, const std::string& name)
{
    const auto track = s.createTrack (TrackKind::instrument, name).value();
    const auto region = s.createMidiRegion (track, 0, barTicks).value();
    for (int i = 0; i < 4; ++i)
        REQUIRE (s.addNote (region, note (i * 960, 60 + i * 4)));
    return { track, region };
}

media::AudioData render (const model::ProjectModel& m, std::int64_t frames)
{
    engine::MediaPool pool;
    engine::OfflineRenderOptions o;
    o.frames = frames;
    return engine::renderOffline (m, pool, o);
}

double peak (const media::AudioData& a)
{
    double p = 0;
    for (const auto& ch : a.channels)
        for (const auto v : ch)
        {
            REQUIRE (std::isfinite (v));
            p = std::max (p, static_cast<double> (std::abs (v)));
        }
    return p;
}
} // namespace

TEST_CASE ("AT-COL-2 three clients x 1,000 random edits with partitions converge and pass the invariants", "[S][sync][AT-COL-2]")
{
    RelayProcess relay;
    SharedProject project (relay);
    auto b = project.join ("Bea");
    auto c = project.join ("Cy");
    Peers peers { project.owner.get(), b.get(), c.get() };

    std::mt19937 rng (20261016);
    constexpr int editsPerClient = 1000;
    std::vector<int> done (peers.size(), 0);
    while (std::any_of (done.begin(), done.end(), [] (int d) { return d < editsPerClient; }))
    {
        const auto i = rng() % peers.size();
        auto& p = *peers[i];
        if (done[i] < editsPerClient)
        {
            randomEdit (p.s(), rng);
            ++done[i];
        }
        // Random network partitions: a client drops off or comes back now and then.
        if (rng() % 60 == 0)
            p.client->setOnline (! p.client->online());
        // Random delivery timing: poll a random subset.
        for (auto* q : peers)
            if (rng() % 3 == 0)
                q->client->poll();
    }
    for (auto* p : peers)
        p->client->setOnline (true);
    waitConverged (peers, 120);

    const auto root = peers[0]->s().document().root();
    for (auto* p : peers)
    {
        CHECK (p->s().document().root() == root);
        CHECK (p->m().trackOrder == peers[0]->m().trackOrder);
        CHECK (p->m().disabledEdges == peers[0]->m().disabledEdges);
        CHECK (p->m().recoveredTrackFor == peers[0]->m().recoveredTrackFor);
        checkInvariants (p->m());
    }
}

TEST_CASE ("AT-COL-3 an expired access token is refreshed transparently when reconnecting", "[S][sync][AT-COL-3]")
{
    RelayProcess relay (true);
    SharedProject project (relay);
    auto& a = *project.owner;
    REQUIRE (a.client->account().accessToken == project.ownerAccount.accessToken);
    collab::Account saved;
    a.client->onAccountChange = [&] (const collab::Account& acct) { saved = acct; };

    // 16 minutes later the access token has expired; a reconnect must still get in.
    relay.advanceClock (16 * 60 * 1000);
    a.client->setOnline (false);
    REQUIRE (a.s().renameTrack (a.m().trackOrder.front(), "While away"));
    a.client->setOnline (true);
    pumpUntil ({ &a }, "a resync with a refreshed token", [&] { return a.synced(); });
    CHECK_FALSE (saved.accessToken.empty());
    CHECK (saved.accessToken != project.ownerAccount.accessToken);
    CHECK (a.client->account().accessToken == saved.accessToken);

    // The REST calls refresh the same way.
    auto stale = project.ownerAccount;
    RelayApi api (stale);
    CHECK (api.members (project.projectId).size() == 1);
    CHECK (stale.accessToken != project.ownerAccount.accessToken);

    // A wrong password is rejected.
    std::string error;
    CHECK_FALSE (RelayApi::signIn (relay.url(), project.ownerAccount.email, "wrong-password", &error));
    CHECK (error == "The email or password is wrong.");
}

TEST_CASE ("AT-COL-4 share, invite an Editor and a Viewer, refuse the Viewer's region change, remove the Editor", "[S][sync][AT-COL-4]")
{
    RelayProcess relay;
    SharedProject project (relay);
    auto& owner = *project.owner;
    const auto [keys, region] = addInstrumentWithNotes (owner.s(), "Keys");
    auto b = project.join ("Bea", "editor");
    CHECK (b->client->role() == "editor");
    REQUIRE (b->m().region (region) != nullptr);

    // B edits, and the Owner sees it.
    REQUIRE (b->s().moveRegion (region, 2 * barTicks));
    pumpUntil ({ &owner, b.get() }, "B's move on the Owner", [&] { return owner.m().region (region)->start == 2 * barTicks; });

    // Viewer C reads, comments and chats; region edits are refused locally...
    auto c = project.join ("Cy", "viewer");
    CHECK (c->client->role() == "viewer");
    CHECK (c->s().access() == Session::Access::commentsOnly);
    CHECK (c->m().region (region)->start == 2 * barTicks);
    const auto refused = c->s().moveRegion (region, 0);
    CHECK_FALSE (refused);
    CHECK (refused.error == "You're a Viewer in this project: you can comment and chat.");
    REQUIRE (c->s().edit ("Add Comment", [] (commands::TxnWriter& w) {
        return w.put (doc::path ({ "collab", "comments", "k1" }), doc::Value::map().set ("id", "k1").set ("text", "Love the bridge"))
                   ? commands::EditResult {}
                   : commands::EditResult::fail ("no");
    }));
    REQUIRE (c->s().edit ("Chat", [] (commands::TxnWriter& w) {
        return w.listInsert (doc::path ({ "collab", "chat" }), 0, doc::Value::map().set ("id", "m1").set ("text", "hi"), "m1")
                   ? commands::EditResult {}
                   : commands::EditResult::fail ("no");
    }));
    pumpUntil ({ &owner, b.get(), c.get() }, "the comment on the Owner",
               [&] { return owner.s().document().get (doc::path ({ "collab", "comments", "k1" })).has_value() && c->synced(); });
    CHECK (owner.s().document().get (doc::path ({ "collab", "chat" }))->asList().size() == 1);

    // ...and by the relay, with `forbidden`, when a client doesn't enforce the role.
    c->s().setAccess (Session::Access::full);
    REQUIRE (c->s().moveRegion (region, 0));
    pumpUntil ({ &owner, b.get(), c.get() }, "the relay's refusal", [&] {
        return std::any_of (c->messages.begin(), c->messages.end(), [] (const std::string& m) { return m.starts_with ("The relay refused a change"); });
    });
    CHECK (c->client->pendingCount() == 1);
    std::this_thread::sleep_for (std::chrono::milliseconds (100));
    owner.client->poll();
    CHECK (owner.m().region (region)->start == 2 * barTicks);

    // The Owner removes B: B's connection closes and the project turns read-only there.
    RelayApi api (project.ownerAccount);
    REQUIRE (api.removeMember (project.projectId, b->account.userId));
    pumpUntil ({ &owner, b.get(), c.get() }, "B's removal", [&] { return b->client->state() == SyncState::removed; });
    CHECK (b->client->statusText() == "Removed from this project");
    CHECK (b->s().access() == Session::Access::none);
    CHECK_FALSE (b->s().renameTrack (keys, "Mine now"));
    CHECK (std::find (b->messages.begin(), b->messages.end(), "You were removed from this project.") != b->messages.end());
    const auto members = api.members (project.projectId);
    CHECK (members.size() == 2);
}

TEST_CASE ("AT-COL-5 with 50 ms RTT a region move reaches the other client within 500 ms in 95% of 200 trials", "[S][sync][AT-COL-5]")
{
    RelayProcess relay;
    SharedProject project (relay, 25);
    auto& a = *project.owner;
    const auto [track, region] = addInstrumentWithNotes (a.s(), "Keys");
    auto b = project.join ("Bea", "editor", 25);
    std::vector<double> latencies;
    for (int i = 0; i < 200; ++i)
    {
        const Ticks to = (i % 2 == 0 ? 4 : 2) * barTicks + i;
        const auto start = std::chrono::steady_clock::now();
        REQUIRE (a.s().moveRegion (region, to));
        pumpUntil ({ &a, b.get() }, "the move on B", [&] {
            const auto* r = b->m().region (region);
            return r != nullptr && r->start == to;
        });
        latencies.push_back (std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count());
    }
    std::sort (latencies.begin(), latencies.end());
    const auto p95 = latencies[189];
    INFO ("median " << latencies[100] << " ms, p95 " << p95 << " ms");
    CHECK (latencies.front() >= 50.0); // the injected delay is real
    CHECK (p95 <= 500.0);
}

TEST_CASE ("AT-COL-6 a 2 s fader drag emits at most 41 changes and both clients end on the same value", "[S][sync][AT-COL-6]")
{
    RelayProcess relay;
    SharedProject project (relay);
    auto& a = *project.owner;
    auto b = project.join ("Bea");
    const auto track = a.m().trackOrder.front();
    int changes = 0;
    a.s().addLocalChangeListener ([&] (const doc::Bytes&) { ++changes; });

    // 2 s of mouse moves at 120 Hz while both clients keep polling.
    a.s().beginGesture ("Volume");
    const auto start = std::chrono::steady_clock::now();
    double value = 0;
    for (int i = 0; std::chrono::steady_clock::now() - start < std::chrono::seconds (2); ++i)
    {
        value = -static_cast<double> (i % 300) / 10.0;
        a.s().setTrackVolume (track, value);
        a.client->poll();
        b->client->poll();
        std::this_thread::sleep_for (std::chrono::milliseconds (8));
    }
    a.s().endGesture();
    waitConverged ({ &a, b.get() });
    CHECK (changes <= 41);
    CHECK (changes >= 1);
    CHECK (b->m().track (track)->strip.volumeDb == a.m().track (track)->strip.volumeDb);
    CHECK (b->m().track (track)->strip.volumeDb == value);
}

TEST_CASE ("AT-COL-7 50 offline edits and 50 online edits converge; the status goes from pending to Synced", "[S][sync][AT-COL-7]")
{
    RelayProcess relay;
    SharedProject project (relay);
    auto& a = *project.owner;
    const auto [track, region] = addInstrumentWithNotes (a.s(), "Keys");
    auto b = project.join ("Bea");
    waitConverged ({ &a, b.get() });

    a.client->setOnline (false);
    for (int i = 0; i < 50; ++i)
        REQUIRE (a.s().addNote (region, note (i * 60, 40 + i % 40)));
    a.client->poll();
    CHECK (a.client->statusText() == "Offline \xe2\x80\x93 50 changes pending");
    for (int i = 0; i < 50; ++i)
        REQUIRE (b->s().addMarker (i * 960, "B" + std::to_string (i)));
    pumpUntil ({ &a, b.get() }, "B synced", [&] { return b->synced(); });

    a.client->setOnline (true);
    waitConverged ({ &a, b.get() });
    CHECK (a.client->statusText() == "Synced");
    CHECK (a.client->pendingCount() == 0);
    CHECK (a.s().document().root() == b->s().document().root());
    CHECK (b->m().sequence (b->m().region (region)->midi.sequenceId)->notes.size() == 54);
    CHECK (a.m().markers.size() == 50);
}

TEST_CASE ("AT-COL-8 concurrent edits from 05 section 4 merge as specified", "[S][sync][AT-COL-8]")
{
    RelayProcess relay;
    SharedProject project (relay);
    auto& a = *project.owner;
    const auto [keys, r1] = addInstrumentWithNotes (a.s(), "Keys");
    const auto keys2 = a.s().createTrack (TrackKind::instrument, "Keys 2").value();
    const auto r2 = a.s().createMidiRegion (keys2, barTicks, barTicks).value();
    REQUIRE (a.s().setRegionName (r1, "Verse"));
    auto b = project.join ("Bea");
    auto pos = [] (Peer& p, const Id& r) { return p.m().region (r)->start; };
    auto both = [&] (const std::function<void (Peer&)>& check) {
        check (a);
        check (*b);
    };

    SECTION ("different regions moved: both apply")
    {
        concurrently (a, *b, [&] (Peer& p) { REQUIRE (p.s().moveRegion (r1, 2 * barTicks)); },
                      [&] (Peer& p) { REQUIRE (p.s().moveRegion (r2, 5 * barTicks)); });
        both ([&] (Peer& p) {
            CHECK (pos (p, r1) == 2 * barTicks);
            CHECK (pos (p, r2) == 5 * barTicks);
        });
    }
    SECTION ("the same region moved by both: one position, the same on both, and the loser is told")
    {
        concurrently (a, *b, [&] (Peer& p) { REQUIRE (p.s().moveRegion (r1, 4 * barTicks)); },
                      [&] (Peer& p) { REQUIRE (p.s().moveRegion (r1, 8 * barTicks)); });
        const auto final = pos (a, r1);
        CHECK (pos (*b, r1) == final);
        CHECK ((final == 4 * barTicks || final == 8 * barTicks));
        auto& loser = final == 4 * barTicks ? *b : a;
        auto& winner = final == 4 * barTicks ? a : *b;
        CHECK (std::find (loser.messages.begin(), loser.messages.end(), "Region \"Verse\" moved by " + winner.account.displayName)
               != loser.messages.end());
    }
    SECTION ("one moves to another track, the other in time: it ends up on the new track at the new time")
    {
        concurrently (a, *b, [&] (Peer& p) { REQUIRE (p.s().moveRegion (r1, 0, keys2)); },
                      [&] (Peer& p) { REQUIRE (p.s().moveRegion (r1, 6 * barTicks)); });
        both ([&] (Peer& p) {
            CHECK (p.m().region (r1)->trackId == keys2);
            CHECK (pos (p, r1) == 6 * barTicks);
        });
    }
    SECTION ("delete versus edit: deleted, and the editor gets a message")
    {
        concurrently (a, *b, [&] (Peer& p) { REQUIRE (p.s().deleteRegions ({ r1 })); },
                      [&] (Peer& p) { REQUIRE (p.s().setRegionMuted (r1, true)); });
        both ([&] (Peer& p) { CHECK (p.m().region (r1) == nullptr); });
        CHECK (std::find (b->messages.begin(), b->messages.end(), "Region \"Verse\" was deleted by Owner") != b->messages.end());
    }
    SECTION ("different notes added: all kept")
    {
        concurrently (a, *b, [&] (Peer& p) { REQUIRE (p.s().addNote (r1, note (100, 70))); },
                      [&] (Peer& p) { REQUIRE (p.s().addNote (r1, note (200, 72))); });
        both ([&] (Peer& p) { CHECK (p.m().sequence (p.m().region (r1)->midi.sequenceId)->notes.size() == 6); });
    }
    SECTION ("the same note's pitch and velocity: both apply")
    {
        const auto* seq = a.m().sequence (a.m().region (r1)->midi.sequenceId);
        const auto original = seq->sortedNotes().front()->id;
        concurrently (
            a, *b,
            [&] (Peer& p) {
                auto n = p.m().sequence (p.m().region (r1)->midi.sequenceId)->notes.at (original);
                n.pitch = 48;
                REQUIRE (p.s().updateNote (r1, n));
            },
            [&] (Peer& p) {
                auto n = p.m().sequence (p.m().region (r1)->midi.sequenceId)->notes.at (original);
                n.velocity = 33;
                REQUIRE (p.s().updateNote (r1, n));
            });
        both ([&] (Peer& p) {
            const auto& n = p.m().sequence (p.m().region (r1)->midi.sequenceId)->notes.at (original);
            CHECK (n.pitch == 48);
            CHECK (n.velocity == 33);
        });
    }
    SECTION ("plug-ins inserted at the same slot: both kept, in the same order on both")
    {
        model::PluginInstance gain, eq;
        gain.pluginUid = "weft.gain";
        gain.name = "Weft Gain";
        eq.pluginUid = "weft.eq";
        eq.name = "Weft EQ";
        concurrently (a, *b, [&] (Peer& p) { REQUIRE (p.s().addInsert (keys, gain, 0)); },
                      [&] (Peer& p) { REQUIRE (p.s().addInsert (keys, eq, 0)); });
        std::vector<std::string> namesA, namesB;
        for (const auto& i : a.m().track (keys)->strip.inserts)
            namesA.push_back (i.name);
        for (const auto& i : b->m().track (keys)->strip.inserts)
            namesB.push_back (i.name);
        CHECK (namesA.size() == 2);
        CHECK (namesA == namesB);
    }
    SECTION ("delete a track versus record onto it: one Recovered track holds the recording")
    {
        commands::RecordedMidi take;
        take.trackId = keys2;
        take.trackName = "Keys 2";
        take.notes = { note (4 * barTicks, 64) };
        take.regionStart = 4 * barTicks;
        take.regionEnd = 5 * barTicks;
        concurrently (a, *b, [&] (Peer& p) { REQUIRE (p.s().deleteTrack (keys2)); },
                      [&] (Peer& p) { REQUIRE (p.s().commitRecording ({}, { take })); });
        waitConverged ({ &a, b.get() });
        both ([&] (Peer& p) {
            CHECK (p.m().track (keys2) == nullptr);
            int recovered = 0;
            for (const auto& [id, t] : p.m().tracks)
                if (t->name.starts_with ("Recovered"))
                    ++recovered;
            CHECK (recovered == 1);
            CHECK (p.m().syntheticTracks.empty());
            bool recordingKept = false;
            for (const auto& [id, r] : p.m().regions)
                if (r->start == 4 * barTicks && p.m().track (r->trackId) != nullptr && p.m().track (r->trackId)->name.starts_with ("Recovered"))
                    recordingKept = true;
            CHECK (recordingKept);
        });
        CHECK (a.s().document().root() == b->s().document().root());
    }
    CHECK (a.s().document().root() == b->s().document().root());
}

TEST_CASE ("AT-COL-9 un-ACKed changes survive a client crash and are re-sent; ACKed changes survive a relay kill", "[S][sync][AT-COL-9]")
{
    RelayProcess relay;
    SharedProject project (relay);
    auto& owner = *project.owner;
    TempDir dir;
    const auto root = dir.path / "Song.weft";
    const auto pendingFile = root / "Cache" / "sync-pending.txt";
    collab::Account bAccount;
    {
        auto b = project.join ("Bea", "editor", 0, pendingFile);
        bAccount = b->account;
        project::ProjectFolder folder (root);
        REQUIRE (folder.create());
        REQUIRE (folder.save (b->s().document()));
        b->s().onLocalChange = [&] (const doc::Bytes& change, std::int64_t t) { folder.log().append (change, t); };
        b->client->setOnline (false);
        for (int i = 0; i < 5; ++i)
            REQUIRE (b->s().addMarker (i * barTicks, "Before crash " + std::to_string (i)));
        b->client->poll();
        CHECK (b->client->pendingCount() == 5);
        b->s().onLocalChange = nullptr;
    } // the "crash": nothing saved, nothing sent

    // Restart: the recovery log brings the edits back and the pending list remembers them.
    project::ProjectFolder folder (root);
    auto opened = folder.open();
    REQUIRE (opened.document.has_value());
    REQUIRE (opened.recoveryAvailable());
    Peer b ("Bea", bAccount, Session (std::move (*opened.recovered), bAccount.userId), project.projectId, 0, pendingFile);
    CHECK (b.m().markers.size() == 5);
    CHECK (b.client->pendingCount() == 5);
    waitConverged ({ &owner, &b });
    CHECK (owner.m().markers.size() == 5);
    CHECK (b.client->pendingCount() == 0);

    // Relay restart: kill it hard after these are ACKed, start it again on the same port.
    for (int i = 0; i < 10; ++i)
        REQUIRE (owner.s().addMarker (i * 960 + 7, "Acked " + std::to_string (i)));
    waitConverged ({ &owner, &b });
    const auto heads = owner.s().document().heads();
    relay.kill();
    pumpUntil ({ &owner, &b }, "the clients to notice", [&] {
        return owner.client->state() == SyncState::reconnecting && b.client->state() == SyncState::reconnecting;
    });
    REQUIRE (b.s().addMarker (99 * barTicks, "While the relay was down"));
    relay.start();
    waitConverged ({ &owner, &b }, 90);
    CHECK (owner.m().markers.size() == 16);
    RelayApi api (project.ownerAccount);
    const auto fresh = api.fetchDocument (project.projectId);
    REQUIRE (fresh);
    auto restored = doc::Document::load (*fresh);
    REQUIRE (restored);
    for (std::size_t i = 0; i + 32 <= heads.size(); i += 32)
        CHECK (restored->hasChange (std::span (heads).subspan (i, 32)));
}

TEST_CASE ("AT-TRK-9 solo is local: soloing on one client changes nothing on the other; mute is shared", "[S][sync][AT-TRK-9]")
{
    RelayProcess relay;
    SharedProject project (relay);
    auto& a = *project.owner;
    const auto [keys, r1] = addInstrumentWithNotes (a.s(), "Keys");
    const auto [keys2, r2] = addInstrumentWithNotes (a.s(), "Keys 2");
    auto b = project.join ("Bea");
    const auto docBefore = b->s().document().root();
    const auto outBefore = render (b->m(), 48000);

    // Client 1 solos Keys: local view state only, so nothing reaches the relay.
    commands::LocalView viewA;
    viewA.toggleSolo (keys);
    CHECK (viewA.soloed.contains (keys));
    a.client->poll();
    std::this_thread::sleep_for (std::chrono::milliseconds (100));
    b->client->poll();
    CHECK (b->s().document().root() == docBefore);
    CHECK ((render (b->m(), 48000).channels == outBefore.channels));

    // Mute is a mix decision and is shared.
    REQUIRE (a.s().setTrackMute (keys2, true));
    waitConverged ({ &a, b.get() });
    CHECK (b->m().track (keys2)->mute);
    CHECK ((render (b->m(), 48000).channels != outBefore.channels));
}

TEST_CASE ("AT-MIX-10 a routing cycle made offline by two clients converges with one disabled edge and no feedback", "[S][sync][AT-MIX-10]")
{
    RelayProcess relay;
    SharedProject project (relay);
    auto& a = *project.owner;
    const auto [keys, region] = addInstrumentWithNotes (a.s(), "Keys");
    const auto bus1 = a.s().createTrack (TrackKind::bus, "Bus 1").value();
    const auto bus2 = a.s().createTrack (TrackKind::bus, "Bus 2").value();
    REQUIRE (a.s().setTrackOutput (keys, bus1));
    auto b = project.join ("Bea");

    concurrently (a, *b, [&] (Peer& p) { REQUIRE (p.s().setTrackOutput (bus1, bus2)); },
                  [&] (Peer& p) { REQUIRE (p.s().setTrackOutput (bus2, bus1)); });
    CHECK (a.s().document().root() == b->s().document().root());
    REQUIRE (a.m().disabledEdges.size() == 1);
    CHECK (a.m().disabledEdges == b->m().disabledEdges);
    // The disabled edge is the later write by CRDT order.
    const auto stampOf = [&] (const Id& bus) {
        return doc::OpStamp::parse (a.s().document().opId (doc::path ({ "tracks", bus, "output" })).value_or ("")).value();
    };
    const auto later = stampOf (bus1) > stampOf (bus2) ? "out:" + bus1 : "out:" + bus2;
    CHECK (*a.m().disabledEdges.begin() == later);

    // Audio: the keys play through, and nothing feeds back (finite, bounded, and silent after the notes).
    const auto out = render (a.m(), 4 * 48000);
    const auto p = peak (out);
    CHECK (p > 0.0);
    CHECK (p < 4.0);
    double tail = 0;
    for (std::size_t i = 3 * 48000; i < out.channels[0].size(); ++i)
        tail = std::max (tail, static_cast<double> (std::abs (out.channels[0][i])));
    CHECK (tail < 1e-3);
}

TEST_CASE ("AT-TMP-7 two tempo points at one tick converge to the greater Id, flagged as a conflict on both", "[S][sync][AT-TMP-7]")
{
    RelayProcess relay;
    SharedProject project (relay);
    auto& a = *project.owner;
    auto b = project.join ("Bea");
    Id idA, idB;
    concurrently (a, *b, [&] (Peer& p) { idA = p.s().addTempoPoint (8 * barTicks, 100.0).value(); },
                  [&] (Peer& p) { idB = p.s().addTempoPoint (8 * barTicks, 140.0).value(); });
    const auto winner = std::max (idA, idB);
    const auto bpm = winner == idA ? 100.0 : 140.0;
    for (auto* p : Peers { &a, b.get() })
    {
        int atTick = 0;
        for (const auto& pt : p->m().tempo.points())
            if (pt.pos == 8 * barTicks)
            {
                ++atTick;
                CHECK (pt.id == winner);
                CHECK (pt.bpm == bpm);
                CHECK (pt.conflicted);
            }
        CHECK (atTick == 1);
    }
}
