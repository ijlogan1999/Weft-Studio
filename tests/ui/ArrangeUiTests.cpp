// Arrange window [UI] criteria: ruler, selection, zoom, tools, markers, track headers.

#include "UiFixture.h"
#include "ui/PluginSearch.h"
#include "ui/ProjectDialogs.h"

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
    CHECK (main.toolMenu().getNumItems() == 11); // D-UI-36 added five

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

    // Double-click on an empty part of the lane adds one; the right-click menu deletes.
    const auto emptyX = static_cast<float> (ui.studio.view.tickToX (8.0 * bar)) + 2.0f;
    ruler.mouseDoubleClick (mouse (ruler, { emptyX, laneY }, {}, 2));
    ui.settle();
    CHECK (ui.m().markers.size() == 2);
    ruler.pickContext (Ruler::contextDelete, { x0, laneY });
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

TEST_CASE ("AT-TRK-14 volume and pan in the track header follow and set the channel strip", "[UI][AT-TRK-14]")
{
    Ui ui;
    auto& header = *ui.main->tracks().headers().front();
    const auto id = header.trackId;
    REQUIRE (header.volume.isVisible());
    REQUIRE (header.pan.isVisible());
    CHECK (header.volume.getValue() == 0.0);

    header.volume.setValue (-12.0, juce::sendNotificationSync);
    header.pan.setValue (-50.0, juce::sendNotificationSync);
    ui.settle();
    CHECK (ui.m().track (id)->strip.volumeDb == -12.0);
    CHECK (ui.m().track (id)->strip.pan == -0.5);

    // An edit made elsewhere (the mixer, a collaborator) moves the header's controls.
    REQUIRE (ui.studio.session().setTrackVolume (id, -3.0));
    ui.studio.edited();
    ui.settle();
    CHECK (header.volume.getValue() == -3.0);

    // The bottom of the fader is -inf.
    header.volume.setValue (header.volume.getMinimum(), juce::sendNotificationSync);
    ui.settle();
    CHECK (ui.m().track (id)->strip.volumeDb == minusInfinityDb);
}

TEST_CASE ("AT-EXP-12 Export Regions as Files needs a selection and writes into the chosen folder", "[UI][AT-EXP-12]")
{
    Ui ui;
    ui.studio.selection.clear();
    CHECK_FALSE (ui.studio.perform (Cmd::exportRegions));
    CHECK (ui.studio.status == "Select regions to export.");
    const auto keys = ui.studio.session().createTrack (model::TrackKind::instrument, "Keys").value();
    const auto region = ui.studio.session().createMidiRegion (keys, 0, 3840).value();
    ui.studio.selection.set (std::vector<Id> { region });
    CHECK (ui.studio.perform (Cmd::exportRegions)); // the folder chooser (desktop only)
    const auto files = ui.studio.exportSelectedRegions (ui.folder.path / "exports");
    REQUIRE (files.size() == 1);
    CHECK (std::filesystem::exists (files[0]));
    CHECK (files[0].extension() == ".mid");
    CHECK (ui.studio.status.startsWith ("Exported 1 region"));
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
    CHECK (items == juce::StringArray { "New Track", "New Audio Track", "New Instrument Track", "New Track with Next Input", "Duplicate Track",
                                        "Duplicate Track with Regions", "Delete Track", "Delete Unused Tracks", "Color", "Move Track Up", "Move Track Down",
                                        "Mute Track", "Solo Track", "Record-Enable Track", "Input Monitoring Track", "Mute Off for All", "Clear/Recall Solo", "Solo Tracks of Selected Regions", "Search and Select Track...", "Lock/Unlock Track", "Create Folder Stack", "Create Summing Stack", "Flatten Stack", "Open/Close Track Stack", "Turn All Tracks On/Off", "Configure Track Header..." });

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
                                              "Delete Track", "Turn Track Off", "Hide Track", "Lock Track", "Add Comment", "Configure Track Header" });
    header->pickMenu (TrackHeader::itemTrackOff); // L-TRH-7
    CHECK (ui.m().track (audio)->off);
    header->pickMenu (TrackHeader::itemTrackOff);
    CHECK_FALSE (ui.m().track (audio)->off);
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
    ruler.pickContext (Ruler::contextDelete, { x0, y0 });
    CHECK (ui.m().tempo.points().size() == 2);
    ruler.pointerDown ({ x0, y0 }, {});
    ruler.pointerDrag ({ x0 + 300.0f, y0 });
    ruler.pointerUp();
    CHECK (ui.m().tempo.points()[0].pos == 0);

    // The right-click menu deletes a later change.
    ruler.pickContext (Ruler::contextDelete, { bar5, ruler.tempoY (added.bpm) });
    CHECK (ui.m().tempo.points().size() == 1);
}

TEST_CASE ("AT-TRK-16 Hide Track removes the row; Show Hidden Tracks brings it back to unhide", "[UI][AT-TRK-16]")
{
    Ui ui;
    const auto first = ui.m().trackOrder.front();
    REQUIRE (ui.studio.session().createTrack (model::TrackKind::audio, "Second"));
    ui.studio.edited();
    ui.settle();
    const auto second = ui.m().trackOrder[1];
    const auto headerFor = [&] (const Id& id) -> TrackHeader* {
        for (const auto& h : ui.main->tracks().headers())
            if (h->trackId == id)
                return h.get();
        return nullptr;
    };
    REQUIRE (headerFor (first) != nullptr);
    headerFor (first)->pickMenu (TrackHeader::itemHide);
    ui.settle();
    CHECK (ui.m().track (first)->hidden);
    CHECK (headerFor (first) == nullptr);
    REQUIRE (ui.main->tracks().headers().size() == 1);
    CHECK (ui.timeline().rows() == 1);
    CHECK (ui.main->tracks().headers().front()->getY() == juce::roundToInt (ui.timeline().rowBounds (0).getY()));
    CHECK (ui.main->tracks().headers().front()->trackId == second);
    // Its mixer strip goes with the row.
    ui.main->run (Cmd::toggleMixer);
    ui.settle();
    const auto& strips = ui.main->bottom().mixerView().strips();
    REQUIRE (strips.size() == 1);
    CHECK (strips.front()->trackId() == second);

    REQUIRE (ui.studio.perform (Cmd::showHiddenTracks));
    ui.settle();
    CHECK (strips.size() == 2);
    REQUIRE (headerFor (first) != nullptr);
    CHECK (ui.timeline().rows() == 2);
    juce::StringArray items;
    const auto hm = headerFor (first)->menu();
    for (juce::PopupMenu::MenuItemIterator it (hm); it.next();)
        items.add (it.getItem().text);
    CHECK (items.contains ("Unhide Track"));
    headerFor (first)->pickMenu (TrackHeader::itemHide);
    ui.settle();
    CHECK_FALSE (ui.m().track (first)->hidden);
    REQUIRE (ui.studio.perform (Cmd::showHiddenTracks));
    ui.settle();
    CHECK (ui.timeline().rows() == 2);

    juce::StringArray view;
    const auto menu = ui.main->getMenuForIndex (5, "View");
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        view.add (it.getItem().text);
    CHECK (view.contains ("Show Hidden Tracks"));
}

TEST_CASE ("AT-TMP-8 Alt-drag copies a tempo change", "[UI][AT-TMP-8]")
{
    Ui ui;
    auto& ruler = ui.ruler();
    const auto bar5 = static_cast<float> (ui.studio.view.tickToX (4.0 * 3840.0));
    const auto bar7 = static_cast<float> (ui.studio.view.tickToX (6.0 * 3840.0));
    REQUIRE (ui.studio.session().addTempoPoint (4 * 3840, 90.0));
    ui.settle();
    const auto y = ruler.tempoY (90.0);

    // A click without moving sideways copies nothing.
    ruler.pointerDown ({ bar5, y }, juce::ModifierKeys::altModifier);
    ruler.pointerDrag ({ bar5 + 1.0f, y });
    ruler.pointerUp();
    ui.settle();
    CHECK (ui.m().tempo.points().size() == 2);

    ruler.pointerDown ({ bar5, y }, juce::ModifierKeys::altModifier);
    ruler.pointerDrag ({ bar5 + 20.0f, y });
    ruler.pointerDrag ({ bar7, y });
    ruler.pointerUp();
    ui.settle();
    REQUIRE (ui.m().tempo.points().size() == 3);
    CHECK (ui.m().tempo.points()[1].pos == 4 * 3840);
    CHECK (ui.m().tempo.points()[1].bpm == 90.0);
    CHECK (ui.m().tempo.points()[2].pos == ui.studio.snapped (6 * 3840));
    CHECK (ui.m().tempo.points()[2].bpm == 90.0);
    CHECK (ui.studio.session().undoName() == "Copy Tempo Change");
    REQUIRE (ui.studio.perform (Cmd::undo));
    CHECK (ui.m().tempo.points().size() == 2);
}

TEST_CASE ("AT-TMP-9 Tap Tempo sets the tempo at the playhead from the taps", "[UI][AT-TMP-9]")
{
    Ui ui;
    const auto before = ui.studio.session().undoName();
    CHECK (ui.studio.tapTempo (1000.0)); // one tap: nothing yet
    CHECK (ui.m().tempo.bpmAt (0) == 120.0);
    CHECK (ui.studio.session().undoName() == before);
    REQUIRE (ui.studio.session().setTempoAt (0, 100.0));
    for (int i = 1; i < 5; ++i)
        CHECK (ui.studio.tapTempo (1000.0 + 500.0 * i));
    CHECK (ui.m().tempo.bpmAt (0) == 120.0);
    CHECK (ui.studio.session().undoName() == "Tap Tempo");

    // A pause over 2 s starts a new run: 400 ms gaps are 150 BPM.
    for (int i = 0; i < 4; ++i)
        CHECK (ui.studio.tapTempo (10000.0 + 400.0 * i));
    CHECK (ui.m().tempo.bpmAt (0) == 150.0);
    REQUIRE (ui.studio.perform (Cmd::undo));
    CHECK (ui.m().tempo.bpmAt (0) == 120.0);
    REQUIRE (ui.studio.perform (Cmd::undo));
    CHECK (ui.m().tempo.bpmAt (0) == 100.0);
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
    ruler.pickContext (Ruler::contextDelete, { x, static_cast<float> (Ruler::signatureTop + 5) });
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
                                  "Apply Quantize", "Export Regions as Files...", "Color", "Add Comment on \"" + juce::String (ui.region (a).name) + "\"" });

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

    // On an empty lane: what the track holds (AT-ARR-34), Paste, Set Project End Here, then Clear Project End.
    const auto empty = juce::Point<float> (static_cast<float> (ui.studio.view.tickToX (12.0 * bar)), onA.y);
    CHECK (labels (tl.contextMenu (empty)) == juce::StringArray { "Create MIDI Region", "Paste", "Set Project End Here", "Add Comment" });
    tl.pickContext (Timeline::contextSetEnd, empty);
    REQUIRE (ui.m().info.endMarker.has_value());
    CHECK (*ui.m().info.endMarker == 12 * bar);
    CHECK (ui.studio.perform (Cmd::goToEnd));
    CHECK (ui.studio.playhead() == 12 * bar);
    CHECK (labels (tl.contextMenu (empty)).contains ("Clear Project End"));
    tl.pickContext (Timeline::contextClearEnd, empty);
    CHECK_FALSE (ui.m().info.endMarker.has_value());
}

TEST_CASE ("Navigate menu: Click During, Return to Start on Stop and MIDI Recording options; K says when the click plays", "[UI][transport]")
{
    Ui ui;
    auto settings = [&] { return ui.studio.engine().settings(); };
    REQUIRE (settings().metronomeDuring == engine::ClickDuring::both); // D-W1-10
    if (settings().metronomeOn)
        CHECK (ui.studio.perform (Cmd::metronome));
    CHECK (ui.studio.perform (Cmd::metronome));
    CHECK (settings().metronomeOn);
    CHECK (ui.studio.status == "Click on.");

    ui.main->menuItemSelected (MainComponent::clickDuringRecord, 4);
    CHECK (settings().metronomeDuring == engine::ClickDuring::record);
    CHECK (ui.studio.perform (Cmd::metronome));
    CHECK (ui.studio.perform (Cmd::metronome));
    CHECK (ui.studio.status.contains ("while recording"));
    const auto returned = settings().returnToStartOnStop;
    ui.main->menuItemSelected (MainComponent::returnToStart, 4);
    CHECK (settings().returnToStartOnStop != returned);
    const auto merge = settings().midiMergeLinear;
    ui.main->menuItemSelected (MainComponent::mergeLinear, 4);
    CHECK (settings().midiMergeLinear != merge);

    juce::StringArray items;
    const auto menu = ui.main->getMenuForIndex (4, "Navigate");
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        if (! it.getItem().isSeparator)
            items.add (it.getItem().text);
    CHECK (items.contains ("Click During"));
    CHECK (items.contains ("Return to Start on Stop"));
    CHECK (items.contains ("MIDI Recording"));
    CHECK (items.contains ("Tap Tempo"));

    // Opening another project keeps these options but takes the project's metronome defaults.
    const auto keep = settings();
    std::string error;
    commands::NewProjectOptions second;
    second.name = "Second";
    REQUIRE (ui.studio.newProject (ui.folder.path / "projects", second, &error));
    CHECK (settings().returnToStartOnStop == keep.returnToStartOnStop);
    CHECK (settings().midiMergeLinear == keep.midiMergeLinear);
    CHECK (settings().metronomeDuring == engine::ClickDuring::both);
}

TEST_CASE ("F-TRN-3 Ctrl+Space pauses where the playhead is; Space returns to the play start", "[UI][transport]")
{
    Ui ui;
    REQUIRE (ui.studio.engine().settings().returnToStartOnStop);
    ui.studio.locate (3840);
    ui.renderAudio();
    CHECK (ui.key (juce::KeyPress::spaceKey, juce::ModifierKeys::commandModifier)); // plays
    ui.renderAudio (48000);
    CHECK (ui.studio.playing());
    CHECK (ui.key (juce::KeyPress::spaceKey, juce::ModifierKeys::commandModifier)); // pauses
    ui.renderAudio();
    CHECK_FALSE (ui.studio.playing());
    const auto paused = ui.studio.playhead();
    CHECK (paused > 3840 + 1500);

    CHECK (ui.key (juce::KeyPress::spaceKey)); // plays on from there...
    ui.renderAudio (48000);
    CHECK (ui.key (juce::KeyPress::spaceKey)); // ...and Stop goes back to where that started
    ui.renderAudio();
    CHECK (ui.studio.playhead() == paused);
}

TEST_CASE ("Loop Region loops up to the next region, else to the cycle end, else 3 repeats (D-ARR-8)", "[UI]")
{
    Ui ui;
    const auto tracks = ui.instrumentTracks (2);
    auto& s = ui.studio.session();
    const auto a = *s.createMidiRegion (tracks[0], 0, bar);
    const auto b = *s.createMidiRegion (tracks[0], 6 * bar, bar);
    const auto c = *s.createMidiRegion (tracks[0], 7 * bar, bar);
    const auto d = *s.createMidiRegion (tracks[1], 0, 2 * bar);
    ui.studio.edited();
    ui.settle();
    auto loopEnd = [&] (const Id& id) { const auto& l = ui.m().region (id)->loop; return l ? l->endTicks : Ticks (-1); };

    ui.studio.selection.set ({ a });
    ui.main->run (Cmd::loopRegion);
    CHECK (loopEnd (a) == 6 * bar);

    ui.studio.selection.set ({ b }); // c starts right after it
    ui.main->run (Cmd::loopRegion);
    CHECK (loopEnd (b) == -1);
    CHECK (ui.studio.status.contains ("no room"));

    ui.studio.selection.set ({ c }); // nothing after it, no cycle: 3 repeats
    ui.main->run (Cmd::loopRegion);
    CHECK (loopEnd (c) == 11 * bar);

    REQUIRE (s.setCycle (true, 0, 9 * bar));
    ui.studio.selection.set ({ d });
    ui.main->run (Cmd::loopRegion);
    CHECK (loopEnd (d) == 9 * bar);
    ui.main->run (Cmd::loopRegion); // toggles back off
    CHECK (loopEnd (d) == -1);
}

TEST_CASE ("AT-TRN-3 Space plays and stops in the tracks area, and types a space in a text field", "[UI][transport][AT-TRN-3]")
{
    Ui ui;
    CHECK (ui.key (juce::KeyPress::spaceKey));
    ui.renderAudio();
    CHECK (ui.studio.playing());
    CHECK (ui.key (juce::KeyPress::spaceKey));
    ui.renderAudio();
    CHECK_FALSE (ui.studio.playing());

    // Renaming a track: the key goes to the text field, which types it, and the transport stays put.
    auto& header = *ui.main->tracks().headers().front();
    header.name.showEditor();
    auto* editor = header.name.getCurrentTextEditor();
    REQUIRE (editor != nullptr);
    editor->setText ("Lead", false);
    editor->moveCaretToEnd();
    CHECK (editor->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey, {}, ' ')));
    ui.renderAudio();
    CHECK (editor->getText() == "Lead ");
    CHECK_FALSE (ui.studio.playing());
}

TEST_CASE ("AT-TRN-18 With follow on, playing past the right edge scrolls the view; with follow off it doesn't", "[UI][transport][AT-TRN-18]")
{
    Ui ui;
    const auto width = static_cast<double> (ui.timeline().getWidth());
    REQUIRE (width > 300);
    auto showFourBars = [&] {
        ui.studio.view.originTick = 0;
        ui.studio.view.pixelsPerTick = width / (4.0 * bar);
    };
    auto visible = [&] {
        const auto x = ui.studio.view.tickToX (static_cast<double> (ui.studio.playhead()));
        return x >= 0.0 && x <= width;
    };

    showFourBars();
    REQUIRE (ui.studio.follow);
    ui.main->run (Cmd::playStop);
    for (int i = 0; i < 100; ++i) // 12.5 s at 120 BPM: past bar 6
    {
        ui.renderAudio (6000);
        ui.main->tracks().followPlayhead();
        CHECK (visible());
    }
    CHECK (ui.studio.view.originTick > 0.0);
    ui.main->run (Cmd::playStop);
    ui.renderAudio();

    ui.studio.follow = false;
    ui.studio.locate (0);
    showFourBars();
    ui.main->run (Cmd::playStop);
    for (int i = 0; i < 100; ++i)
    {
        ui.renderAudio (6000);
        ui.main->tracks().followPlayhead();
    }
    CHECK (ui.studio.view.originTick == 0.0);
    CHECK_FALSE (visible());
    ui.main->run (Cmd::playStop);
    ui.renderAudio();
}

TEST_CASE ("AT-TRN-20 Stop while stopped goes to the start; with Cycle on, Play starts at the cycle", "[UI][AT-TRN-20]")
{
    Ui ui;
    auto& studio = ui.studio;
    const auto run = [&] {
        ui.renderAudio (512);
        studio.tick();
        ui.settle();
    };
    studio.locate (3 * bar);
    REQUIRE (studio.perform (Cmd::stop));
    CHECK (studio.playhead() == 0);

    REQUIRE (studio.session().setCycle (true, 2 * bar, 4 * bar));
    studio.edited();
    ui.settle();
    studio.locate (6 * bar);
    REQUIRE (studio.perform (Cmd::stop));
    CHECK (studio.playhead() == 2 * bar); // the cycle start first
    REQUIRE (studio.perform (Cmd::stop));
    CHECK (studio.playhead() == 0);       // then the project start

    // Play with the cycle on starts at the cycle start, wherever the playhead was.
    studio.locate (6 * bar);
    REQUIRE (ui.key (juce::KeyPress::spaceKey));
    run();
    REQUIRE (studio.playing());
    CHECK (studio.playhead() >= 2 * bar);
    CHECK (studio.playhead() < 3 * bar);
    // Stop (the button, or numpad 0) stops; pressed again it's the "go to start" above.
    REQUIRE (ui.key (juce::KeyPress::numberPad0));
    run();
    CHECK_FALSE (studio.playing());
    CHECK (studio.playhead() == 2 * bar);

    // With the cycle off, Play starts at the playhead as before.
    REQUIRE (studio.session().setCycle (false, 2 * bar, 4 * bar));
    studio.edited();
    studio.locate (6 * bar);
    REQUIRE (studio.perform (Cmd::playStop));
    run();
    CHECK (studio.playhead() >= 6 * bar);
    REQUIRE (studio.perform (Cmd::playStop));
    run();

    juce::StringArray items;
    const auto menu = ui.main->getMenuForIndex (4, "Navigate");
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        items.add (it.getItem().text);
    CHECK (items.contains ("Stop"));
}

TEST_CASE ("AT-TRN-21 Navigate › Metronome Clicks sets the bar, beat and division clicks, count-in only and tone", "[UI][AT-TRN-21]")
{
    Ui ui;
    const auto menu = ui.main->getMenuForIndex (4, "Navigate");
    const juce::PopupMenu* clicks = nullptr;
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        if (it.getItem().text == "Metronome Clicks")
            clicks = it.getItem().subMenu.get();
    REQUIRE (clicks != nullptr);
    juce::StringArray names;
    const juce::PopupMenu* tone = nullptr;
    for (juce::PopupMenu::MenuItemIterator it (*clicks); it.next();)
    {
        names.add (it.getItem().text);
        if (it.getItem().text == "Tone")
            tone = it.getItem().subMenu.get();
    }
    for (auto name : { "Bar", "Beat", "Division", "Only During Count-in", "Tone" })
        CHECK (names.contains (name));
    REQUIRE (tone != nullptr);
    int toneItems = 0, ticked = -1;
    for (juce::PopupMenu::MenuItemIterator it (*tone); it.next();)
    {
        if (it.getItem().isTicked)
            ticked = it.getItem().itemID - MainComponent::clickTone0;
        ++toneItems;
    }
    CHECK (toneItems == engine::Metronome::numTones);
    CHECK (ticked == engine::Metronome::middleTone);

    const auto before = ui.studio.engine().settings();
    CHECK (before.clickBars);
    CHECK (before.clickBeats);
    CHECK_FALSE (before.clickDivisions);
    CHECK_FALSE (before.clickOnlyCountIn);
    ui.main->menuItemSelected (MainComponent::clickBar, 4);
    ui.main->menuItemSelected (MainComponent::clickDivision, 4);
    ui.main->menuItemSelected (MainComponent::clickOnlyCountIn, 4);
    ui.main->menuItemSelected (MainComponent::clickTone0, 4);
    const auto after = ui.studio.engine().settings();
    CHECK_FALSE (after.clickBars);
    CHECK (after.clickBeats);
    CHECK (after.clickDivisions);
    CHECK (after.clickOnlyCountIn);
    CHECK (after.clickTone == 0);
    ui.main->menuItemSelected (MainComponent::clickBeat, 4);
    CHECK_FALSE (ui.studio.engine().settings().clickBeats);
}

TEST_CASE ("AT-TRN-22 go-to, play-from and stop-and-go commands, cycle moves, Play and Stop menus", "[UI][AT-TRN-22]")
{
    Ui ui;
    auto& studio = ui.studio;
    auto& s = studio.session();
    const auto run = [&] {
        ui.renderAudio (512);
        studio.tick();
        ui.settle();
    };
    const auto keys = ui.instrumentTracks (1).front();
    const auto region = *s.createMidiRegion (keys, 4 * bar, 2 * bar);
    REQUIRE (s.setCycle (false, 2 * bar, 3 * bar));
    studio.edited();
    ui.settle();

    // Go to.
    REQUIRE (ui.key (juce::KeyPress::homeKey, juce::ModifierKeys::ctrlModifier));
    CHECK (studio.playhead() == 2 * bar);
    REQUIRE (ui.key (juce::KeyPress::endKey, juce::ModifierKeys::ctrlModifier));
    CHECK (studio.playhead() == 3 * bar);
    CHECK_FALSE (studio.perform (Cmd::goToSelectionStart)); // nothing selected
    studio.selection.set ({ region });
    REQUIRE (ui.key (juce::KeyPress::homeKey, juce::ModifierKeys::shiftModifier));
    CHECK (studio.playhead() == 4 * bar);
    REQUIRE (ui.key (juce::KeyPress::endKey, juce::ModifierKeys::shiftModifier));
    CHECK (studio.playhead() == 6 * bar);
    studio.locate (7 * bar);
    REQUIRE (studio.perform (Cmd::goToStart));
    CHECK (studio.playhead() == 0);
    // The last locate position is where the playhead was last put before that: going there twice stays.
    studio.locate (5 * bar);
    REQUIRE (studio.perform (Cmd::goToLastLocate));
    CHECK (studio.playhead() == 5 * bar);

    // Play from.
    REQUIRE (ui.key (juce::KeyPress::spaceKey, juce::ModifierKeys::shiftModifier)); // from the selection
    run();
    REQUIRE (studio.playing());
    CHECK (studio.playhead() >= 4 * bar);
    CHECK (studio.playhead() < 4 * bar + bar / 2);
    CHECK (studio.lastLocatePosition() == 4 * bar);
    REQUIRE (studio.perform (Cmd::stopGoToCycleStart));
    run();
    CHECK_FALSE (studio.playing());
    CHECK (studio.playhead() == 2 * bar);
    REQUIRE (studio.perform (Cmd::playFromPreviousBar));
    run();
    CHECK (studio.playhead() >= bar);
    CHECK (studio.playhead() < 2 * bar);
    REQUIRE (studio.perform (Cmd::stopGoToStart));
    run();
    CHECK_FALSE (studio.playing());
    CHECK (studio.playhead() == 0);
    studio.view.originTick = 8.0 * bar;
    REQUIRE (studio.perform (Cmd::playFromWindowEdge));
    run();
    CHECK (studio.playhead() >= 8 * bar);
    REQUIRE (studio.perform (Cmd::stopGoToLastLocate));
    run();
    CHECK (studio.playhead() == 8 * bar);
    REQUIRE (studio.perform (Cmd::playFromStart));
    run();
    CHECK (studio.playhead() < bar);
    REQUIRE (studio.perform (Cmd::stop));
    run();

    // Cycle moves: one undo step each.
    REQUIRE (ui.key (juce::KeyPress::rightKey, juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier));
    CHECK (ui.m().info.cycle.start == 3 * bar);
    CHECK (ui.m().info.cycle.end == 4 * bar);
    REQUIRE (studio.perform (Cmd::doubleCycle));
    CHECK (ui.m().info.cycle.end == 5 * bar);
    REQUIRE (studio.perform (Cmd::halveCycle));
    CHECK (ui.m().info.cycle.end == 4 * bar);
    REQUIRE (ui.key (juce::KeyPress::leftKey, juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier));
    CHECK (ui.m().info.cycle.start == 2 * bar);
    REQUIRE (studio.perform (Cmd::undo));
    CHECK (ui.m().info.cycle.start == 3 * bar);

    // Navigate menu groups; Play and Stop open a menu on right-click.
    const auto menu = ui.main->getMenuForIndex (4, "Navigate");
    juce::StringArray subs;
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        if (it.getItem().subMenu != nullptr)
            subs.add (it.getItem().text);
    for (auto name : { "Go To", "Play From", "Stop And", "Cycle Range" })
        CHECK (subs.contains (name));
    auto& controls = ui.main->controlBar();
    CommandButton* play = nullptr;
    for (auto* b : controls.commandButtons())
        if (b->getTitle() == "Play")
            play = b;
    REQUIRE (play != nullptr);
    REQUIRE (play->popupMenu);
    juce::StringArray items;
    const auto playMenu = play->popupMenu();
    for (juce::PopupMenu::MenuItemIterator it (playMenu); it.next();)
        items.add (it.getItem().text);
    CHECK (items.contains ("Play from Cycle Start"));
    play->popupPicked (static_cast<int> (Cmd::playFromCycleStart) + 1);
    run();
    CHECK (studio.playing());
    CHECK (studio.playhead() >= 2 * bar);
    REQUIRE (studio.perform (Cmd::stop));
    run();
}

TEST_CASE ("AT-ARR-31 Deselect All and Invert Selection", "[UI][AT-ARR-31]")
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
    const auto ctrlShift = juce::ModifierKeys (juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier);

    ui.click (ui.centreOf (a));
    REQUIRE (ui.key ('I', ctrlShift));
    CHECK_FALSE (sel.contains (a));
    CHECK (sel.contains (b));
    CHECK (sel.contains (c));
    // Regions on a hidden track stay out of it.
    REQUIRE (s.setTrackHidden (tracks[1], true));
    ui.studio.edited();
    ui.settle();
    sel.clear();
    REQUIRE (ui.key ('I', ctrlShift));
    CHECK (sel.ids().size() == 2);
    CHECK_FALSE (sel.contains (c));
    REQUIRE (ui.key ('A', ctrlShift));
    CHECK (sel.empty());

    juce::StringArray items;
    const auto menu = ui.main->getMenuForIndex (1, "Edit");
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        items.add (it.getItem().text);
    CHECK (items.contains ("Deselect All"));
    CHECK (items.contains ("Invert Selection"));
}

TEST_CASE ("AT-ARR-32 the Mute and Eraser tools act on every selected region", "[UI][AT-ARR-32]")
{
    Ui ui;
    const auto tracks = ui.instrumentTracks (1);
    auto& s = ui.studio.session();
    const auto a = *s.createMidiRegion (tracks[0], 0, bar);
    const auto b = *s.createMidiRegion (tracks[0], 2 * bar, bar);
    const auto c = *s.createMidiRegion (tracks[0], 4 * bar, bar);
    ui.studio.selection.set ({ a, b });
    ui.studio.edited();
    ui.settle();

    ui.studio.tool = Tool::mute;
    ui.click (ui.centreOf (a));
    CHECK (ui.region (a).muted);
    CHECK (ui.region (b).muted);
    CHECK_FALSE (ui.region (c).muted);
    s.undo(); // one step
    ui.studio.edited();
    ui.settle();
    CHECK_FALSE (ui.region (a).muted);
    CHECK_FALSE (ui.region (b).muted);
    // On a region outside the selection the tool takes just that one.
    ui.click (ui.centreOf (c));
    CHECK (ui.region (c).muted);
    CHECK_FALSE (ui.region (a).muted);

    ui.studio.tool = Tool::eraser;
    ui.click (ui.centreOf (b));
    CHECK (ui.m().region (a) == nullptr);
    CHECK (ui.m().region (b) == nullptr);
    CHECK (ui.m().region (c) != nullptr);
}

TEST_CASE ("AT-ARR-33 Shift keeps a region drag to one direction", "[UI][AT-ARR-33]")
{
    Ui ui;
    const auto tracks = ui.instrumentTracks (2);
    auto& s = ui.studio.session();
    const auto a = *s.createMidiRegion (tracks[0], 0, bar);
    ui.studio.snapEnabled = false;
    ui.studio.edited();
    ui.settle();
    auto& tl = ui.timeline();
    const auto shift = juce::ModifierKeys (juce::ModifierKeys::shiftModifier);
    const auto oneBar = std::max (static_cast<float> (bar * ui.studio.view.pixelsPerTick), 150.0f); // further across than down
    const auto oneRow = tl.rowBounds (2).getCentreY() - tl.rowBounds (1).getCentreY();

    // Mostly sideways: the region stays on its track.
    auto from = ui.centreOf (a);
    tl.pointerDown (from, {});
    tl.pointerDrag (from + juce::Point<float> (oneBar, oneRow), shift);
    tl.pointerUp (from + juce::Point<float> (oneBar, oneRow), shift);
    ui.settle();
    CHECK (ui.region (a).start > 0);
    CHECK (ui.region (a).trackId == tracks[0]);
    const auto start = ui.region (a).start;
    // Mostly downwards: it changes track and keeps its position.
    from = ui.centreOf (a);
    tl.pointerDown (from, {});
    tl.pointerDrag (from + juce::Point<float> (oneRow * 0.5f, oneRow), shift);
    tl.pointerUp (from + juce::Point<float> (oneRow * 0.5f, oneRow), shift);
    ui.settle();
    CHECK (ui.region (a).start == start);
    CHECK (ui.region (a).trackId == tracks[1]);
}

TEST_CASE ("AT-ARR-34 an empty lane's menu creates a MIDI region or imports audio there", "[UI][AT-ARR-34]")
{
    Ui ui;
    const auto tracks = ui.instrumentTracks (1);
    ui.settle();
    auto& tl = ui.timeline();
    const auto labels = [] (const juce::PopupMenu& menu) {
        juce::StringArray out;
        for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
            out.add (it.getItem().text);
        return out;
    };
    const auto x = static_cast<float> (ui.studio.view.tickToX (2.0 * bar)) + 3.0f;
    const juce::Point<float> onAudio (x, tl.rowBounds (0).getCentreY()), onInstrument (x, tl.rowBounds (1).getCentreY());
    CHECK (labels (tl.contextMenu (onAudio)).contains ("Import Audio..."));
    CHECK_FALSE (labels (tl.contextMenu (onAudio)).contains ("Create MIDI Region"));
    REQUIRE (labels (tl.contextMenu (onInstrument)).contains ("Create MIDI Region"));
    CHECK_FALSE (labels (tl.contextMenu (onInstrument)).contains ("Import Audio..."));

    tl.pickContext (Timeline::contextCreateMidi, onInstrument);
    ui.settle();
    const auto made = ui.m().regionsOnTrack (tracks[0]);
    REQUIRE (made.size() == 1);
    CHECK (made.front()->isMidi);
    CHECK (made.front()->start == 2 * bar);
    CHECK (made.front()->length == bar);
    CHECK (ui.studio.selection.contains (made.front()->id));
}

TEST_CASE ("AT-ARR-35 markers show as flags on the ruler while the global tracks are hidden", "[UI][AT-ARR-35]")
{
    Ui ui;
    auto& s = ui.studio.session();
    const auto verse = s.addMarker (2 * bar, "Verse");
    const auto chorus = s.addMarker (6 * bar, "Chorus");
    REQUIRE (verse);
    REQUIRE (chorus);
    ui.studio.layout.globalTracks = true;
    ui.studio.edited();
    ui.settle();
    CHECK (ui.ruler().markerFlags().empty()); // the marker lane shows them

    ui.studio.layout.globalTracks = false;
    ui.studio.edited();
    ui.settle();
    const auto flags = ui.ruler().markerFlags();
    REQUIRE (flags.size() == 2);
    CHECK (flags[0].first->name == "Verse");
    CHECK (flags[0].second.getX() == Catch::Approx (ui.studio.view.tickToX (2.0 * bar)).margin (1.0));
    CHECK (flags[1].second.getX() == Catch::Approx (ui.studio.view.tickToX (6.0 * bar)).margin (1.0));
    CHECK (flags[0].second.getBottom() <= static_cast<float> (Ruler::rulerHeight));
    // A click on a flag goes to its marker.
    ui.ruler().pointerDown (flags[1].second.getCentre(), {});
    ui.ruler().pointerUp();
    ui.settle();
    CHECK (ui.studio.playhead() == 6 * bar);
}

TEST_CASE ("AT-TRK-17 keys for the selected track's Mute, Solo and Record-enable; one-step new tracks", "[UI][AT-TRK-17]")
{
    Ui ui;
    const auto audio = ui.m().trackOrder.front();
    ui.studio.selectTrack (audio);
    ui.settle();
    const auto shift = juce::ModifierKeys (juce::ModifierKeys::shiftModifier);
    const auto ctrlAlt = juce::ModifierKeys (juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier);

    REQUIRE (ui.key ('M', shift));
    CHECK (ui.m().track (audio)->mute);
    CHECK (ui.studio.session().undoName() == "Mute Track");
    REQUIRE (ui.key ('M', shift));
    CHECK_FALSE (ui.m().track (audio)->mute);
    REQUIRE (ui.key ('S'));
    CHECK (ui.studio.localView().soloed.contains (audio));
    REQUIRE (ui.key ('S'));
    CHECK_FALSE (ui.studio.localView().soloed.contains (audio));
    REQUIRE (ui.key ('R', shift));
    CHECK (ui.studio.localView().armed.contains (audio));
    REQUIRE (ui.key ('R', shift));
    CHECK_FALSE (ui.studio.localView().armed.contains (audio));

    // New Instrument Track / New Audio Track: one track below the selected one, selected, one undo step, no dialog.
    REQUIRE (ui.key ('I', ctrlAlt));
    REQUIRE (ui.m().trackOrder.size() >= 2);
    const auto keys = ui.m().trackOrder[1];
    CHECK (ui.m().track (keys)->kind == model::TrackKind::instrument);
    CHECK (ui.studio.selectedTrack() == keys);
    ui.studio.selectTrack (audio);
    REQUIRE (ui.key ('A', ctrlAlt));
    const auto second = ui.m().trackOrder[1];
    CHECK (ui.m().track (second)->kind == model::TrackKind::audio);
    CHECK (ui.m().trackOrder[2] == keys);
    ui.studio.session().undo();
    ui.studio.edited();
    ui.settle();
    CHECK (ui.m().track (second) == nullptr);

    juce::StringArray items;
    const auto menu = ui.main->getMenuForIndex (2, "Track");
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        items.add (it.getItem().text);
    for (const auto* name : { "New Audio Track", "New Instrument Track", "Mute Track", "Solo Track", "Record-Enable Track" })
        CHECK (items.contains (name));
}

TEST_CASE ("AT-TRK-21 New Track with Next Input and Search and Select Track", "[UI][AT-TRK-21]")
{
    Ui ui;
    auto& s = ui.studio.session();
    const auto bus = s.createTrack (model::TrackKind::bus, "Drums").value();
    const auto first = s.createTrack (model::TrackKind::audio, "Mono", 1).value();
    model::InputAssignment in;
    in.kind = model::InputAssignment::Kind::audio;
    in.firstChannel = 0;
    REQUIRE (s.setTrackInput (first, in));
    REQUIRE (s.setTrackOutput (first, bus));
    REQUIRE (s.setTrackMonitor (first, "on"));
    ui.studio.edited();
    ui.studio.selectTrack (first);
    ui.settle();
    const auto inputs = ui.studio.device().config().numInputChannels;
    REQUIRE (inputs >= 2);

    // Ctrl+Shift+N: a mono audio track on input 2, out to Drums, monitoring on, below and selected, one undo step.
    const auto before = ui.m().trackOrder.size();
    REQUIRE (ui.key ('N', juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier));
    REQUIRE (ui.m().trackOrder.size() == before + 1);
    const auto made = ui.studio.selectedTrack();
    const auto* t = ui.m().track (made);
    REQUIRE (t != nullptr);
    CHECK (t->kind == model::TrackKind::audio);
    CHECK (t->channels == 1);
    CHECK (t->input.kind == model::InputAssignment::Kind::audio);
    CHECK (t->input.firstChannel == 1);
    CHECK (t->output.busId == bus);
    CHECK (t->monitor == "on");
    const auto& order = ui.m().trackOrder;
    CHECK (std::find (order.begin(), order.end(), made) == std::find (order.begin(), order.end(), first) + 1);
    CHECK (s.undoName() == "New Track");

    // Past the device's last input the new track gets none.
    in.firstChannel = inputs - 1;
    REQUIRE (s.setTrackInput (made, in));
    ui.studio.edited();
    REQUIRE (ui.studio.perform (Cmd::newTrackNextInput));
    CHECK (ui.m().track (ui.studio.selectedTrack())->input.kind == model::InputAssignment::Kind::none);

    // Alt+click on + does the same; a bus selected says so.
    const auto count = ui.m().trackOrder.size();
    juce::ModifierKeys::currentModifiers = juce::ModifierKeys::altModifier;
    ui.main->tracks().addTrackButton().onClick();
    juce::ModifierKeys::currentModifiers = {};
    ui.settle();
    CHECK (ui.m().trackOrder.size() == count + 1);
    ui.studio.selectTrack (bus);
    CHECK_FALSE (ui.studio.perform (Cmd::newTrackNextInput));
    CHECK (ui.m().trackOrder.size() == count + 1);

    // Instrument tracks keep the instrument.
    const auto keys = ui.instrumentTracks (1).front();
    const auto instruments = ui.studio.pluginChoices (true);
    REQUIRE_FALSE (instruments.empty());
    REQUIRE (ui.studio.setInstrument (keys, instruments.back()));
    ui.settle();
    ui.studio.selectTrack (keys);
    REQUIRE (ui.studio.perform (Cmd::newTrackNextInput));
    const auto* inst = ui.m().track (ui.studio.selectedTrack());
    REQUIRE (inst->kind == model::TrackKind::instrument);
    REQUIRE (inst->strip.instrument);
    CHECK (inst->strip.instrument->pluginUid == ui.m().track (keys)->strip.instrument->pluginUid);

    // Search and Select Track: every word, any case; the master is never listed.
    REQUIRE (s.renameTrack (first, "Lead Vocal"));
    ui.studio.edited();
    CHECK (TrackSearch::matches (ui.studio, "vocal LEAD") == std::vector<Id> { first });
    CHECK (TrackSearch::matches (ui.studio, "drums") == std::vector<Id> { bus });
    for (const auto& id : TrackSearch::matches (ui.studio, {}))
        CHECK (ui.m().track (id)->kind != model::TrackKind::master);
    TrackSearch search (ui.studio);
    Id chosen;
    search.onChosen = [&] (const Id& id) { chosen = id; };
    search.setSearch ("drum");
    REQUIRE (search.results().size() == 1);
    REQUIRE (search.selectHighlighted());
    CHECK (ui.studio.selectedTrack() == bus);
    CHECK (chosen == bus);
}

TEST_CASE ("AT-ARR-36 right-click menus in the marker, tempo and signature lanes", "[UI][AT-ARR-36]")
{
    Ui ui;
    auto& s = ui.studio.session();
    const auto marker = s.addMarker (2 * bar, "Verse").value();
    ui.studio.layout.globalTracks = true;
    ui.studio.snapEnabled = true;
    ui.studio.edited();
    ui.settle();
    auto& ruler = ui.ruler();
    struct Entry
    {
        juce::String text;
        bool enabled;
    };
    const auto entries = [] (const juce::PopupMenu& menu) {
        std::vector<Entry> out;
        for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
            if (! it.getItem().isSeparator)
                out.push_back ({ it.getItem().text, it.getItem().isEnabled });
        return out;
    };
    const auto xAt = [&] (double tick) { return static_cast<float> (ui.studio.view.tickToX (tick)) + 2.0f; };
    const float markerY = Ruler::rulerHeight + 5.0f, signatureY = Ruler::signatureTop + 5.0f;

    // Outside the lanes there is no menu.
    CHECK (entries (ruler.contextMenu ({ xAt (2.0 * bar), 12.0f })).empty());

    // A marker: Rename, Color, Delete.
    const juce::Point<float> onMarker (xAt (2.0 * bar), markerY);
    auto items = entries (ruler.contextMenu (onMarker));
    REQUIRE (items.size() == 3);
    CHECK (items[0].text == "Rename Marker...");
    CHECK (items[1].text == "Color");
    CHECK (items[2].text == "Delete Marker");
    ruler.pickContext (Ruler::contextColour + 3, onMarker);
    ui.settle();
    REQUIRE (ui.m().markers.at (marker)->color);
    CHECK (ui.m().markers.at (marker)->color->palette == 2);
    CHECK (s.undoName() == "Marker Color");
    ruler.pickContext (Ruler::contextColour, onMarker);
    ui.settle();
    CHECK_FALSE (ui.m().markers.at (marker)->color);
    // Empty lane space: Add Marker Here, at the snapped position.
    const juce::Point<float> emptyLane (xAt (6.0 * bar), markerY);
    items = entries (ruler.contextMenu (emptyLane));
    REQUIRE (items.size() == 1);
    CHECK (items[0].text == "Add Marker Here");
    ruler.pickContext (Ruler::contextAdd, emptyLane);
    ui.settle();
    REQUIRE (ui.m().markers.size() == 2);
    CHECK (arrange::markersInOrder (ui.m())[1]->pos == 6 * bar);
    ruler.pickContext (Ruler::contextDelete, onMarker);
    ui.settle();
    CHECK (ui.m().markers.find (marker) == ui.m().markers.end());

    // Tempo lane: add on empty space; a change has Set Tempo and Delete, and the first can't be deleted.
    const juce::Point<float> emptyTempo (xAt (4.0 * bar), ruler.tempoY (ui.m().tempo.points()[0].bpm) + 8.0f);
    items = entries (ruler.contextMenu (emptyTempo));
    REQUIRE (items.size() == 1);
    CHECK (items[0].text == "Add Tempo Change Here");
    ruler.pickContext (Ruler::contextAdd, emptyTempo);
    ui.settle();
    REQUIRE (ui.m().tempo.points().size() == 2);
    const auto added = ui.m().tempo.points()[1];
    CHECK (added.pos == 4 * bar);
    const juce::Point<float> onTempo (xAt (4.0 * bar) - 2.0f, ruler.tempoY (added.bpm));
    items = entries (ruler.contextMenu (onTempo));
    REQUIRE (items.size() == 2);
    CHECK (items[0].text == "Set Tempo...");
    CHECK (items[1].text == "Delete Tempo Change");
    CHECK (items[1].enabled);
    const juce::Point<float> firstTempo (xAt (0.0) - 1.0f, ruler.tempoY (ui.m().tempo.points()[0].bpm));
    items = entries (ruler.contextMenu (firstTempo));
    REQUIRE (items.size() == 2);
    CHECK_FALSE (items[1].enabled);
    ruler.pickContext (Ruler::contextDelete, onTempo);
    ui.settle();
    CHECK (ui.m().tempo.points().size() == 1);

    // Signature lane: Edit and Delete on a change (not the first), Add elsewhere.
    REQUIRE (s.addSignaturePoint (2, 3, 4));
    ui.studio.edited();
    ui.settle();
    const juce::Point<float> onSignature (xAt (static_cast<double> (ui.m().signatures.barStart (2))) + 2.0f, signatureY);
    items = entries (ruler.contextMenu (onSignature));
    REQUIRE (items.size() == 2);
    CHECK (items[0].text == "Edit Time Signature...");
    CHECK (items[1].text == "Delete Time Signature");
    CHECK (items[1].enabled);
    items = entries (ruler.contextMenu ({ xAt (0.0), signatureY }));
    REQUIRE (items.size() == 2);
    CHECK_FALSE (items[1].enabled);
    items = entries (ruler.contextMenu ({ xAt (static_cast<double> (ui.m().signatures.barStart (5))) + 60.0f, signatureY }));
    REQUIRE (items.size() == 1);
    CHECK (items[0].text == "Add Time Signature Here...");
    ruler.pickContext (Ruler::contextDelete, onSignature);
    ui.settle();
    CHECK (ui.m().signatures.points().size() == 1);
}

TEST_CASE ("AT-TRK-18 the Add Track button and a double-click below the headers open New Track", "[UI][AT-TRK-18]")
{
    Ui ui;
    auto& tracks = ui.main->tracks();
    std::vector<Cmd> asked;
    ui.studio.uiCommand = [&] (Cmd c) { asked.push_back (c); };
    REQUIRE (tracks.addTrackButton().isVisible());
    CHECK (tracks.addTrackButton().getTooltip().contains ("New Track"));
    CHECK (tracks.addTrackButton().getBottom() <= Ruler::rulerHeight);
    tracks.addTrackButton().onClick();
    CHECK (asked == std::vector<Cmd> { Cmd::newTrack });

    // Below the last header the column is empty: a double-click there asks for a new track too.
    auto& column = tracks.headerArea();
    const auto below = juce::Point<float> (40.0f, static_cast<float> (tracks.headers().back()->getBottom() + 30));
    REQUIRE (column.getComponentAt (below.toInt()) == &column);
    column.mouseDoubleClick (mouse (column, below, {}, 2));
    CHECK (asked.size() == 2);
    // On a header it's the header's own double-click (rename), not a new track.
    CHECK (column.getComponentAt (tracks.headers().front()->getBounds().getCentre()) != &column);
    ui.studio.uiCommand = nullptr;
}

namespace
{
TrackHeader* headerFor (Ui& ui, const Id& id)
{
    for (const auto& h : ui.main->tracks().headers())
        if (h->trackId == id)
            return h.get();
    return nullptr;
}

/** A button click as the mouse would deliver it with these modifiers held. */
void clickWith (Ui& ui, juce::Button& b, juce::ModifierKeys mods)
{
    juce::ModifierKeys::currentModifiers = mods;
    b.onClick();
    juce::ModifierKeys::currentModifiers = {};
    ui.settle();
}
} // namespace

TEST_CASE ("AT-TRK-19 Ctrl+click: only this track; Alt+click: every track; a swipe sets a column", "[UI][AT-TRK-19]")
{
    Ui ui;
    auto& s = ui.studio.session();
    REQUIRE (s.createTrack (model::TrackKind::audio, "Two"));
    REQUIRE (s.createTrack (model::TrackKind::audio, "Three"));
    ui.studio.edited();
    ui.settle();
    std::vector<Id> t;
    for (const auto& id : ui.m().trackOrder)
        if (ui.m().track (id)->kind == model::TrackKind::audio)
            t.push_back (id);
    REQUIRE (t.size() == 3);
    const auto ctrl = juce::ModifierKeys (juce::ModifierKeys::commandModifier);
    const auto alt = juce::ModifierKeys (juce::ModifierKeys::altModifier);
    const auto muted = [&] (std::size_t i) { return ui.m().track (t[i])->mute; };

    clickWith (ui, headerFor (ui, t[0])->mute, {});
    clickWith (ui, headerFor (ui, t[2])->mute, {});
    clickWith (ui, headerFor (ui, t[1])->mute, ctrl);
    CHECK_FALSE (muted (0));
    CHECK (muted (1));
    CHECK_FALSE (muted (2));
    CHECK (s.undoName() == "Mute Only");

    clickWith (ui, headerFor (ui, t[0])->mute, alt);
    CHECK ((muted (0) && muted (1) && muted (2)));
    CHECK (s.undoName() == "Mute All");
    clickWith (ui, headerFor (ui, t[0])->mute, alt);
    CHECK_FALSE ((muted (0) || muted (1) || muted (2)));
    CHECK (s.undoName() == "Unmute All");

    clickWith (ui, headerFor (ui, t[1])->solo, alt);
    CHECK (ui.studio.localView().soloed == std::set<Id> { t.begin(), t.end() });
    clickWith (ui, headerFor (ui, t[1])->solo, alt);
    CHECK (ui.studio.localView().soloed.empty());

    clickWith (ui, headerFor (ui, t[0])->arm, {});
    clickWith (ui, headerFor (ui, t[1])->arm, ctrl);
    CHECK (ui.studio.localView().armed == std::set<Id> { t[1] });

    // A swipe from track 1's Mute over tracks 2 and 3: all three muted, one undo step.
    {
        auto& from = headerFor (ui, t[0])->mute;
        from.swipeOver (&headerFor (ui, t[1])->mute);
        from.swipeOver (&headerFor (ui, t[2])->mute);
        from.swipeOver (&headerFor (ui, t[2])->mute);
        from.swipeOver (&headerFor (ui, t[1])->solo); // another kind of button is ignored
        CHECK (from.endSwipe());
        ui.settle();
    }
    CHECK ((muted (0) && muted (1) && muted (2)));
    CHECK (ui.studio.localView().soloed.empty());
    CHECK (s.undoName() == "Mute Tracks");
    REQUIRE (ui.studio.perform (Cmd::undo));
    ui.settle();
    CHECK_FALSE ((muted (0) || muted (1) || muted (2)));

    // The same with the mouse, on Solo: press on track 1's, drag onto track 2's, release there.
    {
        auto& from = headerFor (ui, t[0])->solo;
        auto& over = headerFor (ui, t[1])->solo;
        const auto centre = from.getLocalBounds().getCentre().toFloat();
        const auto there = from.getLocalPoint (&over, over.getLocalBounds().getCentre().toFloat());
        from.mouseDown (mouse (from, centre));
        from.mouseDrag (mouse (from, there, {}, 1, centre));
        from.mouseUp (mouse (from, there, {}, 1, centre));
        ui.settle();
    }
    CHECK (ui.studio.localView().soloed == std::set<Id> { t[0], t[1] });

    // From a lit Solo, the swipe turns them off.
    {
        auto& from = headerFor (ui, t[0])->solo;
        from.swipeOver (&headerFor (ui, t[1])->solo);
        from.swipeOver (&headerFor (ui, t[2])->solo);
        CHECK (from.endSwipe());
        ui.settle();
    }
    CHECK (ui.studio.localView().soloed.empty());
}

TEST_CASE ("AT-ARR-37 markers 10 to 19, Rename and Delete Marker, Ctrl+click Previous/Next Bar", "[UI][AT-ARR-37]")
{
    Ui ui;
    auto& s = ui.studio.session();
    for (int i = 0; i < 12; ++i)
        REQUIRE (s.addMarker ((i + 2) * bar, "M" + std::to_string (i + 1)));
    ui.studio.edited();
    ui.settle();
    const auto ctrl = juce::ModifierKeys (juce::ModifierKeys::commandModifier);

    ui.key (juce::KeyPress::numberPad1, ctrl);
    CHECK (ui.studio.playhead() == 12 * bar); // the eleventh
    ui.key (juce::KeyPress::numberPad0, ctrl);
    CHECK (ui.studio.playhead() == 11 * bar); // the tenth
    ui.key (juce::KeyPress::numberPad9, ctrl);
    CHECK (ui.studio.playhead() == 11 * bar); // there is no nineteenth

    juce::Button* previous = nullptr;
    juce::Button* next = nullptr;
    for (auto* b : ui.main->controlBar().commandButtons())
    {
        if (b->command == Cmd::previousBar)
            previous = b;
        if (b->command == Cmd::nextBar)
            next = b;
    }
    REQUIRE (previous != nullptr);
    REQUIRE (next != nullptr);
    ui.studio.locate (0);
    clickWith (ui, *next, ctrl);
    CHECK (ui.studio.playhead() == 2 * bar);
    clickWith (ui, *next, ctrl);
    CHECK (ui.studio.playhead() == 3 * bar);
    clickWith (ui, *previous, ctrl);
    CHECK (ui.studio.playhead() == 2 * bar);
    clickWith (ui, *next, {});
    CHECK (ui.studio.playhead() == 3 * bar); // a plain click: one bar

    // Delete Marker acts on the marker at or before the playhead.
    ui.studio.locate (5 * bar + 100);
    ui.settle();
    REQUIRE (ui.studio.markerAtPlayhead() != nullptr);
    CHECK (ui.studio.markerAtPlayhead()->name == "M4");
    ui.key (juce::KeyPress::deleteKey, juce::ModifierKeys (juce::ModifierKeys::altModifier));
    CHECK (ui.m().markers.size() == 11);
    CHECK (s.undoName() == "Delete Marker");
    CHECK (ui.studio.markerAtPlayhead()->name == "M3");
    ui.studio.locate (bar);
    ui.settle();
    ui.key (juce::KeyPress::deleteKey, juce::ModifierKeys (juce::ModifierKeys::altModifier));
    CHECK (ui.m().markers.size() == 11);
    CHECK (ui.studio.status.contains ("No marker"));

    juce::StringArray items;
    const auto menu = ui.main->getMenuForIndex (4, "Navigate");
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        items.add (it.getItem().text);
    CHECK (items.contains ("Rename Marker..."));
    CHECK (items.contains ("Delete Marker"));
}

TEST_CASE ("AT-ARR-38 Left/Right step through a track's regions; Z toggles selection and project", "[UI][AT-ARR-38]")
{
    Ui ui;
    const auto tracks = ui.instrumentTracks (1);
    auto& s = ui.studio.session();
    const auto one = *s.createMidiRegion (tracks[0], 0, bar);
    const auto three = *s.createMidiRegion (tracks[0], 2 * bar, bar);
    const auto five = *s.createMidiRegion (tracks[0], 4 * bar, bar);
    const auto far = *s.createMidiRegion (tracks[0], 40 * bar, bar);
    ui.studio.edited();
    ui.settle();
    const auto selected = [&] { return ui.studio.selection.ids(); };

    ui.studio.selection.set ({ three });
    ui.key (juce::KeyPress::rightKey);
    CHECK (selected() == std::vector<Id> { five });
    ui.key (juce::KeyPress::leftKey);
    ui.key (juce::KeyPress::leftKey);
    CHECK (selected() == std::vector<Id> { one });
    ui.key (juce::KeyPress::leftKey);
    CHECK (selected() == std::vector<Id> { one });

    ui.studio.selection.clear();
    ui.studio.selectTrack (tracks[0]);
    ui.studio.locate (bar);
    ui.settle();
    ui.key (juce::KeyPress::rightKey);
    CHECK (selected() == std::vector<Id> { three });

    // Z: the selection, then the whole project, then a new selection.
    const auto width = static_cast<double> (ui.timeline().getWidth());
    ui.key ('Z');
    const auto selectionWidth = ui.studio.view.tickToX (3.0 * bar) - ui.studio.view.tickToX (2.0 * bar);
    CHECK (selectionWidth / width >= 0.8);
    ui.key ('Z');
    CHECK (ui.studio.view.tickToX (0.0) >= -1.0);
    CHECK (ui.studio.view.tickToX (ui.m().regionEndTick (ui.region (far))) <= width);
    ui.studio.selection.set ({ five });
    ui.key ('Z');
    CHECK ((ui.studio.view.tickToX (5.0 * bar) - ui.studio.view.tickToX (4.0 * bar)) / width >= 0.8);
}

TEST_CASE ("AT-TMP-10 Shift keeps a tempo-change drag to one direction", "[UI][AT-TMP-10]")
{
    Ui ui;
    auto& ruler = ui.ruler();
    ui.studio.layout.globalTracks = true;
    REQUIRE (ui.studio.session().addTempoPoint (4 * bar, 90.0));
    ui.studio.edited();
    ui.settle();
    const auto shift = juce::ModifierKeys (juce::ModifierKeys::shiftModifier);
    const auto x = [&] (int bars) { return static_cast<float> (ui.studio.view.tickToX (static_cast<double> (bars) * bar)); };
    const auto point = [&] { return ui.m().tempo.points()[1]; };

    auto y = ruler.tempoY (90.0);
    ruler.pointerDown ({ x (4), y }, {});
    ruler.pointerDrag ({ x (5), y - 5.0f }, shift);
    ruler.pointerDrag ({ x (6), y - 20.0f }, shift);
    ruler.pointerUp();
    ui.settle();
    CHECK (point().pos == ui.studio.snapped (6 * bar));
    CHECK (point().bpm == 90.0);

    y = ruler.tempoY (90.0);
    ruler.pointerDown ({ x (6), y }, {});
    ruler.pointerDrag ({ x (6) + 2.0f, y - 10.0f }, shift);
    ruler.pointerDrag ({ x (7), y - 20.0f }, shift);
    ruler.pointerUp();
    ui.settle();
    CHECK (point().pos == ui.studio.snapped (6 * bar));
    CHECK (point().bpm > 90.0);

    const auto before = point().bpm;
    y = ruler.tempoY (before);
    ruler.pointerDown ({ x (6), y }, {});
    ruler.pointerDrag ({ x (8), y - 20.0f }, {});
    ruler.pointerUp();
    ui.settle();
    CHECK (point().pos == ui.studio.snapped (8 * bar));
    CHECK (point().bpm > before);
}

TEST_CASE ("AT-UI-12 one key picks a tool while the Tool menu is open", "[UI][AT-UI-12]")
{
    Ui ui;
    juce::StringArray keys;
    const auto menu = ui.main->toolMenu();
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        keys.add (it.getItem().shortcutKeyDescription);
    CHECK (keys.joinIntoString (" ") == "T P I J E M N Z A G Q");

    auto& listener = ui.main->toolKeys;
    CHECK (listener.keyPressed (juce::KeyPress ('p', {}, 'p'), nullptr));
    ui.settle();
    CHECK (ui.studio.tool == Tool::pencil);
    CHECK (listener.keyPressed (juce::KeyPress ('m', {}, 'm'), nullptr));
    CHECK (ui.studio.tool == Tool::mute);
    CHECK_FALSE (listener.keyPressed (juce::KeyPress ('w', {}, 'w'), nullptr));
    CHECK (ui.studio.tool == Tool::mute);
    CHECK_FALSE (listener.keyPressed (juce::KeyPress ('t', juce::ModifierKeys (juce::ModifierKeys::commandModifier), 't'), nullptr));
    CHECK (ui.studio.tool == Tool::mute);
    CHECK (listener.keyPressed (juce::KeyPress ('t', {}, 't'), nullptr));
    CHECK (ui.studio.tool == Tool::pointer);
}

TEST_CASE ("AT-TRK-20 New Track dialog: input, output, monitoring, record enable; header-area menu; Delete Unused Tracks",
           "[UI][AT-TRK-20]")
{
    Ui ui;
    auto& s = ui.studio.session();
    const auto first = ui.m().trackOrder.front();
    const auto bus = s.createTrack (model::TrackKind::bus, "Drums").value();
    ui.studio.selectTrack (first);
    ui.studio.edited();
    ui.settle();

    NewTrackDialog dialog (ui.studio);
    dialog.kind.setSelectedItemIndex (0, juce::sendNotificationSync);
    dialog.channels.setSelectedItemIndex (0, juce::sendNotificationSync); // mono: Input 1, Input 2
    CHECK (dialog.input.getNumItems() == 3);
    CHECK (dialog.input.getSelectedId() == NewTrackDialog::firstInput); // Input 1 by default
    dialog.count.setValue (2);
    dialog.ascending.setToggleState (true, juce::dontSendNotification);
    dialog.output.setSelectedItemIndex (1, juce::dontSendNotification); // the bus
    dialog.monitoring.setToggleState (true, juce::dontSendNotification);
    dialog.recordEnable.setToggleState (true, juce::dontSendNotification);
    const auto ids = dialog.create();
    ui.settle();
    REQUIRE (ids.size() == 2);
    for (std::size_t i = 0; i < 2; ++i)
    {
        const auto* t = ui.m().track (ids[i]);
        INFO ("track " << i);
        CHECK (t->channels == 1);
        CHECK (t->input.kind == model::InputAssignment::Kind::audio);
        CHECK (t->input.firstChannel == static_cast<int> (i)); // ascending: inputs 1 and 2
        CHECK (t->output.busId == bus);
        CHECK (t->monitor == "on");
        CHECK (ui.studio.localView().armed.contains (ids[i]));
    }
    CHECK (s.undoName() == "New Tracks");
    REQUIRE (ui.studio.perform (Cmd::undo));
    ui.settle();
    CHECK (ui.m().track (ids[0]) == nullptr);
    CHECK (ui.m().track (ids[1]) == nullptr);

    // Stereo offers pairs; an instrument track offers the instruments, Weft Keys first.
    dialog.channels.setSelectedItemIndex (1, juce::sendNotificationSync);
    CHECK (dialog.input.getItemText (1) == "Input 1-2");
    dialog.kind.setSelectedItemIndex (1, juce::sendNotificationSync);
    CHECK_FALSE (dialog.input.isEnabled());
    CHECK (dialog.instrument.isEnabled());
    CHECK (dialog.instrument.getItemText (0) == "Weft Keys (default)");

    // Right-click below the headers: the new-track commands and Delete Unused Tracks.
    juce::StringArray items;
    const auto menu = ui.main->tracks().headerAreaMenu();
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        if (it.getItem().itemID != 0)
            items.add (it.getItem().text);
    CHECK (items.joinIntoString ("|") == "New Track|New Audio Track|New Instrument Track|Delete Unused Tracks|Turn All Tracks On/Off");
    const auto before = ui.m().trackOrder.size();
    ui.main->tracks().pickHeaderAreaMenu (static_cast<int> (Cmd::newInstrumentTrack) + 1);
    ui.settle();
    CHECK (ui.m().trackOrder.size() == before + 1);

    // Delete Unused Tracks: audio and instrument tracks without regions go, in one step; the bus stays.
    const auto keys = ui.instrumentTracks (1).front();
    REQUIRE (s.createMidiRegion (keys, 0, bar));
    ui.studio.edited();
    ui.settle();
    REQUIRE (ui.studio.perform (Cmd::deleteUnusedTracks));
    ui.settle();
    std::vector<Id> left;
    for (const auto& id : ui.m().trackOrder)
        left.push_back (id);
    CHECK (left == std::vector<Id> { bus, keys });
    CHECK (s.undoName() == "Delete Unused Tracks");
    CHECK_FALSE (ui.studio.perform (Cmd::deleteUnusedTracks));
    CHECK (ui.studio.status == "No unused tracks.");
    REQUIRE (ui.studio.perform (Cmd::undo));
    ui.settle();
    CHECK (ui.m().track (first) != nullptr);
}

TEST_CASE ("AT-TRK-22 Configure Track Header: optional buttons, hidden parts, column width, all tracks on/off", "[UI][AT-TRK-22]")
{
    Ui ui;
    auto& s = ui.studio.session();
    REQUIRE (s.createTrack (model::TrackKind::audio, "Two"));
    REQUIRE (s.createTrack (model::TrackKind::instrument, "Keys"));
    ui.studio.edited();
    ui.settle();
    std::vector<Id> t;
    for (const auto& id : ui.m().trackOrder)
        if (const auto* track = ui.m().track (id); track->kind == model::TrackKind::audio || track->kind == model::TrackKind::instrument)
            t.push_back (id);
    REQUIRE (t.size() == 3);
    const auto off = [&] (std::size_t i) { return ui.m().track (t[i])->off; };

    // By default the optional buttons don't show; the header's menu and the column's offer the options.
    auto* first = headerFor (ui, t[0]);
    REQUIRE (first != nullptr);
    CHECK_FALSE (first->onOff.isVisible());
    CHECK_FALSE (first->monitor.isVisible());
    CHECK_FALSE (first->hide.isVisible());
    juce::StringArray subMenus;
    const auto headerMenu = first->menu();
    for (juce::PopupMenu::MenuItemIterator it (headerMenu); it.next();)
        if (it.getItem().subMenu != nullptr)
            subMenus.add (it.getItem().text);
    CHECK (subMenus.contains ("Configure Track Header"));
    CHECK (commandInfo (Cmd::configureTrackHeader).shortcut == "Alt+T");

    first->pickMenu (static_cast<int> (TrackHeader::itemHeaderOption) + Studio::headerOnOff);
    ui.main->tracks().pickHeaderAreaMenu (TracksArea::headerOptionBase + Studio::headerMonitor);
    ui.studio.toggleHeaderOption (Studio::headerHide);
    ui.settle();
    first = headerFor (ui, t[0]);
    CHECK (first->onOff.isVisible());
    CHECK (first->monitor.isVisible());
    CHECK (first->hide.isVisible());
    CHECK (first->onOff.getToggleState());
    CHECK_FALSE (headerFor (ui, t[2])->monitor.isVisible()); // input monitoring is for audio tracks
    CHECK (headerFor (ui, t[2])->onOff.isVisible());
    // Left to right: On, M, S, R, I, H; the name keeps the rest.
    CHECK (first->onOff.getRight() < first->mute.getX());
    CHECK (first->arm.getRight() < first->monitor.getX());
    CHECK (first->monitor.getRight() < first->hide.getX());
    CHECK (first->name.getRight() <= first->onOff.getX());

    // On/Off: a click turns the track off; Ctrl+click turns only this one on; Alt+click sets them all.
    const auto ctrl = juce::ModifierKeys (juce::ModifierKeys::commandModifier);
    const auto alt = juce::ModifierKeys (juce::ModifierKeys::altModifier);
    clickWith (ui, headerFor (ui, t[0])->onOff, {});
    CHECK (off (0));
    CHECK (s.undoName() == "Turn Track Off");
    CHECK_FALSE (headerFor (ui, t[0])->onOff.getToggleState());
    clickWith (ui, headerFor (ui, t[1])->onOff, ctrl);
    CHECK ((off (0) && ! off (1) && off (2)));
    CHECK (s.undoName() == "Turn On Only");
    clickWith (ui, headerFor (ui, t[1])->onOff, alt);
    CHECK ((off (0) && off (1) && off (2)));
    CHECK (s.undoName() == "Turn All Tracks Off");

    // Track › Turn All Tracks On/Off: any off turns them all on; then all go off.
    REQUIRE (ui.studio.perform (Cmd::toggleAllTracksOff));
    ui.settle();
    CHECK_FALSE ((off (0) || off (1) || off (2)));
    CHECK (s.undoName() == "Turn All Tracks On");
    REQUIRE (ui.studio.perform (Cmd::toggleAllTracksOff));
    ui.settle();
    CHECK ((off (0) && off (1) && off (2)));
    REQUIRE (ui.studio.perform (Cmd::undo));
    ui.settle();
    CHECK_FALSE ((off (0) || off (1) || off (2)));

    // A swipe across the On/Off column is one undo step.
    {
        auto& from = headerFor (ui, t[0])->onOff;
        from.swipeOver (&headerFor (ui, t[1])->onOff);
        from.swipeOver (&headerFor (ui, t[2])->onOff);
        from.swipeOver (&headerFor (ui, t[1])->mute); // another kind of button is ignored
        CHECK (from.endSwipe());
        ui.settle();
    }
    CHECK ((off (0) && off (1) && off (2)));
    CHECK_FALSE (ui.m().track (t[1])->mute);
    CHECK (s.undoName() == "Turn Tracks On/Off");
    REQUIRE (ui.studio.perform (Cmd::undo));
    ui.settle();

    // Input monitoring: on, then back to auto.
    clickWith (ui, headerFor (ui, t[0])->monitor, {});
    CHECK (ui.m().track (t[0])->monitor == "on");
    CHECK (headerFor (ui, t[0])->monitor.getToggleState());
    clickWith (ui, headerFor (ui, t[0])->monitor, {});
    CHECK (ui.m().track (t[0])->monitor == "auto");

    // Hide: the track goes (until Show Hidden Tracks).
    clickWith (ui, headerFor (ui, t[1])->hide, {});
    CHECK (ui.m().track (t[1])->hidden);
    CHECK (headerFor (ui, t[1]) == nullptr);

    // Volume, pan, numbers, icons.
    first = headerFor (ui, t[0]);
    const auto nameX = first->name.getX();
    CHECK (first->volume.isVisible());
    ui.studio.toggleHeaderOption (Studio::headerVolume);
    ui.settle();
    first = headerFor (ui, t[0]);
    CHECK_FALSE (first->volume.isVisible());
    CHECK (first->pan.isVisible());
    ui.studio.toggleHeaderOption (Studio::headerNumbers);
    ui.settle();
    CHECK (headerFor (ui, t[0])->name.getX() < nameX);
    ui.studio.toggleHeaderOption (Studio::headerNumbers);
    ui.studio.toggleHeaderOption (Studio::headerIcons);
    ui.settle();
    CHECK (headerFor (ui, t[0])->name.getX() > nameX);
    CHECK_FALSE (icon::track (0).isEmpty());
    CHECK_FALSE (icon::track (1).isEmpty());

    // The column's width: drag its edge, within limits; double-click for the default.
    auto& tracks = ui.main->tracks();
    auto& edge = tracks.headerResizer();
    CHECK (tracks.headerWidth() == Studio::defaultHeaderWidth);
    const auto start = edge.getLocalBounds().getCentre().toFloat();
    edge.mouseDown (mouse (edge, start));
    edge.mouseDrag (mouse (edge, start + juce::Point<float> (60.0f, 0.0f), {}, 1, start));
    ui.settle();
    CHECK (tracks.headerWidth() == Studio::defaultHeaderWidth + 60);
    CHECK (headerFor (ui, t[0])->getWidth() == Studio::defaultHeaderWidth + 60);
    CHECK (tracks.timeline().getX() == Studio::defaultHeaderWidth + 60);
    edge.mouseDrag (mouse (edge, start + juce::Point<float> (2000.0f, 0.0f), {}, 1, start));
    ui.settle();
    CHECK (tracks.headerWidth() == Studio::maxHeaderWidth);
    edge.mouseDoubleClick (mouse (edge, start, {}, 2));
    ui.settle();
    CHECK (tracks.headerWidth() == Studio::defaultHeaderWidth);
}

TEST_CASE ("AT-ARR-41 zoom sliders, vertical zoom, saved zooms, navigation back and forward, paging, zoom one track", "[UI][AT-ARR-41]")
{
    Ui ui;
    auto& s = ui.studio.session();
    for (int i = 0; i < 12; ++i)
        REQUIRE (s.createTrack (model::TrackKind::audio, "T" + std::to_string (i)));
    ui.studio.edited();
    ui.settle();
    auto& tracks = ui.main->tracks();
    const auto first = ui.m().trackOrder.front();
    auto& view = ui.studio.view;

    // The horizontal slider is the zoom, on a log scale; the vertical one sets every track's height.
    CHECK (tracks.horizontalZoom.getValue() == Catch::Approx (TracksArea::positionFor (view.pixelsPerTick)));
    CHECK (TracksArea::pixelsPerTickFor (TracksArea::positionFor (0.01)) == Catch::Approx (0.01));
    tracks.horizontalZoom.setValue (0.75, juce::sendNotificationSync);
    CHECK (view.pixelsPerTick == Catch::Approx (TracksArea::pixelsPerTickFor (0.75)));
    tracks.verticalZoom.setValue (80, juce::sendNotificationSync);
    ui.settle();
    for (const auto& id : ui.m().trackOrder)
        CHECK (ui.studio.trackHeight (id) == 80);
    CHECK (tracks.horizontalZoom.getRight() <= tracks.addTrackButton().getX());

    // Ctrl+Down / Ctrl+Up: every track taller / shorter.
    REQUIRE (ui.key (juce::KeyPress::downKey, juce::ModifierKeys::ctrlModifier));
    CHECK (ui.studio.trackHeight (first) == 100);
    REQUIRE (ui.key (juce::KeyPress::upKey, juce::ModifierKeys::ctrlModifier));
    CHECK (ui.studio.trackHeight (first) == 80);

    // Save as Zoom 1, change the view, Recall Zoom 1 brings it back; an unsaved slot only says so.
    view.originTick = 3840 * 4;
    REQUIRE (ui.studio.perform (Cmd::saveZoom1));
    const auto saved = view.pixelsPerTick;
    REQUIRE (ui.studio.perform (Cmd::zoomIn));
    REQUIRE (ui.studio.perform (Cmd::zoomVerticalIn));
    view.originTick = 0;
    REQUIRE (ui.key ('1', juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::altModifier));
    CHECK (view.pixelsPerTick == Catch::Approx (saved));
    CHECK (view.originTick == Catch::Approx (3840 * 4));
    CHECK (ui.studio.trackHeight (first) == 80);
    CHECK_FALSE (ui.studio.perform (Cmd::recallZoom2));

    // Navigation: Back undoes the last zoom change; Forward redoes it.
    REQUIRE (ui.studio.perform (Cmd::navigateBack));
    CHECK (ui.studio.trackHeight (first) == 100);
    REQUIRE (ui.studio.perform (Cmd::navigateBack));
    CHECK (ui.studio.trackHeight (first) == 80);
    CHECK (view.pixelsPerTick == Catch::Approx (saved * 1.5));
    REQUIRE (ui.studio.perform (Cmd::navigateForward));
    CHECK (ui.studio.trackHeight (first) == 100);

    // Page Left/Right move the view by its width; Page Up/Down scroll the tracks by the view's height.
    view.originTick = 0;
    REQUIRE (ui.key (juce::KeyPress::pageDownKey, juce::ModifierKeys::altModifier));
    CHECK (view.tickToX (0) == Catch::Approx (-ui.studio.timelineWidth));
    REQUIRE (ui.studio.perform (Cmd::pageLeft));
    CHECK (view.originTick == Catch::Approx (0.0));
    REQUIRE (ui.studio.perform (Cmd::pageLeft));
    CHECK (view.originTick == Catch::Approx (0.0)); // never before the start
    REQUIRE (ui.key (juce::KeyPress::pageDownKey));
    CHECK (tracks.timeline().scrollY > 0);
    REQUIRE (ui.key (juce::KeyPress::pageUpKey));
    CHECK (tracks.timeline().scrollY == 0);

    // Zoom Selected Track: the selected track grows; again, it goes back.
    ui.studio.selectTrack (first);
    const auto before = ui.studio.trackHeight (first);
    REQUIRE (ui.studio.perform (Cmd::zoomTrack));
    CHECK (ui.studio.trackHeight (first) > before);
    CHECK (ui.studio.trackHeight (ui.m().trackOrder[1]) == before);
    REQUIRE (ui.studio.perform (Cmd::zoomTrack));
    CHECK (ui.studio.trackHeight (first) == before);

    // Ctrl+drag on a header's lower edge sets every track's height.
    auto* header = tracks.headers().front().get();
    const auto edge = juce::Point<float> (40.0f, static_cast<float> (ui.studio.trackHeight (first) - 2));
    header->mouseDown (mouse (*header, edge, juce::ModifierKeys (juce::ModifierKeys::commandModifier | juce::ModifierKeys::leftButtonModifier)));
    header->mouseDrag (mouse (*header, edge + juce::Point<float> (0.0f, 20.0f), juce::ModifierKeys (juce::ModifierKeys::commandModifier | juce::ModifierKeys::leftButtonModifier), 1, edge));
    header->mouseUp (mouse (*header, edge + juce::Point<float> (0.0f, 20.0f), juce::ModifierKeys (juce::ModifierKeys::commandModifier), 1, edge));
    ui.settle();
    CHECK (ui.studio.trackHeight (ui.m().trackOrder[5]) == before + 20);
}

TEST_CASE ("AT-ARR-42 region selection commands, move and trim to the playhead, split at the cycle, Up/Down through tracks", "[UI][AT-ARR-42]")
{
    Ui ui;
    auto& s = ui.studio.session();
    const auto keys = s.createTrack (model::TrackKind::instrument, "Keys").value();
    const auto bass = s.createTrack (model::TrackKind::instrument, "Bass").value();
    const Ticks bar = 3840;
    const auto a = s.createMidiRegion (keys, 0, bar).value();
    const auto b = s.createMidiRegion (keys, 2 * bar, bar).value();
    const auto c = s.createMidiRegion (keys, 4 * bar, bar).value();
    const auto d = s.createMidiRegion (bass, 2 * bar, 2 * bar).value();
    REQUIRE (s.setRegionMuted (b, true));
    REQUIRE (s.setRegionColor (d, model::Color { 5, {} }));
    REQUIRE (s.setRegionColor (a, model::Color { 5, {} }));
    ui.studio.edited();
    ui.settle();
    auto& sel = ui.studio.selection;
    const auto selected = [&] {
        auto ids = sel.ids();
        std::sort (ids.begin(), ids.end());
        return ids;
    };
    const auto sorted = [] (std::vector<Id> ids) {
        std::sort (ids.begin(), ids.end());
        return ids;
    };

    // Select All Following: from the earliest selected region.
    sel.set ({ b });
    REQUIRE (ui.key ('F', juce::ModifierKeys::shiftModifier));
    CHECK (selected() == sorted ({ b, c, d }));
    // Inside the cycle, muted, same colour, first and last.
    REQUIRE (s.setCycle (false, 2 * bar, 4 * bar));
    ui.studio.edited();
    REQUIRE (ui.key ('L', juce::ModifierKeys::shiftModifier));
    CHECK (selected() == sorted ({ b, d }));
    REQUIRE (ui.studio.perform (Cmd::selectMutedRegions));
    CHECK (selected() == std::vector<Id> { b });
    sel.set ({ d });
    REQUIRE (ui.studio.perform (Cmd::selectSameColor));
    CHECK (selected() == sorted ({ a, d }));
    ui.studio.selectTrack (keys);
    REQUIRE (ui.studio.perform (Cmd::selectLast));
    CHECK (selected() == std::vector<Id> { c });
    REQUIRE (ui.studio.perform (Cmd::selectFirst));
    CHECK (selected() == std::vector<Id> { a });
    // Shift+Right adds the next region on the track; Shift+Left from there the previous.
    REQUIRE (ui.key (juce::KeyPress::rightKey, juce::ModifierKeys::shiftModifier));
    CHECK (selected() == sorted ({ a, b }));
    sel.set ({ c });
    REQUIRE (ui.key (juce::KeyPress::leftKey, juce::ModifierKeys::shiftModifier));
    CHECK (selected() == sorted ({ b, c }));

    // Up and Down in the arrange view select the previous and next track.
    ui.studio.selectTrack (keys);
    REQUIRE (ui.key (juce::KeyPress::downKey));
    CHECK (ui.studio.selectedTrack() == bass);
    REQUIRE (ui.key (juce::KeyPress::upKey));
    CHECK (ui.studio.selectedTrack() == keys);

    // Move Region to Playhead (and Select Next).
    sel.set ({ b });
    ui.studio.locate (3 * bar);
    REQUIRE (ui.studio.perform (Cmd::moveToPlayheadSelectNext));
    ui.settle();
    CHECK (ui.m().region (b)->start == 3 * bar);
    CHECK (selected() == std::vector<Id> { c });
    REQUIRE (ui.studio.perform (Cmd::undo));
    ui.settle();
    CHECK (ui.m().region (b)->start == 2 * bar);

    // Set Region Start / End to Playhead trims the selected regions the playhead is inside.
    sel.set ({ d });
    ui.studio.locate (3 * bar);
    REQUIRE (ui.studio.perform (Cmd::regionEndToPlayhead));
    ui.settle();
    CHECK (ui.m().region (d)->length == bar);
    CHECK (s.undoName() == "Set Region End");
    REQUIRE (ui.studio.perform (Cmd::undo));
    ui.studio.locate (3 * bar);
    REQUIRE (ui.studio.perform (Cmd::regionStartToPlayhead));
    ui.settle();
    CHECK (ui.m().region (d)->start == 3 * bar);
    CHECK (ui.m().region (d)->length == bar);
    REQUIRE (ui.studio.perform (Cmd::undo));
    ui.settle();
    ui.studio.locate (10 * bar);
    CHECK_FALSE (ui.studio.perform (Cmd::regionStartToPlayhead));

    // Split Regions at Cycle: a region crossing both locators becomes three, one undo step.
    REQUIRE (s.setCycle (false, 2 * bar + bar / 2, 3 * bar + bar / 2));
    ui.studio.edited();
    sel.set ({ d });
    const auto before = ui.m().regions.size();
    REQUIRE (ui.studio.perform (Cmd::splitAtCycle));
    ui.settle();
    CHECK (ui.m().regions.size() == before + 2);
    CHECK (s.undoName() == "Split at Cycle");
    REQUIRE (ui.studio.perform (Cmd::undo));
    ui.settle();
    CHECK (ui.m().regions.size() == before);

    // Scroll to Selection brings a region far to the right into view.
    const auto far = s.createMidiRegion (bass, 200 * bar, bar).value();
    ui.studio.edited();
    sel.set ({ far });
    ui.studio.view.originTick = 0;
    REQUIRE (ui.studio.perform (Cmd::scrollToSelection));
    CHECK (ui.studio.view.tickToX (200.0 * bar) >= 0.0);
    CHECK (ui.studio.view.tickToX (200.0 * bar) < ui.studio.timelineWidth);
}

TEST_CASE ("AT-ARR-43 marker for the selection, cycle to a marker's range, markers and locators, go to marker number", "[UI][AT-ARR-43]")
{
    Ui ui;
    auto& s = ui.studio.session();
    const Ticks bar = 3840;
    const auto keys = s.createTrack (model::TrackKind::instrument, "Keys").value();
    const auto region = s.createMidiRegion (keys, 6 * bar, 2 * bar).value();
    REQUIRE (s.setRegionName (region, "Bridge"));
    REQUIRE (s.addMarker (0, "Intro"));
    REQUIRE (s.addMarker (2 * bar, "Verse"));
    REQUIRE (s.addMarker (4 * bar, "Chorus"));
    ui.studio.edited();
    ui.settle();
    const auto& cycle = [&] { return ui.m().info.cycle; };

    // Shift+; adds a marker at the selected regions' start, named after the first.
    ui.studio.selection.set ({ region });
    REQUIRE (ui.key (';', juce::ModifierKeys::shiftModifier, ':'));
    const auto markers = arrange::markersInOrder (ui.m());
    REQUIRE (markers.size() == 4);
    CHECK (markers.back()->pos == 6 * bar);
    CHECK (markers.back()->name == "Bridge");

    // Set Cycle to Marker: from the marker at or before the playhead to the next one; cycle on.
    ui.studio.locate (3 * bar);
    REQUIRE (ui.studio.perform (Cmd::cycleToMarker));
    CHECK (cycle().enabled);
    CHECK (cycle().start == 2 * bar);
    CHECK (cycle().end == 4 * bar);
    CHECK (ui.studio.playhead() == 3 * bar); // the playhead stays
    REQUIRE (ui.studio.perform (Cmd::cycleToNextMarker));
    CHECK ((cycle().start == 4 * bar && cycle().end == 6 * bar));
    CHECK (ui.studio.playhead() == 4 * bar);
    REQUIRE (ui.studio.perform (Cmd::cycleToPreviousMarker));
    CHECK ((cycle().start == 2 * bar && cycle().end == 4 * bar));
    // The last marker's range runs to the project end.
    ui.studio.locate (7 * bar);
    REQUIRE (ui.studio.perform (Cmd::cycleToMarker));
    CHECK (cycle().start == 6 * bar);
    CHECK (cycle().end == std::max<Ticks> (commands::projectEndTick (ui.m()), 7 * bar));

    // Previous / Next Marker and Set Locators keep Cycle as it is.
    REQUIRE (s.setCycle (false, 0, bar));
    ui.studio.edited();
    ui.studio.locate (bar);
    REQUIRE (ui.key ('.', juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier));
    CHECK (ui.studio.playhead() == 2 * bar);
    CHECK ((cycle().start == 2 * bar && cycle().end == 4 * bar && ! cycle().enabled));
    REQUIRE (ui.studio.perform (Cmd::previousMarkerSetLocators));
    CHECK (ui.studio.playhead() == 0);
    CHECK ((cycle().start == 0 && cycle().end == 2 * bar));

    // Go to Marker Number.
    CHECK (commandInfo (Cmd::goToMarkerNumber).shortcut == "Ctrl+/");
    REQUIRE (ui.studio.goToMarkerNumber (3));
    CHECK (ui.studio.playhead() == 4 * bar);
    CHECK_FALSE (ui.studio.goToMarkerNumber (9));
}

TEST_CASE ("AT-ARR-45 global tracks: choose which show, single track, protect; Key and Arrangement tracks; create commands", "[UI][AT-ARR-45]")
{
    Ui ui;
    auto& ruler = ui.ruler();
    auto& area = ui.main->tracks();
    const auto bar = 3840;
    // Every prompt answers at once with the next text.
    juce::String answer;
    ruler.promptForText = [&answer] (const juce::String&, const juce::String&, std::function<void (juce::String)> done) { done (answer); };

    // The Configure Global Tracks menu lists the five tracks; showing Key and Arrangement adds their lanes in order.
    juce::StringArray names;
    const auto menu = area.globalTracksMenu();
    for (juce::PopupMenu::MenuItemIterator it (menu, true); it.next();)
        names.add (it.getItem().text);
    for (const auto* n : { "Arrangement", "Markers", "Tempo", "Signature", "Key", "Show Single Global Track", "Marker Track Only", "Add Arrangement Section" })
        CHECK (names.contains (n));
    area.pickGlobalTracksMenu (TracksArea::globalShowBase + laneKey);
    REQUIRE (ui.studio.perform (Cmd::toggleArrangementTrack));
    ui.settle();
    REQUIRE (ruler.lanes().size() == 5);
    CHECK (ruler.lanes()[0].first == laneArrangement);
    CHECK (ruler.lanes()[4].first == laneKey);
    CHECK (ruler.getHeight() == Ruler::rulerHeight + Ruler::arrangementLane + Ruler::markerLane + Ruler::tempoLane + Ruler::signatureLane + Ruler::keyLane);
    CHECK (ruler.laneBounds (laneTempo)->getY() == Ruler::rulerHeight + Ruler::arrangementLane + Ruler::markerLane);

    // Hiding the tempo track; Single shows only the last one clicked; Marker Track Only and back.
    REQUIRE (ui.studio.perform (Cmd::toggleTempoTrack));
    CHECK_FALSE (ruler.laneBounds (laneTempo));
    const auto keyY = static_cast<float> (ruler.laneBounds (laneKey)->getCentreY());
    ruler.pointerDown ({ 5.0f, keyY }, {});
    ruler.pointerUp();
    REQUIRE (ui.studio.perform (Cmd::showSingleGlobalTrack));
    ui.settle();
    REQUIRE (ruler.lanes().size() == 1);
    CHECK (ruler.lanes()[0].first == laneKey);
    REQUIRE (ui.studio.perform (Cmd::showSingleGlobalTrack));
    REQUIRE (ui.studio.perform (Cmd::markerTrackOnly));
    REQUIRE (ruler.lanes().size() == 1);
    CHECK (ruler.lanes()[0].first == laneMarkers);
    REQUIRE (ui.studio.perform (Cmd::markerTrackOnly));
    REQUIRE (ui.studio.perform (Cmd::toggleTempoTrack));
    ui.settle();
    REQUIRE (ruler.lanes().size() == 5);

    // Key track: add a change from the lane's menu, see it in the control bar after it, move it, delete it.
    const auto x = [&] (Ticks t) { return static_cast<float> (ui.studio.view.tickToX (static_cast<double> (t))); };
    const auto laneY = [&] (GlobalLane lane) { return static_cast<float> (ruler.laneBounds (lane)->getCentreY()); };
    answer = "F# minor";
    ruler.pickContext (Ruler::contextEdit, { x (4 * bar) + 1.0f, laneY (laneKey) });
    ui.settle();
    REQUIRE (ui.m().keyChanges.size() == 1);
    const auto change = ui.m().keyChanges.begin()->second->id;
    CHECK (ui.m().keyAt (4 * bar) == model::KeySignature { 6, true });
    ui.studio.locate (5 * bar);
    ui.main->controlBar().refresh();
    CHECK (ui.main->controlBar().keyDisplay().getText().contains ("F"));
    ruler.pointerDown ({ x (4 * bar) + 2.0f, laneY (laneKey) }, {});
    ruler.pointerDrag ({ x (6 * bar) + 2.0f, laneY (laneKey) });
    ruler.pointerUp();
    ui.settle();
    CHECK (ui.m().keyChanges.at (change)->pos == 6 * bar);
    CHECK (ui.studio.session().undoName() == "Move Key Change");

    // Arrangement: add a section from the lane's menu (8 bars), rename it, resize it by its end, select its regions.
    const auto region = ui.studio.session().createMidiRegion (ui.instrumentTracks (1)[0], 2 * bar, bar).value();
    ui.studio.edited();
    ruler.pickContext (Ruler::contextAdd, { x (bar) + 1.0f, laneY (laneArrangement) });
    ui.settle();
    REQUIRE (ui.m().sections.size() == 1);
    const auto section = ui.m().sections.begin()->second->id;
    CHECK (ui.m().sections.at (section)->start == bar);
    CHECK (ui.m().sections.at (section)->length == 8 * bar);
    answer = "Verse";
    ruler.pickContext (Ruler::contextRename, { x (2 * bar), laneY (laneArrangement) });
    CHECK (ui.m().sections.at (section)->name == "Verse");
    ruler.pointerDown ({ x (9 * bar), laneY (laneArrangement) }, {});
    ruler.pointerDrag ({ x (5 * bar), laneY (laneArrangement) });
    ruler.pointerUp();
    ui.settle();
    CHECK (ui.m().sections.at (section)->end() == 5 * bar);
    ruler.pointerDown ({ x (3 * bar), laneY (laneArrangement) }, {});
    ruler.pointerUp();
    CHECK (ui.studio.selection.ids() == std::vector<Id> { region });
    CHECK (ui.studio.playhead() == bar);
    // Add Arrangement Section puts the next one after it.
    REQUIRE (ui.studio.perform (Cmd::addSection));
    REQUIRE (ui.m().sections.size() == 2);
    CHECK (ui.m().sectionsInOrder()[1]->start == 5 * bar);
    CHECK (ui.m().sectionsInOrder()[1]->name == "Verse");
    // Dragging a section drops it at the nearest section edge with its regions (AT-ARR-46); Ctrl copies.
    const auto first = ui.m().sectionsInOrder()[0]->id;
    ruler.pointerDown ({ x (3 * bar), laneY (laneArrangement) }, {});
    ruler.pointerDrag ({ x (12 * bar) + 30.0f, laneY (laneArrangement) });
    CHECK (ruler.sectionDropTarget() == std::optional<Ticks> (13 * bar));
    ruler.pointerUp();
    ui.settle();
    CHECK_FALSE (ruler.sectionDropTarget());
    CHECK (ui.studio.session().undoName() == "Move Section");
    CHECK (ui.m().sections.at (first)->start == 9 * bar);
    CHECK (ui.m().regions.at (region)->start == 10 * bar);
    REQUIRE (ui.studio.session().undo().done);
    ui.studio.edited();
    CHECK (ui.m().regions.at (region)->start == 2 * bar);
    ruler.pointerDown ({ x (3 * bar), laneY (laneArrangement) }, juce::ModifierKeys::commandModifier);
    ruler.pointerDrag ({ x (13 * bar), laneY (laneArrangement) });
    ruler.pointerUp();
    ui.settle();
    CHECK (ui.studio.session().undoName() == "Copy Section");
    CHECK (ui.m().sections.size() == 3);
    CHECK (ui.m().regions.size() == 2);
    REQUIRE (ui.studio.session().undo().done);
    ui.studio.edited();

    // Protect: the lock stops edits there (commands, menus, drags) but not going to a marker.
    const auto marker = ui.studio.session().addMarker (3 * bar, "Hook").value();
    ui.studio.edited();
    ui.studio.toggleLaneProtect (laneMarkers);
    const auto markersBefore = ui.m().markers.size();
    CHECK_FALSE (ui.studio.perform (Cmd::addMarker));
    CHECK (ui.studio.status.contains ("protected"));
    CHECK (ui.m().markers.size() == markersBefore);
    const auto markerMenu = ruler.contextMenu ({ x (3 * bar) + 4.0f, laneY (laneMarkers) });
    for (juce::PopupMenu::MenuItemIterator it (markerMenu); it.next();)
        if (! it.getItem().isSeparator)
            CHECK_FALSE (it.getItem().isEnabled);
    ruler.pointerDown ({ x (3 * bar) + 4.0f, laneY (laneMarkers) }, {});
    ruler.pointerDrag ({ x (7 * bar), laneY (laneMarkers) });
    ruler.pointerUp();
    CHECK (ui.m().markers.at (marker)->pos == 3 * bar);
    CHECK (ui.studio.playhead() == 3 * bar);
    // Clicking the lock beside the name unprotects it.
    const auto lock = area.protectButtonBounds (laneMarkers);
    REQUIRE_FALSE (lock.isEmpty());
    area.mouseDown (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), lock.getCentre().toFloat(), {}, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                      &area, &area, juce::Time::getCurrentTime(), lock.getCentre().toFloat(), juce::Time::getCurrentTime(), 1, false));
    CHECK ((ui.studio.protectedLanes & laneMarkers) == 0);
    CHECK (ui.studio.perform (Cmd::addMarker));

    // Create Time Signature Change asks at the next bar; Create Key Change at the playhead.
    ui.studio.locate (2 * bar + 960);
    answer = "3/4";
    REQUIRE (ui.studio.perform (Cmd::createSignatureChange));
    CHECK (ui.m().signatures.signatureAtBar (3).numerator == 3);
    CHECK (ui.m().signatures.signatureAtBar (2).numerator == 4);
    answer = "Eb";
    REQUIRE (ui.studio.perform (Cmd::createKeyChange));
    CHECK (ui.m().keyAt (2 * bar + 960) == model::KeySignature { 3, false });

    // The choice of tracks survives a restart.
    const auto again = PanelLayout::fromString (ui.studio.layout.toString());
    CHECK (again.globalLanes == allGlobalLanes);
}

TEST_CASE ("AT-REC-27 Record Toggle, Replace and Capture as Recording in Navigate and the control bar; MIDI auto record enable", "[UI][AT-REC-27]")
{
    Ui ui;
    juce::StringArray items;
    const auto menu = ui.main->getMenuForIndex (4, "Navigate");
    bool replaceTicked = true;
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        if (! it.getItem().isSeparator)
        {
            items.add (it.getItem().text);
            if (it.getItem().text == "Replace")
                replaceTicked = it.getItem().isTicked;
        }
    CHECK (items.contains ("Record Toggle"));
    CHECK (items.contains ("Capture as Recording"));
    CHECK (items.contains ("Replace"));
    CHECK_FALSE (replaceTicked);
    CHECK (commandInfo (Cmd::recordToggle).shortcut == "Alt+R");
    CHECK (commandInfo (Cmd::captureRecording).shortcut == "Ctrl+Shift+R");

    // Replace: the command, the control bar button and the MIDI Recording item are one setting.
    auto& bar = ui.main->controlBar();
    CHECK (ui.studio.perform (Cmd::toggleReplace));
    CHECK (ui.studio.engine().settings().replace);
    bar.refresh();
    CommandButton* replaceButton = nullptr;
    for (auto* b : bar.commandButtons())
        if (b->command == Cmd::toggleReplace)
            replaceButton = b;
    REQUIRE (replaceButton != nullptr);
    CHECK (replaceButton->getToggleState());
    ui.main->menuItemSelected (MainComponent::replaceMidi, 4);
    CHECK_FALSE (ui.studio.engine().settings().replace);

    // Capture with nothing played says so.
    const auto keys = ui.instrumentTracks (2);
    ui.studio.selectTrack (keys[0]);
    CHECK_FALSE (ui.studio.perform (Cmd::captureRecording));
    CHECK (ui.studio.status.startsWith ("Nothing to capture"));

    // MIDI auto record enable: selecting an instrument track arms it and disarms the last one armed that way.
    auto p = ui.studio.preferences();
    p.autoArmMidi = true;
    ui.studio.setPreferences (p);
    SettingsDialog dialog (ui.studio);
    CHECK (dialog.autoArmMidi.getToggleState());
    ui.studio.selectTrack (keys[1]);
    CHECK (ui.studio.localView().armed.contains (keys[1]));
    ui.studio.selectTrack (keys[0]);
    CHECK (ui.studio.localView().armed.contains (keys[0]));
    CHECK_FALSE (ui.studio.localView().armed.contains (keys[1]));
    CHECK (Preferences::fromString (ui.studio.preferences().toString()).autoArmMidi);
}
