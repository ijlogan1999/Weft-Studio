// AT-MIX-1, 2, 4, 8, 9 (mixer strips) and AT-PLG-4, 5 (plug-in manager and generic editor).

#include "UiFixture.h"

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
