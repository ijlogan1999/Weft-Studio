#include "ControlBar.h"

#include "Mixer.h"

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
            // Right-click on Play or Stop: a menu of where to play from or go (D-UI-35).
            if (button->popupClick && button->popupMenu)
            {
                button->showPopup();
                return;
            }
            if (button == &stop)
            {
                studio.perform (Cmd::stop); // stopped already: back to the start (D-UI-17)
                return;
            }
            if (button == &play && studio.playing())
                return;
            // Ctrl+click on Previous/Next Bar goes to the previous/next marker (D-UI-26).
            if ((button == &rewind || button == &forward) && juce::ModifierKeys::currentModifiers.isCommandDown())
            {
                studio.perform (button == &rewind ? Cmd::previousMarker : Cmd::nextMarker);
                return;
            }
            studio.perform (button->command);
        };
    }
    const auto commandMenu = [] (std::initializer_list<Cmd> cmds) {
        juce::PopupMenu menu;
        for (auto c : cmds)
            menu.addItem (static_cast<int> (c) + 1, commandInfo (c).name);
        return menu;
    };
    play.popupMenu = [commandMenu] {
        return commandMenu ({ Cmd::playFromStart, Cmd::playFromCycleStart, Cmd::playFromSelection, Cmd::playFromWindowEdge, Cmd::playFromPreviousBar,
                              Cmd::playFromLastLocate });
    };
    stop.popupMenu = [commandMenu] { return commandMenu ({ Cmd::stopGoToStart, Cmd::stopGoToCycleStart, Cmd::stopGoToLastLocate }); };
    for (auto* b : { &play, &stop })
        b->popupPicked = [this] (int item) { studio.perform (static_cast<Cmd> (item - 1)); };
    describe (stop, "Stop", "Stop (Space). Stopped already: go to the start of the cycle or project. Right-click: stop and go to...");
    describe (play, "Play", "Play (Space). Right-click: play from...");
    describe (rewind, "Previous Bar", "Previous Bar (,). Ctrl+click: previous marker");
    describe (forward, "Next Bar", "Next Bar (.). Ctrl+click: next marker");
    // Icons for the transport; the labels stay as the accessible names.
    start.setIcon (icon::goToStart());
    rewind.setIcon (icon::rewind());
    forward.setIcon (icon::forward());
    stop.setIcon (icon::stop());
    play.setIcon (icon::play());
    record.setIcon (icon::record());
    cycle.setIcon (icon::cycle());
    setRole (record, ButtonRole::record, true);
    // Buttons are grouped into segmented controls; each group paints one frame (paintGroup).
    for (const auto& group : groups())
        for (std::size_t i = 0; i < group.size(); ++i)
            group[i]->setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0) | (i + 1 < group.size() ? juce::Button::ConnectedOnRight : 0));

    for (auto* l : { &position, &time, &tempo, &signature, &key, &cpu })
    {
        l->setJustificationType (juce::Justification::centredLeft);
        l->setFont (font (Type::monoValue));
        l->setBorderSize ({});
        addAndMakeVisible (*l);
    }
    position.setFont (font (Type::monoCounter));
    time.setFont (font (Type::monoSmall));
    cpu.setFont (font (Type::monoSmall));
    cpu.setJustificationType (juce::Justification::centredRight);
    describe (position, "Position", "Position in bars and beats (double-click to type)");
    describe (time, "Time", "Position in time");
    describe (tempo, "Tempo", "Tempo at the playhead (double-click to type)");
    describe (signature, "Time signature", "Time signature at the playhead (double-click to type, e.g. 3/4)");
    describe (key, "Key", "The project's key (double-click to type, e.g. F# minor or Bb)");
    describe (cpu, "CPU", "Audio thread load and dropouts");
    position.setEditable (false, true);
    tempo.setEditable (false, true);
    signature.setEditable (false, true);
    key.setEditable (false, true);
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
    key.onTextChange = [this] {
        const auto typed = model::KeySignature::parse (key.getText().toStdString());
        if (! typed)
            studio.setStatus ("Type a key like C, F# minor or Bbm.");
        else if (*typed != studio.model().keyAt (studio.playhead()))
            studio.check (studio.session().setKeyAt (studio.playhead(), *typed)); // the key in effect here (D-UI-44)
        refresh();
    };

    for (const auto choice : model::countInChoices)
        countIn.addItem (choice == 0 ? juce::String ("No count-in") : juce::String (std::abs (choice)) + (choice > 0 ? "-bar" : "-beat") + " count-in",
                         countIn.getNumItems() + 1);
    describe (countIn, "Count-in", "Count-in before recording");
    countIn.onChange = [this] {
        auto settings = studio.engine().settings();
        settings.countInBars = model::countInChoices[static_cast<std::size_t> (std::max (0, countIn.getSelectedItemIndex()))];
        studio.engine().setSettings (settings);
    };
    addAndMakeVisible (countIn);
    // What a narrow window drops stays reachable from this menu (AT-UI-11).
    describe (more, "More", "More controls: the ones this window is too narrow to show");
    more.setMouseClickGrabsKeyboardFocus (false);
    more.onClick = [this] {
        overflowMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (more),
                                      [safe = juce::Component::SafePointer<ControlBar> (this)] (int item) {
                                          if (safe != nullptr)
                                              safe->pickOverflow (item);
                                      });
    };
    addChildComponent (more);

    // The master control beside the modes (D-UI-43): the master's fader, one undo step a drag.
    masterVolume.setNormalisableRange (fader::range());
    masterVolume.setDoubleClickReturnValue (true, 0.0);
    describe (masterVolume, "Master volume", "The master's volume (double-click for 0 dB). Right-click the bar to show an output meter instead");
    masterVolume.onDragStart = [this] { studio.session().beginGesture ("Volume"); };
    masterVolume.onDragEnd = [this] { studio.session().endGesture(); };
    masterVolume.onValueChange = [this] {
        const auto v = masterVolume.getValue();
        studio.check (studio.session().setTrackVolume (studio.model().info.masterTrackId, v <= fader::minDb + 1e-6 ? fader::silentDb : v));
        studio.edited();
    };
    addChildComponent (masterVolume);
    addChildComponent (outputMeter);
    // Right-click on the display (or anywhere on the bar) customizes it; double-click on the CPU reading opens a meter window.
    for (auto* l : { &position, &time, &tempo, &signature, &key, &cpu })
        l->addMouseListener (this, false);
    outputMeter.addMouseListener (this, false);
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
    return { &inspector, &side, &mixer, &editor, &global, &toolbarToggle, &start, &rewind, &forward, &stop, &play, &record, &cycle,
             &metronome, &punch, &snap, &follow, &keyboard };
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
    toolbarToggle.setToggleState (l.toolbar, juce::dontSendNotification);
    if (const auto* master = m.track (m.info.masterTrackId); master != nullptr && ! masterVolume.isMouseButtonDown())
        masterVolume.setValue (std::clamp (master->strip.volumeDb, fader::minDb, fader::maxDb), juce::dontSendNotification);
    if (! (shownBar == l.bar))
    {
        shownBar = l.bar;
        resized();
    }
    play.setToggleState (studio.playing(), juce::dontSendNotification);
    record.setToggleState (studio.recording(), juce::dontSendNotification);
    cycle.setToggleState (m.info.cycle.enabled, juce::dontSendNotification);
    metronome.setToggleState (studio.engine().settings().metronomeOn, juce::dontSendNotification);
    punch.setToggleState (m.info.punch.enabled, juce::dontSendNotification);
    snap.setToggleState (studio.snapEnabled, juce::dontSendNotification);
    follow.setToggleState (studio.follow, juce::dontSendNotification);
    keyboard.setToggleState (studio.keyboardMidiOn(), juce::dontSendNotification);
    countIn.setSelectedItemIndex (model::countInIndex (studio.engine().settings().countInBars), juce::dontSendNotification);
    snapBox.setText (juce::String (arrange::snapLabel (studio.snap)), juce::dontSendNotification);
    time.setColour (juce::Label::textColourId, colour (Token::textDim));
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
    time.setText (formatDisplayTime (seconds, m.info.sampleRate, studio.layout.bar.timeFormat), juce::dontSendNotification);
    if (! tempo.isBeingEdited())
    {
        const auto decimals = studio.layout.bar.tempoDecimals;
        tempo.setText (decimals == 0 ? juce::String (juce::roundToInt (m.tempo.bpmAt (t))) : juce::String (m.tempo.bpmAt (t), decimals), juce::dontSendNotification);
    }
    if (outputMeter.isVisible())
        outputMeter.update();
    if (! signature.isBeingEdited())
    {
        const auto& sig = m.signatures.signatureAtBar (m.signatures.barAt (t).bar);
        signature.setText (juce::String (sig.numerator) + "/" + juce::String (sig.denominator), juce::dontSendNotification);
    }
    if (! key.isBeingEdited())
        key.setText (keyText (m.keyAt (t)), juce::dontSendNotification);
    const auto load = studio.engine().cpuLoad();
    cpu.setText ("CPU " + juce::String (juce::roundToInt (load * 100.0)) + "%", juce::dontSendNotification);
    const auto level = load > 0.9 || studio.engine().dropouts() > 0 ? Token::record : load > 0.7 ? Token::warning : Token::textDim;
    cpu.setColour (juce::Label::textColourId, colour (level));
    // The counts a hardware test pass reads off (docs/hardware-test-pass.md).
    const auto counts = "Audio thread load. Dropouts: " + juce::String (studio.engine().dropouts()) + ", disk underruns: "
                        + juce::String (studio.engine().diskUnderruns());
    if (cpu.getTooltip() != counts)
        cpu.setTooltip (counts);
    play.setToggleState (studio.playing(), juce::dontSendNotification);
    record.setToggleState (studio.recording(), juce::dontSendNotification);
}

juce::PopupMenu ControlBar::overflowMenu() const
{
    juce::PopupMenu menu;
    if (! countIn.isVisible() && allowed (&countIn))
    {
        const auto chosen = model::countInIndex (studio.engine().settings().countInBars);
        for (int i = 0; i < countIn.getNumItems(); ++i)
            menu.addItem (overflowCountIn + i, countIn.getItemText (i), true, chosen == i);
    }
    if (! cpu.isVisible() && allowed (&cpu))
    {
        menu.addSeparator();
        menu.addItem (overflowCpu, cpu.getText() + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 dropouts ")) + juce::String (studio.engine().dropouts())
                                       + ", disk underruns " + juce::String (studio.engine().diskUnderruns()),
                      false);
    }
    return menu;
}

void ControlBar::pickOverflow (int item)
{
    if (item < overflowCountIn || item >= overflowCountIn + countIn.getNumItems())
        return;
    auto settings = studio.engine().settings();
    settings.countInBars = model::countInChoices[static_cast<std::size_t> (item - overflowCountIn)];
    studio.engine().setSettings (settings);
    studio.sendChangeMessage();
}

std::vector<std::vector<juce::Button*>> ControlBar::groups()
{
    return { { &inspector, &side, &mixer, &editor, &global, &toolbarToggle },
             { &start, &rewind, &forward, &stop, &play, &record, &cycle },
             { &metronome, &punch },
             { &snap },
             { &follow, &keyboard } };
}

juce::Rectangle<int> ControlBar::displayBounds() const
{
    auto box = position.getBounds();
    for (const auto* l : { &tempo, &signature, &key })
        if (l->isVisible())
            box = box.getUnion (l->getBounds());
    return box.expanded (space::s1, 0).withY (space::half).withHeight (getHeight() - space::s1);
}

void ControlBar::paint (juce::Graphics& g)
{
    // The bar is the top of the hierarchy: the lightest band, lit from above.
    auto r = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient::vertical (colour (Token::raisedTop), 0.0f, colour (Token::surfaceRaised), r.getBottom()));
    g.fillRect (r);
    depth::highlight (g, r, 0.0f);
    g.setColour (colour (Token::border));
    g.fillRect (r.removeFromBottom (1.0f));
    for (const auto& group : groups())
    {
        juce::Rectangle<int> box;
        for (auto* b : group)
            if (b->isVisible())
                box = box.isEmpty() ? b->getBounds() : box.getUnion (b->getBounds());
        if (box.isEmpty())
            continue;
        paintGroup (g, box);
        g.setColour (colour (Token::lineSubtle));
        for (std::size_t i = 1; i < group.size(); ++i)
            if (group[i]->isVisible())
                g.fillRect (group[i]->getX(), box.getY() + 5, 1, box.getHeight() - 10);
    }
    // The counter: a well pressed into the bar, with small captions over the tempo and signature.
    const auto display = displayBounds().toFloat();
    depth::well (g, display, radius::r2);
    g.setFont (font (Type::monoSmall));
    g.setColour (colour (Token::textDim));
    for (auto [label, caption] : { std::pair<juce::Label*, const char*> { &tempo, "BPM" }, { &signature, "SIG" }, { &key, "KEY" } })
        if (label->isVisible())
            g.drawText (caption, label->getX(), label->getY() - 14, label->getWidth(), 14, juce::Justification::bottomLeft);
}

void ControlBar::resized()
{
    // Natural widths on the 8 px grid; gaps are entries with no component. On narrow windows the
    // time display, CPU meter and count-in drop out first, then everything shrinks together.
    struct Item
    {
        juce::Component* c;
        int width;
        int dropOrder = 0; // 0 = always shown; 1 drops first
    };
    std::vector<Item> items;
    auto gap = [&] (int w) { items.push_back ({ nullptr, w }); };
    items.push_back ({ &inspector, 72 });
    for (auto* b : { &side, &mixer, &editor, &global, &toolbarToggle })
        items.push_back ({ b, b == &toolbarToggle ? 64 : 56 });
    gap (space::s1);
    for (auto* b : { &start, &rewind, &forward, &stop, &play, &record, &cycle })
        items.push_back ({ b, 32 });
    gap (space::s2); // the counter's well reaches 8 px past its values on each side
    items.push_back ({ &position, 136 });
    items.push_back ({ &tempo, 64 });
    items.push_back ({ &signature, 40 });
    items.push_back ({ &key, 64 });
    gap (space::s2);
    items.push_back ({ &metronome, 48 });
    items.push_back ({ &punch, 56 });
    items.push_back ({ &countIn, 128, 3 });
    items.push_back ({ &masterVolume, 96, 1 });
    items.push_back ({ &outputMeter, 96, 1 });
    items.push_back ({ &snap, 48 });
    items.push_back ({ &snapBox, 80 });
    items.push_back ({ &follow, 56 });
    items.push_back ({ &keyboard, 48 });
    items.push_back ({ &cpu, 64, 2 });
    items.push_back ({ &more, 32, -1 }); // only while something is dropped
    avatarsWidth = avatars.preferredWidth();
    if (avatarsWidth > 0)
    {
        gap (space::s1);
        items.push_back ({ &avatars, avatarsWidth });
    }
    else
        avatars.setVisible (false);

    const auto area = getLocalBounds().reduced (space::s1, 0).withSizeKeepingCentre (getWidth() - space::s2, size::control + 2);
    auto spacingAfter = [&] (const Item& it, const Item* next) {
        // Segments of a group touch; separate controls get 8 px.
        if (it.c == nullptr || next == nullptr || next->c == nullptr)
            return 0;
        const auto* a = dynamic_cast<juce::Button*> (it.c);
        const auto* b = dynamic_cast<juce::Button*> (next->c);
        if (a != nullptr && b != nullptr && a->isConnectedOnRight() && b->isConnectedOnLeft())
            return 0;
        return space::s1;
    };
    auto total = [&] {
        int t = 0;
        for (std::size_t i = 0; i < items.size(); ++i)
            if (items[i].c == nullptr || items[i].c->isVisible())
                t += items[i].width + spacingAfter (items[i], i + 1 < items.size() ? &items[i + 1] : nullptr);
        return t;
    };
    for (auto& it : items)
        if (it.c != nullptr)
            it.c->setVisible (it.dropOrder >= 0 && allowed (it.c));
    for (int order = 1; order <= 3 && total() > area.getWidth(); ++order)
        for (auto& it : items)
            if (it.dropOrder == order)
            {
                it.c->setVisible (false);
                if (it.c == &countIn || it.c == &cpu)
                    more.setVisible (true);
            }
    const auto scale = std::clamp (static_cast<double> (area.getWidth()) / std::max (1, total()), 0.7, 1.0);

    auto r = area;
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        const auto& it = items[i];
        const auto w = juce::roundToInt (it.width * scale);
        if (it.c == nullptr)
            r.removeFromLeft (w);
        else if (it.c->isVisible())
        {
            it.c->setBounds (r.removeFromLeft (w));
            r.removeFromLeft (spacingAfter (it, i + 1 < items.size() ? &items[i + 1] : nullptr));
        }
    }
    // Inside the counter: bars and beats over the time; tempo and signature values on the lower
    // line under their captions (painted).
    for (auto* l : { &tempo, &signature, &key })
        l->setBounds (l->getBounds().withTop (getHeight() / 2 - 2).withHeight (18));
    const auto counter = position.getBounds();
    time.setVisible (position.isVisible() && studio.layout.bar.shows (ControlBarLayout::timeDisplay));
    position.setBounds (counter.withY (space::s1 - 1).withHeight (24));
    time.setBounds (counter.withY (position.getBottom() - 2).withHeight (14));
}
// Customize Control Bar (D-UI-43) -----------------------------------------------

namespace
{
struct ShowItem
{
    ControlBarLayout::Item item;
    const char* name;
};
constexpr ShowItem showItems[] = {
    { ControlBarLayout::panelButtons, "View Buttons" },        { ControlBarLayout::toolbarButton, "Toolbar Button" },
    { ControlBarLayout::transportMoves, "Go to Start, Previous and Next Bar" },
    { ControlBarLayout::timeDisplay, "Time" },                 { ControlBarLayout::tempoDisplay, "Tempo" },
    { ControlBarLayout::signatureDisplay, "Time Signature" },  { ControlBarLayout::keyDisplay, "Key" },
    { ControlBarLayout::clickAndPunch, "Metronome and Autopunch" }, { ControlBarLayout::countInMenu, "Count-in" },
    { ControlBarLayout::snapControls, "Snap and Grid" },       { ControlBarLayout::followButton, "Follow" },
    { ControlBarLayout::keysButton, "Keyboard MIDI" },         { ControlBarLayout::cpuMeter, "CPU Meter" },
};
int bitIndex (ControlBarLayout::Item item)
{
    int i = 0;
    while ((1 << i) != static_cast<int> (item))
        ++i;
    return i;
}
} // namespace

bool ControlBar::allowed (const juce::Component* c) const
{
    using L = ControlBarLayout;
    const auto& bar = studio.layout.bar;
    if (c == &inspector || c == &side || c == &mixer || c == &editor || c == &global)
        return bar.shows (L::panelButtons);
    if (c == &toolbarToggle)
        return bar.shows (L::toolbarButton);
    if (c == &start || c == &rewind || c == &forward)
        return bar.shows (L::transportMoves);
    if (c == &tempo)
        return bar.shows (L::tempoDisplay);
    if (c == &signature)
        return bar.shows (L::signatureDisplay);
    if (c == &key)
        return bar.shows (L::keyDisplay);
    if (c == &metronome || c == &punch)
        return bar.shows (L::clickAndPunch);
    if (c == &countIn)
        return bar.shows (L::countInMenu);
    if (c == &snap || c == &snapBox)
        return bar.shows (L::snapControls);
    if (c == &follow)
        return bar.shows (L::followButton);
    if (c == &keyboard)
        return bar.shows (L::keysButton);
    if (c == &cpu)
        return bar.shows (L::cpuMeter);
    if (c == &masterVolume)
        return bar.master == L::Master::volume;
    if (c == &outputMeter)
        return bar.master == L::Master::meter;
    return true;
}

juce::PopupMenu ControlBar::customizeMenu() const
{
    using L = ControlBarLayout;
    const auto& bar = studio.layout.bar;
    juce::PopupMenu menu;
    menu.addSectionHeader ("Show in the Control Bar");
    for (const auto& s : showItems)
        menu.addItem (itemShowBase + bitIndex (s.item), s.name, true, bar.shows (s.item));
    juce::PopupMenu master;
    master.addItem (itemMasterBase + 1, "Master Volume", true, bar.master == L::Master::volume);
    master.addItem (itemMasterBase + 2, "Output Meter", true, bar.master == L::Master::meter);
    master.addItem (itemMasterBase, "None", true, bar.master == L::Master::none);
    menu.addSubMenu ("Master Volume or Output Meter", master);
    menu.addSeparator();
    juce::PopupMenu timeFormat;
    static const char* const formats[] = { "Minutes, Seconds and Milliseconds", "Samples", "Timecode at 24 fps", "Timecode at 25 fps", "Timecode at 30 fps" };
    for (int i = 0; i < 5; ++i)
        timeFormat.addItem (itemTimeBase + i, formats[i], true, static_cast<int> (bar.timeFormat) == i);
    menu.addSubMenu ("Display Time As", timeFormat);
    juce::PopupMenu tempoFormat;
    tempoFormat.addItem (itemTempoBase + 0, "BPM Without Decimals", true, bar.tempoDecimals == 0);
    tempoFormat.addItem (itemTempoBase + 2, "BPM With Two Decimals", true, bar.tempoDecimals == 2);
    tempoFormat.addItem (itemTempoBase + 4, "BPM With Four Decimals", true, bar.tempoDecimals == 4);
    menu.addSubMenu ("Display Tempo As", tempoFormat);
    menu.addSeparator();
    menu.addItem (itemBigBeats, commandInfo (Cmd::bigBeatsDisplay).name);
    menu.addItem (itemBigTime, commandInfo (Cmd::bigTimeDisplay).name);
    menu.addItem (itemPerformance, commandInfo (Cmd::performanceMeter).name);
    menu.addSeparator();
    menu.addItem (itemApplyDefaults, "Apply Defaults", ! (bar == studio.layout.barDefault));
    menu.addItem (itemSaveDefault, "Save As Default", ! (bar == studio.layout.barDefault));
    menu.addItem (itemRevert, "Revert", canRevert);
    return menu;
}

void ControlBar::changeLayout (const ControlBarLayout& next)
{
    if (next == studio.layout.bar)
        return;
    beforeLastChange = studio.layout.bar;
    canRevert = true;
    studio.layout.bar = next;
    studio.sendChangeMessage();
    refresh();
}

void ControlBar::pickCustomize (int item)
{
    using L = ControlBarLayout;
    auto next = studio.layout.bar;
    if (item >= itemShowBase && item < itemShowBase + 13)
        next.items ^= 1 << (item - itemShowBase);
    else if (item >= itemMasterBase && item <= itemMasterBase + 2)
        next.master = static_cast<L::Master> (item - itemMasterBase);
    else if (item >= itemTimeBase && item < itemTimeBase + 5)
        next.timeFormat = static_cast<L::TimeFormat> (item - itemTimeBase);
    else if (item == itemTempoBase || item == itemTempoBase + 2 || item == itemTempoBase + 4)
        next.tempoDecimals = item - itemTempoBase;
    else if (item == itemBigBeats || item == itemBigTime || item == itemPerformance)
    {
        studio.perform (item == itemBigBeats ? Cmd::bigBeatsDisplay : item == itemBigTime ? Cmd::bigTimeDisplay : Cmd::performanceMeter);
        return;
    }
    else if (item == itemApplyDefaults)
        next = studio.layout.barDefault;
    else if (item == itemSaveDefault)
    {
        studio.layout.barDefault = studio.layout.bar;
        studio.sendChangeMessage();
        studio.setStatus ("This control bar is now the default.");
        return;
    }
    else if (item == itemRevert)
    {
        if (! canRevert)
            return;
        const auto back = beforeLastChange;
        changeLayout (back);
        return;
    }
    else
        return;
    changeLayout (next);
}

void ControlBar::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        return;
    customizeMenu().showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe = juce::Component::SafePointer<ControlBar> (this)] (int item) {
        if (safe != nullptr)
            safe->pickCustomize (item);
    });
}

void ControlBar::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.eventComponent != &cpu)
        return;
    // Bigger with Ctrl, biggest with Ctrl+Alt+Shift.
    const int scale = e.mods.isCommandDown() ? (e.mods.isAltDown() && e.mods.isShiftDown() ? 3 : 2) : 1;
    if (onPerformanceMeter)
        onPerformanceMeter (scale);
    else
        studio.perform (Cmd::performanceMeter);
}
} // namespace weft::ui
