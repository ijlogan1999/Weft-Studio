// AT-ARR-19, AT-ARR-20, AT-COL-1, AT-COL-30 (unit level), AT-TMP-5.

#include "Fixtures.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>
#include <string>
#include <vector>

using namespace weft;
using namespace weft::test;
using doc::Value;

namespace
{
struct NamedEdit
{
    std::string name;
    std::function<bool (Scene&)> run;
};

std::vector<NamedEdit> everyEditType()
{
    return {
        { "create audio region", [] (Scene& s) { return s.session.createAudioRegion (s.audioTrack, 30000, s.mediaHash, 1000, 9600).has_value(); } },
        { "create MIDI region", [] (Scene& s) { return s.session.createMidiRegion (s.instTrack, 15360, 3840).has_value(); } },
        { "move region", [] (Scene& s) { return s.session.moveRegion (s.audioRegion, 7680).ok; } },
        { "trim region start", [] (Scene& s) { return s.session.trimRegionStart (s.audioRegion, 4800).ok; } },
        { "trim region length", [] (Scene& s) { return s.session.trimRegionLength (s.midiRegion, 3000).ok; } },
        { "split MIDI region", [] (Scene& s) { return s.session.splitRegion (s.midiRegion, 3840).has_value(); } },
        { "split audio region", [] (Scene& s) { return s.session.splitRegion (s.audioRegion, 5760).has_value(); } },
        { "join regions",
          [] (Scene& s) {
              const auto right = s.session.splitRegion (s.midiRegion, 3840);
              return right && s.session.joinRegions ({ s.midiRegion, *right }).ok;
          } },
        { "delete region", [] (Scene& s) { return s.session.deleteRegions ({ s.midiRegion }).ok; } },
        { "move regions to another track", [] (Scene& s) { return s.session.moveRegions ({ s.audioRegion }, 960, 0).ok; } },
        { "move region below the last track", [] (Scene& s) { return s.session.moveRegions ({ s.midiRegion }, 0, 5).ok; } },
        { "copy regions", [] (Scene& s) { return ! s.session.copyRegions ({ s.audioRegion, s.midiRegion }, 3840, 0).empty(); } },
        { "paste",
          [] (Scene& s) {
              const auto clip = commands::RegionClipboard::of (s.session.model(), { s.midiRegion });
              return ! s.session.pasteRegions (clip, 30720, s.instTrack).empty();
          } },
        { "repeat", [] (Scene& s) { return s.session.repeatRegions ({ s.midiRegion }, 3).size() == 3; } },
        { "nudge", [] (Scene& s) { return s.session.nudgeRegions ({ s.audioRegion }, -240).ok; } },
        { "split at playhead", [] (Scene& s) { return s.session.splitRegionsAt ({ s.audioRegion, s.midiRegion }, 5760).size() == 2; } },
        { "loop region", [] (Scene& s) { return s.session.setRegionLoop (s.midiRegion, 30720).ok; } },
        { "toggle mute", [] (Scene& s) { return s.session.toggleRegionsMuted ({ s.audioRegion, s.midiRegion }).ok; } },
        { "fade", [] (Scene& s) { return s.session.setRegionFades (s.audioRegion, model::Fade { 4800, "linear" }, std::nullopt).ok; } },
        { "region gain", [] (Scene& s) { return s.session.setRegionGain (s.audioRegion, -6.0).ok; } },
        { "mute region", [] (Scene& s) { return s.session.setRegionMuted (s.audioRegion, true).ok; } },
        { "add note",
          [] (Scene& s) {
              model::Note n;
              n.pos = 100;
              n.pitch = 40;
              return s.session.addNote (s.midiRegion, n).has_value();
          } },
        { "move note",
          [] (Scene& s) {
              auto n = s.session.model().sequence (s.session.model().region (s.midiRegion)->midi.sequenceId)->notes.at (s.note);
              n.pos += 480;
              n.pitch += 2;
              return s.session.updateNote (s.midiRegion, n).ok;
          } },
        { "delete note", [] (Scene& s) { return s.session.deleteNotes (s.midiRegion, { s.note }).ok; } },
        { "quantize", [] (Scene& s) { return s.session.quantizeNotes (s.midiRegion, {}, 240, 1.0, 0.0).ok; } },
        { "region quantize", [] (Scene& s) { return s.session.setRegionQuantize (s.midiRegion, model::RegionQuantize { 480, 0.5, 0.2 }).ok; } },
        { "add plugin",
          [] (Scene& s) {
              model::PluginInstance p;
              p.pluginUid = "weft.comp";
              p.name = "Compressor";
              return s.session.addInsert (s.instTrack, p, 0).has_value();
          } },
        { "remove plugin", [] (Scene& s) { return s.session.removeInsert (s.instTrack, s.plugin).ok; } },
        { "change send", [] (Scene& s) { return s.session.setSendLevel (s.instTrack, s.send, -3.0).ok; } },
        { "add send",
          [] (Scene& s) {
              const auto bus2 = s.session.createTrack (TrackKind::bus, "Delay");
              return bus2 && s.session.addSend (s.audioTrack, *bus2).has_value();
          } },
        { "add automation point", [] (Scene& s) { return s.session.addAutomationPoint (s.instTrack, {}, 3840, 0.8).has_value(); } },
        { "tempo change", [] (Scene& s) { return s.session.addTempoPoint (7680, 140.0).has_value(); } },
        { "change initial tempo", [] (Scene& s) { return s.session.setTempoPoint ("initial", 0, 96.0).ok; } },
        { "signature change", [] (Scene& s) { return s.session.addSignaturePoint (2, 7, 8).has_value(); } },
        { "add marker", [] (Scene& s) { return s.session.addMarker (3840, "Verse").has_value(); } },
        { "set cycle", [] (Scene& s) { return s.session.setCycle (true, 3840, 11520).ok; } },
        { "new track", [] (Scene& s) { return s.session.createTrack (TrackKind::audio, "").has_value(); } },
        { "delete track", [] (Scene& s) { return s.session.deleteTrack (s.instTrack).ok; } },
        { "move track", [] (Scene& s) { return s.session.moveTrack (s.bus, 0).ok; } },
        { "volume", [] (Scene& s) { return s.session.setTrackVolume (s.audioTrack, -12.5).ok; } },
        { "rename project", [] (Scene& s) { return s.session.renameProject ("Renamed").ok; } },
    };
}
} // namespace

TEST_CASE ("AT-ARR-19 and AT-COL-1: every edit is one change that undoes and redoes", "[U][undo][AT-ARR-19][AT-COL-1]")
{
    for (const auto& edit : everyEditType())
    {
        DYNAMIC_SECTION (edit.name)
        {
            Scene s;
            const auto original = s.session.document().root();
            std::size_t changesBefore = 0, changesAfter = 0;
            s.session.document().allChanges (&changesBefore);

            REQUIRE (edit.run (s));
            const auto edited = s.session.document().root();
            REQUIRE (edited != original);
            s.session.document().allChanges (&changesAfter);
            const std::size_t expected = edit.name == "join regions" || edit.name == "add send" ? 2 : 1; // these run two commands
            CHECK (changesAfter - changesBefore == expected);

            while (s.session.canUndo() && s.session.document().root() != original)
                REQUIRE (s.session.undo().done);
            CHECK (s.session.document().root() == original);

            while (s.session.canRedo())
                REQUIRE (s.session.redo().done);
            CHECK (s.session.document().root() == edited);

            // AT-COL-1: replaying the change log from empty reproduces the document.
            doc::Document replay;
            REQUIRE (replay.apply (s.session.document().allChanges()) == doc::Status::ok);
            CHECK (replay.root() == s.session.document().root());
        }
    }
}

TEST_CASE ("AT-ARR-20: a 200-event drag is one undo step and one change", "[U][undo][AT-ARR-20]")
{
    Scene s;
    const auto original = s.session.document().root();
    std::size_t before = 0, after = 0;
    s.session.document().allChanges (&before);

    s.session.beginGesture ("Move Region");
    for (int i = 1; i <= 200; ++i)
        REQUIRE (s.session.moveRegion (s.audioRegion, 3840 + i * 10));
    CHECK (s.session.model().region (s.audioRegion)->start == 3840 + 2000); // projection follows the drag
    s.session.endGesture();

    s.session.document().allChanges (&after);
    CHECK (after - before == 1);
    CHECK (s.session.undoName() == "Move Region");
    REQUIRE (s.session.undo().done);
    CHECK (s.session.document().root() == original);
    CHECK (s.session.undoName() != "Move Region");
}

TEST_CASE ("Cancelling a gesture leaves no change and no undo step", "[U][undo]")
{
    Scene s;
    const auto original = s.session.document().root();
    const auto undoName = s.session.undoName();
    s.session.beginGesture ("Volume");
    for (int i = 0; i < 10; ++i)
        s.session.setTrackVolume (s.audioTrack, -1.0 * i);
    s.session.cancelGesture();
    CHECK (s.session.document().root() == original);
    CHECK (s.session.undoName() == undoName);
    CHECK (s.session.model().track (s.audioTrack)->strip.volumeDb == 0.0);
}

TEST_CASE ("A new edit clears the redo stack", "[U][undo]")
{
    Scene s;
    REQUIRE (s.session.setRegionGain (s.audioRegion, -3.0));
    REQUIRE (s.session.undo().done);
    CHECK (s.session.canRedo());
    REQUIRE (s.session.setRegionMuted (s.audioRegion, true));
    CHECK_FALSE (s.session.canRedo());
}

TEST_CASE ("Rejected commands write nothing", "[U][undo]")
{
    Scene s;
    const auto original = s.session.document().root();
    std::size_t before = 0, after = 0;
    s.session.document().allChanges (&before);
    CHECK_FALSE (s.session.moveRegion (s.audioRegion, -5));
    CHECK_FALSE (s.session.moveRegion (s.audioRegion, 0, s.instTrack)); // audio region on an instrument track
    CHECK_FALSE (s.session.deleteTempoPoint ("initial"));
    CHECK_FALSE (s.session.setTempoPoint ("initial", 960, 100.0));
    CHECK_FALSE (s.session.addSend (s.bus, s.bus).has_value());
    CHECK_FALSE (s.session.deleteTrack (s.session.model().info.masterTrackId));
    CHECK_FALSE (s.session.lastError().empty());
    s.session.document().allChanges (&after);
    CHECK (after == before);
    CHECK (s.session.document().root() == original);
}

TEST_CASE ("AT-MIX-9 Sends and outputs that would make a feedback loop are rejected", "[U][routing]")
{
    Scene s;
    // Track A -> Bus 1 -> Bus 2 is allowed.
    const auto bus2 = s.session.createTrack (TrackKind::bus, "Bus B").value();
    REQUIRE (s.session.setTrackOutput (s.audioTrack, s.bus));
    REQUIRE (s.session.setTrackOutput (s.bus, bus2));
    // Bus 2 -> Bus 1, as an output or a send, would close the loop: the mixer disables both.
    CHECK (s.session.wouldCycle (bus2, s.bus, "out:" + bus2));
    CHECK (s.session.wouldCycle (bus2, s.bus));
    CHECK_FALSE (s.session.addSend (bus2, s.bus).has_value());
    CHECK (s.session.lastError().find ("feedback loop") != std::string::npos);
    CHECK_FALSE (s.session.setTrackOutput (bus2, s.bus));
    CHECK (s.session.lastError().find ("feedback loop") != std::string::npos);
    CHECK_FALSE (s.session.wouldCycle (s.audioTrack, bus2));
    CHECK (s.session.addSend (s.audioTrack, bus2).has_value());
}

TEST_CASE ("AT-MIX-8 New Bus from a send, and removing the bus", "[U][routing]")
{
    Scene s;
    const auto before = s.session.model().trackOrder.size();
    CHECK (s.session.nextBusName() == "Bus 1");
    const auto bus = s.session.newBus (s.audioTrack, true);
    REQUIRE (bus);
    const auto& m = s.session.model();
    const auto* b = m.track (*bus);
    REQUIRE (b != nullptr);
    CHECK (b->name == "Bus 1");
    CHECK (b->kind == TrackKind::bus);
    CHECK (b->output.busId.empty()); // Master
    CHECK (std::abs (b->strip.volumeDb) < 1e-9);
    CHECK (m.trackOrder.size() == before + 1);
    CHECK (m.trackOrder.back() == *bus); // a bus track in the tracks area
    const auto& sends = m.track (s.audioTrack)->strip.sends;
    REQUIRE (sends.size() == 1);
    CHECK (sends[0].targetBusId == *bus);
    CHECK (std::abs (sends[0].levelDb) < 1e-9);
    CHECK_FALSE (sends[0].pre);
    CHECK (s.session.nextBusName() == "Bus 2");

    // One undo step.
    REQUIRE (s.session.undo().done);
    CHECK (s.session.model().track (*bus) == nullptr);
    CHECK (s.session.model().track (s.audioTrack)->strip.sends.empty());
    REQUIRE (s.session.redo().done);

    // Another channel outputs to it; removing it removes the send and reroutes that channel to Master.
    REQUIRE (s.session.setTrackOutput (s.instTrack, *bus));
    CHECK (s.session.sendsTo (*bus) == 1);
    REQUIRE (s.session.deleteTrack (*bus));
    CHECK (s.session.model().track (s.audioTrack)->strip.sends.empty());
    CHECK (s.session.model().track (s.instTrack)->output.busId.empty());
}

TEST_CASE ("Insert and send limits", "[U][routing]")
{
    Scene s;
    for (int i = 1; i < model::maxInserts; ++i)
    {
        model::PluginInstance p;
        p.pluginUid = "weft.gain";
        REQUIRE (s.session.addInsert (s.instTrack, p).has_value());
    }
    model::PluginInstance p;
    p.pluginUid = "weft.gain";
    CHECK_FALSE (s.session.addInsert (s.instTrack, p).has_value());

    REQUIRE (s.session.model().track (s.instTrack)->strip.sends.size() == 1); // the scene's send to s.bus
    std::vector<Id> buses;
    for (int i = 0; i < model::maxSends; ++i)
        buses.push_back (s.session.createTrack (TrackKind::bus, "Bus").value());
    for (int i = 1; i < model::maxSends; ++i)
        REQUIRE (s.session.addSend (s.instTrack, buses[static_cast<std::size_t> (i)]).has_value());
    CHECK_FALSE (s.session.addSend (s.instTrack, buses[0]).has_value());
    // A channel sends to a bus once (W-7 step 7).
    const auto other = s.session.addSend (s.audioTrack, s.bus);
    REQUIRE (other);
    CHECK_FALSE (s.session.addSend (s.audioTrack, s.bus).has_value());
    CHECK_FALSE (s.session.setSendTarget (s.instTrack, s.session.model().track (s.instTrack)->strip.sends[1].id, s.bus));
}

TEST_CASE ("Splitting a MIDI region moves the right-hand notes to a new sequence", "[U][regions]")
{
    Scene s;
    const auto right = s.session.splitRegion (s.midiRegion, 3840);
    REQUIRE (right);
    const auto& m = s.session.model();
    const auto* l = m.region (s.midiRegion);
    const auto* r = m.region (*right);
    CHECK (l->length == 3840);
    CHECK (r->start == 3840);
    CHECK (r->length == 3840);
    CHECK (r->midi.sequenceId != l->midi.sequenceId);
    CHECK (m.sequence (l->midi.sequenceId)->notes.size() == 4);
    REQUIRE (m.sequence (r->midi.sequenceId)->notes.size() == 4);
    CHECK (m.sequence (r->midi.sequenceId)->sortedNotes().front()->pos == 13); // rebased to the new region

    // Joining brings them back into the first region.
    REQUIRE (s.session.joinRegions ({ s.midiRegion, *right }));
    CHECK (s.session.model().region (*right) == nullptr);
    CHECK (s.session.model().sequence (s.session.model().region (s.midiRegion)->midi.sequenceId)->notes.size() == 8);
    CHECK (s.session.model().region (s.midiRegion)->length == 7680);
}

TEST_CASE ("Splitting an audio region keeps the audio continuous", "[U][regions]")
{
    Scene s;
    REQUIRE (s.session.setRegionFades (s.audioRegion, model::Fade { 480, "linear" }, model::Fade { 960, "linear" }));
    const auto right = s.session.splitRegion (s.audioRegion, 3840 + 960); // 960 ticks = 24000 samples at 120 BPM, 48 kHz
    REQUIRE (right);
    const auto* l = s.session.model().region (s.audioRegion);
    const auto* r = s.session.model().region (*right);
    CHECK (l->length == 24000);
    CHECK (r->audio.sourceOffset == 24000);
    CHECK (r->length == 48000 * 4 - 24000);
    CHECK (l->audio.fadeIn.length == 480);
    CHECK (l->audio.fadeOut.length == 0);
    CHECK (r->audio.fadeIn.length == 0);
    CHECK (r->audio.fadeOut.length == 960);
    REQUIRE (s.session.joinRegions ({ *right, s.audioRegion }));
    CHECK (s.session.model().region (s.audioRegion)->length == 48000 * 4);
}

TEST_CASE ("AT-TMP-5: inserting a signature change doesn't move regions", "[U][tempo][AT-TMP-5]")
{
    Scene s;
    std::map<Id, Ticks> starts;
    for (const auto& [id, r] : s.session.model().regions)
        starts[id] = r->start;
    REQUIRE (s.session.addSignaturePoint (1, 3, 4).has_value());
    REQUIRE (s.session.addSignaturePoint (2, 6, 8).has_value());
    for (const auto& [id, r] : s.session.model().regions)
        CHECK (r->start == starts[id]);
    CHECK (s.session.model().signatures.barStart (2) == 3840 + 2880);
}

TEST_CASE ("AT-COL-30: undo reverts only what is still mine", "[U][undo][collab][AT-COL-30]")
{
    Scene scene;
    auto& a = scene.session;
    auto b = joinSession (a, 99, "userB");
    const auto r = scene.audioRegion;
    constexpr Ticks bar1 = 0, bar3 = 2 * 3840, bar5 = 4 * 3840;
    REQUIRE (a.moveRegion (r, bar1));
    sync (a, b);

    SECTION ("A moves R, B changes R's gain, A undoes: R goes back and B's gain stays")
    {
        REQUIRE (a.moveRegion (r, bar3));
        sync (a, b);
        REQUIRE (b.setRegionGain (r, -9.0));
        sync (a, b);
        REQUIRE (a.undo().done);
        sync (a, b);
        for (auto* s : { &a, &b })
        {
            CHECK (s->model().region (r)->start == bar1);
            CHECK (s->model().region (r)->audio.gainDb == -9.0);
        }
    }

    SECTION ("A moves R, B moves R again, A undoes: R stays where B put it")
    {
        REQUIRE (a.moveRegion (r, bar3));
        sync (a, b);
        REQUIRE (b.moveRegion (r, bar5));
        sync (a, b);
        a.undo();
        sync (a, b);
        CHECK (a.model().region (r)->start == bar5);
        CHECK (b.model().region (r)->start == bar5);
    }

    SECTION ("A edits R, B deletes R, A undoes: nothing happens and the status explains")
    {
        REQUIRE (a.setRegionGain (r, -4.0));
        sync (a, b);
        REQUIRE (b.deleteRegions ({ r }));
        sync (a, b);
        const auto before = a.document().root();
        const auto result = a.undo();
        CHECK_FALSE (result.done);
        CHECK (result.message.find ("a region was deleted by someone else") != std::string::npos);
        CHECK (a.document().root() == before);
        CHECK (a.model().region (r) == nullptr);
    }

    SECTION ("Undo only touches this user's changes")
    {
        REQUIRE (b.setRegionMuted (r, true));
        sync (a, b);
        REQUIRE (a.setTrackVolume (scene.audioTrack, -6.0));
        sync (a, b);
        REQUIRE (a.undo().done);
        sync (a, b);
        CHECK (a.model().region (r)->muted);
        CHECK (a.model().track (scene.audioTrack)->strip.volumeDb == 0.0);
    }
}
