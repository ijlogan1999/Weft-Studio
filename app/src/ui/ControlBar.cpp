#include "ControlBar.h"

#include "weft/time/Position.h"

namespace weft::ui
{
ControlBar::ControlBar (Studio& s) : studio (s)
{
    for (auto* b : commandButtons())
    {
        addAndMakeVisible (*b);
        auto* button = b;
        b->onClick = [this, button] {
            if (button == &stop)
            {
                if (studio.playing())
                    studio.perform (Cmd::playStop);
                return;
            }
            if (button == &play && studio.playing())
                return;
            studio.perform (button->command);
        };
    }
    describe (stop, "Stop", "Stop (Space)");
    describe (play, "Play", "Play (Space)");

    for (auto* l : { &position, &time, &tempo, &signature, &cpu })
    {
        l->setJustificationType (juce::Justification::centred);
        addAndMakeVisible (*l);
    }
    position.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    describe (position, "Position", "Position in bars and beats (double-click to type)");
    describe (time, "Time", "Position in time");
    describe (tempo, "Tempo", "Tempo at the playhead (double-click to type)");
    describe (signature, "Time signature", "Time signature at the playhead (double-click to type, e.g. 3/4)");
    describe (cpu, "CPU", "Audio thread load and dropouts");
    position.setEditable (false, true);
    tempo.setEditable (false, true);
    signature.setEditable (false, true);
    position.onTextChange = [this] {
        if (const auto t = parseBarsBeats (position.getText().toStdString(), studio.model().signatures))
            studio.locate (*t);
        refresh();
    };
    tempo.onTextChange = [this] {
        const auto bpm = tempo.getText().getDoubleValue();
        if (bpm > 0)
            studio.check (studio.session().setTempoAt (studio.playhead(), bpm));
        refresh();
    };
    signature.onTextChange = [this] {
        const auto parts = juce::StringArray::fromTokens (signature.getText(), "/", "");
        if (parts.size() == 2)
            studio.check (studio.session().setSignatureAt (studio.model().signatures.barAt (studio.playhead()).bar, parts[0].getIntValue(),
                                                           parts[1].getIntValue()));
        refresh();
    };

    countIn.addItemList ({ "No count-in", "1 bar count-in", "2 bar count-in" }, 1);
    describe (countIn, "Count-in", "Count-in before recording");
    countIn.onChange = [this] {
        auto settings = studio.engine().settings();
        settings.countInBars = countIn.getSelectedItemIndex();
        studio.engine().setSettings (settings);
    };
    addAndMakeVisible (countIn);
    int id = 1;
    for (const auto& c : arrange::snapChoices())
        if (c.snap.kind != arrange::Snap::Kind::off)
            snapBox.addItem (juce::String (c.label), id++);
    describe (snapBox, "Grid", "Snap grid value");
    snapBox.onChange = [this] {
        const auto label = snapBox.getText().toStdString();
        for (const auto& c : arrange::snapChoices())
            if (c.label == label)
                studio.snap = c.snap;
        studio.sendChangeMessage();
    };
    addAndMakeVisible (snapBox);

    studio.addChangeListener (this);
    Theme::get().addChangeListener (this);
    refresh();
    startTimerHz (20);
}

ControlBar::~ControlBar()
{
    studio.removeChangeListener (this);
    Theme::get().removeChangeListener (this);
}

std::vector<CommandButton*> ControlBar::commandButtons()
{
    return { &inspector, &side, &mixer, &editor, &global, &start, &rewind, &forward, &stop, &play, &record, &cycle,
             &metronome, &punch, &snap, &follow, &keyboard, &theme };
}

void ControlBar::refresh()
{
    const auto& m = studio.model();
    const auto& l = studio.layout;
    inspector.setToggleState (l.inspector, juce::dontSendNotification);
    side.setToggleState (l.sidePanel, juce::dontSendNotification);
    mixer.setToggleState (l.bottom == BottomTab::mixer, juce::dontSendNotification);
    editor.setToggleState (l.bottom == BottomTab::audioEditor, juce::dontSendNotification);
    global.setToggleState (l.globalTracks, juce::dontSendNotification);
    play.setToggleState (studio.playing(), juce::dontSendNotification);
    record.setToggleState (studio.recording(), juce::dontSendNotification);
    cycle.setToggleState (m.info.cycle.enabled, juce::dontSendNotification);
    metronome.setToggleState (studio.engine().settings().metronomeOn, juce::dontSendNotification);
    punch.setToggleState (m.info.punch.enabled, juce::dontSendNotification);
    snap.setToggleState (studio.snapEnabled, juce::dontSendNotification);
    follow.setToggleState (studio.follow, juce::dontSendNotification);
    keyboard.setToggleState (studio.keyboardMidiOn(), juce::dontSendNotification);
    countIn.setSelectedItemIndex (studio.engine().settings().countInBars, juce::dontSendNotification);
    snapBox.setText (juce::String (arrange::snapLabel (studio.snap)), juce::dontSendNotification);
    record.setColour (juce::TextButton::buttonOnColourId, colour (Token::record));
    record.setColour (juce::TextButton::textColourOnId, colour (Token::onRecord));
    timerCallback();
    repaint();
}

void ControlBar::timerCallback()
{
    const auto& m = studio.model();
    const auto t = studio.playhead();
    if (! position.isBeingEdited())
        position.setText (juce::String (formatBarsBeats (t, m.signatures)), juce::dontSendNotification);
    const auto seconds = m.tempo.tickToSample (static_cast<double> (t)) / m.info.sampleRate;
    time.setText (juce::String::formatted ("%d:%06.3f", static_cast<int> (seconds / 60.0), std::fmod (std::max (seconds, 0.0), 60.0)),
                  juce::dontSendNotification);
    if (! tempo.isBeingEdited())
        tempo.setText (juce::String (m.tempo.bpmAt (t), 2) + " BPM", juce::dontSendNotification);
    if (! signature.isBeingEdited())
    {
        const auto& sig = m.signatures.signatureAtBar (m.signatures.barAt (t).bar);
        signature.setText (juce::String (sig.numerator) + "/" + juce::String (sig.denominator), juce::dontSendNotification);
    }
    const auto load = studio.engine().cpuLoad();
    cpu.setText ("CPU " + juce::String (juce::roundToInt (load * 100.0)) + "%", juce::dontSendNotification);
    const auto level = load > 0.9 || studio.engine().dropouts() > 0 ? Token::record : load > 0.7 ? Token::warning : Token::textDim;
    cpu.setColour (juce::Label::textColourId, colour (level));
    play.setToggleState (studio.playing(), juce::dontSendNotification);
    record.setToggleState (studio.recording(), juce::dontSendNotification);
}

void ControlBar::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::surface));
    g.setColour (colour (Token::border));
    g.drawHorizontalLine (getHeight() - 1, 0.0f, static_cast<float> (getWidth()));
    // The display gets its own raised panel.
    g.setColour (colour (Token::surfaceRaised));
    g.fillRoundedRectangle (position.getBounds().getUnion (signature.getBounds()).expanded (6, 2).toFloat(), 10.0f);
}

void ControlBar::resized()
{
    auto r = getLocalBounds().reduced (8, 8);
    auto place = [&] (juce::Component& c, int w) {
        c.setBounds (r.removeFromLeft (w));
        r.removeFromLeft (4);
    };
    for (auto* b : { &inspector, &side, &mixer, &editor, &global })
        place (*b, 66);
    r.removeFromLeft (12);
    for (auto* b : { &start, &rewind, &forward })
        place (*b, 36);
    for (auto* b : { &stop, &play, &record, &cycle })
        place (*b, 52);
    r.removeFromLeft (16);
    place (position, 128);
    place (time, 92);
    place (tempo, 96);
    place (signature, 44);
    r.removeFromLeft (16);
    place (metronome, 52);
    place (countIn, 110);
    place (punch, 54);
    place (snap, 50);
    place (snapBox, 76);
    place (follow, 56);
    place (keyboard, 50);
    place (theme, 58);
    cpu.setBounds (r.removeFromRight (std::min (r.getWidth(), 90)));
}
} // namespace weft::ui
