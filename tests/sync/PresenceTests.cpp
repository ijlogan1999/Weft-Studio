// [S] presence (spec 05 §5, M11): who is connected, where their pointer and playhead are,
// what they selected and whether they're idle or recording, through a real weft-relay.
// Sync-level halves of AT-COL-10 to AT-COL-12; the drawing is in tests/ui/CollabUiTests.cpp.

#include "SyncFixtures.h"

#include "weft/collab/Presence.h"

#include <atomic>

using namespace weft;
using namespace weft::test;
using collab::Presence;
using collab::RemotePresence;

namespace
{
/** A clock the test moves by hand (Unix ms), shared by the peers that use it. */
struct FakeClock
{
    std::shared_ptr<std::atomic<std::int64_t>> now = std::make_shared<std::atomic<std::int64_t>> (1'800'000'000'000);
    std::function<std::int64_t()> clock (std::int64_t skewMs = 0) const
    {
        return [n = now, skewMs] { return n->load() + skewMs; };
    }
    void advance (std::int64_t ms) const { *now += ms; }
};

const RemotePresence* find (const Peer& viewer, const Peer& who)
{
    for (const auto& [client, p] : viewer.client->collaborators())
        if (client == who.client->clientId())
            return &p;
    return nullptr;
}

/** Polls with the fake clock moving `stepMs` per round, until done. */
void pumpClock (const Peers& peers, const FakeClock& clock, std::int64_t stepMs, const std::string& what, const std::function<bool()>& done, int seconds = 30)
{
    pumpUntil (
        peers, what,
        [&] {
            clock.advance (stepMs);
            return done();
        },
        seconds);
}
} // namespace

TEST_CASE ("AT-COL-10 a collaborator appears within 1 s of connecting, shows as idle after 2 minutes, and goes when they leave", "[S][sync][presence][AT-COL-10]")
{
    RelayProcess relay;
    FakeClock clock;
    SharedProject project (relay, 0, clock.clock());
    auto b = project.join ("Ben", "editor", 0, {}, clock.clock());
    auto& a = *project.owner;
    pumpUntil ({ &a, b.get() }, "B on A", [&] { return find (a, *b) != nullptr && find (a, *b)->hasState; });
    CHECK (find (a, *b)->displayName == "Ben");
    CHECK (find (a, *b)->userId == b->account.userId);
    CHECK (find (*b, a)->displayName == "Owner");
    CHECK (find (a, *b)->color != find (*b, a)->color); // each member has their own colour

    // Ben opens the project on a second computer: a second presence, within 1 s.
    auto bytes = RelayApi (b->account).fetchDocument (project.projectId);
    REQUIRE (bytes);
    const auto started = std::chrono::steady_clock::now();
    Peer laptop ("Ben's laptop", b->account, Session (std::move (*doc::Document::load (*bytes)), b->account.userId), project.projectId, 0, {},
                 clock.clock());
    pumpUntil ({ &a, b.get(), &laptop }, "the laptop on A", [&] { return find (a, laptop) != nullptr; });
    CHECK (std::chrono::steady_clock::now() - started < std::chrono::seconds (1));
    CHECK (a.client->collaborators().size() == 2);

    // Two minutes without input: idle. Input again: active.
    b->client->noteInput();
    laptop.client->noteInput();
    a.client->noteInput();
    pumpClock ({ &a, b.get(), &laptop }, clock, 0, "B active", [&] { return find (a, *b)->state.status == Presence::Status::active; });
    clock.advance (119'000);
    pumpClock ({ &a, b.get(), &laptop }, clock, 0, "a moment", [&] { return true; });
    CHECK (find (a, *b)->state.status == Presence::Status::active);
    pumpClock ({ &a, b.get(), &laptop }, clock, 5, "B idle", [&] { return find (a, *b)->state.status == Presence::Status::idle; });
    b->client->noteInput();
    pumpClock ({ &a, b.get(), &laptop }, clock, 5, "B active again", [&] { return find (a, *b)->state.status == Presence::Status::active; });

    // The laptop disconnects: it goes from A's list (the relay says so; at the latest 45 s).
    const auto laptopId = laptop.client->clientId();
    laptop.client.reset();
    pumpUntil ({ &a, b.get() }, "the laptop gone", [&] { return ! a.client->collaborators().contains (laptopId); }, 45);
    CHECK (find (a, *b) != nullptr);

    // A loses the relay: collaborators are cleared after 45 s offline, not before.
    a.client->setOnline (false);
    pumpClock ({ &a }, clock, 0, "A offline", [&] { return a.client->state() == SyncState::offline; });
    clock.advance (44'000);
    a.client->poll();
    CHECK (find (a, *b) != nullptr);
    clock.advance (1'500);
    a.client->poll();
    CHECK (a.client->collaborators().empty());
}

TEST_CASE ("AT-COL-11 pointers travel as bar and track at no more than 20 Hz; a playing collaborator's playhead extrapolates to within 50 ms",
           "[S][sync][presence][AT-COL-11]")
{
    RelayProcess relay;
    FakeClock clock;
    SharedProject project (relay, 0, clock.clock());
    // Ben's computer clock runs 30 ms ahead of the Owner's.
    auto b = project.join ("Ben", "editor", 0, {}, clock.clock (30));
    auto& a = *project.owner;
    const auto drums = a.m().trackOrder.front();
    pumpUntil ({ &a, b.get() }, "presence both ways", [&] { return find (a, *b) != nullptr && find (*b, a) != nullptr; });

    // Pointer at bar 9 on the first track: musical coordinates, whatever anyone's zoom.
    Presence p;
    p.pointer = Presence::Pointer { Presence::View::arrange, 8 * barTicks, drums, -1 };
    b->client->setPresence (p);
    pumpClock ({ &a, b.get() }, clock, 10, "B's pointer on A", [&] { return find (a, *b)->state.pointer.has_value(); });
    CHECK (find (a, *b)->state.pointer->pos == 8 * barTicks);
    CHECK (find (a, *b)->state.pointer->track == drums);
    CHECK (find (a, *b)->state.pointer->area == Presence::View::arrange);

    // Moving every 5 ms for 2 s sends at most 41 updates (20 Hz), and A ends where B stopped.
    const auto sentBefore = b->client->presenceSent();
    for (int i = 0; i < 400; ++i)
    {
        p.pointer->pos = 8 * barTicks + i * 10;
        b->client->setPresence (p);
        clock.advance (5);
        b->client->poll();
        a.client->poll();
    }
    clock.advance (100);
    b->client->poll();
    const auto sent = b->client->presenceSent() - sentBefore;
    CHECK (sent <= 41);
    CHECK (sent >= 30); // and it does keep up
    pumpClock ({ &a, b.get() }, clock, 10, "the last pointer position", [&] { return find (a, *b)->state.pointer->pos == p.pointer->pos; });

    // B plays from bar 3 at 120 BPM: A extrapolates without further messages.
    const auto& tempo = a.m().tempo;
    const auto startPos = 2 * barTicks;
    p.playhead = { startPos, true, b->client->now() };
    b->client->setPresence (p);
    pumpClock ({ &a, b.get() }, clock, 0, "B playing on A", [&] { return find (a, *b)->state.playhead.playing; });
    const auto playStartedAt = b->client->now();
    for (const auto elapsed : { 400, 1500, 1900 })
    {
        clock.advance (elapsed - (b->client->now() - playStartedAt));
        a.client->poll();
        const auto* who = find (a, *b);
        const auto seen = collab::extrapolatePlayhead (who->state.playhead, tempo, a.client->now(), who->receivedAt);
        const auto actual = tempo.sampleToTick (tempo.tickToSample (static_cast<double> (startPos)) + elapsed * tempo.sampleRate() / 1000.0);
        const auto errorMs = std::abs (tempo.tickToSample (static_cast<double> (seen)) - tempo.tickToSample (actual)) / tempo.sampleRate() * 1000.0;
        INFO ("after " << elapsed << " ms the error is " << errorMs << " ms");
        CHECK (errorMs <= 50.0);
    }
    // The 2 s refresh arrives; stop is sent at once.
    pumpClock ({ &a, b.get() }, clock, 20, "the 2 s refresh", [&] { return b->client->presenceSent() - sentBefore > sent + 1; });
    p.playhead = { 4 * barTicks, false, b->client->now() };
    b->client->setPresence (p);
    pumpClock ({ &a, b.get() }, clock, 0, "B stopped", [&] { return ! find (a, *b)->state.playhead.playing; });
    CHECK (find (a, *b)->state.playhead.pos == 4 * barTicks);

    // A sender whose clock is far off: counted from when the update arrived instead.
    Presence::Playhead skewed { startPos, true, a.client->now() + 60'000 };
    const auto now = a.client->now();
    CHECK (collab::extrapolatePlayhead (skewed, tempo, now + 1000, now) == collab::extrapolatePlayhead ({ startPos, true, now }, tempo, now + 1000, now));
}

TEST_CASE ("AT-COL-12 selections, focus and recording reach collaborators", "[S][sync][presence][AT-COL-12]")
{
    RelayProcess relay;
    FakeClock clock;
    SharedProject project (relay, 0, clock.clock());
    auto b = project.join ("Ben", "editor", 0, {}, clock.clock());
    auto& a = *project.owner;
    const auto tracks = a.s().createTracks (model::TrackKind::instrument, 3);
    const auto r1 = a.s().createMidiRegion (tracks[0], 0, barTicks);
    const auto r2 = a.s().createMidiRegion (tracks[1], barTicks, barTicks);
    REQUIRE (r1);
    REQUIRE (r2);
    waitConverged ({ &a, b.get() });

    // B selects two regions and focuses track 3.
    Presence p;
    p.regions = { *r1, *r2 };
    p.focusTrack = tracks[2];
    b->client->setPresence (p);
    pumpClock ({ &a, b.get() }, clock, 20, "B's selection on A", [&] { return find (a, *b)->state.regions.size() == 2; });
    CHECK (find (a, *b)->state.regions == std::vector<Id> { *r1, *r2 });
    CHECK (find (a, *b)->state.focusTrack == tracks[2]);
    CHECK (find (a, *b)->state.status == Presence::Status::active);

    // B records on track 3.
    p.status = Presence::Status::recording;
    b->client->setPresence (p);
    pumpClock ({ &a, b.get() }, clock, 20, "B recording on A", [&] { return find (a, *b)->state.status == Presence::Status::recording; });
    CHECK (find (a, *b)->state.focusTrack == tracks[2]);

    // Big selections: the first 500 ids and the count.
    p.status = Presence::Status::active;
    p.notes.clear();
    for (int i = 0; i < 700; ++i)
        p.notes.push_back ("n" + std::to_string (i));
    p.selectionCount = 702;
    b->client->setPresence (p);
    pumpClock ({ &a, b.get() }, clock, 20, "the big selection", [&] { return find (a, *b)->state.selectionCount == 702; });
    CHECK (find (a, *b)->state.notes.size() == Presence::maxSelectionIds);
    CHECK (find (a, *b)->state.regions.size() == 2);
}
