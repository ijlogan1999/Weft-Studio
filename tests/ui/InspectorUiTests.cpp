// AT-UI-9: the inspector's channel strips (spec 04 §4.3, D-UI-16).

#include "UiFixture.h"

#include "weft/engine/dsp/BuiltinEffects.h"

using namespace weft;
using namespace weft::ui;
using namespace weft::ui::test;

namespace
{
Inspector& inspector (Ui& ui)
{
    if (! ui.studio.layout.inspector)
        ui.main->run (Cmd::toggleInspector);
    ui.settle();
    return ui.main->inspector();
}

MixerView& showMixer (Ui& ui)
{
    if (ui.studio.layout.bottom != BottomTab::mixer)
        ui.main->run (Cmd::toggleMixer);
    ui.settle();
    return ui.main->bottom().mixerView();
}
} // namespace

TEST_CASE ("AT-UI-9 Inspector shows the selected track's strip and its output's", "[UI]")
{
    Ui ui;
    auto& in = inspector (ui);
    const auto track = ui.m().trackOrder.front();

    ui.studio.selectTrack ({});
    ui.settle();
    CHECK (in.trackStrip == nullptr);
    CHECK (in.outputStrip == nullptr);
    CHECK_FALSE (in.eq.isVisible());

    ui.studio.selectTrack (track);
    ui.settle();
    REQUIRE (in.trackStrip != nullptr);
    REQUIRE (in.outputStrip != nullptr);
    CHECK (in.trackStrip->trackId() == track);
    CHECK (in.outputStrip->trackId() == ui.m().info.masterTrackId); // output: Master
    CHECK (in.trackStrip->isVisible());
    CHECK (in.outputStrip->isVisible());
    CHECK (in.trackStrip->getParentComponent() != nullptr);
    // Side by side, faders level.
    CHECK (in.trackStrip->getRight() <= in.outputStrip->getX());
    CHECK (in.trackStrip->volume.getY() == in.outputStrip->volume.getY());

    // Routing the track to a bus puts the bus's strip beside it.
    const auto bus = ui.studio.session().newBus (track, false);
    REQUIRE (bus.has_value());
    ui.studio.edited();
    ui.settle();
    REQUIRE (in.outputStrip != nullptr);
    CHECK (in.outputStrip->trackId() == *bus);
    CHECK (in.outputStrip->output.getButtonText() == "Master");

    // Clicking the output's strip doesn't move the selection off the track.
    in.outputStrip->mouseDown (mouse (*in.outputStrip, { 10.0f, 10.0f }));
    ui.settle();
    CHECK (ui.studio.selectedTrack() == track);

    // Selecting the bus shows the bus and the master.
    ui.studio.selectTrack (*bus);
    ui.settle();
    CHECK (in.trackStrip->trackId() == *bus);
    CHECK (in.outputStrip->trackId() == ui.m().info.masterTrackId);
}

TEST_CASE ("AT-UI-9 Inspector strip edits are the mixer's edits", "[UI]")
{
    Ui ui;
    auto& in = inspector (ui);
    auto& mixer = showMixer (ui);
    const auto track = ui.m().trackOrder.front();
    ui.studio.selectTrack (track);
    ui.settle();
    REQUIRE (in.trackStrip != nullptr);

    // Inspector -> model -> mixer.
    in.trackStrip->volume.setValue (-6.0, juce::sendNotificationSync);
    in.trackStrip->pan.setValue (-50.0, juce::sendNotificationSync);
    in.trackStrip->mute.onClick();
    ui.settle();
    CHECK (ui.m().track (track)->strip.volumeDb == Catch::Approx (-6.0));
    CHECK (ui.m().track (track)->strip.pan == Catch::Approx (-0.5));
    CHECK (ui.m().track (track)->mute);
    auto* strip = mixer.stripFor (track);
    REQUIRE (strip != nullptr);
    CHECK (strip->volume.getValue() == Catch::Approx (-6.0));
    CHECK (strip->pan.getValue() == Catch::Approx (-50.0));
    CHECK (strip->mute.getToggleState());

    // Mixer -> model -> inspector.
    strip->volume.setValue (-12.0, juce::sendNotificationSync);
    ui.settle();
    CHECK (in.trackStrip->volume.getValue() == Catch::Approx (-12.0));
    CHECK (in.trackStrip->readout.getText() == strip->readout.getText());

    // The master's fader in the inspector is the mixer's master fader.
    in.outputStrip->volume.setValue (-3.0, juce::sendNotificationSync);
    ui.settle();
    CHECK (mixer.masterStrip().volume.getValue() == Catch::Approx (-3.0));

    // Inserts and sends added in the inspector show in the mixer.
    in.trackStrip->pickInsert (0, ChannelStrip::itemPlugin + 1); // the second built-in
    ui.settle();
    REQUIRE (ui.m().track (track)->strip.inserts.size() == 1);
    strip = mixer.stripFor (track);
    CHECK (strip->inserts.size() == 2);
    CHECK (strip->inserts[0]->label == in.trackStrip->inserts[0]->label);
    in.trackStrip->pickSend (0, ChannelStrip::itemNewBus);
    ui.settle();
    REQUIRE (ui.m().track (track)->strip.sends.size() == 1);
    CHECK (mixer.stripFor (track)->sends.size() == 2);
    CHECK (in.trackStrip->sends.size() == 2);

    // Undo goes back through the same history.
    ui.studio.session().undo();
    ui.studio.edited();
    ui.settle();
    CHECK (ui.m().track (track)->strip.sends.empty());
    CHECK (in.trackStrip->sends.size() == 1);
}

TEST_CASE ("AT-UI-9 Inspector EQ thumbnail adds, opens and draws the track's EQ", "[UI]")
{
    Ui ui;
    auto& in = inspector (ui);
    const auto track = ui.m().trackOrder.front();
    ui.studio.selectTrack (track);
    ui.settle();
    REQUIRE (in.eq.isVisible());
    CHECK (in.eq.eqId().empty());
    for (const auto db : in.eq.curve (16))
        CHECK (db == 0.0); // no EQ: flat

    // Clicking adds a Weft EQ to the first free slot and opens it.
    const auto centre = in.eq.getLocalBounds().getCentre().toFloat();
    in.eq.mouseUp (mouse (in.eq, centre));
    ui.settle();
    const auto& inserts = ui.m().track (track)->strip.inserts;
    REQUIRE (inserts.size() == 1);
    CHECK (inserts[0].pluginUid == "weft.eq");
    CHECK (in.eq.eqId() == inserts[0].id);
    CHECK (ui.main->pluginWindows().find (inserts[0].id) != nullptr);

    // The curve follows the EQ's parameters: +12 dB at 1 kHz.
    auto eq = std::dynamic_pointer_cast<engine::WeftEq> (ui.studio.processor (inserts[0].id));
    REQUIRE (eq != nullptr);
    eq->setValue ("mid1Freq", 1000.0);
    eq->setValue ("mid1Gain", 12.0);
    const auto curve = in.eq.curve (301); // 20 Hz .. 20 kHz: 1 kHz is index 170
    const auto peak = std::max_element (curve.begin(), curve.end());
    CHECK (*peak == Catch::Approx (12.0).margin (0.2));
    CHECK (std::abs (std::distance (curve.begin(), peak) - 170) <= 1);
    CHECK (curve.front() == Catch::Approx (0.0).margin (0.2));

    // Bypassed: drawn flat.
    ui.studio.session().setInsertBypass (track, inserts[0].id, true);
    ui.studio.edited();
    ui.settle();
    for (const auto db : in.eq.curve (16))
        CHECK (db == 0.0);

    // A second click opens the same EQ rather than adding another.
    in.eq.mouseUp (mouse (in.eq, centre));
    ui.settle();
    CHECK (ui.m().track (track)->strip.inserts.size() == 1);
}

TEST_CASE ("AT-UI-9 Inspector and mixer meters both see a channel's peaks", "[UI]")
{
    Ui ui;
    const auto file = writeSine (ui.folder.path / "browse" / "sine.wav");
    const auto outcome = ui.studio.importFiles ({ file }, { 0, {} });
    REQUIRE (outcome.regions.size() == 1);
    const auto track = ui.region (outcome.regions.front()).trackId;
    ui.settle();
    int mixerReader = 0, inspectorReader = 0;
    // Both views read every frame; their first reads register them.
    ui.studio.takeMeter (track, &mixerReader);
    ui.studio.takeMeter (track, &inspectorReader);
    ui.studio.perform (Cmd::playStop);
    ui.renderAudio (4800);
    // The mixer reads first; the inspector still sees the same peak (the 0.5 sine, -6 dB).
    const auto first = ui.studio.takeMeter (track, &mixerReader);
    const auto second = ui.studio.takeMeter (track, &inspectorReader);
    CHECK (first.output == Catch::Approx (0.5f).margin (0.02f));
    CHECK (second.output == Catch::Approx (first.output));
    // Each reader's peak is cleared by its own read only.
    ui.studio.perform (Cmd::playStop);
    ui.renderAudio (4800);
    ui.studio.takeMeter (track, &mixerReader);
    CHECK (ui.studio.takeMeter (track, &mixerReader).output == 0.0f);
    ui.studio.forgetMeterReader (&mixerReader);
    ui.studio.forgetMeterReader (&inspectorReader);
}

// Not an acceptance test: the arrange window with the inspector's channel section, as a PNG for
// reports. Run with: weft_ui_tests "[.screenshot-inspector]" (writes WEFT_SCREENSHOT or ./inspector.png).
TEST_CASE ("Screenshot of the inspector", "[.screenshot-inspector]")
{
    Ui ui;
    auto& s = ui.studio.session();
    const auto vocal = ui.m().trackOrder.front();
    s.renameTrack (vocal, "Lead Vocal");
    const auto keys = s.createTrack (model::TrackKind::instrument, "Keys").value();
    const auto bus = s.newBus (vocal, false).value();
    s.renameTrack (bus, "Vocal Bus");
    const auto verb = s.newBus (vocal, true).value();
    s.renameTrack (verb, "Reverb");
    s.setTrackOutput (keys, {});
    for (const auto& [track, uid, name] : std::vector<std::tuple<Id, std::string, std::string>> {
             { vocal, "weft.eq", "Weft EQ" }, { vocal, "weft.compressor", "Weft Compressor" }, { bus, "weft.compressor", "Weft Compressor" },
             { verb, "weft.reverb", "Weft Reverb" } })
    {
        model::PluginInstance p;
        p.pluginUid = uid;
        p.name = name;
        REQUIRE (s.addInsert (track, p));
    }
    s.setTrackVolume (vocal, -3.5);
    s.setTrackPan (vocal, 0.15);
    s.setTrackVolume (bus, -1.0);
    const auto file = writeSine (ui.folder.path / "browse" / "vocal.wav", 6.0);
    REQUIRE (ui.studio.importFiles ({ file }, { 0, vocal }).regions.size() == 1);
    ui.studio.selectTrack (vocal);
    ui.studio.layout.bottom = BottomTab::none;
    ui.studio.layout.inspectorWidth = 248;
    ui.studio.edited();
    ui.settle();
    auto& in = ui.main->inspector();
    const auto eqId = ui.m().track (vocal)->strip.inserts[0].id;
    auto eq = std::dynamic_pointer_cast<engine::WeftEq> (ui.studio.processor (eqId));
    REQUIRE (eq != nullptr);
    eq->setValue ("hpOn", 1.0);
    eq->setValue ("hpFreq", 90.0);
    eq->setValue ("mid1Freq", 350.0);
    eq->setValue ("mid1Gain", -4.0);
    eq->setValue ("mid2Freq", 3200.0);
    eq->setValue ("mid2Gain", 5.0);
    eq->setValue ("highGain", 2.5);
    ui.studio.selection.clear();
    ui.studio.edited();
    ui.settle();
    in.trackStrip->meter.update (0.45f, 0.16f, false, 1.0);
    in.outputStrip->meter.update (0.5f, 0.2f, false, 1.0);
    const auto image = ui.main->createComponentSnapshot (ui.main->getLocalBounds());
    const auto* env = std::getenv ("WEFT_SCREENSHOT");
    juce::File out (env != nullptr ? juce::String (env) : juce::File::getCurrentWorkingDirectory().getChildFile ("inspector.png").getFullPathName());
    out.deleteFile();
    juce::FileOutputStream stream (out);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
}

TEST_CASE ("AT-TRK-24 Inspector track parameters for instrument tracks", "[UI][AT-TRK-24]")
{
    Ui ui;
    auto& in = inspector (ui);
    in.midiDeviceNames = [] { return std::vector<std::string> { "USB Keys", "Pads" }; };
    const auto audio = ui.m().trackOrder.front();
    const auto keys = ui.studio.session().createTrack (model::TrackKind::instrument, "Keys").value();
    ui.studio.edited();

    // Audio tracks don't show them.
    ui.studio.selectTrack (audio);
    ui.settle();
    CHECK_FALSE (in.trackTranspose.isVisible());
    CHECK_FALSE (in.midiIn.isVisible());

    ui.studio.selectTrack (keys);
    ui.settle();
    REQUIRE (in.trackTranspose.isVisible());
    CHECK (in.midiIn.getNumItems() == 3); // All, USB Keys, Pads
    CHECK (in.midiIn.getText() == "All");
    CHECK (in.keyLow.getTextFromValue (in.keyLow.getValue()) == juce::String (commands::noteName (0)));
    CHECK (in.velocityHigh.getValue() == 127.0);
    CHECK (in.trackTranspose.getY() > in.midiIn.getY());

    // A drag is one undo step.
    in.trackTranspose.onDragStart();
    in.trackTranspose.setValue (5, juce::sendNotificationSync);
    in.trackTranspose.setValue (7, juce::sendNotificationSync);
    in.trackTranspose.onDragEnd();
    ui.settle();
    CHECK (ui.m().track (keys)->playback.transpose == 7);
    CHECK (ui.studio.session().undoName() == "Track Parameters");
    ui.studio.session().undo();
    ui.studio.edited();
    ui.settle();
    CHECK (ui.m().track (keys)->playback.transpose == 0);
    CHECK (in.trackTranspose.getValue() == 0.0);

    // Limits pushed past their partner take it along.
    in.keyHigh.setValue (40, juce::sendNotificationSync);
    in.keyLow.setValue (60, juce::sendNotificationSync);
    ui.settle();
    CHECK (ui.m().track (keys)->playback.keyLow == 60);
    CHECK (ui.m().track (keys)->playback.keyHigh == 60);
    in.velocityLow.setValue (90, juce::sendNotificationSync);
    ui.settle();
    CHECK (ui.m().track (keys)->playback.velocityLow == 90);

    // Delay in ticks; switching the unit sets the delay to zero.
    in.trackDelay.setValue (25.5, juce::sendNotificationSync);
    ui.settle();
    CHECK (ui.m().track (keys)->playback.delay == 25.5);
    in.delayUnit.setSelectedId (2, juce::sendNotificationSync);
    ui.settle();
    CHECK (ui.m().track (keys)->playback.delayInTicks);
    CHECK (ui.m().track (keys)->playback.delay == 0.0);
    CHECK (in.trackDelay.getMaximum() == model::TrackPlayback::maxDelayTicks);
    in.trackDelay.setValue (240, juce::sendNotificationSync);
    ui.settle();
    CHECK (ui.m().track (keys)->playback.delay == 240.0);
    CHECK (in.trackDelay.getTextFromValue (240) == "240");

    // Out channel, No Transpose.
    in.midiOutChannel.setSelectedId (11, juce::sendNotificationSync); // channel 10
    in.noTranspose.setToggleState (true, juce::sendNotificationSync);
    ui.settle();
    CHECK (ui.m().track (keys)->playback.outChannel == 10);
    CHECK (ui.m().track (keys)->playback.noTranspose);

    // MIDI input device and channel set the track's input.
    in.midiIn.setSelectedId (3, juce::sendNotificationSync); // Pads
    in.midiInChannel.setSelectedId (3, juce::sendNotificationSync); // channel 2
    ui.settle();
    CHECK (ui.m().track (keys)->input.kind == model::InputAssignment::Kind::midi);
    CHECK (ui.m().track (keys)->input.device == "Pads");
    CHECK (ui.m().track (keys)->input.channel == 2);

    // A device that went away stays listed while the track uses it.
    in.midiDeviceNames = [] { return std::vector<std::string> { "USB Keys" }; };
    ui.studio.selectTrack (audio);
    ui.settle();
    ui.studio.selectTrack (keys);
    ui.settle();
    CHECK (in.midiIn.getText() == "Pads");
    CHECK (in.midiInChannel.getText() == "2");
}
