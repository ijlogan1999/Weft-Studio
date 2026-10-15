// Arrange-view editing (spec 06 AT-ARR-2, 3, 5, 6, 7, 8, 9, 10, 12, 18, 21, 24, 27, 28)
// and the selection and zoom math behind AT-ARR-1, 4 and 22.

#include "Fixtures.h"

#include "weft/arrange/Grid.h"
#include "weft/arrange/MediaBrowser.h"
#include "weft/arrange/RegionDrag.h"
#include "weft/arrange/Selection.h"

#include <catch2/catch_approx.hpp>

using namespace weft;
using namespace weft::test;
using arrange::Snap;

namespace
{
constexpr Ticks bar = 3840;
const std::string media = "b3:00000000000000000000000000000000000000000000000000000000000000aa";

/** Audio 1-3 and Inst 1, a 20 s mono file in the pool. */
struct Arrange
{
    Session s = newSession();
    Id a1, a2, a3, inst;

    Arrange()
    {
        a1 = firstTrackOfKind (s, TrackKind::audio);
        a2 = s.createTrack (TrackKind::audio, "").value();
        a3 = s.createTrack (TrackKind::audio, "").value();
        inst = s.createTrack (TrackKind::instrument, "").value();
        model::MediaRef m;
        m.hash = media;
        m.originalName = "loop.wav";
        m.channels = 1;
        m.lengthSamples = 48000 * 20;
        REQUIRE (s.addMedia (m));
    }

    Id audio (const Id& track, Ticks start, Samples length = 96000, Samples offset = 0)
    {
        return s.createAudioRegion (track, start, media, offset, length).value();
    }
    const model::Region& region (const Id& id) const { return *s.model().region (id); }
    std::size_t undoDepth()
    {
        std::size_t n = 0;
        while (s.canUndo() && s.undo().done)
            ++n;
        for (std::size_t i = 0; i < n; ++i)
            s.redo();
        return n;
    }
};
} // namespace

TEST_CASE ("AT-ARR-2: a dragged region's start snaps; Alt or snap off doesn't", "[U][arrange][AT-ARR-2]")
{
    Arrange a;
    const auto r = a.audio (a.a1, 1000);
    const auto& sigs = a.s.model().signatures;

    SECTION ("snap 1/16 lands at 1,200")
    {
        arrange::RegionDrag drag (a.s, { r }, r, Snap::ticks (240), 0.0);
        drag.update (130, 0, false, false);
        CHECK (drag.anchorStart() == 1200);
        drag.commit();
        CHECK (a.region (r).start == 1200);
    }
    SECTION ("Alt held lands at 1,130")
    {
        arrange::RegionDrag drag (a.s, { r }, r, Snap::ticks (240), 0.0);
        drag.update (130, 0, true, false);
        drag.commit();
        CHECK (a.region (r).start == 1130);
    }
    SECTION ("snap off lands at 1,130")
    {
        arrange::RegionDrag drag (a.s, { r }, r, Snap::off(), 0.0);
        drag.update (130, 0, false, false);
        drag.commit();
        CHECK (a.region (r).start == 1130);
    }
    CHECK (arrange::snapTick (1130, Snap::ticks (240), sigs) == 1200);
    CHECK (arrange::snapTick (1079, Snap::ticks (240), sigs) == 960);
    CHECK (arrange::snapTick (1080, Snap::ticks (240), sigs) == 1200); // ties go later
    CHECK (arrange::snapTick (-50, Snap::ticks (240), sigs) == 0);
}

TEST_CASE ("AT-ARR-3: triplet grid and smart snap by zoom", "[U][arrange][AT-ARR-3]")
{
    Arrange a;
    const auto& sigs = a.s.model().signatures;
    CHECK (arrange::gridTicks (Snap::ticks (320), sigs, 0, 0.0) == 320);
    CHECK (arrange::snapLabel (Snap::ticks (320)) == "1/8T");
    CHECK (arrange::snapTick (500, Snap::ticks (320), sigs) == 640);

    // One bar = 40 px: beats would be 10 px apart, under the 16 px minimum, so smart snap uses bars.
    const double barAt40px = 40.0 / static_cast<double> (bar);
    CHECK (arrange::gridIsBars (Snap::smart(), sigs, 0, barAt40px));
    CHECK (arrange::snapTick (2000, Snap::smart(), sigs, barAt40px) == bar);
    CHECK (arrange::snapTick (1900, Snap::smart(), sigs, barAt40px) == 0);
    // Zoomed in: 1 bar = 400 px gives 25 px per 1/16, so smart snap uses 1/16.
    CHECK (arrange::gridTicks (Snap::smart(), sigs, 0, 400.0 / bar) == 240);
    // 1 bar = 100 px gives 25 px per beat.
    CHECK (arrange::gridTicks (Snap::smart(), sigs, 0, 100.0 / bar) == 960);
}

TEST_CASE ("Grid points restart at each bar line across a meter change", "[U][arrange]")
{
    Arrange a;
    REQUIRE (a.s.addSignaturePoint (1, 7, 8));
    const auto& sigs = a.s.model().signatures;
    // Bar 2 (0-based 1) starts at 3840 and is 7/8 = 3360 ticks long; bar 3 starts at 7200.
    CHECK (arrange::snapTick (3840 + 3300, Snap::ticks (640), sigs) == 7200);
    CHECK (arrange::snapTick (5000, Snap::bar(), sigs) == 3840);
    CHECK (arrange::snapTick (5600, Snap::bar(), sigs) == 7200);
    CHECK (arrange::gridTicks (Snap::beat(), sigs, 4000, 0.0) == 480);
}

TEST_CASE ("AT-ARR-5: moving two regions by a track and two bars; MIDI stays off audio tracks", "[U][arrange][AT-ARR-5]")
{
    Arrange a;
    const auto r1 = a.audio (a.a1, 0);
    const auto r2 = a.audio (a.a2, 2 * bar);
    REQUIRE (a.s.moveRegions ({ r1, r2 }, 2 * bar, 1));
    CHECK (a.region (r1).trackId == a.a2);
    CHECK (a.region (r1).start == 2 * bar);
    CHECK (a.region (r2).trackId == a.a3);
    CHECK (a.region (r2).start == 4 * bar);
    CHECK (a.s.undoName() == "Move Regions");

    const auto m = a.s.createMidiRegion (a.inst, 0, bar).value();
    REQUIRE (a.s.moveRegions ({ m }, bar, -1)); // row above is Audio 3
    CHECK (a.region (m).trackId == a.inst);
    CHECK (a.region (m).start == bar);
}

TEST_CASE ("Moved regions go on top of the regions they land on (D-W3-2)", "[U][arrange]")
{
    Arrange a;
    const auto low = a.audio (a.a1, 0);
    const auto high = a.audio (a.a1, bar);
    REQUIRE (a.region (high).z > a.region (low).z);
    REQUIRE (a.s.moveRegions ({ low }, bar, 0));
    CHECK (a.region (low).z > a.region (high).z);
}

TEST_CASE ("AT-ARR-6: dragging below the last track makes a track; Esc cancels", "[U][arrange][AT-ARR-6]")
{
    Arrange a;
    const auto m = a.s.createMidiRegion (a.inst, 0, bar).value();
    const auto r = a.audio (a.a3, 0);
    const auto tracksBefore = a.s.model().trackOrder.size();

    SECTION ("a new track of each region's kind")
    {
        arrange::RegionDrag drag (a.s, { m }, m, Snap::bar(), 0.0);
        drag.update (0, 1, false, false);
        REQUIRE (drag.preview().newTracks.size() == 1);
        drag.commit();
        REQUIRE (a.s.model().trackOrder.size() == tracksBefore + 1);
        const auto* t = a.s.model().track (a.region (m).trackId);
        CHECK (t->kind == TrackKind::instrument);
        CHECK (a.s.model().trackOrder.back() == t->id);

        // Audio 3 is two rows above the new last row; +2 rows from it is below the last track.
        REQUIRE (a.s.moveRegions ({ r }, 0, 3));
        const auto* audioTrack = a.s.model().track (a.region (r).trackId);
        CHECK (audioTrack->kind == TrackKind::audio);
        CHECK (audioTrack->channels == 1); // follows the mono file
        CHECK (a.s.model().trackOrder.size() == tracksBefore + 2);
        a.s.undo();
        CHECK (a.s.model().trackOrder.size() == tracksBefore + 1);
    }
    SECTION ("Esc during the drag leaves the document alone")
    {
        const auto heads = a.s.document().heads();
        const auto undoName = a.s.undoName();
        arrange::RegionDrag drag (a.s, { m, r }, m, Snap::bar(), 0.0);
        for (int i = 0; i < 50; ++i)
            drag.update (i * 100, i % 3, false, false);
        drag.cancel();
        CHECK (drag.commit().empty());
        CHECK (a.s.document().heads() == heads);
        CHECK (a.s.undoName() == undoName);
        CHECK (a.s.model().trackOrder.size() == tracksBefore);
    }
}

TEST_CASE ("AT-ARR-7: Ctrl+drag copies; copy and paste at the playhead on the selected track", "[U][arrange][AT-ARR-7]")
{
    Arrange a;
    const auto m = a.s.createMidiRegion (a.inst, 0, bar).value();
    model::Note n;
    n.pos = 480;
    REQUIRE (a.s.addNote (m, n));

    SECTION ("Ctrl+drag")
    {
        arrange::RegionDrag drag (a.s, { m }, m, Snap::bar(), 0.0);
        drag.update (bar + 100, 0, false, true);
        const auto copies = drag.commit();
        REQUIRE (copies.size() == 1);
        const auto& c = a.region (copies[0]);
        CHECK (c.id != m);
        CHECK (c.start == bar);
        CHECK (c.midi.sequenceId != a.region (m).midi.sequenceId);
        CHECK (a.s.model().sequence (c.midi.sequenceId)->notes.size() == 1);
        CHECK (a.region (m).start == 0);
        CHECK (a.s.undoName() == "Copy Region");
    }
    SECTION ("Ctrl+C, playhead at bar 9, track 3, Ctrl+V")
    {
        const auto r1 = a.audio (a.a1, bar, 48000, 100);
        const auto r2 = a.audio (a.a2, 2 * bar);
        const auto clip = commands::RegionClipboard::of (a.s.model(), { r1, r2 });
        const auto pasted = a.s.pasteRegions (clip, 8 * bar, a.a3);
        REQUIRE (pasted.size() == 2);
        const model::Region* first = &a.region (pasted[0]);
        const model::Region* second = &a.region (pasted[1]);
        if (first->start > second->start)
            std::swap (first, second);
        CHECK (first->start == 8 * bar);
        CHECK (first->trackId == a.a3);
        CHECK (first->audio.media == media);
        CHECK (first->audio.sourceOffset == 100);
        CHECK (second->start == 9 * bar);
        // One row below Audio 3 is the instrument track, so the audio copy stays on its own track... which is Audio 2.
        CHECK (second->trackId == a.a2);
        CHECK (a.s.undoName() == "Paste");
        REQUIRE (a.s.undo().done);
        CHECK (a.s.model().region (pasted[0]) == nullptr);
    }
    SECTION ("pasting regions whose track was deleted puts them on the target track")
    {
        const auto r1 = a.audio (a.a1, 0);
        const auto clip = commands::RegionClipboard::of (a.s.model(), { r1 });
        REQUIRE (a.s.deleteTrack (a.a1));
        const auto pasted = a.s.pasteRegions (clip, bar, a.a2);
        REQUIRE (pasted.size() == 1);
        CHECK (a.region (pasted[0]).trackId == a.a2);
    }
}

TEST_CASE ("AT-ARR-8: trimming keeps the audio in place and stops at the file's edges", "[U][arrange][AT-ARR-8]")
{
    Arrange a;
    // 4,800 samples at 120 BPM and 48 kHz is 192 ticks.
    const auto r = a.audio (a.a1, bar, 96000, 0);
    REQUIRE (a.s.trimRegionStart (r, bar + 192));
    CHECK (a.region (r).audio.sourceOffset == 4800);
    CHECK (a.region (r).length == 96000 - 4800);
    // The audio at timeline sample t comes from source sample t - regionStartSample + sourceOffset: unchanged.
    const auto startSample = static_cast<Samples> (a.s.model().tempo.tickToSample (static_cast<double> (a.region (r).start)));
    CHECK (startSample - a.region (r).audio.sourceOffset == 96000);

    REQUIRE (a.s.trimRegionLength (r, 48000 * 30));
    CHECK (a.region (r).audio.sourceOffset + a.region (r).length == 48000 * 20);
    REQUIRE (a.s.trimRegionStart (r, 0));
    CHECK (a.region (r).audio.sourceOffset == 0);
    CHECK (a.region (r).start == bar);

    const auto m = a.s.createMidiRegion (a.inst, 0, bar).value();
    model::Note n;
    n.pos = 0;
    REQUIRE (a.s.addNote (m, n));
    REQUIRE (a.s.trimRegionLength (m, 8 * bar));
    CHECK (a.region (m).length == 8 * bar);
}

TEST_CASE ("AT-ARR-9: Ctrl+T splits at the playhead with the right offsets and fades", "[U][arrange][AT-ARR-9]")
{
    Arrange a;
    const auto r = a.audio (a.a1, 0, 192000); // bars 1-3
    REQUIRE (a.s.setRegionFades (r, model::Fade { 480, "linear" }, model::Fade { 960, "sCurve" }));
    const auto pieces = a.s.splitRegionsAt ({ r }, bar);
    REQUIRE (pieces.size() == 1);
    const auto& left = a.region (r);
    const auto& right = a.region (pieces[0]);
    CHECK (left.start == 0);
    CHECK (left.length == 96000);
    CHECK (left.audio.sourceOffset == 0);
    CHECK (right.start == bar);
    CHECK (right.audio.sourceOffset == 96000);
    CHECK (right.audio.media == left.audio.media);
    CHECK (left.audio.fadeIn.length == 480);
    CHECK (left.audio.fadeOut.length == 0);
    CHECK (right.audio.fadeIn.length == 0);
    CHECK (right.audio.fadeOut.length == 960);
    CHECK (a.s.undoName() == "Split Region");

    SECTION ("several regions split as one undo step; regions the playhead misses are left alone")
    {
        const auto other = a.audio (a.a2, 0, 192000);
        const auto missed = a.audio (a.a3, 3 * bar);
        const auto more = a.s.splitRegionsAt ({ r, pieces[0], other, missed }, bar + bar / 2);
        CHECK (more.size() == 2);
        CHECK (a.s.undoName() == "Split Regions");
        REQUIRE (a.s.undo().done);
        CHECK (a.s.model().regions.size() == 4);
    }
}

TEST_CASE ("AT-ARR-10: a MIDI split keeps a crossing note whole on the left, separate sequences", "[U][arrange][AT-ARR-10]")
{
    Arrange a;
    const auto m = a.s.createMidiRegion (a.inst, 0, 2 * bar).value();
    model::Note crossing;
    crossing.pos = bar - 480;
    crossing.length = 960;
    model::Note later;
    later.pos = bar + 480;
    later.pitch = 64;
    REQUIRE (a.s.addNote (m, crossing));
    REQUIRE (a.s.addNote (m, later));
    const auto pieces = a.s.splitRegionsAt ({ m }, bar);
    REQUIRE (pieces.size() == 1);
    const auto* left = a.s.model().sequence (a.region (m).midi.sequenceId);
    const auto* right = a.s.model().sequence (a.region (pieces[0]).midi.sequenceId);
    REQUIRE (left != right);
    REQUIRE (left->notes.size() == 1);
    CHECK (left->notes.begin()->second.length == 960);
    REQUIRE (right->notes.size() == 1);
    auto note = right->notes.begin()->second;
    CHECK (note.pos == 480);
    note.velocity = 30;
    REQUIRE (a.s.updateNote (pieces[0], note));
    CHECK (a.s.model().sequence (a.region (m).midi.sequenceId)->notes.begin()->second.velocity == 100);
}

TEST_CASE ("AT-ARR-12: Delete leaves later regions in place and media in the pool", "[U][arrange][AT-ARR-12]")
{
    Arrange a;
    const auto r1 = a.audio (a.a1, 0);
    const auto r2 = a.audio (a.a1, 4 * bar);
    REQUIRE (a.s.deleteRegions ({ r1 }));
    CHECK (a.s.model().region (r1) == nullptr);
    CHECK (a.region (r2).start == 4 * bar);
    CHECK (a.s.model().mediaRef (media) != nullptr);
}

TEST_CASE ("AT-ARR-18: region name and color live on the region; no color follows the track", "[U][arrange][AT-ARR-18]")
{
    Arrange a;
    const auto r = a.audio (a.a1, 0);
    const auto other = a.audio (a.a1, 4 * bar);
    REQUIRE (a.s.setRegionName (r, "Verse gtr"));
    REQUIRE (a.s.setRegionColor (r, model::Color { 5, {} }));
    CHECK (a.region (r).name == "Verse gtr");
    CHECK (a.s.model().regionColor (a.region (r)).palette == 5);
    CHECK (a.region (other).name == "loop");
    CHECK (! a.region (other).color.has_value());
    CHECK (a.s.model().track (a.a1)->name == "Audio 1");
    REQUIRE (a.s.setTrackColor (a.a1, model::Color { 9, {} }));
    CHECK (a.s.model().regionColor (a.region (other)).palette == 9);
    CHECK (a.s.model().regionColor (a.region (r)).palette == 5);
    REQUIRE (a.s.setRegionColor (r, std::nullopt));
    CHECK (a.s.model().regionColor (a.region (r)).palette == 9);
}

TEST_CASE ("AT-ARR-21: markers are added, renamed, moved snapped, deleted and jumped to", "[U][arrange][AT-ARR-21]")
{
    Arrange a;
    const auto m1 = a.s.addMarker (2 * bar, "").value();
    const auto m2 = a.s.addMarker (5 * bar, "").value();
    CHECK (a.s.model().markers.at (m1)->name == "Marker 1");
    CHECK (a.s.model().markers.at (m2)->name == "Marker 2");
    REQUIRE (a.s.renameMarker (m1, "Chorus"));
    CHECK (a.s.model().markers.at (m1)->name == "Chorus");
    REQUIRE (a.s.moveMarker (m2, arrange::snapTick (6 * bar + 300, Snap::bar(), a.s.model().signatures)));
    CHECK (a.s.model().markers.at (m2)->pos == 6 * bar);
    // Clicking a marker moves the playhead to its position.
    CHECK (arrange::markerNumber (a.s.model(), 2) == 6 * bar);
    REQUIRE (a.s.deleteMarker (m1));
    CHECK (a.s.model().markers.size() == 1);
}

TEST_CASE ("AT-ARR-24: Alt+arrows nudge by the grid and clamp at zero", "[U][arrange][AT-ARR-24]")
{
    Arrange a;
    const auto& sigs = a.s.model().signatures;
    const auto r = a.audio (a.a1, bar);
    const auto step = arrange::gridStep (bar, +1, Snap::ticks (240), sigs) - bar;
    REQUIRE (a.s.nudgeRegions ({ r }, step));
    CHECK (a.region (r).start == bar + 240);
    CHECK (a.s.undoName() == "Nudge");

    const auto early = a.audio (a.a2, 100);
    REQUIRE (a.s.nudgeRegions ({ early }, arrange::gridStep (100, -1, Snap::ticks (240), sigs) - 100));
    CHECK (a.region (early).start == 0);
    CHECK (arrange::gridStep (bar, -1, Snap::bar(), sigs) == 0);
    CHECK (arrange::gridStep (5, -1, Snap::off(), sigs) == 4);
}

TEST_CASE ("AT-ARR-27: previous, next and numbered markers", "[U][arrange][AT-ARR-27]")
{
    Arrange a;
    // Bars 5, 9, 17 (1-based) start at ticks 4, 8 and 16 bars in. Added out of order on purpose.
    REQUIRE (a.s.addMarker (16 * bar, "C"));
    REQUIRE (a.s.addMarker (4 * bar, "A"));
    REQUIRE (a.s.addMarker (8 * bar, "B"));
    const auto playhead = 9 * bar; // bar 10
    CHECK (arrange::previousMarker (a.s.model(), playhead) == 8 * bar);
    CHECK (arrange::nextMarker (a.s.model(), playhead) == 16 * bar);
    CHECK (arrange::markerNumber (a.s.model(), 1) == 4 * bar);
    CHECK (! arrange::markerNumber (a.s.model(), 4).has_value());
    CHECK (! arrange::previousMarker (a.s.model(), 4 * bar).has_value());
    CHECK (! arrange::nextMarker (a.s.model(), 16 * bar).has_value());
}

TEST_CASE ("AT-ARR-28: Ctrl+R repeats a region or a block, one undo step", "[U][arrange][AT-ARR-28]")
{
    Arrange a;
    const auto m = a.s.createMidiRegion (a.inst, 2 * bar, 2 * bar).value(); // bar 3, 2 bars
    model::Note n;
    n.pos = 100;
    REQUIRE (a.s.addNote (m, n));

    SECTION ("once")
    {
        const auto copies = a.s.repeatRegions ({ m });
        REQUIRE (copies.size() == 1);
        CHECK (copies[0] != m);
        CHECK (a.region (copies[0]).start == 4 * bar);
        CHECK (a.s.model().sequence (a.region (copies[0]).midi.sequenceId)->notes.begin()->second.pos == 100);
    }
    SECTION ("three times")
    {
        const auto before = a.s.model().regions.size();
        const auto copies = a.s.repeatRegions ({ m }, 3);
        REQUIRE (copies.size() == 3);
        std::vector<Ticks> starts;
        for (const auto& id : copies)
            starts.push_back (a.region (id).start);
        std::sort (starts.begin(), starts.end());
        CHECK (starts == std::vector<Ticks> { 4 * bar, 6 * bar, 8 * bar });
        CHECK (a.s.undoName() == "Repeat");
        REQUIRE (a.s.undo().done);
        CHECK (a.s.model().regions.size() == before);
    }
    SECTION ("a block repeats after its last end")
    {
        const auto r = a.audio (a.a1, 0, 96000); // bar 1-2; the block is bar 1 to bar 5
        const auto copies = a.s.repeatRegions ({ m, r });
        REQUIRE (copies.size() == 2);
        for (const auto& id : copies)
        {
            const auto& c = a.region (id);
            CHECK (c.start == (c.isMidi ? 6 * bar : 4 * bar));
        }
    }
}

TEST_CASE ("Takes in a folder move and copy together (D-ARR-6)", "[U][arrange]")
{
    Arrange a;
    commands::RecordedAudio rec;
    rec.trackId = a.a1;
    rec.media.hash = "b3:00000000000000000000000000000000000000000000000000000000000000bb";
    rec.media.originalName = "rec.wav";
    rec.media.lengthSamples = 48000 * 8;
    rec.takes = { { 0, 0, 96000 }, { 0, 96000, 96000 } };
    rec.takeFolder = true;
    rec.folderStart = 0;
    rec.folderEnd = bar;
    REQUIRE (a.s.commitRecording ({ rec }, {}));
    REQUIRE (a.s.model().takeFolders.size() == 1);
    const auto& folder = *a.s.model().takeFolders.begin()->second;
    const auto take = folder.takeIds.front();
    REQUIRE (a.s.moveRegions ({ take }, bar, 1));
    const auto& moved = *a.s.model().takeFolders.begin()->second;
    CHECK (moved.trackId == a.a2);
    CHECK (moved.start == bar);
    for (const auto& id : moved.takeIds)
    {
        CHECK (a.region (id).trackId == a.a2);
        CHECK (a.region (id).start == bar);
    }
    const auto copies = a.s.copyRegions ({ take }, 4 * bar, 0);
    CHECK (copies.size() == 2);
    CHECK (a.s.model().takeFolders.size() == 2);
}

TEST_CASE ("Loop, mute and crossfade commands", "[U][arrange]")
{
    Arrange a;
    const auto r = a.audio (a.a1, 0, 96000);
    REQUIRE (a.s.setRegionLoop (r, 4 * bar));
    REQUIRE (a.region (r).loop.has_value());
    CHECK (a.region (r).loop->endTicks == 4 * bar);
    REQUIRE (a.s.moveRegions ({ r }, bar, 0));
    CHECK (a.region (r).loop->endTicks == 5 * bar); // the loop end moves with the region
    REQUIRE (a.s.setRegionLoop (r, std::nullopt));
    CHECK (! a.region (r).loop.has_value());

    const auto r2 = a.audio (a.a1, 0, 96000);
    REQUIRE (a.s.toggleRegionsMuted ({ r, r2 }));
    CHECK ((a.region (r).muted && a.region (r2).muted));
    REQUIRE (a.s.toggleRegionsMuted ({ r, r2 }));
    CHECK ((! a.region (r).muted && ! a.region (r2).muted));

    // r is bar 2-3 now; r3 starts half a bar into it: a 24,000-sample overlap.
    const auto r3 = a.audio (a.a1, bar + bar / 2, 96000, 96000);
    REQUIRE (a.s.crossfadeRegions ({ r, r3 }));
    CHECK (a.region (r).audio.fadeOut.length == 48000);
    CHECK (a.region (r).audio.fadeOut.curve == "equalPower");
    CHECK (a.region (r3).audio.fadeIn.length == 48000);
    CHECK (! a.s.crossfadeRegions ({ r2 }).ok);
}

TEST_CASE ("Selection: click, Shift+click, rubber band, select all, prune", "[U][arrange]")
{
    Arrange a;
    const auto r1 = a.audio (a.a1, 0);
    const auto r2 = a.audio (a.a2, 2 * bar);
    const auto r3 = a.audio (a.a3, 6 * bar);
    arrange::Selection sel;
    sel.click (r1, false);
    sel.click (r2, true);
    CHECK (sel.regions() == std::set<Id> { r1, r2 });
    sel.click (r1, true);
    CHECK (sel.regions() == std::set<Id> { r2 });
    sel.click (r3, false);
    CHECK (sel.regions() == std::set<Id> { r3 });
    sel.rubberBand (a.s.model(), 0, 2, bar - 10, 3 * bar, false); // touches r1's end and r2, not r3
    CHECK (sel.regions() == std::set<Id> { r1, r2 });
    sel.selectAll (a.s.model());
    CHECK (sel.regions().size() == 3);
    REQUIRE (a.s.deleteRegions ({ r3 }));
    sel.prune (a.s.model());
    CHECK (sel.regions().size() == 2);
    sel.clear();
    CHECK (sel.empty());
}

TEST_CASE ("Zoom keeps the tick under the pointer and Z fits the selection", "[U][arrange]")
{
    arrange::TimelineView v;
    v.originTick = 40 * bar;
    const double pointerX = 300.0;
    const auto tick = v.xToTick (pointerX);
    v.zoomAround (2.0, pointerX);
    CHECK (v.tickToX (tick) == Catch::Approx (pointerX));
    v.zoomAround (0.25, pointerX);
    CHECK (std::abs (v.tickToX (tick) - pointerX) <= 2.0);

    v.fit (4 * bar, 6 * bar, 1000.0);
    CHECK (v.tickToX (6 * bar) - v.tickToX (4 * bar) >= 800.0);
    CHECK (v.tickToX (4 * bar) >= 0.0);
    CHECK (v.tickToX (6 * bar) <= 1000.0);
}

TEST_CASE ("AT-LIB-2: the media browser lists each file with its duration and usage count", "[U][arrange][AT-LIB-2]")
{
    Arrange a;
    const auto r1 = a.audio (a.a1, 0);
    a.audio (a.a2, bar);
    auto list = arrange::mediaUsage (a.s.model());
    REQUIRE (list.size() == 1);
    CHECK (list[0].hash == media);
    CHECK (list[0].name == "loop.wav");
    CHECK (list[0].seconds == Catch::Approx (20.0));
    CHECK (list[0].regions == 2);
    REQUIRE (a.s.deleteRegions ({ r1 }));
    REQUIRE (a.s.deleteRegions ({ a.s.model().regions.begin()->first }));
    list = arrange::mediaUsage (a.s.model());
    REQUIRE (list.size() == 1);
    CHECK (list[0].regions == 0);
}
