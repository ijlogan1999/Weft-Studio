#include "Inspector.h"

#include "PianoRoll.h"

#include "weft/time/Position.h"

namespace weft::ui
{
namespace
{
const juce::StringArray curveNames { "Linear", "Equal power", "S-curve" };
const char* curveIds[] = { "linear", "equalPower", "sCurve" };

int curveIndex (const std::string& id)
{
    for (int i = 0; i < 3; ++i)
        if (id == curveIds[i])
            return i;
    return 1;
}
} // namespace

Inspector::Inspector (Studio& s) : studio (s)
{
    setTitle ("Inspector");
    regionName.setEditable (false, true, true);
    regionName.setFont (font (Type::strong));
    regionName.setBorderSize ({});
    describe (regionName, "Region name", "Region name (double-click to rename)");
    regionName.onTextChange = [this] {
        if (const auto* r = region(); r != nullptr && regionName.getText().trim().isNotEmpty())
            studio.check (studio.session().setRegionName (r->id, regionName.getText().trim().toStdString()));
        else
            refresh();
    };
    describe (regionMute, Cmd::muteRegions);
    regionMute.onClick = [this] {
        if (const auto* r = region())
            studio.check (studio.session().setRegionMuted (r->id, regionMute.getToggleState()));
    };
    describe (regionLoop, Cmd::loopRegion);
    regionLoop.onClick = [this] {
        if (const auto* r = region())
        {
            const auto& m = studio.model();
            // Turning loop on repeats the region once; dragging the loop handle extends it.
            const auto end = regionLoop.getToggleState() ? std::optional<Ticks> (static_cast<Ticks> (m.regionEndTick (*r)) * 2 - r->start) : std::nullopt;
            studio.check (studio.session().setRegionLoop (r->id, end));
        }
    };
    auto setupSlider = [] (juce::Slider& sl, double lo, double hi, double step, const juce::String& suffix) {
        sl.setSliderStyle (juce::Slider::LinearHorizontal);
        sl.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, size::control);
        sl.setRange (lo, hi, step);
        sl.setTextValueSuffix (suffix);
    };
    setupSlider (regionGain, -30.0, 30.0, 0.1, " dB");
    regionGain.setDoubleClickReturnValue (true, 0.0);
    describe (regionGain, "Region gain", "Region gain in dB (double-click resets)");
    regionGain.onDragStart = [this] { studio.session().beginGesture ("Region Gain"); };
    regionGain.onDragEnd = [this] {
        studio.session().endGesture();
        studio.edited();
    };
    regionGain.onValueChange = [this] {
        if (const auto* r = region(); r != nullptr && ! refreshing)
            studio.check (studio.session().setRegionGain (r->id, regionGain.getValue()));
    };
    setupSlider (fadeIn, 0.0, 10000.0, 1.0, " ms");
    setupSlider (fadeOut, 0.0, 10000.0, 1.0, " ms");
    describe (fadeIn, "Fade in", "Fade-in length in milliseconds");
    describe (fadeOut, "Fade out", "Fade-out length in milliseconds");
    for (auto* f : { &fadeIn, &fadeOut })
    {
        f->setSkewFactorFromMidPoint (500.0);
        f->onValueChange = [this, f] {
            const auto* r = region();
            if (r == nullptr || refreshing || r->isMidi)
                return;
            const auto samples = static_cast<Samples> (f->getValue() / 1000.0 * studio.model().info.sampleRate);
            if (f == &fadeIn)
                studio.check (studio.session().setRegionFades (
                    r->id, model::Fade { std::min (samples, r->length - r->audio.fadeOut.length), r->audio.fadeIn.curve }, std::nullopt));
            else
                studio.check (studio.session().setRegionFades (
                    r->id, std::nullopt, model::Fade { std::min (samples, r->length - r->audio.fadeIn.length), r->audio.fadeOut.curve }));
        };
    }
    for (auto* c : { &fadeInCurve, &fadeOutCurve })
    {
        c->addItemList (curveNames, 1);
        c->onChange = [this, c] {
            const auto* r = region();
            if (r == nullptr || refreshing || r->isMidi)
                return;
            const std::string curve = curveIds[std::max (0, c->getSelectedItemIndex())];
            if (c == &fadeInCurve)
                studio.check (studio.session().setRegionFades (r->id, model::Fade { r->audio.fadeIn.length, curve }, std::nullopt));
            else
                studio.check (studio.session().setRegionFades (r->id, std::nullopt, model::Fade { r->audio.fadeOut.length, curve }));
        };
    }
    describe (fadeInCurve, "Fade-in curve");
    describe (fadeOutCurve, "Fade-out curve");

    regionQuantize.addItem ("Off", 1);
    for (std::size_t i = 1; i < gridValues().size(); ++i)
        regionQuantize.addItem (gridValues()[i].name, static_cast<int> (i) + 1);
    describe (regionQuantize, "Region quantize", "Playback quantize for the region: Off or a grid value (notes keep their positions)");
    regionQuantize.onChange = [this] { setRegionQuantize(); };
    setupSlider (quantizeStrength, 0.0, 100.0, 1.0, "%");
    setupSlider (quantizeSwing, 0.0, 100.0, 1.0, "%");
    describe (quantizeStrength, "Region quantize strength");
    describe (quantizeSwing, "Region quantize swing");
    for (auto* sl : { &quantizeStrength, &quantizeSwing })
    {
        sl->onDragStart = [this] { studio.session().beginGesture ("Region Quantize"); };
        sl->onDragEnd = [this] {
            studio.session().endGesture();
            studio.edited();
        };
        sl->onValueChange = [this] { setRegionQuantize(); };
    }
    describe (applyQuantize, Cmd::applyQuantize);
    applyQuantize.onClick = [this] {
        if (const auto* r = region())
            studio.check (studio.session().applyRegionQuantize (r->id));
    };

    const auto noteField = [this] (juce::Label& l, const juce::String& name, std::function<void (const juce::String&)> apply) {
        l.setEditable (true, true, true);
        setField (l);
        describe (l, name, name + " of the selected notes (click to type)");
        l.onTextChange = [this, &l, apply] {
            if (! refreshing && l.getText().trim().isNotEmpty() && l.getText() != juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x94")))
                apply (l.getText().trim());
            refresh();
        };
    };
    noteField (notePitch, "Pitch", [this] (const juce::String& t) {
        int pitch = -1;
        if (t.containsOnly ("0123456789"))
            pitch = t.getIntValue();
        else
            for (int p = 0; p < 128; ++p)
                if (t.equalsIgnoreCase (juce::String (commands::noteName (p))))
                    pitch = p;
        if (pitch < 0 || pitch > 127)
            studio.setStatus ("Pitch is a note name like C3 or a number 0-127.");
        else
            editNotes ("Pitch", [pitch] (model::Note& n) { n.pitch = pitch; });
    });
    noteField (noteVelocity, "Velocity", [this] (const juce::String& t) {
        studio.check (studio.session().setNotesVelocity (notes(), std::clamp (t.getIntValue(), 1, 127)));
    });
    noteField (noteLength, "Length", [this] (const juce::String& t) {
        const auto ticks = std::max<Ticks> (1, t.getLargeIntValue());
        editNotes ("Length", [ticks] (model::Note& n) { n.length = ticks; });
    });
    noteField (notePosition, "Position", [this] (const juce::String& t) {
        const auto at = parseBarsBeats (t.toStdString(), studio.model().signatures);
        if (! at)
        {
            studio.setStatus ("Position is bar beat division tick, like 2 1 1 1.");
            return;
        }
        auto& session = studio.session();
        session.beginGesture ("Position");
        for (const auto& ref : notes())
            if (const auto* n = commands::findNote (studio.model(), ref))
                session.setNoteSpan (ref, *at, n->length);
        session.endGesture();
        studio.edited();
    });
    noteField (noteChannel, "Channel", [this] (const juce::String& t) {
        const auto ch = std::clamp (t.getIntValue(), 1, 16);
        editNotes ("Channel", [ch] (model::Note& n) { n.channel = ch; });
    });

    trackName.setEditable (false, true, true);
    trackName.setFont (font (Type::strong));
    trackName.setBorderSize ({});
    describe (trackName, "Track name", "Track name (double-click to rename)");
    trackName.onTextChange = [this] {
        const auto text = trackName.getText().trim();
        if (text.isEmpty() || studio.selectedTrack().empty())
            refresh();
        else
            studio.check (studio.session().renameTrack (studio.selectedTrack(), text.toStdString()));
    };
    for (int i = 0; i < paletteSize; ++i)
        trackColour.addItem ("Colour " + juce::String (i + 1), i + 1);
    describe (trackColour, "Track colour");
    trackColour.onChange = [this] {
        if (! refreshing && ! studio.selectedTrack().empty() && trackColour.getSelectedId() > 0)
            studio.check (studio.session().setTrackColor (studio.selectedTrack(), model::Color { trackColour.getSelectedId() - 1, {} }));
    };
    monitor.addItemList ({ "Auto", "On", "Off" }, 1);
    describe (monitor, "Input monitoring", "Input monitoring: Auto, On or Off");
    monitor.onChange = [this] {
        static const char* modes[] = { "auto", "on", "off" };
        if (! refreshing && ! studio.selectedTrack().empty() && monitor.getSelectedItemIndex() >= 0)
            studio.check (studio.session().setTrackMonitor (studio.selectedTrack(), modes[monitor.getSelectedItemIndex()]));
    };

    for (juce::Component* c : std::initializer_list<juce::Component*> { &regionName, &regionMute, &regionLoop, &regionGain, &fadeIn, &fadeOut,
                                                                          &fadeInCurve, &fadeOutCurve, &regionQuantize, &quantizeStrength, &quantizeSwing,
                                                                          &applyQuantize, &notePitch, &noteVelocity, &noteLength, &notePosition,
                                                                          &noteChannel, &trackName, &trackColour, &monitor })
    {
        // Every control is reachable with Tab; a click doesn't take focus from the arrange window.
        c->setWantsKeyboardFocus (true);
        if (dynamic_cast<juce::Label*> (c) == nullptr)
            c->setMouseClickGrabsKeyboardFocus (false);
        c->setHasFocusOutline (true);
        addChildComponent (c);
    }
    studio.addChangeListener (this);
    refresh();
}

Inspector::~Inspector() { studio.removeChangeListener (this); }

const model::Region* Inspector::region() const
{
    const auto ids = studio.selection.ids();
    return ids.empty() ? nullptr : studio.model().region (ids.front());
}

std::vector<commands::NoteRef> Inspector::notes() const
{
    if (! studio.notesFocused())
        return {};
    return { studio.piano.selection.begin(), studio.piano.selection.end() };
}

void Inspector::editNotes (std::string_view name, const std::function<void (model::Note&)>& change)
{
    auto& s = studio.session();
    s.beginGesture (std::string (name));
    for (const auto& ref : notes())
        if (const auto* n = commands::findNote (studio.model(), ref))
        {
            auto copy = *n;
            change (copy);
            s.updateNote (ref.regionId, copy);
        }
    s.endGesture();
    studio.edited();
}

void Inspector::setRegionQuantize()
{
    const auto* r = region();
    if (r == nullptr || refreshing || ! r->isMidi)
        return;
    const auto index = regionQuantize.getSelectedId() - 1;
    std::optional<model::RegionQuantize> q;
    if (index > 0)
        q = model::RegionQuantize { gridValues()[static_cast<std::size_t> (index)].ticks, quantizeStrength.getValue() / 100.0, quantizeSwing.getValue() / 100.0 };
    studio.check (studio.session().setRegionQuantize (r->id, q));
}

void Inspector::refresh()
{
    const juce::ScopedValueSetter<bool> guard (refreshing, true);
    const auto* r = region();
    const bool audio = r != nullptr && ! r->isMidi;
    for (juce::Component* c : std::initializer_list<juce::Component*> { &regionName, &regionMute, &regionLoop })
        c->setVisible (r != nullptr);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &regionGain, &fadeIn, &fadeOut, &fadeInCurve, &fadeOutCurve })
        c->setVisible (audio);
    if (r != nullptr)
    {
        regionName.setText (juce::String (r->name), juce::dontSendNotification);
        regionMute.setToggleState (r->muted, juce::dontSendNotification);
        regionLoop.setToggleState (r->loop && r->loop->enabled, juce::dontSendNotification);
    }
    if (audio)
    {
        const auto rate = static_cast<double> (studio.model().info.sampleRate);
        regionGain.setValue (r->audio.gainDb, juce::dontSendNotification);
        fadeIn.setValue (static_cast<double> (r->audio.fadeIn.length) * 1000.0 / rate, juce::dontSendNotification);
        fadeOut.setValue (static_cast<double> (r->audio.fadeOut.length) * 1000.0 / rate, juce::dontSendNotification);
        fadeInCurve.setSelectedItemIndex (curveIndex (r->audio.fadeIn.curve), juce::dontSendNotification);
        fadeOutCurve.setSelectedItemIndex (curveIndex (r->audio.fadeOut.curve), juce::dontSendNotification);
    }
    const bool midi = r != nullptr && r->isMidi;
    for (juce::Component* c : std::initializer_list<juce::Component*> { &regionQuantize, &quantizeStrength, &quantizeSwing, &applyQuantize })
        c->setVisible (midi);
    if (midi)
    {
        const auto& q = r->midi.quantize;
        int id = 1;
        for (std::size_t i = 1; q && i < gridValues().size(); ++i)
            if (gridValues()[i].ticks == q->grid)
                id = static_cast<int> (i) + 1;
        regionQuantize.setSelectedId (id, juce::dontSendNotification);
        quantizeStrength.setValue (q ? q->strength * 100.0 : 100.0, juce::dontSendNotification);
        quantizeSwing.setValue (q ? q->swing * 100.0 : 0.0, juce::dontSendNotification);
        quantizeStrength.setEnabled (q.has_value());
        quantizeSwing.setEnabled (q.has_value());
        applyQuantize.setEnabled (q.has_value());
    }

    // Note section: one value, or a dash when the selected notes differ.
    const auto selected = notes();
    std::vector<const model::Note*> found;
    for (const auto& ref : selected)
        if (const auto* n = commands::findNote (studio.model(), ref))
            found.push_back (n);
    for (auto* l : { &notePitch, &noteVelocity, &noteLength, &notePosition, &noteChannel })
        l->setVisible (! found.empty());
    if (! found.empty())
    {
        const auto common = [&found] (auto field) {
            const auto v = field (*found.front());
            return std::all_of (found.begin(), found.end(), [&] (const model::Note* n) { return field (*n) == v; }) ? std::optional (v) : std::nullopt;
        };
        const juce::String mixed (juce::CharPointer_UTF8 ("\xe2\x80\x94"));
        const auto pitch = common ([] (const model::Note& n) { return n.pitch; });
        notePitch.setText (pitch ? juce::String (commands::noteName (*pitch)) : mixed, juce::dontSendNotification);
        // Velocity shows the range of a mixed selection, like "64–110" (W-4 step 9).
        const auto [lowest, highest] = std::minmax_element (found.begin(), found.end(), [] (const model::Note* a, const model::Note* b) { return a->velocity < b->velocity; });
        noteVelocity.setText ((*lowest)->velocity == (*highest)->velocity
                                  ? juce::String ((*lowest)->velocity)
                                  : juce::String ((*lowest)->velocity) + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x93")) + juce::String ((*highest)->velocity),
                              juce::dontSendNotification);
        const auto length = common ([] (const model::Note& n) { return n.length; });
        noteLength.setText (length ? juce::String (*length) : mixed, juce::dontSendNotification);
        const auto channel = common ([] (const model::Note& n) { return n.channel; });
        noteChannel.setText (channel ? juce::String (*channel) : mixed, juce::dontSendNotification);
        std::optional<Ticks> position;
        for (std::size_t i = 0; i < found.size(); ++i)
        {
            const auto at = commands::noteTimelineTick (*studio.model().region (selected[i].regionId), found[i]->pos);
            position = i == 0 || (position && *position == at) ? std::optional (at) : std::nullopt;
            if (! position)
                break;
        }
        notePosition.setText (position ? juce::String (formatBarsBeats (*position, studio.model().signatures)) : mixed, juce::dontSendNotification);
    }

    const auto* t = studio.model().track (studio.selectedTrack());
    for (juce::Component* c : std::initializer_list<juce::Component*> { &trackName, &trackColour, &monitor })
        c->setVisible (t != nullptr);
    if (t != nullptr)
    {
        trackName.setText (juce::String (t->name), juce::dontSendNotification);
        trackColour.setSelectedId (t->color.rgb.empty() ? t->color.palette + 1 : 0, juce::dontSendNotification);
        monitor.setSelectedItemIndex (t->monitor == "on" ? 1 : t->monitor == "off" ? 2 : 0, juce::dontSendNotification);
        monitor.setVisible (commands::LocalView::canArm (*t));
    }
    resized();
    repaint();
}

namespace
{
constexpr int labelX = space::s2, labelWidth = 72, controlX = labelX + labelWidth + space::s1;
}

void Inspector::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::surface));
    // Section headers: a caption on a slightly raised band, so Region, Note and Track read as
    // separate groups at a glance.
    auto header = [&] (int top, const juce::String& text) {
        const auto band = juce::Rectangle<int> (0, top, getWidth(), size::panelHeader);
        g.setColour (colour (Token::surfaceRaised));
        g.fillRect (band);
        depth::highlight (g, band.toFloat(), 0.0f);
        g.setColour (colour (Token::lineSubtle));
        g.fillRect (band.withTop (band.getBottom() - 1));
        g.setColour (colour (Token::textDim));
        g.setFont (font (Type::caption));
        g.drawText (captionText (text), labelX, top, getWidth() - 2 * labelX, size::panelHeader, juce::Justification::centredLeft);
    };
    if (regionName.isVisible())
        header (regionTop, "Region");
    if (notePitch.isVisible())
        header (noteTop, "Note");
    if (trackName.isVisible())
        header (trackTop, "Track");
    if (! regionName.isVisible() && ! trackName.isVisible())
    {
        g.setColour (colour (Token::textDim));
        g.setFont (font (Type::body));
        g.drawText ("Select a track or region.", getLocalBounds().reduced (space::s2), juce::Justification::centredTop);
    }
    g.setFont (font (Type::body));
    g.setColour (colour (Token::textDim));
    auto label = [&] (juce::Component& c, const juce::String& text) {
        if (c.isVisible())
            g.drawText (text, labelX, c.getY(), labelWidth, c.getHeight(), juce::Justification::centredLeft);
    };
    label (regionGain, "Gain");
    label (fadeIn, "Fade in");
    label (fadeOut, "Fade out");
    label (regionQuantize, "Quantize");
    label (quantizeStrength, "Strength");
    label (quantizeSwing, "Swing");
    label (notePitch, "Pitch");
    label (noteVelocity, "Velocity");
    label (noteLength, "Length");
    label (notePosition, "Position");
    label (noteChannel, "Channel");
    label (trackColour, "Colour");
    label (monitor, "Monitor");
}

void Inspector::resized()
{
    auto r = getLocalBounds();
    auto row = [&r] {
        auto out = r.removeFromTop (size::control).withTrimmedLeft (labelX).withTrimmedRight (space::s2);
        r.removeFromTop (space::half);
        return out;
    };
    auto header = [&r] {
        const auto top = r.getY();
        r.removeFromTop (size::panelHeader + space::s1);
        return top;
    };
    auto withLabel = [] (juce::Rectangle<int> b) { return b.withLeft (controlX); };
    if (regionName.isVisible())
    {
        regionTop = header();
        regionName.setBounds (row());
        auto toggles = row();
        regionMute.setBounds (toggles.removeFromLeft (toggles.getWidth() / 2));
        regionLoop.setBounds (toggles);
        if (regionGain.isVisible())
        {
            regionGain.setBounds (withLabel (row()));
            fadeIn.setBounds (withLabel (row()));
            fadeInCurve.setBounds (withLabel (row()));
            fadeOut.setBounds (withLabel (row()));
            fadeOutCurve.setBounds (withLabel (row()));
        }
        if (regionQuantize.isVisible())
        {
            regionQuantize.setBounds (withLabel (row()));
            quantizeStrength.setBounds (withLabel (row()));
            quantizeSwing.setBounds (withLabel (row()));
            applyQuantize.setBounds (withLabel (row()));
        }
        r.removeFromTop (space::s1);
    }
    if (notePitch.isVisible())
    {
        noteTop = header();
        for (auto* l : { &notePitch, &noteVelocity, &noteLength, &notePosition, &noteChannel })
            l->setBounds (withLabel (row()));
        r.removeFromTop (space::s1);
    }
    trackTop = header();
    trackName.setBounds (row());
    trackColour.setBounds (withLabel (row()));
    monitor.setBounds (withLabel (row()));
}
} // namespace weft::ui
