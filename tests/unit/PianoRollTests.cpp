// Piano roll and quantize commands (spec 06 §7 PRL, §8 QNT).

#include "Fixtures.h"

#include "weft/time/Quantize.h"

#include <algorithm>

using namespace weft;
using namespace weft::test;
using commands::NoteRef;
using commands::NoteSpan;

namespace
{
struct Roll
{
    Session s = newSession();
    Id track, region;

    explicit Roll (Ticks start = 0, Ticks length = 7680)
    {
        track = s.createTrack (TrackKind::instrument, "Keys").value();
        region = s.createMidiRegion (track, start, length).value();
    }

    const model::Region& r() const { return *s.model().region (region); }

    NoteRef add (Ticks timelinePos, int pitch = 60, int velocity = 100, Ticks length = 240)
    {
        const auto ref = s.drawNote (region, timelinePos, pitch, length, velocity);
        REQUIRE (ref.has_value());
        return *ref;
    }

    const model::Note& note (const NoteRef& ref) const
    {
        const auto* n = commands::findNote (s.model(), ref);
        REQUIRE (n != nullptr);
        return *n;
    }

    Ticks at (const NoteRef& ref) const { return commands::noteTimelineTick (r(), note (ref).pos); }
};
} // namespace

TEST_CASE ("AT-PRL-2 pencil creates a note with defaults; past the end extends; before the start does nothing", "[U]")
{
    Roll roll (3840, 7680);
    const auto ref = roll.add (3840 + 960);
    const auto& n = roll.note (ref);
    CHECK (roll.at (ref) == 3840 + 960);
    CHECK (n.length == 240);
    CHECK (n.velocity == 100);
    CHECK (n.channel == 1);
    CHECK (n.pitch == 60);

    // Past the end (bar 4 ends at 11520): the region grows to the end of the bar holding the note.
    roll.add (11520 + 100);
    CHECK (roll.r().length == 11520);
    CHECK (roll.r().start + roll.r().length == 15360);

    const auto notesBefore = roll.s.model().sequence (roll.r().midi.sequenceId)->notes.size();
    CHECK_FALSE (roll.s.drawNote (roll.region, 100, 60, 240, 100).has_value());
    CHECK (roll.s.model().sequence (roll.r().midi.sequenceId)->notes.size() == notesBefore);
}

TEST_CASE ("AT-PRL-4 transpose and nudge move the selection as a group", "[U]")
{
    Roll roll;
    const auto a = roll.add (960, 60);
    const auto b = roll.add (1920, 64);
    REQUIRE (roll.s.moveNotes ({ a, b }, 0, 1));
    CHECK (roll.note (a).pitch == 61);
    CHECK (roll.note (b).pitch == 65);
    REQUIRE (roll.s.moveNotes ({ a, b }, 0, 12));
    CHECK (roll.note (a).pitch == 73);

    const auto high = roll.add (2880, 120);
    CHECK_FALSE (roll.s.moveNotes ({ high }, 0, 12)); // 132 > 127: nothing moves
    CHECK (roll.note (high).pitch == 120);
    CHECK_FALSE (roll.s.moveNotes ({ a, high }, 0, 12));
    CHECK (roll.note (a).pitch == 73);

    REQUIRE (roll.s.moveNotes ({ a }, 480, 0)); // Right arrow with grid 1/8
    CHECK (roll.at (a) == 1440);

    // Moving left stops at the region start for the whole group.
    REQUIRE (roll.s.moveNotes ({ a, b }, -5000, 0));
    CHECK (roll.at (a) == 0);
    CHECK (roll.at (b) == 1920 - 1440);
}

TEST_CASE ("AT-PRL-5 resizing a note snaps the moved edge", "[U]")
{
    const NoteSpan n { 960, 240 };
    CHECK (commands::resizeNote (n, true, 960 + 500, 240) == NoteSpan { 960, 480 });
    const auto left = commands::resizeNote (n, false, 1000, 120);
    CHECK (left.end() == n.end());
    CHECK (left.start == 960 + 0);
    CHECK (commands::resizeNote (n, false, 1100, 120).end() == n.end());
    CHECK (commands::resizeNote (n, true, 900, 0) == NoteSpan { 960, 1 });
    CHECK (commands::resizeNote (n, false, 5000, 0) == NoteSpan { 1199, 1 });
    CHECK (commands::resizeNote (n, true, 900, 240).length == 240);

    Roll roll;
    const auto a = roll.add (960);
    REQUIRE (roll.s.setNoteSpan (a, 960, 480));
    CHECK (roll.note (a).length == 480);
    REQUIRE (roll.s.setNoteSpan (a, 1200, 240));
    CHECK (roll.at (a) == 1200);
}

TEST_CASE ("AT-PRL-6 delete removes the selected notes", "[U]")
{
    Roll roll;
    const auto a = roll.add (0);
    const auto b = roll.add (960);
    const auto c = roll.add (1920);
    REQUIRE (roll.s.deleteNotes ({ a, c }));
    CHECK (commands::findNote (roll.s.model(), a) == nullptr);
    CHECK (commands::findNote (roll.s.model(), b) != nullptr);
    CHECK (commands::findNote (roll.s.model(), c) == nullptr);
    REQUIRE (roll.s.deleteNotes ({ b })); // the eraser deletes one note
    CHECK (roll.s.undoName() == "Delete Note");
}

TEST_CASE ("AT-PRL-7 copy and paste at the playhead keeps offsets with new ids", "[U]")
{
    Roll roll (0, 3840);
    const auto second = roll.s.createMidiRegion (roll.track, 7680, 3840).value();
    const auto a = roll.add (240, 60);
    const auto b = roll.add (720, 62);
    const auto c = roll.add (1200, 64);
    const auto clip = commands::NoteClipboard::of (roll.s.model(), { a, b, c });
    REQUIRE (clip.notes.size() == 3);

    const auto pasted = roll.s.pasteNotes (clip, roll.track, 7680 + 960);
    REQUIRE (pasted.size() == 3);
    std::vector<std::pair<Ticks, int>> placed;
    for (const auto& ref : pasted)
    {
        CHECK (ref.regionId == second);
        CHECK (ref.noteId != a.noteId);
        CHECK (ref.noteId != b.noteId);
        CHECK (ref.noteId != c.noteId);
        const auto* n = commands::findNote (roll.s.model(), ref);
        REQUIRE (n != nullptr);
        placed.emplace_back (commands::noteTimelineTick (*roll.s.model().region (second), n->pos), n->pitch);
    }
    std::sort (placed.begin(), placed.end());
    CHECK (placed == std::vector<std::pair<Ticks, int>> { { 8640, 60 }, { 9120, 62 }, { 9600, 64 } });

    // No MIDI region under the playhead: nothing is pasted.
    CHECK (roll.s.pasteNotes (clip, roll.track, 5000).empty());
}

TEST_CASE ("AT-PRL-8 velocity edits clamp per note; brightness rises with velocity", "[U]")
{
    Roll roll;
    const auto a = roll.add (0, 60, 50);
    const auto b = roll.add (960, 60, 100);
    const auto c = roll.add (1920, 60, 120);
    REQUIRE (roll.s.offsetNotesVelocity ({ a, b, c }, 20));
    CHECK (roll.note (a).velocity == 70);
    CHECK (roll.note (b).velocity == 120);
    CHECK (roll.note (c).velocity == 127);
    REQUIRE (roll.s.setNotesVelocity ({ a, b, c }, 64));
    CHECK (roll.note (a).velocity == 64);
    CHECK (roll.note (b).velocity == 64);
    CHECK (roll.note (c).velocity == 64);
    REQUIRE (roll.s.setNotesVelocity ({ b }, 90)); // a velocity-lane bar drag
    CHECK (roll.note (b).velocity == 90);

    for (int v = 1; v < 127; ++v)
        CHECK (commands::velocityBrightness (v) < commands::velocityBrightness (v + 1));
}

TEST_CASE ("AT-PRL-10 destructive quantize with strength, ties and the region start", "[U]")
{
    Roll roll;
    const auto a = roll.add (250);
    const auto b = roll.add (470);
    const auto c = roll.add (1000, 60, 100, 333);
    REQUIRE (roll.s.quantizeNotes (std::vector<Id> { roll.region }, { a, b, c }, 240, 1.0, 0.0));
    CHECK (roll.at (a) == 240);
    CHECK (roll.at (b) == 480);
    CHECK (roll.at (c) == 960);
    CHECK (roll.note (c).length == 333);

    REQUIRE (roll.s.undo().done);
    REQUIRE (roll.s.quantizeNotes (std::vector<Id> { roll.region }, { a, b, c }, 240, 0.5, 0.0));
    CHECK (roll.at (a) == 245);
    CHECK (roll.at (b) == 475);
    CHECK (roll.at (c) == 980);

    const auto tie = roll.add (120);
    REQUIRE (roll.s.quantizeNotes (std::vector<Id> { roll.region }, { tie }, 240, 1.0, 0.0));
    CHECK (roll.at (tie) == 0);

    Roll late (3900, 3840);
    const auto early = late.add (3910);
    REQUIRE (late.s.quantizeNotes (std::vector<Id> { late.region }, { early }, 240, 1.0, 0.0));
    CHECK (late.at (early) == 3900); // 3840 is before the region start
    // The per-region command clamps the same way.
    const auto early2 = late.add (3905);
    REQUIRE (late.s.quantizeNotes (late.region, { early2.noteId }, 240, 1.0, 0.0));
    CHECK (late.at (early2) == 3900);
}

TEST_CASE ("AT-PRL-11 swing shifts off-beat grid points", "[U]")
{
    CHECK (quantizeTick (0, 240, 1.0, 100.0) == 0);
    CHECK (quantizeTick (240, 240, 1.0, 100.0) == 320);
    CHECK (quantizeTick (480, 240, 1.0, 100.0) == 480);
    CHECK (quantizeTick (720, 240, 1.0, 100.0) == 800);
    CHECK (quantizeTick (240, 240, 1.0, 0.0) == 240);
    CHECK (quantizeTick (240, 240, 1.0, 50.0) == 280);

    Roll roll;
    const auto off = roll.add (250);
    REQUIRE (roll.s.quantizeNotes (std::vector<Id> { roll.region }, { off }, 240, 1.0, 100.0));
    CHECK (roll.at (off) == 320);
}

TEST_CASE ("AT-PRL-12 quantize is idempotent and covers every shown note when none are selected", "[U]")
{
    Roll roll (0, 3840);
    const auto other = roll.s.createMidiRegion (roll.track, 3840, 3840).value();
    const auto a = roll.add (250);
    const auto b = roll.add (1013);
    const auto c = roll.s.drawNote (other, 3840 + 470, 60, 240, 100).value();
    REQUIRE (roll.s.quantizeNotes (std::vector<Id> { roll.region, other }, {}, 240, 1.0, 30.0));
    const auto first = std::vector<Ticks> { roll.at (a), roll.at (b) };
    CHECK (roll.at (a) == weft::quantizeTick (250, 240, 1.0, 30.0));
    CHECK (commands::noteTimelineTick (*roll.s.model().region (other), roll.note (c).pos) == weft::quantizeTick (3840 + 470, 240, 1.0, 30.0));
    roll.s.quantizeNotes (std::vector<Id> { roll.region, other }, {}, 240, 1.0, 30.0);
    CHECK (roll.at (a) == first[0]);
    CHECK (roll.at (b) == first[1]);
}

TEST_CASE ("AT-PRL-14 controller points store CC and 14-bit pitch bend values", "[U]")
{
    Roll roll;
    model::CcEvent cc;
    cc.kind = "cc";
    cc.number = 1;
    cc.pos = 0;
    cc.value = 0;
    REQUIRE (roll.s.addController (roll.region, cc));
    cc.id = {};
    cc.pos = 3840;
    cc.value = 127;
    REQUIRE (roll.s.addController (roll.region, cc));
    const auto stored = roll.s.model().sequence (roll.r().midi.sequenceId)->controllers;
    REQUIRE (stored.size() == 2);
    std::vector<std::pair<Ticks, int>> points;
    for (const auto& [id, e] : stored)
        points.emplace_back (e.pos, e.value);
    std::sort (points.begin(), points.end());
    CHECK (points == std::vector<std::pair<Ticks, int>> { { 0, 0 }, { 3840, 127 } });

    model::CcEvent bend;
    bend.kind = "pitchBend";
    bend.value = -8192;
    CHECK (roll.s.addController (roll.region, bend));
    bend.id = {};
    bend.value = 8191;
    CHECK (roll.s.addController (roll.region, bend));
    bend.id = {};
    bend.value = 8192;
    CHECK_FALSE (roll.s.addController (roll.region, bend));
    cc.id = {};
    cc.value = 128;
    CHECK_FALSE (roll.s.addController (roll.region, cc));

    const auto first = stored.begin()->first;
    REQUIRE (roll.s.moveController (roll.region, first, 960, 200));
    CHECK (roll.s.model().sequence (roll.r().midi.sequenceId)->controllers.at (first).value <= 127);
    REQUIRE (roll.s.deleteControllers (roll.region, { first }));
    CHECK_FALSE (roll.s.model().sequence (roll.r().midi.sequenceId)->controllers.contains (first));
}

TEST_CASE ("AT-PRL-15 Ctrl+M mutes the selected notes and again unmutes", "[U]")
{
    Roll roll;
    const auto a = roll.add (0);
    const auto b = roll.add (960);
    REQUIRE (roll.s.toggleNotesMuted ({ a, b }));
    CHECK (roll.note (a).muted);
    CHECK (roll.note (b).muted);
    CHECK (roll.s.undoName() == "Mute Notes");
    REQUIRE (roll.s.toggleNotesMuted ({ a, b }));
    CHECK_FALSE (roll.note (a).muted);
    CHECK_FALSE (roll.note (b).muted);
}

TEST_CASE ("AT-QNT-1 Apply Quantize bakes region quantize in one undo step", "[U]")
{
    Roll roll;
    const auto a = roll.add (250);
    const auto b = roll.add (700);
    REQUIRE (roll.s.setRegionQuantize (roll.region, model::RegionQuantize { 480, 1.0, 0.0 }));
    CHECK (roll.at (a) == 250); // stored positions are unchanged
    REQUIRE (roll.s.applyRegionQuantize (roll.region));
    CHECK (roll.s.undoName() == "Apply Quantize");
    CHECK (roll.at (a) == 480);
    CHECK (roll.at (b) == 480);
    CHECK_FALSE (roll.r().midi.quantize.has_value());
    REQUIRE (roll.s.undo().done);
    CHECK (roll.at (a) == 250);
    CHECK (roll.r().midi.quantize.has_value());
}

TEST_CASE ("AT-QNT-2 region quantize applies on top of destructive quantize", "[U]")
{
    Roll roll;
    const auto a = roll.add (700);
    REQUIRE (roll.s.quantizeNotes (std::vector<Id> { roll.region }, { a }, 240, 1.0, 0.0));
    CHECK (roll.at (a) == 720); // stored
    REQUIRE (roll.s.setRegionQuantize (roll.region, model::RegionQuantize { 960, 1.0, 0.0 }));
    CHECK (roll.at (a) == 720); // stored stays; playback uses the region grid on top
    const auto q = *roll.r().midi.quantize;
    CHECK (weft::quantizeTick (roll.at (a), q.grid, q.strength, q.swing * 100.0) == 960);
}

TEST_CASE ("AT-QNT-3 Q is one undo step named Quantize", "[U]")
{
    Roll roll;
    roll.add (250);
    roll.add (470);
    roll.add (1013);
    REQUIRE (roll.s.quantizeNotes (std::vector<Id> { roll.region }, {}, 240, 1.0, 0.0));
    CHECK (roll.s.undoName() == "Quantize");
    REQUIRE (roll.s.undo().done);
    CHECK (roll.s.undoName() == "Add Note");
}

TEST_CASE ("Note and controller edits refresh just that event and leave the projection equal to a full read", "[U]")
{
    // Session::refreshFor re-reads one event for a note edit (functionality audit F12); this checks
    // the shortcut against reading the whole document after every kind of event edit, undo and redo.
    Roll r;
    auto sequence = [&] (const model::ProjectModel& m) { return m.sequence (m.region (r.region)->midi.sequenceId); };
    auto same = [&] {
        const auto full = model::Projector::project (r.s.document());
        const auto* a = sequence (r.s.model());
        const auto* b = sequence (full);
        REQUIRE (a != nullptr);
        REQUIRE (b != nullptr);
        return a->notes == b->notes && a->controllers.size() == b->controllers.size();
    };
    std::vector<Id> notes;
    for (int i = 0; i < 20; ++i)
    {
        model::Note n;
        n.pos = i * 120;
        n.pitch = 40 + i;
        notes.push_back (r.s.addNote (r.region, n).value());
    }
    CHECK (same());
    auto edited = sequence (r.s.model())->notes.at (notes[3]);
    edited.velocity = 20;
    edited.pos += 7;
    REQUIRE (r.s.updateNote (r.region, edited));
    CHECK (same());
    REQUIRE (r.s.deleteNotes (r.region, { notes[5], notes[6] }));
    CHECK (same());
    model::CcEvent cc;
    cc.pos = 300;
    const auto ccId = r.s.addController (r.region, cc);
    REQUIRE (ccId);
    CHECK (same());
    REQUIRE (r.s.deleteControllers (r.region, { *ccId }));
    CHECK (same());
    for (int i = 0; i < 24; ++i) // back to the empty region
    {
        REQUIRE (r.s.undo().done);
        CHECK (same());
    }
    CHECK (sequence (r.s.model())->notes.empty());
    for (int i = 0; i < 24; ++i)
    {
        REQUIRE (r.s.redo().done);
        CHECK (same());
    }
    CHECK (sequence (r.s.model())->notes.size() == 18);
}
