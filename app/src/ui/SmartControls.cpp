#include "SmartControls.h"

#include "Theme.h"

namespace weft::ui
{
namespace
{
constexpr int barHeight = 32;
constexpr int inspectorWidth = 300;
constexpr int rowHeight = 24;
constexpr int labelHeight = 16;
} // namespace

SmartControlsPanel::SmartControlsPanel (Studio& s) : studio (s)
{
    setTitle ("Smart Controls");
    addAndMakeVisible (title);
    title.setFont (font (Type::strong));
    for (const auto n : model::SmartControls::layouts)
        layoutBox.addItem (juce::String (n) + " Knobs", n);
    describe (layoutBox, "Layout", "How many knobs the track's Smart Controls have");
    layoutBox.onChange = [this] {
        if (! refreshing && layoutBox.getSelectedId() > 0)
            setLayout (layoutBox.getSelectedId());
    };
    addAndMakeVisible (layoutBox);
    describe (automatic, "Automatic", "Maps the knobs to the track's plug-ins, sends, Volume and Pan again");
    automatic.onClick = [this] { useAutomatic(); };
    addAndMakeVisible (automatic);
    describe (inspectorButton, "Smart Control Inspector", "Name a knob, choose what it controls and assign a hardware controller");
    inspectorButton.setClickingTogglesState (true);
    inspectorButton.onClick = [this] { setInspectorShown (inspectorButton.getToggleState()); };
    addAndMakeVisible (inspectorButton);

    describe (nameField, "Knob name", "The knob's name; empty, it's named after what it controls");
    nameField.onReturnKey = [this] { setKnobName (nameField.getText()); };
    nameField.onFocusLost = [this] { setKnobName (nameField.getText()); };
    addChildComponent (nameField);
    describe (addButton, "Add Mapping", "Another parameter for the knob to control");
    addButton.onClick = [this] {
        juce::PopupMenu menu;
        for (std::size_t i = 0; i < choices.size(); ++i)
            menu.addItem (static_cast<int> (i) + 1, juce::String (choices[i].name));
        juce::Component::SafePointer<SmartControlsPanel> self (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (addButton), [self] (int id) {
            if (self != nullptr && id > 0)
                self->addMapping (id - 1);
        });
    };
    addChildComponent (addButton);
    describe (learn, "Learn", "Turn a knob or move a fader on a MIDI controller to assign it to this knob");
    learn.onClick = [this] { learnController(); };
    addChildComponent (learn);
    describe (clear, "Clear", "The knob has no hardware controller");
    clear.onClick = [this] { clearController(); };
    addChildComponent (clear);
    addChildComponent (controllerLabel);
    controllerLabel.setFont (font (Type::small));

    studio.addChangeListener (this);
    refresh();
    startTimerHz (10);
}

SmartControlsPanel::~SmartControlsPanel()
{
    stopTimer();
    studio.removeChangeListener (this);
}

void SmartControlsPanel::buildKnobs (int count)
{
    knobs.clear();
    for (int i = 0; i < count; ++i)
    {
        auto k = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
        k->setRange (0.0, 1.0);
        describe (*k, "Smart Control " + juce::String (i + 1));
        k->onDragStart = [this, i] {
            selectKnob (i);
            studio.session().beginGesture ("Smart Control");
            gesture = true;
        };
        k->onValueChange = [this, i] {
            if (! refreshing)
                studio.turnSmartControl (trackId, i, knobs[static_cast<std::size_t> (i)]->getValue());
        };
        k->onDragEnd = [this, i] {
            if (gesture)
            {
                studio.session().endGesture();
                gesture = false;
            }
            studio.releaseSmartControl (trackId, i);
            studio.edited();
        };
        addAndMakeVisible (*k);
        knobs.push_back (std::move (k));
    }
}

void SmartControlsPanel::buildRows()
{
    rows.clear();
    const auto controls = studio.smartControls (trackId);
    if (! showInspector || selected >= static_cast<int> (controls.controls.size()))
        return;
    const auto& control = controls.controls[static_cast<std::size_t> (selected)];
    for (std::size_t m = 0; m < control.mappings.size(); ++m)
    {
        auto row = std::make_unique<MappingRow>();
        const auto& mapping = control.mappings[m];
        for (std::size_t c = 0; c < choices.size(); ++c)
            row->target.addItem (juce::String (choices[c].name), static_cast<int> (c) + 1);
        const auto key = mapping.target.key();
        const auto it = std::find_if (choices.begin(), choices.end(), [&] (const automation::TargetChoice& c) { return c.target.key() == key; });
        if (it != choices.end())
            row->target.setSelectedId (static_cast<int> (it - choices.begin()) + 1, juce::dontSendNotification);
        else
            row->target.setTextWhenNothingSelected ("(not on this track)");
        describe (row->target, "Mapping " + juce::String (static_cast<int> (m) + 1), "What this mapping controls");
        const auto index = static_cast<int> (m);
        row->target.onChange = [this, index] {
            if (! refreshing)
                setMappingTarget (index, rows[static_cast<std::size_t> (index)]->target.getSelectedId() - 1);
        };
        for (auto* sl : { &row->min, &row->max })
        {
            sl->setRange (0.0, 100.0, 1.0);
            sl->setTextValueSuffix ("%");
            sl->setChangeNotificationOnlyOnRelease (true);
            sl->onValueChange = [this, index] {
                if (refreshing)
                    return;
                const auto& r = *rows[static_cast<std::size_t> (index)];
                setMappingRange (index, r.min.getValue() / 100.0, r.max.getValue() / 100.0);
            };
        }
        describe (row->min, "Range minimum", "Where the mapping is with the knob all the way down");
        describe (row->max, "Range maximum", "Where the mapping is with the knob all the way up");
        row->min.setValue (mapping.min * 100.0, juce::dontSendNotification);
        row->max.setValue (mapping.max * 100.0, juce::dontSendNotification);
        describe (row->remove, "Remove mapping");
        row->remove.onClick = [this, index] { removeMapping (index); };
        for (juce::Component* c : std::initializer_list<juce::Component*> { &row->target, &row->min, &row->max, &row->remove })
            addAndMakeVisible (*c);
        rows.push_back (std::move (row));
    }
}

void SmartControlsPanel::refresh()
{
    const juce::ScopedValueSetter<bool> guard (refreshing, true);
    trackId = studio.selectedTrack();
    const auto* t = studio.model().track (trackId);
    if (t == nullptr)
    {
        trackId.clear();
        title.setText ("Select a track", juce::dontSendNotification);
        buildKnobs (0);
        rows.clear();
        for (juce::Component* c : std::initializer_list<juce::Component*> { &layoutBox, &automatic, &inspectorButton })
            c->setEnabled (false);
        resized();
        repaint();
        return;
    }
    for (juce::Component* c : std::initializer_list<juce::Component*> { &layoutBox, &automatic, &inspectorButton })
        c->setEnabled (true);
    choices = studio.automationTargets (trackId);
    const auto controls = studio.smartControls (trackId);
    title.setText (juce::String (t->name) + (t->smartControls ? "" : "  (automatic)"), juce::dontSendNotification);
    layoutBox.setSelectedId (controls.layout, juce::dontSendNotification);
    automatic.setEnabled (t->smartControls.has_value());
    inspectorButton.setToggleState (showInspector, juce::dontSendNotification);
    if (knobCount() != controls.layout)
        buildKnobs (controls.layout);
    selected = std::clamp (selected, 0, std::max (0, controls.layout - 1));
    updateKnobs();

    for (juce::Component* c : std::initializer_list<juce::Component*> { &nameField, &addButton, &learn, &clear, &controllerLabel })
        c->setVisible (showInspector);
    if (showInspector)
    {
        const auto& control = controls.controls[static_cast<std::size_t> (selected)];
        if (! nameField.hasKeyboardFocus (true))
            nameField.setText (juce::String (control.name), juce::dontSendNotification);
        nameField.setTextToShowWhenEmpty (studio.smartControlName (trackId, model::SmartControl { {}, control.mappings, -1, 0 }), colour (Token::textDim));
        const bool learning = studio.smartLearn && *studio.smartLearn == selected;
        learn.setToggleState (learning, juce::dontSendNotification);
        controllerLabel.setText (learning ? juce::String ("Move a controller")
                                 : control.cc >= 0 ? "CC " + juce::String (control.cc) + (control.ccChannel > 0 ? ", channel " + juce::String (control.ccChannel) : juce::String (", any channel"))
                                                   : juce::String ("No controller"),
                                 juce::dontSendNotification);
        clear.setEnabled (control.cc >= 0);
    }
    buildRows();
    resized();
    repaint();
}

void SmartControlsPanel::updateKnobs()
{
    if (trackId.empty())
        return;
    const auto controls = studio.smartControls (trackId);
    const juce::ScopedValueSetter<bool> guard (refreshing, true);
    for (std::size_t i = 0; i < knobs.size() && i < controls.controls.size(); ++i)
    {
        auto& k = *knobs[i];
        const auto position = studio.smartControlPosition (trackId, controls.controls[i]);
        k.setEnabled (position.has_value());
        if (! k.isMouseButtonDown())
            k.setValue (position.value_or (0.0), juce::dontSendNotification);
    }
}

void SmartControlsPanel::timerCallback()
{
    // Knobs follow automation and changes made elsewhere (the mixer, a plug-in window).
    if (isShowing())
    {
        updateKnobs();
        repaint (knobArea());
    }
}

void SmartControlsPanel::selectKnob (int index)
{
    if (index < 0 || index >= knobCount() || index == selected)
        return;
    selected = index;
    if (studio.smartLearn)
        studio.smartLearn = selected;
    refresh();
}

void SmartControlsPanel::setInspectorShown (bool shown)
{
    showInspector = shown;
    refresh();
}

void SmartControlsPanel::apply (const std::function<void (model::SmartControls&)>& change)
{
    if (trackId.empty())
        return;
    auto controls = studio.smartControls (trackId);
    const auto before = controls.toValue();
    change (controls);
    if (controls.toValue() == before && studio.model().track (trackId)->smartControls)
        return;
    studio.check (studio.session().setSmartControls (trackId, controls));
    studio.edited();
}

void SmartControlsPanel::setLayout (int knobsInLayout)
{
    if (trackId.empty())
        return;
    // From the automatic mapping, the new layout is mapped automatically too.
    auto controls = studio.smartControls (trackId, knobsInLayout);
    studio.check (studio.session().setSmartControls (trackId, controls));
    studio.edited();
}

void SmartControlsPanel::setKnobName (const juce::String& name)
{
    apply ([&] (model::SmartControls& c) {
        if (selected < static_cast<int> (c.controls.size()))
            c.controls[static_cast<std::size_t> (selected)].name = name.trim().toStdString();
    });
}

void SmartControlsPanel::addMapping (int choice)
{
    if (choice < 0 || choice >= static_cast<int> (choices.size()))
        return;
    const auto target = choices[static_cast<std::size_t> (choice)].target;
    apply ([&] (model::SmartControls& c) {
        if (selected < static_cast<int> (c.controls.size()))
            c.controls[static_cast<std::size_t> (selected)].mappings.push_back ({ target, 0.0, 1.0 });
    });
}

void SmartControlsPanel::setMappingTarget (int mapping, int choice)
{
    if (choice < 0 || choice >= static_cast<int> (choices.size()))
        return;
    const auto target = choices[static_cast<std::size_t> (choice)].target;
    apply ([&] (model::SmartControls& c) {
        auto& maps = c.controls[static_cast<std::size_t> (selected)].mappings;
        if (mapping >= 0 && mapping < static_cast<int> (maps.size()))
            maps[static_cast<std::size_t> (mapping)].target = target;
    });
}

void SmartControlsPanel::setMappingRange (int mapping, double min, double max)
{
    apply ([&] (model::SmartControls& c) {
        auto& maps = c.controls[static_cast<std::size_t> (selected)].mappings;
        if (mapping >= 0 && mapping < static_cast<int> (maps.size()))
        {
            maps[static_cast<std::size_t> (mapping)].min = std::clamp (min, 0.0, 1.0);
            maps[static_cast<std::size_t> (mapping)].max = std::clamp (max, 0.0, 1.0);
        }
    });
}

void SmartControlsPanel::removeMapping (int mapping)
{
    apply ([&] (model::SmartControls& c) {
        auto& maps = c.controls[static_cast<std::size_t> (selected)].mappings;
        if (mapping >= 0 && mapping < static_cast<int> (maps.size()))
            maps.erase (maps.begin() + mapping);
    });
}

void SmartControlsPanel::learnController()
{
    if (studio.smartLearn && *studio.smartLearn == selected)
        studio.smartLearn.reset();
    else
        studio.smartLearn = selected;
    refresh();
}

void SmartControlsPanel::clearController()
{
    studio.smartLearn.reset();
    apply ([&] (model::SmartControls& c) {
        c.controls[static_cast<std::size_t> (selected)].cc = -1;
        c.controls[static_cast<std::size_t> (selected)].ccChannel = 0;
    });
}

void SmartControlsPanel::useAutomatic()
{
    if (trackId.empty())
        return;
    studio.check (studio.session().setSmartControls (trackId, std::nullopt));
    studio.edited();
}

juce::Rectangle<int> SmartControlsPanel::inspectorArea() const
{
    auto r = getLocalBounds().withTrimmedTop (barHeight);
    return showInspector ? r.removeFromLeft (inspectorWidth) : juce::Rectangle<int>();
}

juce::Rectangle<int> SmartControlsPanel::knobArea() const
{
    auto r = getLocalBounds().withTrimmedTop (barHeight);
    if (showInspector)
        r.removeFromLeft (inspectorWidth);
    return r.reduced (space::s2);
}

void SmartControlsPanel::resized()
{
    auto bar = getLocalBounds().removeFromTop (barHeight).reduced (space::s1, space::half);
    title.setBounds (bar.removeFromLeft (220));
    inspectorButton.setBounds (bar.removeFromRight (90));
    bar.removeFromRight (space::s1);
    automatic.setBounds (bar.removeFromRight (90));
    bar.removeFromRight (space::s1);
    layoutBox.setBounds (bar.removeFromRight (100));

    // Knobs: a row of up to eight; twelve make two rows of six.
    const auto area = knobArea();
    const int count = knobCount();
    if (count > 0)
    {
        const int columns = count > 8 ? count / 2 : count;
        const int rowsOfKnobs = (count + columns - 1) / columns;
        const int cellW = area.getWidth() / columns;
        const int cellH = area.getHeight() / rowsOfKnobs;
        for (int i = 0; i < count; ++i)
        {
            const auto cell = juce::Rectangle<int> (area.getX() + (i % columns) * cellW, area.getY() + (i / columns) * cellH, cellW, cellH);
            const int size = std::max (16, std::min (cell.getWidth() - space::s2, cell.getHeight() - 2 * labelHeight - space::s1));
            knobs[static_cast<std::size_t> (i)]->setBounds (juce::Rectangle<int> (size, size).withCentre (cell.getCentre()));
        }
    }

    if (! showInspector)
        return;
    auto r = inspectorArea().reduced (space::s2);
    r.removeFromTop (labelHeight); // "Knob n"
    nameField.setBounds (r.removeFromTop (rowHeight));
    r.removeFromTop (space::s1 + labelHeight); // "Mappings"
    for (auto& row : rows)
    {
        auto line = r.removeFromTop (rowHeight);
        row->remove.setBounds (line.removeFromRight (64));
        row->target.setBounds (line.withTrimmedRight (space::half));
        auto range = r.removeFromTop (rowHeight);
        row->min.setBounds (range.removeFromLeft (range.getWidth() / 2).withTrimmedRight (space::half));
        row->max.setBounds (range);
        r.removeFromTop (space::half);
    }
    addButton.setBounds (r.removeFromTop (rowHeight).removeFromLeft (120));
    r.removeFromTop (space::s1 + labelHeight); // "Controller"
    auto hw = r.removeFromTop (rowHeight);
    learn.setBounds (hw.removeFromLeft (64));
    hw.removeFromLeft (space::half);
    clear.setBounds (hw.removeFromLeft (64));
    hw.removeFromLeft (space::s1);
    controllerLabel.setBounds (hw);
}

void SmartControlsPanel::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::surface));
    g.setColour (colour (Token::border));
    g.drawHorizontalLine (barHeight - 1, 0.0f, static_cast<float> (getWidth()));
    if (trackId.empty())
    {
        g.setColour (colour (Token::textDim));
        g.setFont (font (Type::body));
        g.drawText ("Select a track to see its Smart Controls.", knobArea(), juce::Justification::centred);
        return;
    }
    const auto controls = studio.smartControls (trackId);
    for (int i = 0; i < knobCount() && i < static_cast<int> (controls.controls.size()); ++i)
    {
        const auto& k = *knobs[static_cast<std::size_t> (i)];
        const auto& control = controls.controls[static_cast<std::size_t> (i)];
        const auto box = k.getBounds();
        if (showInspector && i == selected)
        {
            g.setColour (colour (Token::accent));
            g.drawRoundedRectangle (box.expanded (space::half).toFloat(), radius::r1, 1.0f);
        }
        const auto name = studio.smartControlName (trackId, control);
        g.setColour (colour (k.isEnabled() ? Token::text : Token::textDim));
        g.setFont (font (Type::small));
        g.drawText (name.isEmpty() ? juce::String ("Unassigned") : name, box.getX() - 30, box.getY() - labelHeight, box.getWidth() + 60, labelHeight,
                    juce::Justification::centred, true);
        g.setColour (colour (Token::textDim));
        g.setFont (font (Type::monoSmall));
        g.drawText (studio.smartControlText (trackId, control), box.getX() - 30, box.getBottom(), box.getWidth() + 60, labelHeight, juce::Justification::centred, true);
    }
    if (showInspector)
    {
        const auto area = inspectorArea();
        g.setColour (colour (Token::surfaceRaised));
        g.fillRect (area);
        g.setColour (colour (Token::border));
        g.drawVerticalLine (area.getRight(), static_cast<float> (area.getY()), static_cast<float> (area.getBottom()));
        g.setColour (colour (Token::textDim));
        g.setFont (font (Type::caption));
        const auto x = area.getX() + space::s2;
        const auto w = area.getWidth() - 2 * space::s2;
        g.drawText (captionText ("Knob " + juce::String (selected + 1)), x, nameField.getY() - labelHeight, w, labelHeight, juce::Justification::centredLeft);
        g.drawText (captionText ("Mappings"), x, nameField.getBottom() + space::s1, w, labelHeight, juce::Justification::centredLeft);
        g.drawText (captionText ("Controller"), x, learn.getY() - labelHeight, w, labelHeight, juce::Justification::centredLeft);
    }
}
} // namespace weft::ui
