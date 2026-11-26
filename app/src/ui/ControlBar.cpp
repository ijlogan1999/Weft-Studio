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
        else if (*typed != studio.model().info.key)
            studio.check (studio.session().setKey (*typed));
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
    time.setText (juce::String::formatted ("%d:%06.3f", static_cast<int> (seconds / 60.0), std::fmod (std::max (seconds, 0.0), 60.0)),
                  juce::dontSendNotification);
    if (! tempo.isBeingEdited())
        tempo.setText (juce::String (m.tempo.bpmAt (t), 2), juce::dontSendNotification);
    if (! signature.isBeingEdited())
    {
        const auto& sig = m.signatures.signatureAtBar (m.signatures.barAt (t).bar);
        signature.setText (juce::String (sig.numerator) + "/" + juce::String (sig.denominator), juce::dontSendNotification);
    }
    if (! key.isBeingEdited())
        key.setText (keyText (m.info.key), juce::dontSendNotification);
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
    if (! countIn.isVisible())
    {
        const auto chosen = model::countInIndex (studio.engine().settings().countInBars);
        for (int i = 0; i < countIn.getNumItems(); ++i)
            menu.addItem (overflowCountIn + i, countIn.getItemText (i), true, chosen == i);
    }
    if (! cpu.isVisible())
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
    return { { &inspector, &side, &mixer, &editor, &global },
             { &start, &rewind, &forward, &stop, &play, &record, &cycle },
             { &metronome, &punch },
             { &snap },
             { &follow, &keyboard } };
}

juce::Rectangle<int> ControlBar::displayBounds() const
{
    return position.getBounds().getUnion (key.getBounds()).expanded (space::s1, 0).withY (space::half).withHeight (getHeight() - space::s1);
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
    for (auto* b : { &side, &mixer, &editor, &global })
        items.push_back ({ b, 56 });
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
            it.c->setVisible (it.dropOrder >= 0);
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
    time.setVisible (position.isVisible());
    position.setBounds (counter.withY (space::s1 - 1).withHeight (24));
    time.setBounds (counter.withY (position.getBottom() - 2).withHeight (14));
}
} // namespace weft::ui
