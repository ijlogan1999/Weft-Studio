// AT-AUT-1 (automation view, selectors, "+") and AT-AUT-3 (drawing and editing points), plus
// the mixer's controls following and writing automation (F-AUT-6).

#include "UiFixture.h"

using namespace weft;
using namespace weft::ui;
using namespace weft::ui::test;

namespace
{
model::AutomationTarget target (const char* kind)
{
    model::AutomationTarget t;
    t.kind = kind;
    return t;
}

juce::ModifierKeys shift() { return juce::ModifierKeys (juce::ModifierKeys::shiftModifier | juce::ModifierKeys::leftButtonModifier); }
juce::ModifierKeys altKey() { return juce::ModifierKeys (juce::ModifierKeys::altModifier | juce::ModifierKeys::leftButtonModifier); }

void drag (Ui& ui, juce::Point<float> from, juce::Point<float> to, juce::ModifierKeys mods = {})
{
    ui.timeline().pointerDown (from, mods);
    const auto mid = from + (to - from) * 0.5f;
    ui.timeline().pointerDrag (mid, mods);
    ui.timeline().pointerDrag (to, mods);
    ui.timeline().pointerUp (to, mods);
    ui.settle();
}

/** Screen position of a lane value at a tick in the first track's first automation lane. */
juce::Point<float> at (Ui& ui, Ticks tick, double value, std::size_t lane = 0)
{
    const auto box = ui.timeline().automationBounds (0, lane);
    const float pad = 6.0f;
    return { static_cast<float> (ui.studio.view.tickToX (static_cast<double> (tick))),
             box.getY() + pad + static_cast<float> (1.0 - value) * (box.getHeight() - 2 * pad) };
}

std::vector<model::AutoPoint> points (Ui& ui, const Id& track, const char* kind = "volume")
{
    const auto* lane = ui.m().lane (track, target (kind));
    return lane != nullptr ? automation::sortedPoints (*lane) : std::vector<model::AutoPoint> {};
}
} // namespace

TEST_CASE ("AT-AUT-1 A toggles automation view; headers show the parameter and mode selectors; + adds a lane", "[UI]")
{
    Ui ui;
    const auto track = ui.m().trackOrder.front();
    auto& header = *ui.main->tracks().headers().front();
    const auto plain = header.getHeight();
    CHECK_FALSE (ui.studio.automationView);
    CHECK_FALSE (header.mode.isVisible());

    CHECK (ui.key ('A', {}, 'a'));
    CHECK (ui.studio.automationView);
    CHECK (header.mode.isVisible());
    CHECK (header.mode.getText() == "Read");
    REQUIRE (header.parameters.size() == 1);
    CHECK (header.parameters[0]->getText() == "Volume");
    CHECK (header.getHeight() == plain + Studio::automationLaneHeight);
    // The parameter list (AT-AUT-2 on screen): Volume, Pan, Mute.
    CHECK (header.parameters[0]->getItemText (1) == "Pan");
    CHECK (header.parameters[0]->getItemText (2) == "Mute");

    // "+" adds another lane, showing the next parameter.
    header.addLane[0]->onClick();
    ui.settle();
    REQUIRE (header.parameters.size() == 2);
    CHECK (header.parameters[1]->getText() == "Pan");
    CHECK (header.getHeight() == plain + 2 * Studio::automationLaneHeight);

    // The parameter selector changes what a lane shows; the mode selector sets the track's mode.
    header.parameters[1]->setSelectedItemIndex (2, juce::sendNotificationSync);
    ui.settle();
    CHECK (ui.studio.shownLanes (track)[1].kind == "mute");
    header.mode.setSelectedItemIndex (2, juce::sendNotificationSync); // Touch
    ui.settle();
    CHECK (ui.m().track (track)->automationMode == "touch");

    CHECK (ui.key ('A', {}, 'a'));
    CHECK_FALSE (ui.studio.automationView);
    CHECK_FALSE (header.mode.isVisible());
    CHECK (header.getHeight() == plain);
}

TEST_CASE ("AT-AUT-3 clicking adds a snapped point, dragging moves it, Shift constrains, rubber-band and Delete remove", "[UI]")
{
    Ui ui;
    const auto track = ui.m().trackOrder.front();
    ui.studio.snap = arrange::Snap::ticks (960);
    ui.studio.snapEnabled = true;
    ui.studio.view.pixelsPerTick = 0.1;
    ui.studio.view.originTick = 0;
    ui.main->run (Cmd::toggleAutomation);
    ui.settle();

    // Click: a point at the snapped time and the clicked value.
    ui.click (at (ui, 1000, 0.25));
    auto p = points (ui, track);
    REQUIRE (p.size() == 1);
    CHECK (p[0].pos == 960);
    CHECK (std::abs (p[0].value - 0.25) < 0.03);
    CHECK (ui.studio.pointSelection.points.contains (p[0].id));
    ui.click (at (ui, 3840, 0.75));
    ui.click (at (ui, 7680, 0.5));
    REQUIRE (points (ui, track).size() == 3);
    CHECK (ui.studio.session().undoName() == "Add Automation Point");

    // Drag: time (snapped) and value.
    drag (ui, at (ui, 3840, 0.75), at (ui, 4900, 0.6));
    p = points (ui, track);
    CHECK (p[1].pos == 4800);
    CHECK (std::abs (p[1].value - 0.6) < 0.03);
    CHECK (ui.studio.session().undoName() == "Move Automation Point");

    // Shift-drag constrains to the larger axis: mostly sideways keeps the value.
    drag (ui, at (ui, 4800, p[1].value), at (ui, 5800, p[1].value + 0.1), shift());
    const auto before = p[1].value;
    p = points (ui, track);
    CHECK (p[1].pos == 5760);
    CHECK (std::abs (p[1].value - before) < 1e-9);
    // Mostly up keeps the time.
    drag (ui, at (ui, 5760, before), at (ui, 5800, before + 0.2), shift());
    p = points (ui, track);
    CHECK (p[1].pos == 5760);
    CHECK (p[1].value > before + 0.1);

    // Alt+drag on a segment bends it.
    drag (ui, at (ui, 2500, 0.5), at (ui, 2500, 0.2), altKey());
    p = points (ui, track);
    CHECK (std::abs (p[0].curve) > 0.1);
    CHECK (ui.studio.session().undoName() == "Bend Automation");

    // Rubber band around the last two points, then Delete: one undo step.
    const auto box = ui.timeline().automationBounds (0, 0);
    drag (ui, { static_cast<float> (ui.studio.view.tickToX (5000.0)), box.getY() + 2 },
          { static_cast<float> (ui.studio.view.tickToX (8000.0)), box.getBottom() - 2 });
    CHECK (ui.studio.pointSelection.points.size() == 2);
    ui.main->run (Cmd::deleteSelection);
    ui.settle();
    REQUIRE (points (ui, track).size() == 1);
    CHECK (points (ui, track)[0].pos == 960);
    ui.main->run (Cmd::undo);
    ui.settle();
    CHECK (points (ui, track).size() == 3);
}

TEST_CASE ("The mixer fader follows automation and, in Touch, a gesture writes it", "[UI]")
{
    Ui ui;
    const auto track = ui.m().trackOrder.front();
    auto& s = ui.studio.session();
    REQUIRE (s.addAutomationPoint (track, target ("volume"), 0, automation::fromDb (-12.0)));
    REQUIRE (s.setTrackAutomationMode (track, "touch"));
    ui.studio.edited();
    ui.main->run (Cmd::toggleMixer);
    ui.settle();
    auto* strip = ui.main->bottom().mixerView().stripFor (track);
    REQUIRE (strip != nullptr);

    // The fader's travel follows the taper: 0 dB at about 79 %.
    CHECK (std::abs (strip->volume.valueToProportionOfLength (0.0) - automation::fromDb (0.0)) < 1e-6);

    ui.main->run (Cmd::playStop);
    ui.renderAudio (4800);
    ui.studio.tick(); // the window's timer
    strip->followAutomation();
    CHECK (std::abs (strip->volume.getValue() - -12.0) < 0.05);

    // A gesture while playing goes to the live writer, not the static volume.
    REQUIRE (ui.studio.automationLive (track, target ("volume")));
    strip->volume.onDragStart();
    strip->volume.setValue (-3.0, juce::sendNotificationSync);
    ui.renderAudio (9600);
    ui.studio.tick();
    strip->volume.setValue (-4.0, juce::sendNotificationSync);
    ui.renderAudio (9600);
    ui.studio.tick();
    strip->volume.onDragEnd();
    CHECK (std::abs (ui.m().track (track)->strip.volumeDb) < 1e-9); // static value untouched
    ui.renderAudio (4800);
    ui.studio.tick();
    ui.main->run (Cmd::playStop);
    ui.renderAudio (512);
    ui.studio.tick();
    ui.settle();
    const auto written = points (ui, track);
    REQUIRE (written.size() >= 3);
    CHECK (ui.studio.session().undoName() == "Write Automation");
    bool wroteMinus4 = false;
    for (const auto& p : written)
        wroteMinus4 = wroteMinus4 || std::abs (automation::toDb (p.value) - -4.0) < 0.01;
    CHECK (wroteMinus4);
}

// Run with: weft_ui_tests "[.screenshot-automation]" (writes WEFT_SCREENSHOT or ./automation.png).
TEST_CASE ("Screenshot of automation lanes", "[.screenshot-automation]")
{
    Ui ui;
    auto& s = ui.studio.session();
    const auto guitar = ui.m().trackOrder.front();
    s.renameTrack (guitar, "Guitar");
    const auto keys = s.createTrack (model::TrackKind::instrument, "Keys").value();
    REQUIRE (s.createMidiRegion (keys, 0, 8 * bar));
    model::PluginInstance delay;
    delay.pluginUid = "weft.delay";
    delay.name = "Weft Delay";
    REQUIRE (s.addInsert (keys, delay));
    ui.studio.edited();
    ui.settle();
    const std::vector<std::pair<Ticks, double>> fade { { 0, 0.79 }, { 2 * bar, 0.79 }, { 4 * bar, 0.45 }, { 6 * bar, 0.7 }, { 8 * bar, 0.79 } };
    for (const auto& [pos, value] : fade)
        REQUIRE (s.addAutomationPoint (guitar, target ("volume"), pos, value, pos == 2 * bar ? 0.6 : 0.0));
    for (int i = 0; i <= 16; ++i)
        REQUIRE (s.addAutomationPoint (guitar, target ("pan"), i * bar / 2, i % 2 == 0 ? 0.3 : 0.7, i % 2 == 0 ? -0.5 : 0.5));
    REQUIRE (s.addAutomationPoint (keys, target ("volume"), 0, 0.6));
    REQUIRE (s.addAutomationPoint (keys, target ("volume"), 3 * bar, 0.75));
    REQUIRE (s.setTrackAutomationMode (keys, "touch"));
    ui.studio.view.pixelsPerTick = 0.06;
    ui.studio.view.originTick = 0;
    ui.main->run (Cmd::toggleAutomation);
    ui.studio.addShownLane (guitar);
    ui.studio.edited();
    ui.settle();
    const auto image = ui.main->createComponentSnapshot (ui.main->getLocalBounds());
    const auto* env = std::getenv ("WEFT_SCREENSHOT");
    juce::File out (env != nullptr ? juce::String (env) : juce::File::getCurrentWorkingDirectory().getChildFile ("automation.png").getFullPathName());
    out.deleteFile();
    juce::FileOutputStream stream (out);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
}
