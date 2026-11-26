// AT-MIX-1, 2, 4, 8, 9 (mixer strips) and AT-PLG-4, 5 (plug-in manager and generic editor).

#include "UiFixture.h"

#include "ui/PluginSearch.h"

using namespace weft;
using namespace weft::ui;
using namespace weft::ui::test;

namespace
{
struct MenuEntry
{
    int id = 0;
    juce::String text;
    bool enabled = false, ticked = false;
};

/** Every item of a menu and its submenus. */
std::vector<MenuEntry> entries (const juce::PopupMenu& menu)
{
    std::vector<MenuEntry> out;
    for (juce::PopupMenu::MenuItemIterator it (menu, true); it.next();)
    {
        const auto& item = it.getItem();
        if (item.itemID != 0)
            out.push_back ({ item.itemID, item.text, item.isEnabled, item.isTicked });
    }
    return out;
}

std::optional<MenuEntry> entry (const juce::PopupMenu& menu, const juce::String& textStart)
{
    for (const auto& e : entries (menu))
        if (e.text.startsWith (textStart))
            return e;
    return std::nullopt;
}

MixerView& showMixer (Ui& ui)
{
    if (ui.studio.layout.bottom != BottomTab::mixer)
        ui.main->run (Cmd::toggleMixer);
    ui.settle();
    return ui.main->bottom().mixerView();
}

juce::ModifierKeys alt() { return juce::ModifierKeys (juce::ModifierKeys::altModifier | juce::ModifierKeys::leftButtonModifier); }

/** A fresh plug-in list (the settings folder is shared by the run). */
void forgetPlugins()
{
    std::error_code ec;
    std::filesystem::remove (Studio::settingsFolder() / "plugins.json", ec);
}

std::filesystem::path installFixture (const std::filesystem::path& folder, const char* artefact)
{
    const std::filesystem::path from (artefact);
    const auto to = folder / from.filename();
    std::filesystem::copy (from, to, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
    return to;
}
} // namespace

TEST_CASE ("AT-MIX-1 strips in track order, then buses, then Master pinned; selecting a strip selects its track", "[UI]")
{
    Ui ui;
    auto& s = ui.studio.session();
    const auto audio = ui.m().trackOrder.front();
    const auto keys = s.createTrack (model::TrackKind::instrument, "Keys").value();
    const auto bus = s.createTrack (model::TrackKind::bus, "Verb").value();
    const auto late = s.createTrack (model::TrackKind::audio, "Late").value();
    ui.studio.edited();
    auto& mixer = showMixer (ui);

    REQUIRE (mixer.strips().size() == 4);
    CHECK (mixer.strips()[0]->trackId() == audio);
    CHECK (mixer.strips()[1]->trackId() == keys);
    CHECK (mixer.strips()[2]->trackId() == late); // tracks first, in track order
    CHECK (mixer.strips()[3]->trackId() == bus);  // then buses
    CHECK (mixer.strips()[1]->name.getText() == "Keys");
    for (std::size_t i = 1; i < mixer.strips().size(); ++i)
        CHECK (mixer.strips()[i]->getX() > mixer.strips()[i - 1]->getX());

    // Master is outside the scrolling strips, at the right edge.
    auto& master = mixer.masterStrip();
    CHECK (master.trackId() == ui.m().info.masterTrackId);
    CHECK (master.getParentComponent() == &mixer);
    CHECK (master.getRight() == mixer.getWidth());
    CHECK (master.name.getText() == "Master");
    CHECK_FALSE (master.output.isVisible());

    // Selecting a strip selects its track, and the strip shows the track's selection.
    auto& strip = *mixer.strips()[1];
    strip.mouseDown (mouse (strip, { 50.0f, 6.0f }));
    ui.settle();
    CHECK (ui.studio.selectedTrack() == keys);

    // New tracks get strips.
    s.createTrack (model::TrackKind::audio, "Another");
    ui.studio.edited();
    ui.settle();
    CHECK (mixer.strips().size() == 5);
}

TEST_CASE ("AT-MIX-2 fader and pan controls: Alt+click resets, typed levels, -inf", "[UI]")
{
    Ui ui;
    auto& mixer = showMixer (ui);
    const auto id = ui.m().trackOrder.front();
    auto& strip = *mixer.stripFor (id);
    auto& s = ui.studio.session();

    REQUIRE (s.setTrackVolume (id, -10.0));
    REQUIRE (s.setTrackPan (id, 0.5));
    ui.studio.edited();
    ui.settle();
    CHECK (strip.volume.getValue() == Catch::Approx (-10.0));
    CHECK (strip.readout.getText() == "-10.0");
    CHECK (strip.pan.getValue() == Catch::Approx (50.0));

    strip.volume.mouseDown (mouse (strip.volume, strip.volume.getLocalBounds().getCentre().toFloat(), alt()));
    strip.volume.mouseUp (mouse (strip.volume, strip.volume.getLocalBounds().getCentre().toFloat(), alt()));
    ui.settle();
    CHECK (ui.m().track (id)->strip.volumeDb == Catch::Approx (0.0));
    strip.pan.mouseDown (mouse (strip.pan, strip.pan.getLocalBounds().getCentre().toFloat(), alt()));
    strip.pan.mouseUp (mouse (strip.pan, strip.pan.getLocalBounds().getCentre().toFloat(), alt()));
    ui.settle();
    CHECK (ui.m().track (id)->strip.pan == Catch::Approx (0.0));

    strip.readout.setText ("-6.02 dB", juce::sendNotificationSync);
    ui.settle();
    CHECK (ui.m().track (id)->strip.volumeDb == Catch::Approx (-6.02));
    strip.readout.setText ("12", juce::sendNotificationSync); // clamped to the +6 dB maximum
    ui.settle();
    CHECK (ui.m().track (id)->strip.volumeDb == Catch::Approx (6.0));
    strip.readout.setText ("-inf", juce::sendNotificationSync);
    ui.settle();
    CHECK (ui.m().track (id)->strip.volumeDb < -150.0);
    CHECK (strip.readout.getText() == "-inf");

    // The fader at the bottom of its travel is -inf.
    strip.volume.setValue (fader::minDb, juce::sendNotificationSync);
    ui.settle();
    CHECK (ui.m().track (id)->strip.volumeDb < -150.0);
}

TEST_CASE ("D-UI-15 faders reset on double-click and show their value while dragged", "[U][UI]")
{
    Ui ui;
    auto& mixer = showMixer (ui);
    const auto id = ui.m().trackOrder.front();
    auto& strip = *mixer.stripFor (id);
    REQUIRE (ui.studio.session().setTrackVolume (id, -12.0));
    ui.studio.edited();
    ui.settle();
    const auto centre = strip.volume.getLocalBounds().getCentre().toFloat();
    strip.volume.mouseDoubleClick (mouse (strip.volume, centre, {}, 2));
    ui.settle();
    CHECK (ui.m().track (id)->strip.volumeDb == Catch::Approx (0.0));

    // The meter and the fader share one scale: 0 dB on the meter is level with 0 dB on the fader.
    const auto travel = strip.volume.getBounds().toFloat().reduced (0.0f, size::faderCap / 2.0f);
    const auto faderZero = travel.getBottom() - travel.getHeight() * static_cast<float> (strip.volume.valueToProportionOfLength (0.0));
    const auto bars = strip.meter.barArea().translated (static_cast<float> (strip.meter.getX()), static_cast<float> (strip.meter.getY())).reduced (2.0f, 0.0f);
    const auto meterZero = bars.getBottom() - bars.getHeight() * MeterView::proportion (0.0f);
    CHECK (meterZero == Catch::Approx (faderZero).margin (1.0));

    // RMS sits under the peak.
    strip.meter.update (0.5f, 0.2f, false, 1.0);
    CHECK (strip.meter.rmsDb == Catch::Approx (-13.98).margin (0.05));
    CHECK (strip.meter.rmsDb < strip.meter.ballistics.levelDb());
}

TEST_CASE ("AT-MIX-4 the clip light stays lit until clicked", "[UI]")
{
    Ui ui;
    auto& mixer = showMixer (ui);
    auto& meter = mixer.masterStrip().meter;
    meter.update (1.1f, true, 1.0);
    meter.update (0.0f, false, 1.5); // later quiet blocks don't clear it
    CHECK (meter.ballistics.clip());
    CHECK (meter.ballistics.holdDb() > 0.5f);
    meter.mouseDown (mouse (meter, { 5.0f, 3.0f }));
    CHECK_FALSE (meter.ballistics.clip());
}

TEST_CASE ("AT-MIX-8 New Bus from a send; removing the bus asks first", "[UI]")
{
    Ui ui;
    auto& mixer = showMixer (ui);
    auto& s = ui.studio.session();
    const auto audio = ui.m().trackOrder.front();
    const auto keys = s.createTrack (model::TrackKind::instrument, "Keys").value();
    ui.studio.edited();
    ui.settle();

    auto* strip = mixer.stripFor (audio);
    REQUIRE (strip->sends.size() == 1); // the free slot
    const auto newBus = entry (strip->sendMenu (0), "New Bus");
    REQUIRE (newBus);
    strip->pickSend (0, newBus->id);
    ui.settle();

    const auto& m = ui.m();
    Id bus;
    for (const auto& tid : m.trackOrder)
        if (m.track (tid)->kind == model::TrackKind::bus)
            bus = tid;
    REQUIRE_FALSE (bus.empty());
    CHECK (m.track (bus)->name == "Bus 1");
    CHECK (m.track (bus)->output.busId.empty());
    CHECK (m.track (bus)->strip.volumeDb == Catch::Approx (0.0));
    REQUIRE (m.track (audio)->strip.sends.size() == 1);
    CHECK (m.track (audio)->strip.sends[0].levelDb == Catch::Approx (0.0));
    CHECK (mixer.stripFor (bus) != nullptr);                             // a bus strip
    const auto& headers = ui.main->tracks().headers(); // and a bus track in the tracks area
    CHECK (std::any_of (headers.begin(), headers.end(), [&] (const auto& h) { return h->trackId == bus; }));
    strip = mixer.stripFor (audio);
    REQUIRE (strip->sends.size() == 2);
    CHECK (strip->sends[0]->target.getButtonText() == "Bus 1");

    // The send's menu: Pre/Post and mute.
    const auto pre = entry (strip->sendMenu (0), "Pre Fader");
    REQUIRE (pre);
    strip->pickSend (0, pre->id);
    ui.settle();
    CHECK (ui.m().track (audio)->strip.sends[0].pre);

    // Keys outputs to the bus.
    auto* keysStrip = mixer.stripFor (keys);
    const auto toBus = entry (keysStrip->outputMenu(), "Bus 1");
    REQUIRE (toBus);
    keysStrip->pickOutput (toBus->id);
    ui.settle();
    CHECK (ui.m().track (keys)->output.busId == bus);
    CHECK (mixer.stripFor (keys)->output.getButtonText() == "Bus 1");

    // Removing it asks, then removes the send and sends Keys to Master.
    ui.studio.selectTrack (bus);
    ui.main->run (Cmd::deleteTrack);
    ui.settle();
    REQUIRE (ui.main->questionPending());
    CHECK (ui.main->questionText() == "Remove Bus 1 and 1 send?");
    ui.main->answer (false);
    CHECK (ui.m().track (bus) != nullptr);

    ui.main->run (Cmd::deleteTrack);
    ui.settle();
    ui.main->answer (true);
    ui.settle();
    CHECK (ui.m().track (bus) == nullptr);
    CHECK (ui.m().track (audio)->strip.sends.empty());
    CHECK (ui.m().track (keys)->output.busId.empty());
    CHECK (mixer.stripFor (bus) == nullptr);
}

TEST_CASE ("AT-MIX-9 outputs and sends that would loop are disabled in the menus", "[UI]")
{
    Ui ui;
    auto& mixer = showMixer (ui);
    auto& s = ui.studio.session();
    const auto a = ui.m().trackOrder.front();
    const auto bus1 = s.createTrack (model::TrackKind::bus, "Bus 1").value();
    const auto bus2 = s.createTrack (model::TrackKind::bus, "Bus 2").value();
    REQUIRE (s.setTrackOutput (a, bus1));
    REQUIRE (s.setTrackOutput (bus1, bus2)); // A -> Bus 1 -> Bus 2 is fine
    ui.studio.edited();
    ui.settle();

    auto* strip2 = mixer.stripFor (bus2);
    const auto out = entry (strip2->outputMenu(), "Bus 1");
    REQUIRE (out);
    CHECK_FALSE (out->enabled);
    CHECK (out->text.contains ("Would create a feedback loop"));
    const auto send = entry (strip2->sendMenu (0), "Bus 1");
    REQUIRE (send);
    CHECK_FALSE (send->enabled);
    CHECK (send->text.contains ("Would create a feedback loop"));
    // Picking it anyway changes nothing.
    strip2->pickOutput (out->id);
    ui.settle();
    CHECK (ui.m().track (bus2)->output.busId.empty());

    // From track A, Bus 2 is a fine send target.
    const auto fine = entry (mixer.stripFor (a)->sendMenu (0), "Bus 2");
    REQUIRE (fine);
    CHECK (fine->enabled);
}

TEST_CASE ("Insert slots: add opens the editor, bypass, reorder, remove, placeholders", "[UI]")
{
    Ui ui;
    auto& mixer = showMixer (ui);
    const auto id = ui.m().trackOrder.front();
    auto* strip = mixer.stripFor (id);
    REQUIRE (strip->inserts.size() == 1);
    const auto menu = entries (strip->insertMenu (0));
    REQUIRE (menu.size() >= 5);
    CHECK (menu.front().text == "Weft EQ"); // built-ins first
    const auto eq = entry (strip->insertMenu (0), "Weft EQ");
    strip->pickInsert (0, eq->id);
    ui.settle();
    const auto& inserts = ui.m().track (id)->strip.inserts;
    REQUIRE (inserts.size() == 1);
    CHECK (inserts[0].pluginUid == "weft.eq");
    CHECK (ui.main->pluginWindows().find (inserts[0].id) != nullptr); // W-6 step 2: its editor opens

    strip = mixer.stripFor (id);
    const auto delay = entry (strip->insertMenu (1), "Weft Delay");
    strip->pickInsert (1, delay->id);
    ui.settle();
    REQUIRE (ui.m().track (id)->strip.inserts.size() == 2);
    strip = mixer.stripFor (id);
    REQUIRE (strip->inserts.size() == 3);
    CHECK (strip->inserts[1]->label == "Weft Delay");

    // Bypass from the left edge.
    auto& slot = *strip->inserts[0];
    slot.mouseDown (mouse (slot, { 4.0f, 8.0f }));
    ui.settle();
    CHECK (ui.m().track (id)->strip.inserts[0].bypass);

    // Drag the delay above the EQ.
    strip = mixer.stripFor (id);
    auto& delaySlot = *strip->inserts[1];
    const auto target = strip->inserts[0]->getBounds().getCentre().toFloat() - delaySlot.getPosition().toFloat();
    const auto start = juce::Point<float> (40.0f, 8.0f);
    delaySlot.mouseDown (mouse (delaySlot, start));
    delaySlot.mouseDrag (mouse (delaySlot, target, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 1, start));
    delaySlot.mouseUp (mouse (delaySlot, target, {}, 1, start));
    ui.settle();
    CHECK (ui.m().track (id)->strip.inserts[0].pluginUid == "weft.delay");

    // Remove.
    strip = mixer.stripFor (id);
    const auto remove = entry (strip->insertMenu (0), "Remove");
    strip->pickInsert (0, remove->id);
    ui.settle();
    REQUIRE (ui.m().track (id)->strip.inserts.size() == 1);
    CHECK (ui.m().track (id)->strip.inserts[0].pluginUid == "weft.eq");

    // A plug-in that isn't installed shows a warning and keeps its settings (F-PLG-15).
    model::PluginInstance missing;
    missing.format = "vst3";
    missing.pluginUid = "0123456789ABCDEF0123456789ABCDEF";
    missing.name = "Faraway Chorus";
    missing.state = doc::Value (std::vector<std::uint8_t> { 1, 2, 3 });
    REQUIRE (ui.studio.session().addInsert (id, missing));
    ui.studio.edited();
    ui.settle();
    strip = mixer.stripFor (id);
    REQUIRE (strip->inserts.size() == 3);
    CHECK (strip->inserts[1]->placeholder);
    CHECK (strip->inserts[1]->getTooltip().contains ("Not installed on this computer"));
    const auto missingId = ui.m().track (id)->strip.inserts[1].id;
    strip->openEditor (missingId);
    auto* window = ui.main->pluginWindows().find (missingId);
    REQUIRE (window != nullptr);
    REQUIRE (window->generic() != nullptr);
    CHECK (window->generic()->rows.empty());
    CHECK (window->generic()->message.getText().contains ("Not installed on this computer"));
    ui.main->pluginWindows().close (missingId);
    CHECK (ui.m().track (id)->strip.inserts[1].state == doc::Value (std::vector<std::uint8_t> { 1, 2, 3 }));
}

TEST_CASE ("AT-MIX-15 a slot dragged to another strip moves the plug-in, Ctrl copies it, Alt+click removes it", "[UI][AT-MIX-15]")
{
    Ui ui;
    const auto first = ui.m().trackOrder.front();
    const auto second = ui.studio.session().createTrack (model::TrackKind::audio, "Second").value();
    ui.studio.edited();
    auto& mixer = showMixer (ui);
    auto* strip = mixer.stripFor (first);
    const auto eq = entry (strip->insertMenu (0), "Weft EQ");
    REQUIRE (eq);
    strip->pickInsert (0, eq->id);
    ui.settle();
    REQUIRE (ui.m().track (first)->strip.inserts.size() == 1);
    const auto pluginId = ui.m().track (first)->strip.inserts[0].id;
    REQUIRE (ui.main->pluginWindows().find (pluginId) != nullptr);

    // Drags a slot of one strip onto a slot of another, as the mouse would.
    const auto dragSlot = [&] (const Id& from, std::size_t slot, const Id& to, std::size_t toSlot, juce::ModifierKeys mods) {
        auto& source = *mixer.stripFor (from)->inserts[slot];
        auto& target = *mixer.stripFor (to)->inserts[toSlot];
        const auto start = juce::Point<float> (40.0f, 8.0f);
        const auto end = source.getLocalPoint (&target, target.getLocalBounds().getCentre()).toFloat();
        source.mouseDown (mouse (source, start));
        source.mouseDrag (mouse (source, end, mods.withFlags (juce::ModifierKeys::leftButtonModifier), 1, start));
        source.mouseUp (mouse (source, end, mods, 1, start));
        ui.settle();
    };

    dragSlot (first, 0, second, 0, {});
    CHECK (ui.m().track (first)->strip.inserts.empty());
    REQUIRE (ui.m().track (second)->strip.inserts.size() == 1);
    CHECK (ui.m().track (second)->strip.inserts[0].id == pluginId); // the same instance: its window stays open
    CHECK (ui.main->pluginWindows().find (pluginId) != nullptr);
    CHECK (ui.studio.session().undoName() == "Move Plug-in");

    // Ctrl+drag back: the original stays and the first strip gets a copy.
    dragSlot (second, 0, first, 0, juce::ModifierKeys::commandModifier);
    REQUIRE (ui.m().track (second)->strip.inserts.size() == 1);
    REQUIRE (ui.m().track (first)->strip.inserts.size() == 1);
    CHECK (ui.m().track (first)->strip.inserts[0].id != pluginId);
    CHECK (ui.m().track (first)->strip.inserts[0].pluginUid == "weft.eq");
    CHECK (ui.studio.session().undoName() == "Copy Plug-in");

    // Alt+click takes a plug-in out; the click doesn't open its editor.
    const auto copyId = ui.m().track (first)->strip.inserts[0].id;
    ui.main->pluginWindows().close (copyId);
    auto& slot = *mixer.stripFor (first)->inserts[0];
    slot.mouseDown (mouse (slot, { 40.0f, 8.0f }, juce::ModifierKeys (juce::ModifierKeys::altModifier | juce::ModifierKeys::leftButtonModifier)));
    slot.mouseUp (mouse (slot, { 40.0f, 8.0f }, juce::ModifierKeys::altModifier));
    ui.settle();
    CHECK (ui.m().track (first)->strip.inserts.empty());
    CHECK (ui.main->pluginWindows().find (copyId) == nullptr);
    CHECK (ui.studio.session().undoName() == "Remove Plug-in");
}

TEST_CASE ("AT-PLG-5 a plug-in without an editor gets sliders that change its parameters", "[UI]")
{
    forgetPlugins();
    Ui ui;
    const auto folder = ui.folder.path / "vst3";
    std::filesystem::create_directories (folder);
    installFixture (folder, WEFT_FIXTURE_TESTGAIN);
    ui.studio.pluginFolders = { folder };
    ui.studio.scannerPath = WEFT_SCANNER;
    ui.studio.rescanPlugins();

    auto& mixer = showMixer (ui);
    const auto id = ui.m().trackOrder.front();
    auto* strip = mixer.stripFor (id);
    const auto gain = entry (strip->insertMenu (0), "TestGain");
    REQUIRE (gain);
    strip->pickInsert (0, gain->id);
    ui.settle();
    REQUIRE (ui.m().track (id)->strip.inserts.size() == 1);
    const auto pluginId = ui.m().track (id)->strip.inserts[0].id;
    CHECK (ui.m().track (id)->strip.inserts[0].format == "vst3");

    auto* window = ui.main->pluginWindows().find (pluginId);
    REQUIRE (window != nullptr);
    auto* editor = window->generic();
    REQUIRE (editor != nullptr);
    const auto proc = ui.studio.processor (pluginId);
    REQUIRE (proc != nullptr);
    REQUIRE (editor->rows.size() == proc->parameters().size());
    std::size_t g = 0;
    while (g < editor->rows.size() && editor->rows[g].label->getText() != "Gain")
        ++g;
    REQUIRE (g < editor->rows.size());
    const auto before = proc->parameter (static_cast<int> (g));

    editor->rows[g].slider->setValue (0.8, juce::sendNotificationSync);
    CHECK (proc->parameter (static_cast<int> (g)) == Catch::Approx (0.8).margin (1e-4));
    window->pollParameters();
    ui.settle();
    const auto paramId = proc->parameters()[g].id;
    const auto& params = ui.m().track (id)->strip.inserts[0].params;
    REQUIRE (params.contains (paramId));
    CHECK (params.at (paramId) == Catch::Approx (0.8).margin (1e-4));

    // One undo step takes it back, in the model and the plug-in.
    ui.main->run (Cmd::undo);
    ui.settle();
    CHECK_FALSE (ui.m().track (id)->strip.inserts[0].params.contains (paramId));
    CHECK (proc->parameter (static_cast<int> (g)) == Catch::Approx (before).margin (1e-4));
    window->pollParameters(); // the undo isn't reported back as an edit
    ui.settle();
    CHECK_FALSE (ui.m().track (id)->strip.inserts[0].params.contains (paramId));

    // The window's Bypass button is the insert's bypass (AT-PLG-12, D-UI-24), and follows it when it changes elsewhere.
    auto* frame = window->frame();
    REQUIRE (frame != nullptr);
    REQUIRE (frame->bypass.isVisible());
    CHECK_FALSE (frame->bypass.getToggleState());
    CHECK (editor->getY() == PluginFrame::headerHeight);
    CHECK (frame->getHeight() == editor->getHeight() + PluginFrame::headerHeight);
    frame->bypass.onClick();
    ui.settle();
    CHECK (ui.m().track (id)->strip.inserts[0].bypass);
    CHECK (frame->bypass.getToggleState());
    strip->pickInsert (0, ChannelStrip::itemBypass);
    ui.settle();
    frame->refresh();
    CHECK_FALSE (ui.m().track (id)->strip.inserts[0].bypass);
    CHECK_FALSE (frame->bypass.getToggleState());

    // Closing the window writes the state chunk (D-DM-5).
    editor->rows[g].slider->setValue (0.3, juce::sendNotificationSync);
    window->pollParameters();
    ui.main->pluginWindows().close (pluginId);
    ui.settle();
    CHECK (ui.main->pluginWindows().find (pluginId) == nullptr);
    CHECK_FALSE (ui.m().track (id)->strip.inserts[0].state.isNull());
}

TEST_CASE ("AT-PLG-4 plug-in manager: list, disable, rescan", "[UI]")
{
    forgetPlugins();
    Ui ui;
    const auto folder = ui.folder.path / "vst3";
    std::filesystem::create_directories (folder);
    installFixture (folder, WEFT_FIXTURE_TESTGAIN);
    ui.studio.pluginFolders = { folder };
    ui.studio.scannerPath = WEFT_SCANNER;

    PluginManager manager (ui.studio);
    CHECK (manager.rows() == 0);
    manager.rescan.onClick();
    REQUIRE (manager.rows() == 1);
    CHECK (manager.cellText (0, PluginManager::colName) == "TestGain");
    CHECK (manager.cellText (0, PluginManager::colVendor) == "Weft Test Fixtures");
    CHECK (manager.cellText (0, PluginManager::colFormat) == "VST3");
    CHECK (manager.cellText (0, PluginManager::colStatus) == "OK");

    auto& mixer = showMixer (ui);
    const auto id = ui.m().trackOrder.front();
    CHECK (entry (mixer.stripFor (id)->insertMenu (0), "TestGain").has_value());
    manager.setEnabled (0, false);
    CHECK (manager.cellText (0, PluginManager::colStatus) == "Disabled");
    CHECK_FALSE (entry (mixer.stripFor (id)->insertMenu (0), "TestGain").has_value());
    manager.setEnabled (0, true);
    CHECK (entry (mixer.stripFor (id)->insertMenu (0), "TestGain").has_value());

    // A newly installed plug-in shows up after Rescan; an instrument goes in the instrument menu.
    installFixture (folder, WEFT_FIXTURE_TESTSYNTH);
    manager.rescan.onClick();
    REQUIRE (manager.rows() == 2);
    const auto keys = ui.studio.session().createTrack (model::TrackKind::instrument, "Keys").value();
    ui.studio.edited();
    ui.settle();
    const auto synth = entry (mixer.stripFor (keys)->instrumentMenu(), "TestSynth");
    REQUIRE (synth);
    mixer.stripFor (keys)->pickInstrument (synth->id);
    ui.settle();
    REQUIRE (ui.m().track (keys)->strip.instrument.has_value());
    CHECK (ui.m().track (keys)->strip.instrument->name == "TestSynth");
    CHECK (mixer.stripFor (keys)->input.getButtonText() == "TestSynth");

    // The list survives a restart.
    engine::PluginCatalog reloaded (Studio::settingsFolder() / "plugins.json");
    REQUIRE (reloaded.load());
    CHECK (reloaded.plugins().size() == 2);
    ui.main->pluginWindows().closeAll();
    forgetPlugins();
}

// Not an acceptance test: the mixer with some content, as a PNG for reports.
// Run with: weft_ui_tests "[.screenshot-mixer]" (writes WEFT_SCREENSHOT or ./mixer.png).
TEST_CASE ("Screenshot of the mixer", "[.screenshot-mixer]")
{
    Ui ui;
    auto& s = ui.studio.session();
    const auto audio = ui.m().trackOrder.front();
    s.renameTrack (audio, "Guitar");
    const auto keys = s.createTrack (model::TrackKind::instrument, "Keys").value();
    const auto bass = s.createTrack (model::TrackKind::instrument, "Bass").value();
    const auto verb = s.newBus (audio, true).value();
    s.renameTrack (verb, "Reverb");
    REQUIRE (s.addSend (keys, verb));
    for (const auto& [track, uid, name] : std::vector<std::tuple<Id, std::string, std::string>> {
             { audio, "weft.eq", "Weft EQ" }, { audio, "weft.compressor", "Weft Compressor" }, { keys, "weft.delay", "Weft Delay" },
             { verb, "weft.reverb", "Weft Reverb" }, { ui.m().info.masterTrackId, "weft.gain", "Weft Gain" } })
    {
        model::PluginInstance p;
        p.pluginUid = uid;
        p.name = name;
        REQUIRE (s.addInsert (track, p));
    }
    s.setTrackVolume (audio, -4.5);
    s.setTrackPan (keys, -0.4);
    s.setTrackVolume (bass, -9.0);
    s.setTrackOutput (bass, verb);
    ui.studio.selectTrack (keys);
    ui.studio.layout.bottom = BottomTab::mixer;
    ui.studio.layout.bottomHeight = 600;
    ui.studio.edited();
    ui.settle();
    auto& mixer = ui.main->bottom().mixerView();
    mixer.stripFor (audio)->meter.update (0.5f, 0.18f, false, 1.0);
    mixer.stripFor (keys)->meter.update (0.3f, 0.09f, false, 1.0);
    mixer.masterStrip().meter.update (1.2f, 0.45f, true, 1.0);
    const auto image = ui.main->createComponentSnapshot (ui.main->getLocalBounds());
    const auto* env = std::getenv ("WEFT_SCREENSHOT");
    juce::File out (env != nullptr ? juce::String (env) : juce::File::getCurrentWorkingDirectory().getChildFile ("mixer.png").getFullPathName());
    out.deleteFile();
    juce::FileOutputStream stream (out);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
}

TEST_CASE ("AT-MIX-16 Mute buttons show which tracks a solo silences; Alt+click mutes every strip", "[UI][AT-MIX-16]")
{
    Ui ui;
    auto& s = ui.studio.session();
    const auto a = ui.m().trackOrder.front();
    const auto b = s.createTrack (model::TrackKind::audio, "B").value();
    const auto bus = s.newBus (a, false).value(); // A's output goes to the bus
    ui.studio.edited();
    auto& mixer = showMixer (ui);

    // The audible set: a solo keeps what it feeds and what feeds it.
    CHECK (commands::audibleWithSolo (ui.m(), {}).empty());
    CHECK (commands::audibleWithSolo (ui.m(), { a }) == std::set<Id> { a, bus });
    CHECK (commands::audibleWithSolo (ui.m(), { bus }) == std::set<Id> { a, bus });

    const auto header = [&] (const Id& id) -> TrackHeader& {
        for (const auto& h : ui.main->tracks().headers())
            if (h->trackId == id)
                return *h;
        FAIL ("no header");
        return *ui.main->tracks().headers().front();
    };
    ui.studio.toggleSolo (a);
    ui.settle();
    CHECK (silencedBySolo (header (b).mute));
    CHECK (silencedBySolo (mixer.stripFor (b)->mute));
    CHECK_FALSE (silencedBySolo (header (a).mute));
    CHECK_FALSE (silencedBySolo (mixer.stripFor (a)->mute));
    CHECK_FALSE (silencedBySolo (mixer.stripFor (bus)->mute));
    ui.studio.toggleSolo (a);
    ui.settle();
    CHECK_FALSE (silencedBySolo (header (b).mute));
    CHECK_FALSE (silencedBySolo (mixer.stripFor (b)->mute));

    juce::ModifierKeys::currentModifiers = juce::ModifierKeys (juce::ModifierKeys::altModifier);
    mixer.stripFor (b)->mute.onClick();
    juce::ModifierKeys::currentModifiers = {};
    ui.settle();
    for (const auto& id : { a, b, bus })
        CHECK (ui.m().track (id)->mute);
    CHECK_FALSE (ui.m().track (ui.m().info.masterTrackId)->mute);
}

TEST_CASE ("AT-MIX-17 Shift+click and swipes bypass inserts; Insert Before puts a plug-in ahead of one", "[UI][AT-MIX-17]")
{
    Ui ui;
    const auto first = ui.m().trackOrder.front();
    const auto second = ui.studio.session().createTrack (model::TrackKind::audio, "Second").value();
    ui.studio.edited();
    auto& mixer = showMixer (ui);
    for (const auto& id : { first, first, second })
    {
        auto* strip = mixer.stripFor (id);
        const auto slot = ui.m().track (id)->strip.inserts.size();
        const auto eq = entry (strip->insertMenu (slot), "Weft EQ");
        REQUIRE (eq);
        strip->pickInsert (slot, eq->id);
        ui.settle();
    }
    const auto bypass = [&] (const Id& id, std::size_t i) { return ui.m().track (id)->strip.inserts[i].bypass; };
    auto* a = mixer.stripFor (first);
    auto* b = mixer.stripFor (second);
    REQUIRE (a->inserts.size() >= 2);
    REQUIRE (b->inserts.size() >= 1);

    // Shift+click toggles bypass and opens nothing.
    std::vector<Id> opened; // adding opened their windows; closing one writes its state, so copy the ids first
    for (const auto& id : { first, second })
        for (const auto& p : ui.m().track (id)->strip.inserts)
            opened.push_back (p.id);
    for (const auto& id : opened)
        ui.main->pluginWindows().close (id);
    ui.settle();
    REQUIRE (ui.main->pluginWindows().find (ui.m().track (first)->strip.inserts[0].id) == nullptr);
    auto& slot0 = *a->inserts[0];
    const auto middle = slot0.getLocalBounds().getCentre().toFloat();
    slot0.mouseDown (mouse (slot0, middle, juce::ModifierKeys (juce::ModifierKeys::shiftModifier | juce::ModifierKeys::leftButtonModifier)));
    slot0.mouseUp (mouse (slot0, middle, juce::ModifierKeys::shiftModifier));
    ui.settle();
    CHECK (bypass (first, 0));
    CHECK (ui.main->pluginWindows().find (ui.m().track (first)->strip.inserts[0].id) == nullptr);
    a->pickInsert (0, ChannelStrip::itemBypass);
    ui.settle();
    REQUIRE_FALSE (bypass (first, 0));

    // A press on a dot, then across the other dots: all bypassed, one undo step, the slots stay the same objects.
    {
        auto* from = a->inserts[0].get();
        const auto dot = juce::Point<float> (4.0f, static_cast<float> (from->getHeight()) / 2.0f);
        from->mouseDown (mouse (*from, dot, juce::ModifierKeys::leftButtonModifier));
        ui.settle();
        CHECK (bypass (first, 0));
        CHECK (a->inserts[0].get() == from);
        from->swipeBypassOver (a->inserts[1].get());
        from->swipeBypassOver (b->inserts[0].get());
        from->swipeBypassOver (b->inserts.back().get()); // the free slot: nothing to bypass
        from->mouseUp (mouse (*from, dot));
        ui.settle();
    }
    CHECK ((bypass (first, 0) && bypass (first, 1) && bypass (second, 0)));
    CHECK (ui.studio.session().undoName() == "Bypass Plug-ins");
    REQUIRE (ui.studio.perform (Cmd::undo));
    ui.settle();
    CHECK_FALSE ((bypass (first, 0) || bypass (first, 1) || bypass (second, 0)));

    // Insert Before: a new plug-in in that slot, the rest move down.
    const auto was = ui.m().track (first)->strip.inserts[1].id;
    std::optional<MenuEntry> delay;
    for (const auto& e : entries (a->insertMenu (1)))
        if (e.id >= ChannelStrip::itemInsertBefore && e.text.startsWith ("Weft Delay"))
            delay = e;
    REQUIRE (delay);
    a->pickInsert (1, delay->id);
    ui.settle();
    REQUIRE (ui.m().track (first)->strip.inserts.size() == 3);
    CHECK (ui.m().track (first)->strip.inserts[1].pluginUid == "weft.delay");
    CHECK (ui.m().track (first)->strip.inserts[2].id == was);
}

TEST_CASE ("AT-PLG-13 Search and Add Plug-in", "[UI][AT-PLG-13]")
{
    Ui ui;
    const auto audio = ui.m().trackOrder.front();
    const auto keys = ui.studio.session().createTrack (model::TrackKind::instrument, "Keys").value();
    ui.studio.edited();
    ui.settle();

    // No track selected: the command says so.
    ui.studio.selectTrack ({});
    ui.key ('P', juce::ModifierKeys (juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier));
    CHECK (ui.studio.status.contains ("Select a track"));

    ui.studio.selectTrack (audio);
    ui.settle();
    const auto all = PluginSearch::matches (ui.studio, "");
    CHECK (all.size() == ui.studio.pluginChoices (false).size()); // effects only on an audio track
    const auto delay = PluginSearch::matches (ui.studio, "  dEL ");
    REQUIRE (delay.size() == 1);
    CHECK (delay[0].label == "Weft Delay");
    CHECK (PluginSearch::matches (ui.studio, "weft del").size() == 1); // every word must match
    CHECK (PluginSearch::matches (ui.studio, "zzz").empty());

    PluginSearch search (ui.studio);
    search.setSearch ("eq");
    REQUIRE_FALSE (search.results().empty());
    CHECK (search.addSelected());
    ui.settle();
    REQUIRE (ui.m().track (audio)->strip.inserts.size() == 1);
    CHECK (ui.m().track (audio)->strip.inserts[0].pluginUid == "weft.eq");

    // An instrument track also finds instruments, and an instrument goes in its instrument slot.
    ui.studio.selectTrack (keys);
    ui.settle();
    const auto withInstruments = PluginSearch::matches (ui.studio, "");
    CHECK (withInstruments.size() == ui.studio.pluginChoices (true).size() + ui.studio.pluginChoices (false).size());
    const auto instrument = ui.studio.pluginChoices (true).front();
    CHECK (PluginSearch::add (ui.studio, instrument));
    ui.settle();
    REQUIRE (ui.m().track (keys)->strip.instrument.has_value());
    CHECK (ui.m().track (keys)->strip.instrument->pluginUid == instrument.plugin.pluginUid);
    ui.studio.selectTrack (audio);
    CHECK_FALSE (PluginSearch::add (ui.studio, instrument));
    CHECK (ui.studio.status.contains ("instrument track"));

    juce::StringArray items;
    const auto menu = ui.main->getMenuForIndex (3, "Mix");
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        items.add (it.getItem().text);
    CHECK (items.contains ("Search and Add Plug-in..."));
}

TEST_CASE ("AT-PLG-14 plug-in settings: save, load, next/previous, compare, copy/paste, reset, default; hide all windows", "[UI][AT-PLG-14]")
{
    Ui ui;
    const auto first = ui.m().trackOrder.front();
    const auto second = ui.studio.session().createTrack (model::TrackKind::audio, "Second").value();
    ui.studio.edited();
    auto& mixer = showMixer (ui);
    const auto addEq = [&] (const Id& trackId) {
        auto* strip = mixer.stripFor (trackId);
        const auto slot = ui.m().track (trackId)->strip.inserts.size();
        const auto eq = entry (strip->insertMenu (slot), "Weft EQ");
        REQUIRE (eq);
        strip->pickInsert (slot, eq->id);
        ui.settle();
        return ui.m().track (trackId)->strip.inserts.back().id;
    };
    const auto a = addEq (first);
    const auto b = addEq (second);
    auto* window = ui.main->pluginWindows().find (a);
    REQUIRE (window != nullptr);
    auto* frame = window->frame();
    auto* editor = window->generic();
    REQUIRE ((frame != nullptr && editor != nullptr));
    const auto proc = ui.studio.processor (a);
    REQUIRE (proc != nullptr);
    REQUIRE_FALSE (editor->rows.empty());
    std::size_t g = 0; // the first continuous parameter
    while (g < editor->rows.size() && proc->parameters()[g].steps > 1)
        ++g;
    REQUIRE (g < editor->rows.size());
    const auto set = [&] (double v) {
        window->pollParameters(); // the window's timer has seen the last session edit
        editor->rows[g].slider->setValue (v, juce::sendNotificationSync);
        window->pollParameters();
        ui.settle();
        frame->refresh();
    };
    const auto value = [&] { return proc->parameter (static_cast<int> (g)); };
    const auto& names = [&] { return plugin_settings::names (Studio::pluginSettingsRoot(), *ui.studio.pluginInstance (a)); };

    // Nothing saved yet: the button says Settings; Compare and Paste are off.
    CHECK (frame->settings.getButtonText() == "Settings");
    CHECK_FALSE (frame->compare.isEnabled());
    CHECK_FALSE (frame->paste.isEnabled());
    CHECK (window->isResizable()); // the slider list can be widened
    CHECK (frame->getWidth() >= PluginFrame::minWidth);

    // Save Setting As (asks for the name), then a second one.
    set (0.8);
    frame->promptForText = [] (const juce::String&, const juce::String&, std::function<void (juce::String)> done) { done ("Bright"); };
    frame->pickSettings (PluginFrame::itemSaveAs);
    CHECK (frame->settingName() == "Bright");
    CHECK (frame->settings.getButtonText() == "Bright");
    set (0.5);
    CHECK (frame->modified());
    CHECK (frame->settings.getButtonText() == "Bright *");
    CHECK (frame->compare.isEnabled());

    // Compare: the saved setting, then the edits again.
    REQUIRE (frame->toggleCompare());
    ui.settle();
    CHECK (frame->comparing());
    CHECK (value() == Catch::Approx (0.8).margin (1e-4));
    REQUIRE (frame->toggleCompare());
    ui.settle();
    CHECK_FALSE (frame->comparing());
    CHECK (value() == Catch::Approx (0.5).margin (1e-4));

    REQUIRE (frame->saveSettingAs ("Air"));
    CHECK (names() == std::vector<std::string> { "Air", "Bright" });

    // Next and Previous go through the saved settings in name order, and wrap; loading is one undo step.
    set (0.1);
    REQUIRE (frame->stepSetting (1));
    ui.settle();
    CHECK (frame->settingName() == "Bright");
    CHECK (value() == Catch::Approx (0.8).margin (1e-4));
    CHECK (ui.studio.session().undoName() == "Load Setting");
    frame->next.onClick();
    ui.settle();
    CHECK (frame->settingName() == "Air");
    CHECK (value() == Catch::Approx (0.5).margin (1e-4));
    frame->pickSettings (PluginFrame::itemPrevious);
    ui.settle();
    CHECK (frame->settingName() == "Bright");
    REQUIRE (ui.studio.perform (Cmd::undo));
    ui.settle();
    CHECK (value() == Catch::Approx (0.5).margin (1e-4));

    // The Settings menu lists the saved settings, ticking the current one.
    {
        const auto menu = frame->settingsMenu();
        juce::StringArray items, ticked;
        for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
            if (it.getItem().itemID != 0)
            {
                items.add (it.getItem().text);
                if (it.getItem().isTicked)
                    ticked.add (it.getItem().text);
            }
        CHECK (items.joinIntoString ("|")
               == "Next Setting|Previous Setting|Copy Setting|Paste Setting|Reset Setting|Save Setting|Save Setting As...|Save Setting as Default|"
                  "Recall Default Setting|Delete \"Bright\"|Air|Bright");
        CHECK (ticked == juce::StringArray { "Bright" });
    }

    // Copy on one EQ, Paste on the other.
    set (0.3);
    frame->copy.onClick();
    auto* other = ui.main->pluginWindows().find (b);
    REQUIRE (other != nullptr);
    other->frame()->refresh();
    CHECK (other->frame()->paste.isEnabled());
    other->frame()->paste.onClick();
    ui.settle();
    CHECK (ui.studio.processor (b)->parameter (static_cast<int> (g)) == Catch::Approx (0.3).margin (1e-4));
    CHECK (ui.studio.session().undoName() == "Paste Setting");

    // Reset Setting: every parameter to its default.
    frame->pickSettings (PluginFrame::itemReset);
    ui.settle();
    CHECK (value() == Catch::Approx (proc->parameters()[g].defaultValue).margin (1e-4));
    CHECK (ui.studio.session().undoName() == "Reset Setting");
    CHECK (frame->settingName().empty());

    // Save as Default: a new Weft EQ starts from it; Recall Default Setting loads it back.
    set (0.7);
    frame->pickSettings (PluginFrame::itemSaveDefault);
    const auto c = addEq (second);
    const auto paramId = proc->parameters()[g].id;
    REQUIRE (ui.studio.pluginInstance (c)->params.contains (paramId));
    CHECK (ui.studio.pluginInstance (c)->params.at (paramId) == Catch::Approx (0.7).margin (1e-4));
    CHECK (ui.studio.processor (c)->parameter (static_cast<int> (g)) == Catch::Approx (0.7).margin (1e-4));
    set (0.2);
    frame->pickSettings (PluginFrame::itemRecallDefault);
    ui.settle();
    CHECK (value() == Catch::Approx (0.7).margin (1e-4));

    // Delete the current setting.
    REQUIRE (frame->loadSetting ("Bright"));
    frame->pickSettings (PluginFrame::itemDelete);
    CHECK (names() == std::vector<std::string> { "Air" });
    CHECK (frame->settingName().empty());

    // V hides every plug-in window and shows them again; with none open it only says so.
    auto& windows = ui.main->pluginWindows();
    CHECK (commandInfo (Cmd::togglePluginWindows).shortcut == "V");
    ui.main->run (Cmd::togglePluginWindows);
    CHECK (windows.allWindowsHidden());
    ui.main->run (Cmd::togglePluginWindows);
    CHECK_FALSE (windows.allWindowsHidden());
    windows.closeAll();
    ui.settle();
    CHECK_FALSE (windows.toggleAll());
    CHECK_FALSE (windows.allWindowsHidden());
}

TEST_CASE ("AT-MIX-18 peak number, Mute Off for All, Clear/Recall Solo, solo by regions, monitoring key, copy and paste a strip", "[UI][AT-MIX-18]")
{
    Ui ui;
    auto& s = ui.studio.session();
    const auto first = ui.m().trackOrder.front();
    const auto second = s.createTrack (model::TrackKind::instrument, "Second").value();
    const auto third = s.createTrack (model::TrackKind::instrument, "Third").value();
    ui.studio.edited();
    auto& mixer = showMixer (ui);
    auto* strip = mixer.stripFor (first);
    REQUIRE (strip != nullptr);

    // The peak number holds the highest level, red over 0 dB; a click resets it and the clip light.
    CHECK (strip->peak.getText() == "-inf");
    strip->meter.update (2.0f, 0.5f, true, 0.0);
    strip->updateMeter (0.1);
    CHECK (strip->peak.getText() == "+6.0");
    CHECK (strip->peak.findColour (juce::Label::textColourId) == colour (Token::meterClip));
    strip->meter.update (0.5f, 0.1f, false, 0.2);
    strip->updateMeter (0.3);
    CHECK (strip->peak.getText() == "+6.0"); // held
    strip->mouseDown (mouse (strip->peak, strip->peak.getLocalBounds().getCentre().toFloat()));
    CHECK (strip->peak.getText() == "-inf");
    CHECK_FALSE (strip->meter.ballistics.clip());
    CHECK (strip->peak.getRight() <= strip->getWidth());
    CHECK (strip->readout.getRight() <= strip->peak.getX());

    // Mute Off for All: one undo step.
    REQUIRE (s.setTrackMute (first, true));
    REQUIRE (s.setTrackMute (third, true));
    ui.studio.edited();
    REQUIRE (ui.studio.perform (Cmd::unmuteAll));
    ui.settle();
    CHECK_FALSE ((ui.m().track (first)->mute || ui.m().track (third)->mute));
    CHECK (s.undoName() == "Mute Off for All");
    CHECK_FALSE (ui.studio.perform (Cmd::unmuteAll));

    // Clear/Recall Solo.
    ui.studio.toggleSolo (first);
    ui.studio.toggleSolo (second);
    REQUIRE (ui.studio.perform (Cmd::clearRecallSolo));
    CHECK (ui.studio.localView().soloed.empty());
    REQUIRE (ui.studio.perform (Cmd::clearRecallSolo));
    CHECK (ui.studio.localView().soloed == std::set<Id> { first, second });
    REQUIRE (ui.studio.perform (Cmd::clearRecallSolo));
    ui.studio.toggleSolo (third); // a new solo replaces what was kept
    REQUIRE (ui.studio.perform (Cmd::clearRecallSolo));
    REQUIRE (ui.studio.perform (Cmd::clearRecallSolo));
    CHECK (ui.studio.localView().soloed == std::set<Id> { third });

    // Solo Tracks of Selected Regions: exactly those tracks.
    const auto r1 = s.createMidiRegion (second, 0, 3840).value();
    const auto r2 = s.createMidiRegion (third, 3840, 3840).value();
    ui.studio.edited();
    ui.studio.selection.set ({ r1, r2 });
    REQUIRE (ui.studio.perform (Cmd::soloSelectedRegions));
    CHECK (ui.studio.localView().soloed == std::set<Id> { second, third });
    ui.studio.selection.set (std::vector<Id> {});
    CHECK_FALSE (ui.studio.perform (Cmd::soloSelectedRegions));

    // Shift+I: the selected track's input monitoring On, then Auto.
    ui.studio.selectTrack (first);
    REQUIRE (ui.key ('I', juce::ModifierKeys::shiftModifier));
    CHECK (ui.m().track (first)->monitor == "on");
    REQUIRE (ui.studio.perform (Cmd::monitorTrack));
    CHECK (ui.m().track (first)->monitor == "auto");

    // Copy Channel Strip Setting from one track, paste onto another: volume, pan and inserts, one undo step.
    REQUIRE (s.setTrackVolume (first, -6.0));
    REQUIRE (s.setTrackPan (first, 0.3));
    ui.studio.edited();
    strip = mixer.stripFor (first);
    const auto eq = entry (strip->insertMenu (0), "Weft EQ");
    REQUIRE (eq);
    strip->pickInsert (0, eq->id);
    ui.settle();
    const auto sourceInsert = ui.m().track (first)->strip.inserts.at (0);
    REQUIRE (s.setPluginParam (first, sourceInsert.id, "hpOn", 1.0));
    ui.studio.edited();
    CHECK_FALSE (ui.studio.hasChannelStripCopy());
    REQUIRE (ui.studio.perform (Cmd::copyChannelStrip));
    CHECK (ui.studio.hasChannelStripCopy());
    ui.studio.selectTrack (third);
    REQUIRE (ui.studio.perform (Cmd::pasteChannelStrip));
    ui.settle();
    const auto& pasted = ui.m().track (third)->strip;
    CHECK (pasted.volumeDb == Catch::Approx (-6.0));
    CHECK (pasted.pan == Catch::Approx (0.3));
    REQUIRE (pasted.inserts.size() == 1);
    CHECK (pasted.inserts[0].pluginUid == sourceInsert.pluginUid);
    CHECK (pasted.inserts[0].id != sourceInsert.id);
    CHECK (pasted.inserts[0].params.at ("hpOn") == Catch::Approx (1.0));
    CHECK (s.undoName() == "Paste Channel Strip Setting");
    REQUIRE (ui.studio.perform (Cmd::undo));
    ui.settle();
    CHECK (ui.m().track (third)->strip.inserts.empty());
    CHECK (ui.m().track (third)->strip.volumeDb == Catch::Approx (0.0));
}
