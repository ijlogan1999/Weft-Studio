// Collaborators in the window (M11, spec 04 §3.6, §6 and §9; 05 §5 and §8): avatars,
// pointers, playheads, selection outlines and header badges; regions whose audio is still
// on its way; the Missing Plug-ins notice. Two windows in one process share a project on a
// real weft-relay. The [UI] halves of AT-COL-10, 11, 12, 17 and 31.

#include "UiFixture.h"

#include "../sync/SyncFixtures.h"

#include "ui/ShareDialogs.h"

#include <atomic>

using namespace weft;
using namespace weft::ui;
using namespace weft::ui::test;

namespace
{
struct FakeClock
{
    std::shared_ptr<std::atomic<std::int64_t>> now = std::make_shared<std::atomic<std::int64_t>> (1'800'000'000'000);
    std::function<std::int64_t()> fn() const
    {
        return [n = now] { return n->load(); };
    }
    void advance (std::int64_t ms) const { *now += ms; }
};

/** When set, pump() moves this clock on 10 ms a round (presence rates count in clock time). */
const FakeClock* ticking = nullptr;

void pump (std::initializer_list<Ui*> uis, const std::string& what, const std::function<bool()>& done, int seconds = 30)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (seconds);
    for (;;)
    {
        if (ticking != nullptr)
            ticking->advance (10);
        for (auto* u : uis)
        {
            u->studio.tick();
            u->settle();
        }
        if (done())
            return;
        juce::String states;
        for (auto* u : uis)
            states << u->studio.syncStatus() << " (" << u->studio.status << "); ";
        INFO ("waiting for " << what << ": " << states);
        REQUIRE (std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
}

/** A shares its project and B joins it as an Editor. */
void shareWith (Ui& a, Ui& b, const weft::test::RelayProcess& relay)
{
    a.studio.signedInAs (weft::test::signUp (relay, "Ana"));
    std::string error;
    REQUIRE (a.studio.shareProject (&error));
    pump ({ &a }, "the upload", [&] { return a.studio.syncStatus().startsWith ("Synced"); });
    const auto invite = a.studio.withRelay ([&] (collab::RelayApi& api) { return api.createInvite (a.m().info.id, "editor"); });
    REQUIRE (invite);
    b.studio.signedInAs (weft::test::signUp (relay, "Ben"));
    const bool joined = b.studio.joinProject (invite->url, &error);
    INFO (error);
    REQUIRE (joined);
    pump ({ &a, &b }, "both synced", [&] { return a.studio.syncStatus() == "Synced" && b.studio.syncStatus() == "Synced"; });
}

const collab::RemotePresence* other (Ui& ui)
{
    const auto who = ui.studio.collaborators();
    return who.empty() ? nullptr : who.front();
}

juce::Colour pixel (juce::Component& c, juce::Point<float> p)
{
    const auto image = c.createComponentSnapshot (c.getLocalBounds());
    return image.getPixelAt (juce::roundToInt (p.x), juce::roundToInt (p.y));
}

bool similarColour (juce::Colour a, juce::Colour b)
{
    return std::abs (a.getRed() - b.getRed()) + std::abs (a.getGreen() - b.getGreen()) + std::abs (a.getBlue() - b.getBlue()) < 60;
}

void forgetAccounts (Ui& ui)
{
    std::error_code ec;
    std::filesystem::remove (ui.studio.accountFile(), ec);
}
} // namespace

TEST_CASE ("AT-COL-10 AT-COL-11 AT-COL-12 [UI] avatars, pointers, playheads, selections and header badges", "[UI][S][AT-COL-10][AT-COL-11][AT-COL-12]")
{
    weft::test::RelayProcess relay;
    FakeClock clock;
    Ui a, b;
    a.studio.syncClock = clock.fn();
    b.studio.syncClock = clock.fn();
    ticking = &clock;
    struct Stop
    {
        ~Stop() { ticking = nullptr; }
    } const stopTicking;
    const auto tracks = a.instrumentTracks (3);
    const auto r1 = a.studio.session().createMidiRegion (tracks[0], 0, 2 * bar);
    const auto r2 = a.studio.session().createMidiRegion (tracks[1], 2 * bar, 2 * bar);
    REQUIRE (r1);
    REQUIRE (r2);
    a.studio.edited();

    // AT-COL-10: B's avatar shows on A within 1 s of B connecting.
    CHECK (a.main->controlBar().collaboratorAvatars().preferredWidth() == 0);
    const auto joined = std::chrono::steady_clock::now();
    shareWith (a, b, relay);
    pump ({ &a, &b }, "B's avatar on A", [&] { return other (a) != nullptr; });
    CHECK (std::chrono::steady_clock::now() - joined < std::chrono::seconds (3)); // sign-up, upload, join and download included
    auto& avatars = a.main->controlBar().collaboratorAvatars();
    CHECK (avatars.isVisible());
    CHECK (avatars.preferredWidth() == Avatars::size + 2);
    CHECK (avatars.getWidth() > 0);
    CHECK (other (a)->displayName == "Ben");
    CHECK (other (b)->displayName == "Ana");
    CHECK (initials (other (a)->displayName) == "BE");
    CHECK (initials ("Ana Lima") == "AL");
    CHECK (describe (a.studio, *other (a)).startsWith ("Ben\n"));

    // AT-COL-11: B's pointer at bar 9 on track 2 lands on A at bar 9 on track 2, whatever A's zoom.
    a.studio.view.pixelsPerTick = 0.02;
    b.studio.view.pixelsPerTick = 0.06;
    b.studio.view.originTick = 4 * bar;
    a.settle();
    const auto row = 2; // Audio 1, then the instrument tracks
    REQUIRE (b.m().trackOrder[row] == tracks[1]);
    const auto bAt = juce::Point<float> (static_cast<float> (b.studio.view.tickToX (8.0 * bar)), b.timeline().laneBounds (row).getCentreY());
    b.timeline().mouseMove (mouse (b.timeline(), bAt));
    pump ({ &b }, "B's pointer", [&] { return b.studio.presence().pointer.has_value(); });
    CHECK (b.studio.presence().pointer->pos == 8 * bar);
    CHECK (b.studio.presence().pointer->track == tracks[1]);
    pump ({ &a, &b }, "B's pointer on A", [&] { return other (a)->state.pointer.has_value(); });
    const auto shown = a.timeline().collaboratorPointer (other (a)->clientId);
    REQUIRE (shown);
    CHECK (shown->x == Catch::Approx (a.studio.view.tickToX (8.0 * bar)).margin (0.5));
    CHECK (shown->y == Catch::Approx (a.timeline().laneBounds (row).getCentreY()));
    const auto movedAt = other (a)->pointerMovedAt;
    REQUIRE (clock.now->load() - movedAt < 4000);
    CHECK (pointerAlpha (a.studio, *other (a)) == 1.0f);
    clock.advance (movedAt + 4500 - clock.now->load());
    CHECK (pointerAlpha (a.studio, *other (a)) == Catch::Approx (0.5f));
    clock.advance (1000);
    CHECK (pointerAlpha (a.studio, *other (a)) == 0.0f); // faded out after 5 s without movement
    b.timeline().mouseExit (mouse (b.timeline(), bAt));
    pump ({ &a, &b }, "B's pointer gone", [&] { return ! other (a)->state.pointer.has_value(); });

    // B plays: A draws B's playhead where B's transport is (within 50 ms).
    b.studio.locate (bar);
    b.renderAudio (256);
    b.studio.perform (Cmd::playStop);
    b.renderAudio (256);
    REQUIRE (b.studio.playing());
    ticking = nullptr; // from here B's transport and the clock move together
    pump ({ &a, &b }, "B playing on A", [&] { return other (a)->state.playhead.playing; });
    b.renderAudio (48000);
    clock.advance (1000);
    pump ({ &a, &b }, "a tick", [] { return true; });
    ticking = &clock;
    const auto& tempo = a.m().tempo;
    const auto seen = tempo.tickToSample (static_cast<double> (a.studio.collaboratorPlayhead (*other (a))));
    const auto actual = tempo.tickToSample (static_cast<double> (b.studio.playhead()));
    CHECK (std::abs (seen - actual) / 48.0 <= 50.0);
    b.studio.perform (Cmd::playStop);
    b.renderAudio (256);
    pump ({ &a, &b }, "B stopped on A", [&] { return ! other (a)->state.playhead.playing; });

    // AT-COL-12: B selects two regions; A outlines them in B's colour.
    b.studio.selection.set ({ *r1, *r2 });
    b.studio.selectTrack (tracks[2]);
    pump ({ &a, &b }, "B's selection on A", [&] { return other (a)->state.regions.size() == 2 && other (a)->state.focusTrack == tracks[2]; });
    a.studio.view.pixelsPerTick = 0.05;
    a.studio.view.originTick = 0;
    a.settle();
    const auto box = a.timeline().regionBounds (*a.m().region (*r2));
    CHECK (similarColour (pixel (a.timeline(), { box.getX(), box.getCentreY() }), colourOf (*other (a))));
    CHECK_FALSE (a.studio.selection.contains (*r2)); // B's selection isn't A's

    // B focuses track 3: B's badge on A's track 3 header; B records there: the badge turns red.
    const auto& headers = a.main->tracks().headers();
    auto header = [&] (const Id& id) -> TrackHeader& {
        for (const auto& h : headers)
            if (h->trackId == id)
                return *h;
        FAIL ("no header");
        return *headers.front();
    };
    CHECK (header (tracks[2]).badges().size() == 1);
    CHECK (header (tracks[0]).badges().empty());
    b.studio.toggleArm (tracks[2]);
    b.studio.perform (Cmd::record);
    b.renderAudio (256);
    REQUIRE (b.studio.recording());
    pump ({ &a, &b }, "B recording on A", [&] { return other (a)->state.status == collab::Presence::Status::recording; });
    CHECK (header (tracks[2]).badges().front()->state.status == collab::Presence::Status::recording);
    CHECK (describe (a.studio, *other (a)).contains ("Recording"));
    b.studio.perform (Cmd::playStop);
    b.renderAudio (256);
    pump ({ &a, &b }, "B stopped recording", [&] { return other (a)->state.status == collab::Presence::Status::active; });

    // AT-COL-10: two minutes without input and B shows as idle (dimmed); input brings B back.
    clock.advance (121'000);
    pump ({ &a, &b }, "B idle on A", [&] { return other (a)->state.status == collab::Presence::Status::idle; });
    CHECK (describe (a.studio, *other (a)).contains ("Idle"));
    b.main->mouseMove (mouse (*b.main, { 10, 10 }));
    pump ({ &a, &b }, "B active on A", [&] { return other (a)->state.status == collab::Presence::Status::active; });

    // Clicking B's avatar scrolls A to B's selection.
    a.studio.view.originTick = 40 * bar;
    avatars.goTo (0);
    CHECK (a.studio.view.originTick < 2 * bar);

    // B goes offline: the avatar goes.
    b.studio.setOnline (false);
    pump ({ &a, &b }, "B gone from A", [&] { return other (a) == nullptr; }, 45);
    CHECK (avatars.preferredWidth() == 0);
    forgetAccounts (a);
}

TEST_CASE ("AT-COL-17 [UI] a region shows its download, the status bar counts transfers, and the share question gives the audio size", "[UI][S][AT-COL-17]")
{
    weft::test::RelayProcess relay;
    Ui a, b;
    std::atomic<bool> uploadAllowed { false };
    a.studio.transferOptions.beforeChunk = [&] (const std::string&, std::int64_t) { return uploadAllowed.load(); };
    const auto file = writeSine (a.folder.path / "browse" / "take.wav", 2.0);
    const auto outcome = a.studio.importFiles ({ file }, { 0, {} });
    REQUIRE (outcome.regions.size() == 1);
    const auto region = outcome.regions.front();
    const auto hash = a.region (region).audio.media;

    // The share question says how much audio goes up with the project (04 §9.2).
    a.studio.signedInAs (weft::test::signUp (relay, "Ana"));
    {
        ShareDialog share (a.studio);
        CHECK (share.uploadNote.getText().contains ("and its 0.5 MB of audio"));
    }
    b.studio.signedInAs (weft::test::signUp (relay, "Ben"));
    std::string error;
    REQUIRE (a.studio.shareProject (&error));
    pump ({ &a }, "the upload", [&] { return a.studio.syncStatus().startsWith ("Synced"); });
    const auto invite = a.studio.withRelay ([&] (collab::RelayApi& api) { return api.createInvite (a.m().info.id, "editor"); });
    REQUIRE (invite);
    const bool joined = b.studio.joinProject (invite->url, &error);
    INFO (error);
    REQUIRE (joined);

    // A's upload is held: B has the region but not its audio.
    pump ({ &a, &b }, "B waiting for the audio", [&] {
        const auto d = b.studio.downloadState (hash);
        return d && d->waiting;
    });
    CHECK (b.m().region (region) != nullptr);
    CHECK (a.studio.syncStatus().contains ("Uploading 1"));
    CHECK (b.studio.syncStatus().contains ("Downloading 1"));
    CHECK_FALSE (b.studio.media().available (hash));
    CHECK (engine::regionsMissingMedia (b.m(), b.studio.media()) == 1);
    const auto boxB = b.timeline().regionBounds (*b.m().region (region));
    // The part not downloaded is hatched: it looks different once the audio is here.
    const auto snapshot = [&] { return b.timeline().createComponentSnapshot (boxB.toNearestInt()); };
    const auto waiting = snapshot();

    // The upload goes through; B downloads, verifies, and the region plays its file.
    uploadAllowed = true;
    a.studio.edited(); // the next look at the document resumes the upload
    pump ({ &a, &b }, "the download", [&] { return b.studio.media().available (hash); }, 60);
    CHECK_FALSE (b.studio.downloadState (hash));
    CHECK (std::filesystem::exists (b.studio.mediaDir() / media::mediaFileName (hash)));
    pump ({ &a, &b }, "quiet status", [&] { return a.studio.syncStatus() == "Synced" && b.studio.syncStatus() == "Synced"; });
    CHECK (engine::regionsMissingMedia (b.m(), b.studio.media()) == 0);
    CHECK_FALSE (b.studio.peaks (hash).empty());
    b.settle();
    const auto here = snapshot();
    int differ = 0;
    for (int y = 0; y < here.getHeight(); y += 3)
        for (int x = 0; x < here.getWidth(); x += 3)
            differ += here.getPixelAt (x, y) != waiting.getPixelAt (x, y) ? 1 : 0;
    CHECK (differ > here.getWidth() * here.getHeight() / 9 / 4);
    forgetAccounts (a);
}

TEST_CASE ("AT-COL-31 [UI] a collaborator's plug-in that isn't installed here: placeholder, notice with who added it", "[UI][S][AT-COL-31]")
{
    weft::test::RelayProcess relay;
    Ui a, b;
    shareWith (a, b, relay);
    const auto track = a.m().trackOrder.front();
    model::PluginInstance plugin;
    plugin.format = "vst3";
    plugin.pluginUid = "weft.tests.not-installed";
    plugin.name = "Phantom EQ";
    plugin.vendor = "Nobody";
    plugin.state = doc::Bytes { 1, 2, 3 };
    const auto pluginId = a.studio.session().addInsert (track, plugin);
    REQUIRE (pluginId);
    a.studio.edited();
    pump ({ &a, &b }, "the plug-in on B", [&] { return b.studio.pluginInstance (*pluginId) != nullptr; });
    REQUIRE (b.studio.processor (*pluginId) != nullptr);
    CHECK (b.studio.processor (*pluginId)->isPlaceholder());
    CHECK (b.main->lastAlert().contains ("Phantom EQ, added by Ana"));
    CHECK (b.studio.status.contains ("Phantom EQ (added by Ana) isn't installed here"));
    const auto list = b.studio.missingPluginList();
    REQUIRE (list.size() == 1);
    CHECK (list.front().name == "Phantom EQ");
    CHECK (list.front().addedBy == "Ana");

    // Said once: B's own fader move doesn't bring the notice back.
    b.main->lastAlert();
    const auto before = b.main->lastAlert();
    REQUIRE (b.studio.check (b.studio.session().setTrackVolume (track, -6.0)));
    b.studio.edited();
    REQUIRE (a.studio.check (a.studio.session().renameTrack (track, "Lead")));
    a.studio.edited();
    pump ({ &a, &b }, "the rename on B", [&] { return b.m().track (track)->name == "Lead"; });
    CHECK (b.main->lastAlert() == before);
    CHECK (a.m().track (track)->strip.inserts.front().state == plugin.state);
    forgetAccounts (a);
}

// Run with: weft_ui_tests "[.screenshot-m11]" (writes the PNGs into WEFT_SCREENSHOT_DIR or the working folder).
TEST_CASE ("Screenshots of collaborators in the window (M11)", "[.screenshot-m11]")
{
    weft::test::RelayProcess relay;
    FakeClock clock;
    Ui a, b;
    a.studio.syncClock = clock.fn();
    b.studio.syncClock = clock.fn();
    ticking = &clock;
    struct Stop
    {
        ~Stop() { ticking = nullptr; }
    } const stopTicking;
    // A's download of B's take stops partway, so the screenshot shows it arriving.
    std::atomic<std::int64_t> received { 0 };
    std::atomic<bool> release { false };
    struct Release
    {
        std::atomic<bool>& flag;
        ~Release() { flag = true; }
    } const releaseOnExit { release };
    a.studio.transferOptions.tamper = [&] (const std::string&, collab::Bytes& data) {
        while (received.load() >= (4 << 20) && ! release.load())
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
        received += static_cast<std::int64_t> (data.size());
    };
    const auto tracks = a.instrumentTracks (3);
    for (int i = 0; i < 3; ++i)
        REQUIRE (a.studio.session().renameTrack (tracks[static_cast<std::size_t> (i)], i == 0 ? "Keys" : i == 1 ? "Bass" : "Lead"));
    const auto r1 = a.studio.session().createMidiRegion (tracks[0], 0, 4 * bar);
    const auto r2 = a.studio.session().createMidiRegion (tracks[1], 4 * bar, 4 * bar);
    for (int i = 0; i < 16; ++i)
    {
        model::Note n;
        n.pos = i * 960;
        n.length = 480;
        n.pitch = 60 + (i * 5) % 12;
        REQUIRE (a.studio.session().addNote (*r1, n));
        n.pitch = 36 + (i * 3) % 7;
        REQUIRE (a.studio.session().addNote (*r2, n));
    }
    a.studio.edited();
    shareWith (a, b, relay);

    // B selects the bass region, works on Lead and points at bar 6 on Keys.
    b.studio.selection.set ({ *r2 });
    b.studio.selectTrack (tracks[2]);
    const auto bAt = juce::Point<float> (static_cast<float> (b.studio.view.tickToX (5.5 * bar)), b.timeline().laneBounds (1).getCentreY());
    b.timeline().mouseMove (mouse (b.timeline(), bAt));
    pump ({ &a, &b }, "B on A", [&] { return other (a) != nullptr && other (a)->state.pointer && other (a)->state.regions.size() == 1; });

    // A big audio take on B's side is still uploading to A.
    const auto file = writeSine (b.folder.path / "take.wav", 30.0);
    REQUIRE (b.studio.importFiles ({ file }, { 9 * bar, {} }).regions.size() == 1);
    b.studio.edited();
    pump ({ &a, &b }, "the take arriving on A", [&] {
        for (const auto& [id, r] : a.m().regions)
            if (! r->isMidi)
                if (const auto d = a.studio.downloadState (r->audio.media); d && d->fraction > 0.4)
                    return true;
        return false;
    }, 60);
    a.settle();

    const auto dir = juce::File (juce::SystemStats::getEnvironmentVariable ("WEFT_SCREENSHOT_DIR", juce::File::getCurrentWorkingDirectory().getFullPathName()));
    auto save = [&] (juce::Component& c, const juce::String& name) {
        juce::PNGImageFormat png;
        const auto f = dir.getChildFile (name);
        f.deleteFile();
        juce::FileOutputStream out (f);
        REQUIRE (png.writeImageToStream (c.createComponentSnapshot (c.getLocalBounds()), out));
    };
    save (*a.main, "m11-collaborators.png");
    forgetAccounts (a);
}
