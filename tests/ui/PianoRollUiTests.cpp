// Piano roll window (spec 06 AT-PRL-1, 2, 3, 4, 7, 8, 15; AT-QNT-3), driven through its pointer API and keys.

#include "UiFixture.h"

using namespace weft;
using namespace weft::ui;
using namespace weft::ui::test;
using commands::NoteRef;

namespace
{
struct RollUi : Ui
{
    Id track, midi;

    RollUi()
    {
        track = instrumentTracks (1).front();
        midi = studio.session().createMidiRegion (track, 0, 2 * bar).value();
        studio.edited();
        settle();
    }

    PianoRoll& roll() { return main->bottom().pianoRoll(); }

    NoteRef note (Ticks at, int pitch, int velocity = 100, Ticks length = 240, const Id& in = {})
    {
        const auto ref = studio.session().drawNote (in.empty() ? midi : in, at, pitch, length, velocity);
        REQUIRE (ref);
        studio.edited();
        settle();
        return *ref;
    }

    void open (const Id& id)
    {
        timeline().pointerDown (centreOf (id), {}, 2);
        timeline().pointerUp (centreOf (id), {});
        settle();
        REQUIRE (roll().isVisible());
    }

    juce::Point<float> at (double tick, int pitch)
    {
        return { static_cast<float> (roll().tickToX (tick)), roll().pitchToY (pitch) + static_cast<float> (roll().rowHeight) / 2.0f };
    }

    void press (juce::Point<float> p, juce::ModifierKeys mods = {})
    {
        roll().pointerDown (p, mods);
        roll().pointerUp();
        settle();
    }

    void dragFrom (juce::Point<float> from, juce::Point<float> to, juce::ModifierKeys mods = {})
    {
        roll().pointerDown (from, mods);
        roll().pointerDrag (to, mods);
        roll().pointerUp();
        settle();
    }

    const model::Note& get (const NoteRef& ref)
    {
        const auto* n = commands::findNote (m(), ref);
        REQUIRE (n != nullptr);
        return *n;
    }

    Ticks tickOf (const NoteRef& ref) { return commands::noteTimelineTick (*m().region (ref.regionId), get (ref).pos); }

    std::vector<NoteRef> allNotes()
    {
        std::vector<NoteRef> out;
        for (const auto& n : roll().shownNotes())
            out.push_back (n.ref);
        return out;
    }
};
} // namespace

TEST_CASE ("AT-PRL-1 double-click opens the piano roll; two selected regions show together", "[UI]")
{
    RollUi ui;
    const auto second = ui.studio.session().createMidiRegion (ui.track, 3 * bar, bar).value();
    ui.note (100, 60);
    ui.note (3 * bar + 100, 64, 100, 240, second);
    ui.open (ui.midi);
    CHECK (ui.studio.layout.bottom == BottomTab::pianoRoll);
    CHECK (commands::noteName (60) == "C3");
    CHECK (ui.roll().shownNotes().size() == 1);

    ui.studio.selection.set ({ ui.midi, second });
    ui.studio.edited();
    ui.settle();
    CHECK (ui.studio.pianoRollRegions() == std::vector<Id> { ui.midi, second });
    CHECK (ui.roll().shownNotes().size() == 2);
    // Both regions are on screen after the refit.
    CHECK (ui.roll().tickToX (0) >= ui.roll().gridArea().getX());
    CHECK (ui.roll().tickToX (4 * bar) <= ui.roll().gridArea().getRight());
}

TEST_CASE ("AT-PRL-2 pencil draws a snapped note and remembers length and velocity", "[UI]")
{
    RollUi ui;
    ui.open (ui.midi);
    ui.studio.piano.grid = 240;
    ui.roll().setTool (NoteTool::pencil);
    ui.press (ui.at (1000, 60));
    auto notes = ui.roll().shownNotes();
    REQUIRE (notes.size() == 1);
    CHECK (notes[0].stored == 960);
    CHECK (notes[0].length == 240);
    CHECK (notes[0].velocity == 100);
    CHECK (notes[0].channel == 1);
    CHECK (ui.studio.session().undoName() == "Add Note");

    // Drawing and dragging on makes a longer note; the next one uses that length.
    ui.dragFrom (ui.at (1920 + 10, 62), ui.at (1920 + 480, 62));
    const auto longer = ui.studio.piano.selection;
    REQUIRE (longer.size() == 1);
    CHECK (ui.get (*longer.begin()).length == 480);
    CHECK (ui.studio.session().undoName() == "Add Note"); // drawing and stretching is one step

    // The velocity tool sets the last-used velocity.
    ui.roll().setTool (NoteTool::velocity);
    ui.dragFrom (ui.at (1920 + 100, 62), ui.at (1920 + 100, 62) + juce::Point<float> (0.0f, 10.0f));
    CHECK (ui.get (*longer.begin()).velocity == 90);

    ui.roll().setTool (NoteTool::pencil);
    ui.press (ui.at (3840 + 10, 64));
    const auto third = *ui.studio.piano.selection.begin();
    CHECK (ui.get (third).length == 480);
    CHECK (ui.get (third).velocity == 90);
}

TEST_CASE ("AT-PRL-3 piano keys and rubber band select notes", "[UI]")
{
    RollUi ui;
    const auto e1 = ui.note (0, 64);
    const auto e2 = ui.note (960, 64);
    const auto g = ui.note (1920, 67);
    const auto c = ui.note (2880, 60);
    ui.open (ui.midi);
    auto& roll = ui.roll();

    const auto key = [&] (int pitch) { return juce::Point<float> (10.0f, roll.pitchToY (pitch) + 3.0f); };
    ui.press (key (64));
    CHECK (ui.studio.piano.selection == std::set<NoteRef> { e1, e2 });
    ui.press (key (67), juce::ModifierKeys::shiftModifier);
    CHECK (ui.studio.piano.selection == std::set<NoteRef> { e1, e2, g });
    ui.press (key (60));
    CHECK (ui.studio.piano.selection == std::set<NoteRef> { c });

    // A rubber band from empty grid touching the two E3 notes.
    ui.dragFrom (ui.at (100, 66), ui.at (1000, 63));
    CHECK (ui.studio.piano.selection == std::set<NoteRef> { e1, e2 });
}

TEST_CASE ("AT-PRL-4 arrow keys transpose and move the selected notes", "[UI]")
{
    RollUi ui;
    const auto a = ui.note (960, 60);
    const auto high = ui.note (1920, 120);
    ui.open (ui.midi);
    ui.roll().topPitch = 66; // the fit centres between 60 and 120; scroll to 60
    ui.press (ui.at (960 + 100, 60));
    REQUIRE (ui.studio.piano.selection == std::set<NoteRef> { a });

    ui.key (juce::KeyPress::upKey);
    CHECK (ui.get (a).pitch == 61);
    ui.key (juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier);
    CHECK (ui.get (a).pitch == 73);
    ui.key (juce::KeyPress::downKey);
    CHECK (ui.get (a).pitch == 72);

    ui.studio.piano.grid = 480;
    ui.key (juce::KeyPress::rightKey);
    CHECK (ui.tickOf (a) == 1440);

    ui.studio.piano.selection = { high };
    ui.key (juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier);
    CHECK (ui.get (high).pitch == 120);

    // Clicking the arrange view gives the keys back to it: the arrow does nothing to notes.
    ui.studio.piano.selection = { a };
    ui.click (ui.timeline().getLocalBounds().toFloat().getBottomRight() - juce::Point<float> (5.0f, 5.0f));
    ui.key (juce::KeyPress::upKey);
    CHECK (ui.get (a).pitch == 72);
}

TEST_CASE ("AT-PRL-5 dragging a note edge resizes it on the grid", "[UI]")
{
    RollUi ui;
    const auto a = ui.note (960, 60);
    ui.open (ui.midi);
    ui.studio.piano.grid = 240;
    const auto right = ui.at (960 + 240, 60) - juce::Point<float> (1.0f, 0.0f);
    ui.dragFrom (right, ui.at (960 + 500, 60));
    CHECK (ui.get (a).length == 480);
    ui.dragFrom (ui.at (960, 60) + juce::Point<float> (1.0f, 0.0f), ui.at (1200 + 20, 60));
    CHECK (ui.tickOf (a) == 1200);
    CHECK (ui.tickOf (a) + ui.get (a).length == 1440);
}

TEST_CASE ("AT-PRL-6 Delete and the eraser remove notes", "[UI]")
{
    RollUi ui;
    const auto a = ui.note (0, 60);
    const auto b = ui.note (960, 62);
    ui.open (ui.midi);
    ui.press (ui.at (100, 60));
    ui.key (juce::KeyPress::deleteKey);
    CHECK (commands::findNote (ui.m(), a) == nullptr);
    ui.roll().setTool (NoteTool::eraser);
    ui.press (ui.at (1000, 62));
    CHECK (commands::findNote (ui.m(), b) == nullptr);
}

TEST_CASE ("AT-PRL-7 Ctrl+C and Ctrl+V paste notes at the playhead", "[UI]")
{
    RollUi ui;
    ui.note (0, 60);
    ui.note (240, 62);
    ui.note (720, 64);
    ui.open (ui.midi);
    ui.press (ui.at (100, 60));
    ui.key ('A', juce::ModifierKeys::commandModifier);
    REQUIRE (ui.studio.piano.selection.size() == 3);
    ui.key ('C', juce::ModifierKeys::commandModifier);
    ui.studio.locate (bar);
    ui.key ('V', juce::ModifierKeys::commandModifier);
    REQUIRE (ui.studio.piano.selection.size() == 3);
    std::vector<Ticks> ticks;
    for (const auto& ref : ui.studio.piano.selection)
        ticks.push_back (ui.tickOf (ref));
    std::sort (ticks.begin(), ticks.end());
    CHECK (ticks == std::vector<Ticks> { bar, bar + 240, bar + 720 });
    CHECK (ui.roll().shownNotes().size() == 6);

    // Ctrl+drag copies too.
    ui.studio.piano.selection.clear();
    ui.press (ui.at (100, 60));
    ui.dragFrom (ui.at (100, 60), ui.at (100 + 2880, 60), juce::ModifierKeys::commandModifier);
    CHECK (ui.roll().shownNotes().size() == 7);
}

TEST_CASE ("AT-PRL-8 velocity lane, velocity tool and the inspector set velocities", "[UI]")
{
    RollUi ui;
    const auto a = ui.note (0, 60, 50);
    const auto b = ui.note (960, 62, 100);
    const auto c = ui.note (1920, 64, 120);
    ui.open (ui.midi);
    auto& roll = ui.roll();
    REQUIRE (roll.lane == LaneKind {});

    // A velocity-lane bar drag sets that note.
    const auto x = static_cast<float> (roll.tickToX (960)) + 1.0f;
    roll.pointerDown ({ x, roll.laneY (100) });
    roll.pointerDrag ({ x, roll.laneY (30) });
    roll.pointerUp();
    ui.settle();
    CHECK (std::abs (ui.get (b).velocity - 30) <= 1);
    REQUIRE (ui.studio.session().setNotesVelocity ({ b }, 100));
    ui.studio.edited();
    ui.settle();

    // The velocity tool drags all selected notes by the same amount, each clamped.
    ui.studio.piano.selection = { a, b, c };
    roll.setTool (NoteTool::velocity);
    ui.dragFrom (ui.at (1000, 62), ui.at (1000, 62) - juce::Point<float> (0.0f, 20.0f));
    CHECK (ui.get (a).velocity == 70);
    CHECK (ui.get (b).velocity == 120);
    CHECK (ui.get (c).velocity == 127);

    // Typing in the inspector's note section sets all three.
    auto& inspector = ui.main->inspector();
    REQUIRE (inspector.noteVelocity.isVisible());
    CHECK (inspector.noteVelocity.getText() == juce::String (juce::CharPointer_UTF8 ("70\xe2\x80\x93" "127")));
    inspector.noteVelocity.setText ("64", juce::sendNotificationSync);
    ui.settle();
    CHECK (ui.get (a).velocity == 64);
    CHECK (ui.get (b).velocity == 64);
    CHECK (ui.get (c).velocity == 64);
    CHECK (inspector.noteVelocity.getText() == "64");
}

TEST_CASE ("AT-PRL-8 a pencil line across the velocity lane ramps velocities", "[UI]")
{
    RollUi ui;
    const auto a = ui.note (0, 60, 100);
    const auto b = ui.note (960, 60, 100);
    const auto c = ui.note (1920, 60, 100);
    ui.open (ui.midi);
    auto& roll = ui.roll();
    roll.setTool (NoteTool::pencil);
    const auto x = [&] (double t) { return static_cast<float> (roll.tickToX (t)); };
    roll.pointerDown ({ x (0) - 2.0f, roll.laneY (20) });
    roll.pointerDrag ({ x (1920) + 2.0f, roll.laneY (120) });
    roll.pointerUp();
    ui.settle();
    CHECK (ui.get (a).velocity < ui.get (b).velocity);
    CHECK (ui.get (b).velocity < ui.get (c).velocity);
    CHECK (std::abs (ui.get (b).velocity - 70) <= 3);
    CHECK (ui.studio.session().undoName() == "Velocity");
}

TEST_CASE ("AT-PRL-14 the controller lane adds, moves and deletes points", "[UI]")
{
    RollUi ui;
    ui.open (ui.midi);
    auto& roll = ui.roll();
    roll.laneBox.setSelectedId (2, juce::sendNotificationSync); // Modulation (CC 1)
    REQUIRE (roll.lane == LaneKind { LaneKind::Type::cc, 1 });
    const auto x = [&] (double t) { return static_cast<float> (roll.tickToX (t)); };
    ui.press ({ x (0) + 1.0f, roll.laneY (0) });
    ui.press ({ x (3840), roll.laneY (127) });
    const auto& seq = *ui.m().sequence (ui.region (ui.midi).midi.sequenceId);
    REQUIRE (seq.controllers.size() == 2);
    std::vector<std::pair<Ticks, int>> points;
    for (const auto& [id, e] : seq.controllers)
    {
        CHECK (e.kind == "cc");
        CHECK (e.number == 1);
        points.emplace_back (e.pos, e.value);
    }
    std::sort (points.begin(), points.end());
    CHECK (points == std::vector<std::pair<Ticks, int>> { { 0, 0 }, { 3840, 127 } });

    // Clicking a point selects it; Delete removes it.
    ui.press ({ x (3840), roll.laneY (127) });
    REQUIRE (ui.studio.piano.point.has_value());
    ui.key (juce::KeyPress::deleteKey);
    CHECK (ui.m().sequence (ui.region (ui.midi).midi.sequenceId)->controllers.size() == 1);

    // Pitch bend points span -8192..8191.
    roll.laneBox.setSelectedId (4, juce::sendNotificationSync);
    ui.press ({ x (960), roll.laneY (-8192) });
    bool bendLow = false;
    for (const auto& [id, e] : ui.m().sequence (ui.region (ui.midi).midi.sequenceId)->controllers)
        bendLow = bendLow || (e.kind == "pitchBend" && e.value == -8192);
    CHECK (bendLow);
}

TEST_CASE ("AT-PRL-15 Ctrl+M mutes and unmutes the selected notes", "[UI]")
{
    RollUi ui;
    const auto a = ui.note (0, 60);
    ui.open (ui.midi);
    ui.press (ui.at (100, 60));
    ui.key ('M', juce::ModifierKeys::commandModifier);
    CHECK (ui.get (a).muted);
    ui.key ('M', juce::ModifierKeys::commandModifier);
    CHECK_FALSE (ui.get (a).muted);
}

TEST_CASE ("AT-QNT-3 Q quantizes the shown notes as one undo step", "[UI]")
{
    RollUi ui;
    const auto a = ui.note (250, 60);
    const auto b = ui.note (1013, 62);
    ui.open (ui.midi);
    ui.press (ui.at (3000, 70)); // empty grid: focus, nothing selected
    REQUIRE (ui.studio.piano.selection.empty());
    ui.key ('Q');
    CHECK (ui.tickOf (a) == 240);
    CHECK (ui.tickOf (b) == 960);
    CHECK (ui.studio.session().undoName() == "Quantize");
    ui.key ('Z', juce::ModifierKeys::commandModifier);
    CHECK (ui.tickOf (a) == 250);
    CHECK (ui.tickOf (b) == 1013);
}

TEST_CASE ("AT-QNT-1 the inspector sets region quantize and applies it", "[UI]")
{
    RollUi ui;
    const auto a = ui.note (250, 60);
    ui.studio.selection.set ({ ui.midi });
    ui.studio.edited();
    ui.settle();
    auto& inspector = ui.main->inspector();
    REQUIRE (inspector.regionQuantize.isVisible());
    inspector.regionQuantize.setSelectedId (5, juce::sendNotificationSync); // 1/8
    ui.settle();
    REQUIRE (ui.region (ui.midi).midi.quantize.has_value());
    CHECK (ui.region (ui.midi).midi.quantize->grid == 480);
    CHECK (ui.tickOf (a) == 250);
    inspector.applyQuantize.onClick();
    ui.settle();
    CHECK (ui.tickOf (a) == 480);
    CHECK_FALSE (ui.region (ui.midi).midi.quantize.has_value());
}

// Run with: weft_ui_tests "[.screenshot-pianoroll]" (writes WEFT_SCREENSHOT or ./pianoroll.png).
TEST_CASE ("Screenshot of the piano roll", "[.screenshot-pianoroll]")
{
    RollUi ui;
    auto& s = ui.studio.session();
    s.renameTrack (ui.track, "Keys");
    const int chord[] = { 60, 64, 67, 72 };
    for (int i = 0; i < 8; ++i)
        ui.note (i * 960, chord[i % 4], 40 + i * 12, 720);
    const auto muted = ui.note (4 * 960 + 480, 69, 90, 240);
    s.toggleNotesMuted ({ muted });
    for (int i = 0; i < 5; ++i)
    {
        model::CcEvent e;
        e.number = 1;
        e.pos = i * 1440;
        e.value = 20 + i * 25;
        s.addController (ui.midi, e);
    }
    ui.studio.layout.bottomHeight = 420;
    ui.studio.edited();
    ui.settle();
    ui.open (ui.midi);
    ui.main->resized();
    ui.settle();
    ui.roll().fitRegions();
    ui.studio.piano.selection = { muted };
    ui.press (ui.at (100, 60));
    ui.press (ui.at (960 + 100, 64), juce::ModifierKeys::shiftModifier);
    const auto image = ui.main->createComponentSnapshot (ui.main->getLocalBounds());
    const auto* env = std::getenv ("WEFT_SCREENSHOT");
    juce::File out (env != nullptr ? juce::String (env) : juce::File::getCurrentWorkingDirectory().getChildFile ("pianoroll.png").getFullPathName());
    out.deleteFile();
    juce::FileOutputStream stream (out);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
}
