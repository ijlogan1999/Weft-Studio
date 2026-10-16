#include "ControlBar.h"

#include "weft/time/Position.h"

namespace weft::ui
{
ControlBar::ControlBar (Studio& s) : studio (s)
{
    addChildComponent (avatars);
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
    if (avatars.preferredWidth() != avatarsWidth)
        resized();
    avatars.repaint();
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
    // Natural widths; gaps are entries with no component. On narrow windows the time display,
    // CPU meter and count-in drop out first, then everything shrinks together.
    struct Item
    {
        juce::Component* c;
        int width;
        int dropOrder = 0; // 0 = always shown; 1 drops first
    };
    std::vector<Item> items;
    auto gap = [&] (int w) { items.push_back ({ nullptr, w }); };
    for (auto* b : { &inspector, &side, &mixer, &editor, &global })
        items.push_back ({ b, b == &inspector ? 70 : 58 });
    gap (10);
    for (auto* b : { &start, &rewind, &forward })
        items.push_back ({ b, 34 });
    for (auto* b : { &stop, &play, &record, &cycle })
        items.push_back ({ b, 50 });
    gap (12);
    items.push_back ({ &position, 112 });
    items.push_back ({ &time, 86, 1 });
    items.push_back ({ &tempo, 86 });
    items.push_back ({ &signature, 42 });
    gap (12);
    items.push_back ({ &metronome, 48 });
    items.push_back ({ &countIn, 104, 3 });
    items.push_back ({ &punch, 52 });
    items.push_back ({ &snap, 48 });
    items.push_back ({ &snapBox, 72 });
    items.push_back ({ &follow, 56 });
    items.push_back ({ &keyboard, 46 });
    items.push_back ({ &theme, 56 });
    items.push_back ({ &cpu, 80, 2 });
    avatarsWidth = avatars.preferredWidth();
    if (avatarsWidth > 0)
    {
        gap (8);
        items.push_back ({ &avatars, avatarsWidth });
    }
    else
        avatars.setVisible (false);

    const auto area = getLocalBounds().reduced (8, 8);
    constexpr int spacing = 4;
    auto total = [&] {
        int t = 0;
        for (const auto& it : items)
            if (it.c == nullptr || it.c->isVisible())
                t += it.width + (it.c != nullptr ? spacing : 0);
        return t;
    };
    for (auto& it : items)
        if (it.c != nullptr)
            it.c->setVisible (true);
    for (int order = 1; order <= 3 && total() > area.getWidth(); ++order)
        for (auto& it : items)
            if (it.dropOrder == order)
                it.c->setVisible (false);
    const auto scale = std::clamp (static_cast<double> (area.getWidth()) / std::max (1, total()), 0.7, 1.0);

    auto r = area;
    for (const auto& it : items)
    {
        const auto w = juce::roundToInt (it.width * scale);
        if (it.c == nullptr)
            r.removeFromLeft (w);
        else if (it.c->isVisible())
        {
            it.c->setBounds (r.removeFromLeft (w));
            r.removeFromLeft (spacing);
        }
    }
}
} // namespace weft::ui
