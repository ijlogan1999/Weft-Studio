// Arrange window [UI] criteria: ruler, selection, zoom, tools, markers, track headers.

#include "UiFixture.h"

#include "weft/time/Position.h"

using namespace weft;
using namespace weft::ui;
using namespace weft::ui::test;

TEST_CASE ("AT-ARR-1 ruler bar numbers follow the signature map; Ctrl+wheel zooms at the pointer", "[UI]")
{
    Ui ui;
    REQUIRE (ui.studio.session().addSignaturePoint (2, 3, 4)); // bar 3 onward in 3/4
    ui.studio.view.pixelsPerTick = 80.0 / bar;
    ui.studio.edited();
    ui.settle();
    const auto& m = ui.m();
    const auto labels = Ruler::barLabels (m, ui.studio.view, 2000);
    REQUIRE (labels.size() >= 6);
    for (const auto& [number, x] : labels)
    {
        INFO ("bar " << number);
        CHECK (x == Catch::Approx (ui.studio.view.tickToX (static_cast<double> (m.signatures.barStart (number - 1)))));
    }
    // Bars 1-2 are 4/4, bar 3 on is 3/4: label spacing changes accordingly.
    CHECK (labels[1].second - labels[0].second == Catch::Approx (bar * ui.studio.view.pixelsPerTick));
    CHECK (labels[3].second - labels[2].second == Catch::Approx (bar * 3 / 4 * ui.studio.view.pixelsPerTick));

    // Zoomed far out, labels thin out instead of colliding.
    auto far = ui.studio.view;
    far.pixelsPerTick = 2.0 / bar;
    const auto thin = Ruler::barLabels (m, far, 2000);
    for (std::size_t i = 1; i < thin.size(); ++i)
        CHECK (thin[i].second - thin[i - 1].second >= 30.0);

    auto& tl = ui.timeline();
    for (const float x : { 137.0f, 512.0f, 901.0f })
        for (const float delta : { 0.5f, -0.35f })
        {
            const auto tickUnder = ui.studio.view.xToTick (x);
            tl.wheel ({ x, 40.0f }, juce::ModifierKeys::commandModifier, delta);
            ui.settle();
            CHECK (std::abs (ui.studio.view.tickToX (tickUnder) - x) <= 2.0);
        }
}

TEST_CASE ("AT-ARR-4 click, Shift+click, rubber band, Ctrl+A, click empty", "[UI]")
{
    Ui ui;
    const auto tracks = ui.instrumentTracks (2);
    auto& s = ui.studio.session();
    const auto a = *s.createMidiRegion (tracks[0], 0, bar);
    const auto b = *s.createMidiRegion (tracks[0], 2 * bar, bar);
    const auto c = *s.createMidiRegion (tracks[1], bar, bar);
    ui.studio.edited();
    ui.settle();
    auto& sel = ui.studio.selection;

    ui.click (ui.centreOf (a));
    CHECK (sel.ids() == std::vector<Id> { a });
    ui.click (ui.centreOf (b), juce::ModifierKeys::shiftModifier);
    CHECK (sel.contains (a));
    CHECK (sel.contains (b));
    ui.click (ui.centreOf (a), juce::ModifierKeys::shiftModifier);
    CHECK_FALSE (sel.contains (a));
    CHECK (sel.contains (b));

    // Click on empty lane space deselects.
    const auto empty = juce::Point<float> (static_cast<float> (ui.studio.view.tickToX (6.0 * bar)), ui.centreOf (a).y);
    ui.click (empty);
    CHECK (sel.ids().empty());

    // Rubber band from empty space across both instrument tracks, touching a and c.
    auto& tl = ui.timeline();
    const auto x1 = static_cast<float> (ui.studio.view.tickToX (0.5 * bar));
    const auto x2 = static_cast<float> (ui.studio.view.tickToX (1.5 * bar));
    const auto top = tl.laneBounds (1).getY() + 1.0f; // track row 1 = first instrument track
    const auto bottom = tl.laneBounds (2).getBottom() - 1.0f;
    // Start the band where there's no region: the gap at bar 1-2 on the first instrument track is empty above c.
    const juce::Point<float> from { x2, top }, to { x1, bottom };
    REQUIRE (tl.regionAt (from) == nullptr);
    tl.pointerDown (from, {});
    tl.pointerDrag (to, {});
    tl.pointerUp (to, {});
    ui.settle();
    CHECK (sel.contains (a));
    CHECK (sel.contains (c));
    CHECK_FALSE (sel.contains (b));

    ui.key ('A', juce::ModifierKeys::commandModifier);
    CHECK (sel.ids().size() == 3);
}

TEST_CASE ("AT-ARR-22 Z zooms to the selection or the whole project", "[UI]")
{
    Ui ui;
    const auto tracks = ui.instrumentTracks (1);
    auto& s = ui.studio.session();
    const auto a = *s.createMidiRegion (tracks[0], 8 * bar, bar);
    const auto b = *s.createMidiRegion (tracks[0], 11 * bar, 2 * bar);
    const auto far = *s.createMidiRegion (tracks[0], 60 * bar, bar);
    ui.studio.edited();
    ui.settle();
    const auto width = static_cast<double> (ui.timeline().getWidth());
    REQUIRE (width > 300);

    ui.studio.selection.set ({ a, b });
    ui.key ('Z');
    const auto x1 = ui.studio.view.tickToX (8.0 * bar), x2 = ui.studio.view.tickToX (13.0 * bar);
    CHECK (x1 >= 0.0);
    CHECK (x2 <= width);
    CHECK ((x2 - x1) / width >= 0.8);

    ui.studio.selection.clear();
    ui.key ('Z');
    CHECK (ui.studio.view.tickToX (0.0) >= -1.0);
    CHECK (ui.studio.view.tickToX (ui.m().regionEndTick (ui.region (far))) <= width);
}

TEST_CASE ("AT-ARR-23 tool menu: pencil, scissors, glue, eraser, Esc", "[UI]")
{
    Ui ui;
    const auto tracks = ui.instrumentTracks (1);
    ui.studio.view.pixelsPerTick = 400.0 / bar; // zoomed in so the smart grid is finer than the region
    auto& main = *ui.main;
    auto& tl = ui.timeline();
    const auto lane = tl.laneBounds (1);

    // T opens the menu (through the window); the menu lists every tool.
    std::optional<Cmd> asked;
    ui.studio.uiCommand = [&] (Cmd c) { asked = c; };
    ui.key ('T');
    CHECK (asked == Cmd::toolMenu);
    CHECK (main.toolMenu().getNumItems() == 6);

    main.pickTool (static_cast<int> (Tool::pencil) + 1);
    ui.settle();
    REQUIRE (ui.studio.tool == Tool::pencil);
    const auto at = juce::Point<float> (static_cast<float> (ui.studio.view.tickToX (2.0 * bar + 500.0)), lane.getCentreY());
    ui.click (at);
    const auto regions = ui.m().regionsOnTrack (tracks[0]);
    REQUIRE (regions.size() == 1);
    const auto made = regions.front()->id;
    CHECK (ui.region (made).isMidi);
    CHECK (ui.region (made).start == ui.studio.snapped (2 * bar + 500));
    CHECK (ui.region (made).length == bar);

    // Scissors splits at the click.
    main.pickTool (static_cast<int> (Tool::scissors) + 1);
    const auto start = ui.region (made).start;
    const auto cut = juce::Point<float> (static_cast<float> (ui.studio.view.tickToX (static_cast<double> (start + bar / 2))), lane.getCentreY());
    ui.click (cut);
    REQUIRE (ui.m().regionsOnTrack (tracks[0]).size() == 2);
    CHECK (ui.region (made).length == bar / 2);

    // Glue joins the clicked region with the selected one.
    const auto pieces = ui.m().regionsOnTrack (tracks[0]);
    const auto other = pieces[0]->id == made ? pieces[1]->id : pieces[0]->id;
    ui.studio.selection.set ({ made });
    main.pickTool (static_cast<int> (Tool::glue) + 1);
    ui.click (ui.centreOf (other));
    REQUIRE (ui.m().regionsOnTrack (tracks[0]).size() == 1);
    const auto joined = ui.m().regionsOnTrack (tracks[0]).front()->id;
    CHECK (ui.region (joined).length == bar);

    // Eraser deletes.
    main.pickTool (static_cast<int> (Tool::eraser) + 1);
    ui.click (ui.centreOf (joined));
    CHECK (ui.m().regionsOnTrack (tracks[0]).empty());

    ui.key (juce::KeyPress::escapeKey);
    CHECK (ui.studio.tool == Tool::pointer);
}

TEST_CASE ("AT-ARR-21 markers in the ruler: add, rename, move snapped, delete, click to jump", "[UI]")
{
    Ui ui;
    auto& ruler = ui.ruler();
    ui.studio.locate (3 * bar);
    ui.key (';', {}, ';');
    REQUIRE (ui.m().markers.size() == 1);
    const auto id = ui.m().markers.begin()->first;
    CHECK (ui.m().markers.begin()->second->name == "Marker 1");
    CHECK (ui.m().markers.begin()->second->pos == 3 * bar);

    const float laneY = Ruler::rulerHeight + Ruler::markerLane / 2.0f;
    // Drag it by about one and a bit bars: it lands on the grid.
    const auto x0 = static_cast<float> (ui.studio.view.tickToX (3.0 * bar)) + 10.0f;
    const auto x1 = static_cast<float> (ui.studio.view.tickToX (4.2 * bar)) + 10.0f;
    ruler.pointerDown ({ x0, laneY }, {});
    ruler.pointerDrag ({ x1, laneY });
    ruler.pointerUp();
    ui.settle();
    const auto moved = ui.m().markers.begin()->second->pos;
    CHECK (moved == ui.studio.snapped (moved));
    CHECK (moved > 3 * bar);
    // One undo step for the drag.
    ui.main->run (Cmd::undo);
    ui.settle();
    CHECK (ui.m().markers.begin()->second->pos == 3 * bar);

    REQUIRE (ui.studio.session().renameMarker (id, "Chorus"));
    CHECK (ui.m().markers.begin()->second->name == "Chorus");

    // Clicking a marker moves the playhead to it.
    ui.studio.locate (0);
    ruler.pointerDown ({ x0, laneY }, {});
    ruler.pointerUp();
    CHECK (ui.studio.playhead() == 3 * bar);

    // Double-click on an empty part of the lane adds one; right-click deletes.
    const auto emptyX = static_cast<float> (ui.studio.view.tickToX (8.0 * bar)) + 2.0f;
    ruler.mouseDoubleClick (mouse (ruler, { emptyX, laneY }, {}, 2));
    ui.settle();
    CHECK (ui.m().markers.size() == 2);
    ruler.pointerDown ({ x0, laneY }, juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier));
    ruler.pointerUp();
    ui.settle();
    CHECK (ui.m().markers.size() == 1);
    CHECK (ui.m().markers.find (id) == ui.m().markers.end());
}

TEST_CASE ("AT-ARR-27 marker keys from the keyboard", "[UI]")
{
    Ui ui;
    auto& s = ui.studio.session();
    for (const int b : { 5, 9, 17 })
        REQUIRE (s.addMarker (ui.m().signatures.barStart (b - 1), ""));
    ui.studio.edited();
    ui.studio.locate (ui.m().signatures.barStart (9));
    ui.key (',', juce::ModifierKeys::altModifier, ',');
    CHECK (ui.studio.playhead() == ui.m().signatures.barStart (8));
    ui.key ('.', juce::ModifierKeys::altModifier, '.');
    CHECK (ui.studio.playhead() == ui.m().signatures.barStart (16));
    ui.key ('1');
    CHECK (ui.studio.playhead() == ui.m().signatures.barStart (4));
    ui.key ('4');
    CHECK (ui.studio.playhead() == ui.m().signatures.barStart (4));
}

TEST_CASE ("AT-TRK-4 New Track dialog creates numbered tracks below the selection", "[UI]")
{
    Ui ui;
    const auto first = ui.m().trackOrder.front();
    ui.studio.selectTrack (first);
    NewTrackDialog dialog (ui.studio);
    dialog.kind.setSelectedItemIndex (0);
    dialog.count.setValue (3);
    const auto ids = dialog.create();
    ui.settle();
    REQUIRE (ids.size() == 3);
    const auto& order = ui.m().trackOrder;
    REQUIRE (order.size() == 4);
    CHECK (order[0] == first);
    CHECK (ui.m().track (order[1])->name == "Audio 2");
    CHECK (ui.m().track (order[2])->name == "Audio 3");
    CHECK (ui.m().track (order[3])->name == "Audio 4");
    CHECK (ui.main->tracks().headers().size() == 4);
    // One undo removes all three.
    ui.main->run (Cmd::undo);
    ui.settle();
    CHECK (ui.m().trackOrder.size() == 1);
}

TEST_CASE ("AT-TRK-6 renaming in the track header: Enter, Esc, empty", "[UI]")
{
    Ui ui;
    auto& header = *ui.main->tracks().headers().front();
    auto& label = header.name;
    const auto id = header.trackId;

    label.showEditor();
    REQUIRE (label.getCurrentTextEditor() != nullptr);
    label.getCurrentTextEditor()->setText ("Vox");
    label.hideEditor (false); // Enter
    ui.settle();
    CHECK (ui.m().track (id)->name == "Vox");

    label.showEditor();
    label.getCurrentTextEditor()->setText ("Drums");
    label.hideEditor (true); // Esc
    ui.settle();
    CHECK (ui.m().track (id)->name == "Vox");
    CHECK (label.getText() == "Vox");

    label.showEditor();
    label.getCurrentTextEditor()->setText ("   ");
    label.hideEditor (false);
    ui.settle();
    CHECK (ui.m().track (id)->name == "Vox");
    CHECK (label.getText() == "Vox");
}

TEST_CASE ("AT-TRK-5 dragging track 3 above track 1 makes the order 3, 1, 2; Move Track Up/Down", "[UI]")
{
    Ui ui;
    const auto added = ui.instrumentTracks (2);
    const auto order = [&] { return ui.m().trackOrder; };
    const std::vector<Id> start = order();
    REQUIRE (start.size() == 3);
    auto headerOf = [&] (const Id& id) -> TrackHeader& {
        for (const auto& h : ui.main->tracks().headers())
            if (h->trackId == id)
                return *h;
        FAIL ("no header");
        throw;
    };

    // Drag the third header up onto the first: it shows where it lands, then drops there.
    auto& third = headerOf (start[2]);
    const auto& first = headerOf (start[0]);
    const juce::Point<float> from { 60.0f, 10.0f };
    const auto onto = from.translated (0.0f, static_cast<float> (first.getY() - third.getY()));
    third.mouseDown (mouse (third, from));
    third.mouseDrag (mouse (third, onto, {}, 1, from));
    CHECK (third.dropTarget() == &first);
    CHECK (first.dropMark == -1);
    third.mouseUp (mouse (third, onto, {}, 1, from));
    ui.settle();
    CHECK (order() == std::vector<Id> { start[2], start[0], start[1] });
    CHECK (first.dropMark == 0);

    // One undo step puts it back.
    CHECK (ui.studio.perform (Cmd::undo));
    CHECK (order() == start);

    // A short wiggle isn't a drag.
    auto& second = headerOf (start[1]);
    second.mouseDown (mouse (second, from));
    second.mouseDrag (mouse (second, from.translated (0.0f, 3.0f), {}, 1, from));
    second.mouseUp (mouse (second, from.translated (0.0f, 3.0f), {}, 1, from));
    CHECK (order() == start);

    // Move Track Up/Down act on the selected track and stop at the ends.
    ui.studio.selectTrack (start[0]);
    CHECK_FALSE (ui.studio.perform (Cmd::moveTrackUp));
    CHECK (ui.studio.perform (Cmd::moveTrackDown));
    CHECK (order() == std::vector<Id> { start[1], start[0], start[2] });
    CHECK (ui.studio.perform (Cmd::moveTrackUp));
    CHECK (order() == start);
    juce::ignoreUnused (added);
}

TEST_CASE ("AT-TRK-4 [UI] Duplicate Track, Color and Delete from the Track menu and the header menu", "[UI]")
{
    Ui ui;
    const auto audio = ui.m().trackOrder.front();
    ui.studio.selectTrack (audio);

    // Ctrl+D copies the settings below the original and selects the copy; Ctrl+Shift+D brings regions.
    CHECK (ui.key ('D', juce::ModifierKeys::commandModifier));
    REQUIRE (ui.m().trackOrder.size() == 2);
    const auto copy = ui.m().trackOrder[1];
    CHECK (ui.m().track (copy)->name == ui.m().track (audio)->name + " copy");
    CHECK (ui.studio.selectedTrack() == copy);

    juce::StringArray items;
    const auto menu = ui.main->getMenuForIndex (2, "Track");
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        if (! it.getItem().isSeparator)
            items.add (it.getItem().text);
    CHECK (items == juce::StringArray { "New Track", "Duplicate Track", "Duplicate Track with Regions", "Delete Track", "Color", "Move Track Up",
                                        "Move Track Down", "Lock/Unlock Track" });

    // Color › swatch 7 from the menu bar.
    ui.main->menuItemSelected (MainComponent::colourMenuBase + 7, 0);
    CHECK (ui.m().track (copy)->color.palette == 6);

    // The header's right-click menu offers the same, and acts on its own track.
    TrackHeader* header = nullptr;
    for (const auto& h : ui.main->tracks().headers())
        if (h->trackId == audio)
            header = h.get();
    REQUIRE (header != nullptr);
    juce::StringArray headerItems;
    const auto hm = header->menu();
    for (juce::PopupMenu::MenuItemIterator it (hm); it.next();)
        if (! it.getItem().isSeparator)
            headerItems.add (it.getItem().text);
    CHECK (headerItems == juce::StringArray { "Rename", "Duplicate Track", "Duplicate Track with Regions", "Color", "Move Track Up", "Move Track Down",
                                              "Delete Track", "Lock Track", "Add Comment" });
    header->pickMenu (TrackHeader::itemColour + 3);
    CHECK (ui.m().track (audio)->color.palette == 2);
    header->pickMenu (TrackHeader::itemMoveDown);
    ui.settle();
    CHECK (ui.m().trackOrder.back() == audio);
    header = nullptr;
    for (const auto& h : ui.main->tracks().headers())
        if (h->trackId == audio)
            header = h.get();
    REQUIRE (header != nullptr);
    header->pickMenu (TrackHeader::itemDelete); // nothing on it: no question
    ui.settle();
    CHECK (ui.m().track (audio) == nullptr);
}

TEST_CASE ("AT-TRK-11 dragging a header's lower edge resizes; Ctrl+Shift+wheel resizes all", "[UI]")
{
    Ui ui;
    ui.instrumentTracks (1);
    auto& tracksArea = ui.main->tracks();
    auto& header = *tracksArea.headers().front();
    const auto id = header.trackId;
    const auto h = header.getHeight();
    REQUIRE (h == defaultTrackHeight);

    const juce::Point<float> edge { 100.0f, static_cast<float> (h - 2) };
    header.mouseDown (mouse (header, edge));
    header.mouseDrag (mouse (header, edge.translated (0, 50), {}, 1, edge));
    ui.settle();
    CHECK (ui.studio.trackHeight (id) == h + 50);
    CHECK (tracksArea.headers().front()->getHeight() == h + 50);
    header.mouseDrag (mouse (header, edge.translated (0, 1000), {}, 1, edge));
    CHECK (ui.studio.trackHeight (id) == maxTrackHeight);
    header.mouseDrag (mouse (header, edge.translated (0, -1000), {}, 1, edge));
    CHECK (ui.studio.trackHeight (id) == minTrackHeight);

    // A press away from the edge doesn't resize.
    ui.studio.setTrackHeight (id, 100);
    header.mouseDown (mouse (header, { 100.0f, 10.0f }));
    header.mouseDrag (mouse (header, { 100.0f, 60.0f }, {}, 1, juce::Point<float> { 100.0f, 10.0f }));
    CHECK (ui.studio.trackHeight (id) == 100);

    ui.timeline().wheel ({ 300.0f, 20.0f }, juce::ModifierKeys (juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier), 0.5f);
    ui.settle();
    for (const auto& t : ui.m().trackOrder)
        CHECK (ui.studio.trackHeight (t) == 120);
}

TEST_CASE ("AT-REC-13 clicking the take strip switches the active take", "[UI]")
{
    Ui ui;
    const auto track = ui.m().trackOrder.front();
    commands::RecordedAudio rec;
    rec.trackId = track;
    rec.media.hash = "b3:00000000000000000000000000000000000000000000000000000000000000cc";
    rec.media.originalName = "rec.wav";
    rec.media.lengthSamples = 48000 * 8;
    rec.takes = { { 0, 0, 96000 }, { 0, 96000, 96000 } };
    rec.takeFolder = true;
    rec.folderStart = 0;
    rec.folderEnd = bar;
    REQUIRE (ui.studio.session().commitRecording ({ rec }, {}));
    ui.studio.edited();
    ui.settle();
    const auto& folder = *ui.m().takeFolders.begin()->second;
    const auto active = folder.activeTakeId;
    auto& tl = ui.timeline();
    const auto box = tl.regionBounds (ui.region (active));
    const juce::Point<float> strip { box.getRight() - 30.0f, box.getY() + 6.0f };
    REQUIRE (tl.zoneAt (ui.region (active), strip) == Timeline::Zone::takes);
    ui.click (strip);
    const auto& after = *ui.m().takeFolders.begin()->second;
    CHECK (after.activeTakeId != active);
    // Only the active take is hit-tested and drawn.
    CHECK (tl.regionAt (box.getCentre())->id == after.activeTakeId);
}

TEST_CASE ("AT-REC-20 keyboard MIDI mode takes letter keys first", "[UI]")
{
    Ui ui;
    const auto tracks = ui.instrumentTracks (1);
    ui.studio.selectTrack (tracks[0]);
    REQUIRE (ui.studio.session().addMarker (2 * bar, ""));
    ui.studio.edited();

    std::optional<Cmd> asked;
    ui.studio.uiCommand = [&] (Cmd c) { asked = c; };

    // Off: letters and numbers run their commands.
    ui.studio.locate (0);
    ui.key ('1');
    CHECK (ui.studio.playhead() == 2 * bar);
    ui.key ('T', {}, 't');
    CHECK (asked == Cmd::toolMenu);
    asked.reset();
    ui.key ('K', juce::ModifierKeys::commandModifier);
    REQUIRE (ui.studio.keyboardMidiOn());

    // On: letters play notes instead of running commands.
    ui.studio.locate (0);
    CHECK (ui.key ('A', {}, 'a'));
    CHECK (ui.key ('T', {}, 't'));
    CHECK_FALSE (asked.has_value());
    // Ctrl shortcuts still work with the mode on.
    ui.key ('K', juce::ModifierKeys::commandModifier);
    CHECK_FALSE (ui.studio.keyboardMidiOn());
}

TEST_CASE ("AT-ARR-20 a 200-event drag in the tracks area is one undo step", "[UI]")
{
    Ui ui;
    const auto tracks = ui.instrumentTracks (1);
    ui.studio.view.pixelsPerTick = 200.0 / bar;
    const auto id = *ui.studio.session().createMidiRegion (tracks[0], bar, bar);
    ui.studio.edited();
    ui.settle();
    auto& tl = ui.timeline();

    // Move: 200 pointer moves, then release.
    const auto from = ui.centreOf (id);
    tl.pointerDown (from, {});
    for (int i = 1; i <= 200; ++i)
        tl.pointerDrag (from.translated (static_cast<float> (i), 0.0f), {});
    tl.pointerUp (from.translated (200.0f, 0.0f), {});
    ui.settle();
    const auto movedTo = ui.region (id).start;
    CHECK (movedTo > bar);

    // Trim the end: 200 moves inside one gesture.
    const auto box = tl.regionBounds (ui.region (id));
    const juce::Point<float> corner { box.getRight() - 2.0f, box.getBottom() - 4.0f };
    REQUIRE (tl.zoneAt (ui.region (id), corner) == Timeline::Zone::trimEnd);
    tl.pointerDown (corner, {});
    for (int i = 1; i <= 200; ++i)
        tl.pointerDrag (corner.translated (static_cast<float> (i) * 0.5f, 0.0f), {});
    tl.pointerUp (corner.translated (100.0f, 0.0f), {});
    ui.settle();
    CHECK (ui.region (id).length > bar);

    ui.main->run (Cmd::undo);
    ui.settle();
    CHECK (ui.region (id).length == bar);
    CHECK (ui.region (id).start == movedTo);
    ui.main->run (Cmd::undo);
    ui.settle();
    CHECK (ui.region (id).start == bar);
}

TEST_CASE ("File › Import Audio puts files at the playhead, on the selected audio track or new tracks", "[UI][import]")
{
    Ui ui;
    media::AudioData a;
    a.sampleRate = 48000;
    a.channels.assign (1, std::vector<float> (4800, 0.25f));
    const auto file = ui.folder.path / "browse" / "hit.wav";
    REQUIRE (media::writeWav (file, a, media::SampleFormat::int24));
    CHECK (commandInfo (Cmd::importAudio).shortcut == "Ctrl+I");

    const auto audio = ui.m().trackOrder.front();
    ui.studio.selectTrack (audio);
    ui.studio.locate (2 * 3840);
    const auto onTrack = ui.studio.importAtPlayhead ({ file });
    REQUIRE (onTrack.regions.size() == 1);
    CHECK (ui.region (onTrack.regions[0]).trackId == audio);
    CHECK (ui.region (onTrack.regions[0]).start == 2 * 3840);

    // With an instrument track selected the file gets a track of its own.
    const auto inst = ui.instrumentTracks (1).front();
    ui.studio.selectTrack (inst);
    const auto newTrack = ui.studio.importAtPlayhead ({ file });
    REQUIRE (newTrack.regions.size() == 1);
    CHECK (newTrack.tracks.size() == 1);
    CHECK (ui.region (newTrack.regions[0]).trackId == newTrack.tracks[0]);
}

TEST_CASE ("AT-TMP-3 Ctrl+click in the tempo lane adds a change; dragging it changes BPM in 0.01 steps; the first tempo stays", "[UI]")
{
    Ui ui;
    auto& ruler = ui.ruler();
    REQUIRE (ui.studio.layout.globalTracks);
    const auto laneY = static_cast<float> (Ruler::tempoTop + Ruler::tempoLane / 2);
    const auto bar5 = static_cast<float> (ui.studio.view.tickToX (4.0 * 3840.0));

    ruler.pointerDown ({ bar5, laneY }, juce::ModifierKeys::commandModifier);
    ruler.pointerUp();
    ui.settle();
    REQUIRE (ui.m().tempo.points().size() == 2);
    const auto added = ui.m().tempo.points()[1];
    CHECK (added.pos == 4 * 3840);
    CHECK (added.bpm == ruler.bpmAtY (laneY));

    // Dragging up 5 px raises the BPM by 5 px of the lane's range, rounded to 0.01; the position stays.
    const auto [lo, hi] = ruler.tempoRange();
    const auto y = ruler.tempoY (added.bpm);
    ruler.pointerDown ({ bar5, y }, {});
    ruler.pointerDrag ({ bar5 + 1.0f, y - 2.0f });
    ruler.pointerDrag ({ bar5 + 1.0f, y - 5.0f });
    ruler.pointerUp();
    ui.settle();
    const auto dragged = ui.m().tempo.points()[1];
    CHECK (dragged.pos == 4 * 3840);
    CHECK (std::abs (dragged.bpm - std::round ((added.bpm + 5.0 * (hi - lo) / (Ruler::tempoLane - 10)) * 100.0) / 100.0) < 1e-9);
    CHECK (std::abs (dragged.bpm * 100.0 - std::round (dragged.bpm * 100.0)) < 1e-6);
    CHECK (ui.studio.perform (Cmd::undo)); // the drag is one undo step
    CHECK (ui.m().tempo.points()[1].bpm == added.bpm);

    // The first tempo can't be deleted or moved.
    const auto x0 = static_cast<float> (ui.studio.view.tickToX (0.0)) + 1.0f;
    const auto y0 = ruler.tempoY (ui.m().tempo.points()[0].bpm);
    ruler.pointerDown ({ x0, y0 }, juce::ModifierKeys::rightButtonModifier);
    ruler.pointerUp();
    CHECK (ui.m().tempo.points().size() == 2);
    ruler.pointerDown ({ x0, y0 }, {});
    ruler.pointerDrag ({ x0 + 300.0f, y0 });
    ruler.pointerUp();
    CHECK (ui.m().tempo.points()[0].pos == 0);

    // Right-click deletes a later change.
    ruler.pointerDown ({ bar5, ruler.tempoY (added.bpm) }, juce::ModifierKeys::rightButtonModifier);
    ruler.pointerUp();
    CHECK (ui.m().tempo.points().size() == 1);
}

TEST_CASE ("W-11 signature lane: a change at a bar start from text; right-click deletes it", "[UI]")
{
    Ui ui;
    auto& ruler = ui.ruler();
    CHECK (ruler.setSignatureFromText (2, "3/4"));
    ui.settle();
    REQUIRE (ui.m().signatures.points().size() == 2);
    CHECK (ui.m().signatures.points()[1].bar == 2);
    CHECK (ui.m().signatures.points()[1].numerator == 3);
    CHECK (ui.m().signatures.barStart (3) == 2 * 3840 + 2880);
    CHECK_FALSE (ruler.setSignatureFromText (2, "three"));
    CHECK_FALSE (ruler.setSignatureFromText (2, "5/3"));
    CHECK (ruler.setSignatureFromText (0, "6/8")); // the first one changes in place
    CHECK (ui.m().signatures.points()[0].numerator == 6);

    const auto x = static_cast<float> (ui.studio.view.tickToX (static_cast<double> (ui.m().signatures.barStart (2)))) + 4.0f;
    CHECK (ruler.nearestBar (x) == 2);
    ruler.pointerDown ({ x, static_cast<float> (Ruler::signatureTop + 5) }, juce::ModifierKeys::rightButtonModifier);
    ruler.pointerUp();
    CHECK (ui.m().signatures.points().size() == 1);
}

TEST_CASE ("AT-TMP-6 G toggles the global tracks; the marker, tempo and signature lanes follow the timeline's zoom and scroll", "[UI]")
{
    Ui ui;
    auto& ruler = ui.ruler();
    const auto shown = ruler.preferredHeight();
    CHECK (shown == Ruler::rulerHeight + Ruler::markerLane + Ruler::tempoLane + Ruler::signatureLane);
    CHECK (ui.key ('G'));
    CHECK_FALSE (ui.studio.layout.globalTracks);
    CHECK (ruler.preferredHeight() == Ruler::rulerHeight);
    CHECK (ruler.getHeight() == Ruler::rulerHeight);
    CHECK (ui.key ('G'));
    CHECK (ruler.getHeight() == shown);

    REQUIRE (ui.studio.session().addTempoPoint (8 * 3840, 90.0));
    REQUIRE (ui.studio.session().addSignaturePoint (8, 7, 8));
    REQUIRE (ui.studio.session().addMarker (8 * 3840, "Bridge"));
    ui.studio.edited();
    ui.settle();
    for (int step = 0; step < 2; ++step)
    {
        const auto x = ui.studio.view.tickToX (8.0 * 3840.0);
        const auto* tempo = ruler.tempoPointAt (x);
        REQUIRE (tempo != nullptr);
        CHECK (tempo->pos == 8 * 3840);
        REQUIRE (ruler.signatureAt (x + 2.0) != nullptr);
        CHECK (ruler.signatureAt (x + 2.0)->bar == 8);
        REQUIRE (ruler.markerAt (x + 2.0) != nullptr);
        CHECK (ruler.markerAt (x + 2.0)->name == "Bridge");
        CHECK (ui.studio.perform (Cmd::zoomIn)); // and again zoomed in
        ui.studio.view.originTick = 2.0 * 3840.0;
    }
}

TEST_CASE ("W-1d Autopunch defaults to the cycle or 4 bars from the playhead; its ends drag in the ruler", "[UI][punch]")
{
    Ui ui;
    auto& ruler = ui.ruler();
    const auto bar = Ticks (3840);
    const auto& punch = [&]() -> const model::TimeRange& { return ui.m().info.punch; };

    // Cycle off: 4 bars from the bar the playhead is in.
    ui.studio.locate (2 * bar + 500);
    CHECK (ui.studio.perform (Cmd::autopunch));
    CHECK (punch().enabled);
    CHECK (punch().start == 2 * bar);
    CHECK (punch().end == 6 * bar);

    // Dragging the end moves it, as one undo step.
    const auto y = static_cast<float> (Ruler::cycleStrip / 2);
    const auto endX = static_cast<float> (ui.studio.view.tickToX (static_cast<double> (6 * bar)));
    const auto newEnd = static_cast<float> (ui.studio.view.tickToX (static_cast<double> (8 * bar)));
    ruler.pointerDown ({ endX - 1.0f, y }, {});
    ruler.pointerDrag ({ (endX + newEnd) / 2.0f, y });
    ruler.pointerDrag ({ newEnd, y });
    ruler.pointerUp();
    CHECK (punch().start == 2 * bar);
    CHECK (punch().end == 8 * bar);
    CHECK_FALSE (ui.m().info.cycle.enabled); // the cycle wasn't touched
    CHECK (ui.studio.perform (Cmd::undo));
    CHECK (punch().end == 6 * bar);

    // Shift+drag sets a new range.
    const auto x10 = static_cast<float> (ui.studio.view.tickToX (static_cast<double> (10 * bar)));
    const auto x12 = static_cast<float> (ui.studio.view.tickToX (static_cast<double> (12 * bar)));
    ruler.pointerDown ({ x10, y }, juce::ModifierKeys::shiftModifier);
    ruler.pointerDrag ({ x12, y });
    ruler.pointerUp();
    CHECK (punch().start == 10 * bar);
    CHECK (punch().end == 12 * bar);

    // Off, then on with the cycle on: the cycle's range.
    CHECK (ui.studio.perform (Cmd::autopunch));
    CHECK_FALSE (punch().enabled);
    REQUIRE (ui.studio.session().setCycle (true, 4 * bar, 5 * bar));
    CHECK (ui.studio.perform (Cmd::autopunch));
    CHECK (punch().start == 4 * bar);
    CHECK (punch().end == 5 * bar);
}

TEST_CASE ("Region and lane right-click menus: edit commands, Split Here, Color, project end", "[UI][context]")
{
    Ui ui;
    const auto bar = Ticks (3840);
    const auto inst = ui.instrumentTracks (1).front();
    const auto a = ui.studio.session().createMidiRegion (inst, 0, 4 * bar).value();
    const auto b = ui.studio.session().createMidiRegion (inst, 4 * bar, 4 * bar).value();
    ui.studio.edited();
    ui.settle();
    auto& tl = ui.timeline();

    auto labels = [] (const juce::PopupMenu& menu) {
        juce::StringArray out;
        for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
            if (! it.getItem().isSeparator)
                out.add (it.getItem().text);
        return out;
    };
    const auto onA = ui.centreOf (a);
    CHECK (labels (tl.contextMenu (onA))
           == juce::StringArray { "Cut", "Copy", "Paste", "Delete", "Split Here", "Join", "Mute Regions", "Repeat Regions", "Loop Region", "Crossfade",
                                  "Apply Quantize", "Color", "Add Comment on \"" + juce::String (ui.region (a).name) + "\"" });

    // Color applies to the whole selection as one undo step; Track Color clears the override.
    ui.studio.selection.set ({ a, b });
    tl.pickContext (Timeline::contextTrackColour + 5, onA);
    REQUIRE (ui.region (a).color.has_value());
    CHECK (ui.region (a).color->palette == 4);
    CHECK (ui.region (b).color->palette == 4);
    CHECK (ui.studio.perform (Cmd::undo));
    CHECK_FALSE (ui.region (a).color.has_value());
    CHECK_FALSE (ui.region (b).color.has_value());
    CHECK (ui.studio.perform (Cmd::redo));
    tl.pickContext (Timeline::contextTrackColour, onA);
    CHECK_FALSE (ui.region (a).color.has_value());

    // Split Here cuts the selection where the menu was opened.
    ui.studio.selection.set ({ a });
    const auto at = juce::Point<float> (static_cast<float> (ui.studio.view.tickToX (2.0 * bar)), onA.y);
    tl.pickContext (Timeline::contextSplitHere, at);
    CHECK (ui.region (a).length == 2 * bar);

    // A command item runs the command.
    ui.studio.selection.set ({ b });
    tl.pickContext (Timeline::contextCommand + static_cast<int> (Cmd::muteRegions) + 1, ui.centreOf (b));
    CHECK (ui.region (b).muted);

    // On an empty lane: Paste, Set Project End Here, then Clear Project End.
    const auto empty = juce::Point<float> (static_cast<float> (ui.studio.view.tickToX (12.0 * bar)), onA.y);
    CHECK (labels (tl.contextMenu (empty)) == juce::StringArray { "Paste", "Set Project End Here", "Add Comment" });
    tl.pickContext (Timeline::contextSetEnd, empty);
    REQUIRE (ui.m().info.endMarker.has_value());
    CHECK (*ui.m().info.endMarker == 12 * bar);
    CHECK (ui.studio.perform (Cmd::goToEnd));
    CHECK (ui.studio.playhead() == 12 * bar);
    CHECK (labels (tl.contextMenu (empty)).contains ("Clear Project End"));
    tl.pickContext (Timeline::contextClearEnd, empty);
    CHECK_FALSE (ui.m().info.endMarker.has_value());
}
