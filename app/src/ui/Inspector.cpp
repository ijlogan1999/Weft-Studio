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
        const auto name = regionName.getText().trim().toStdString();
        if (! name.empty() && ! refreshing && region() != nullptr)
            forRegions ("Rename Regions", Kind::any, [&] (const model::Region& r) { return studio.session().setRegionName (r.id, name); });
        else
            refresh();
    };
    describe (regionMute, Cmd::muteRegions);
    regionMute.onClick = [this] {
        const bool on = regionMute.getToggleState();
        forRegions ("Mute Regions", Kind::any, [&] (const model::Region& r) { return studio.session().setRegionMuted (r.id, on); });
    };
    describe (regionLoop, Cmd::loopRegion);
    regionLoop.onClick = [this] {
        const bool on = regionLoop.getToggleState();
        forRegions ("Loop Regions", Kind::any, [&] (const model::Region& r) {
            // Turning loop on repeats the region once; dragging the loop handle extends it.
            const auto end = on ? std::optional<Ticks> (static_cast<Ticks> (studio.model().regionEndTick (r)) * 2 - r.start) : std::nullopt;
            return studio.session().setRegionLoop (r.id, end);
        });
    };
    auto setupSlider = [this] (juce::Slider& sl, double lo, double hi, double step, const juce::String& suffix) {
        sl.setSliderStyle (juce::Slider::LinearHorizontal);
        sl.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, size::control);
        sl.setRange (lo, hi, step);
        // A dash while the selected regions differ (spec 04 §4); otherwise the value and its unit. The unit is
        // added here, not as the slider's suffix, so the dash stands alone.
        sl.textFromValueFunction = [this, &sl, step, suffix] (double v) {
            if (mixed.contains (&sl))
                return dash();
            return (step < 1.0 ? juce::String (v, 1) : juce::String (juce::roundToInt (v))) + suffix;
        };
    };
    setupSlider (regionGain, -30.0, 24.0, 0.1, " dB");
    regionGain.setDoubleClickReturnValue (true, 0.0);
    describe (regionGain, "Region gain", "Region gain in dB (double-click resets)");
    regionGain.onDragStart = [this] { studio.session().beginGesture ("Region Gain"); };
    regionGain.onDragEnd = [this] {
        studio.session().endGesture();
        studio.edited();
    };
    regionGain.onValueChange = [this] {
        if (! refreshing)
            forRegions ("Region Gain", Kind::audio, [&] (const model::Region& r) { return studio.session().setRegionGain (r.id, regionGain.getValue()); });
    };
    setupSlider (regionDelay, -500.0, 500.0, 0.1, " ms");
    regionDelay.setDoubleClickReturnValue (true, 0.0);
    describe (regionDelay, "Region delay", "Plays the region this much later, or earlier when negative (double-click resets)");
    regionDelay.onDragStart = [this] { studio.session().beginGesture ("Region Delay"); };
    regionDelay.onDragEnd = [this] {
        studio.session().endGesture();
        studio.edited();
    };
    regionDelay.onValueChange = [this] {
        if (! refreshing)
            forRegions ("Region Delay", Kind::any, [&] (const model::Region& r) { return studio.session().setRegionDelay (r.id, regionDelay.getValue()); });
    };
    setupSlider (fadeIn, 0.0, 10000.0, 1.0, " ms");
    setupSlider (fadeOut, 0.0, 10000.0, 1.0, " ms");
    describe (fadeIn, "Fade in", "Fade-in length in milliseconds");
    describe (fadeOut, "Fade out", "Fade-out length in milliseconds");
    for (auto* f : { &fadeIn, &fadeOut })
    {
        f->setSkewFactorFromMidPoint (500.0);
        f->onValueChange = [this, f] {
            if (refreshing)
                return;
            const auto samples = static_cast<Samples> (f->getValue() / 1000.0 * studio.model().info.sampleRate);
            forRegions (f == &fadeIn ? "Fade In" : "Fade Out", Kind::audio, [&] (const model::Region& r) {
                if (f == &fadeIn)
                    return studio.session().setRegionFades (
                        r.id, model::Fade { std::min (samples, r.length - r.audio.fadeOut.length), r.audio.fadeIn.curve }, std::nullopt);
                return studio.session().setRegionFades (
                    r.id, std::nullopt, model::Fade { std::min (samples, r.length - r.audio.fadeIn.length), r.audio.fadeOut.curve });
            });
        };
    }
    for (auto* c : { &fadeInCurve, &fadeOutCurve })
    {
        c->addItemList (curveNames, 1);
        c->onChange = [this, c] {
            if (refreshing || c->getSelectedItemIndex() < 0)
                return;
            const std::string curve = curveIds[c->getSelectedItemIndex()];
            forRegions ("Fade Curve", Kind::audio, [&] (const model::Region& r) {
                if (c == &fadeInCurve)
                    return studio.session().setRegionFades (r.id, model::Fade { r.audio.fadeIn.length, curve }, std::nullopt);
                return studio.session().setRegionFades (r.id, std::nullopt, model::Fade { r.audio.fadeOut.length, curve });
            });
        };
        c->setTextWhenNothingSelected (dash());
    }
    describe (fadeInCurve, "Fade-in curve");
    describe (fadeOutCurve, "Fade-out curve");

    regionQuantize.addItem ("Off", 1);
    for (std::size_t i = 1; i < gridValues().size(); ++i)
        regionQuantize.addItem (gridValues()[i].name, static_cast<int> (i) + 1);
    regionQuantize.setTextWhenNothingSelected (dash());
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
    setupSlider (regionTranspose, -96.0, 96.0, 1.0, " st");
    setupSlider (regionVelocity, -99.0, 99.0, 1.0, "");
    describe (regionTranspose, "Region transpose", "Plays the region's notes this many semitones up or down (double-click resets)");
    describe (regionVelocity, "Region velocity", "Adds to every note's velocity when the region plays (double-click resets)");
    for (auto* sl : { &regionTranspose, &regionVelocity })
    {
        sl->setDoubleClickReturnValue (true, 0.0);
        const auto name = sl == &regionTranspose ? "Region Transpose" : "Region Velocity";
        sl->onDragStart = [this, name] { studio.session().beginGesture (name); };
        sl->onDragEnd = [this] {
            studio.session().endGesture();
            studio.edited();
        };
        sl->onValueChange = [this, sl, name] {
            if (refreshing)
                return;
            const auto v = static_cast<int> (std::lround (sl->getValue()));
            forRegions (name, Kind::midi, [&] (const model::Region& r) {
                return sl == &regionTranspose ? studio.session().setRegionTranspose (r.id, v) : studio.session().setRegionVelocityOffset (r.id, v);
            });
        };
    }
    describe (applyQuantize, Cmd::applyQuantize);
    applyQuantize.onClick = [this] {
        forRegions ("Apply Quantize", Kind::midi, [&] (const model::Region& r) { return studio.session().applyRegionQuantize (r.id); });
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

    for (juce::Component* c : std::initializer_list<juce::Component*> { &regionName, &regionMute, &regionLoop, &regionGain, &regionDelay, &fadeIn, &fadeOut,
                                                                          &fadeInCurve, &fadeOutCurve, &regionQuantize, &quantizeStrength, &quantizeSwing,
                                                                          &applyQuantize, &regionTranspose, &regionVelocity, &notePitch, &noteVelocity, &noteLength, &notePosition,
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

std::vector<const model::Region*> Inspector::regions() const
{
    std::vector<const model::Region*> out;
    for (const auto& id : studio.selection.ids())
        if (const auto* r = studio.model().region (id))
            out.push_back (r);
    return out;
}

juce::String Inspector::dash() { return juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x94")); }

void Inspector::forRegions (std::string_view name, Kind kind, const std::function<commands::EditResult (const model::Region&)>& change)
{
    auto& session = studio.session();
    // A slider drag has its own gesture open; anything else on several regions becomes one undo step here.
    // One region keeps the step its own command names.
    const auto ids = studio.selection.ids();
    const bool own = ! session.inGesture() && ids.size() > 1;
    if (own)
        session.beginGesture (std::string (name));
    for (const auto& id : ids)
    {
        // Looked up again each time: every edit rebuilds the model.
        const auto* r = studio.model().region (id);
        if (r == nullptr || (kind == Kind::audio && r->isMidi) || (kind == Kind::midi && ! r->isMidi))
            continue;
        if (! studio.check (change (*r)))
            break;
    }
    if (own)
    {
        session.endGesture();
        studio.edited();
    }
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
    if (refreshing || regionQuantize.getSelectedId() <= 0)
        return;
    const auto index = regionQuantize.getSelectedId() - 1;
    std::optional<model::RegionQuantize> q;
    if (index > 0)
        q = model::RegionQuantize { gridValues()[static_cast<std::size_t> (index)].ticks, quantizeStrength.getValue() / 100.0, quantizeSwing.getValue() / 100.0 };
    forRegions ("Region Quantize", Kind::midi, [&] (const model::Region& r) { return studio.session().setRegionQuantize (r.id, q); });
}

void Inspector::refresh()
{
    const juce::ScopedValueSetter<bool> guard (refreshing, true);
    // Region section: with several regions selected each field shows their common value, or a
    // dash when they differ; an edit sets them all (spec 04 §4, AT-UI-10).
    const auto all = regions();
    const auto* r = all.empty() ? nullptr : all.front();
    const bool audio = r != nullptr && std::none_of (all.begin(), all.end(), [] (const model::Region* x) { return x->isMidi; });
    const bool midi = r != nullptr && std::all_of (all.begin(), all.end(), [] (const model::Region* x) { return x->isMidi; });
    const auto same = [&all] (auto field) {
        return std::all_of (all.begin(), all.end(), [&] (const model::Region* x) { return ! (field (*x) < field (*all.front())) && ! (field (*all.front()) < field (*x)); });
    };
    mixed.clear();
    const auto show = [this] (juce::Slider& sl, double value, bool allSame) {
        if (! allSame)
            mixed.insert (&sl);
        sl.setValue (value, juce::dontSendNotification);
        sl.updateText();
    };
    for (juce::Component* c : std::initializer_list<juce::Component*> { &regionName, &regionMute, &regionLoop, &regionDelay })
        c->setVisible (r != nullptr);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &regionGain, &fadeIn, &fadeOut, &fadeInCurve, &fadeOutCurve })
        c->setVisible (audio);
    if (r != nullptr)
    {
        regionName.setText (all.size() == 1 ? juce::String (r->name) : juce::String (static_cast<int> (all.size())) + " regions", juce::dontSendNotification);
        regionMute.setToggleState (std::all_of (all.begin(), all.end(), [] (const model::Region* x) { return x->muted; }), juce::dontSendNotification);
        regionLoop.setToggleState (std::all_of (all.begin(), all.end(), [] (const model::Region* x) { return x->loop && x->loop->enabled; }), juce::dontSendNotification);
        show (regionDelay, r->delayMs, same ([] (const model::Region& x) { return x.delayMs; }));
    }
    if (audio)
    {
        const auto rate = static_cast<double> (studio.model().info.sampleRate);
        show (regionGain, r->audio.gainDb, same ([] (const model::Region& x) { return x.audio.gainDb; }));
        show (fadeIn, static_cast<double> (r->audio.fadeIn.length) * 1000.0 / rate, same ([] (const model::Region& x) { return x.audio.fadeIn.length; }));
        show (fadeOut, static_cast<double> (r->audio.fadeOut.length) * 1000.0 / rate, same ([] (const model::Region& x) { return x.audio.fadeOut.length; }));
        fadeInCurve.setSelectedItemIndex (same ([] (const model::Region& x) { return x.audio.fadeIn.curve; }) ? curveIndex (r->audio.fadeIn.curve) : -1, juce::dontSendNotification);
        fadeOutCurve.setSelectedItemIndex (same ([] (const model::Region& x) { return x.audio.fadeOut.curve; }) ? curveIndex (r->audio.fadeOut.curve) : -1, juce::dontSendNotification);
    }
    for (juce::Component* c : std::initializer_list<juce::Component*> { &regionQuantize, &quantizeStrength, &quantizeSwing, &applyQuantize,
                                                                          &regionTranspose, &regionVelocity })
        c->setVisible (midi);
    if (midi)
    {
        show (regionTranspose, r->midi.transpose, same ([] (const model::Region& x) { return x.midi.transpose; }));
        show (regionVelocity, r->midi.velocityOffset, same ([] (const model::Region& x) { return x.midi.velocityOffset; }));
        const auto& q = r->midi.quantize;
        const bool sameGrid = same ([] (const model::Region& x) { return x.midi.quantize ? x.midi.quantize->grid : Ticks (-1); });
        int id = 1;
        for (std::size_t i = 1; q && i < gridValues().size(); ++i)
            if (gridValues()[i].ticks == q->grid)
                id = static_cast<int> (i) + 1;
        regionQuantize.setSelectedId (sameGrid ? id : 0, juce::dontSendNotification);
        show (quantizeStrength, q ? q->strength * 100.0 : 100.0, same ([] (const model::Region& x) { return x.midi.quantize ? x.midi.quantize->strength : 1.0; }));
        show (quantizeSwing, q ? q->swing * 100.0 : 0.0, same ([] (const model::Region& x) { return x.midi.quantize ? x.midi.quantize->swing : 0.0; }));
        const bool any = std::any_of (all.begin(), all.end(), [] (const model::Region* x) { return x->midi.quantize.has_value(); });
        quantizeStrength.setEnabled (any && sameGrid);
        quantizeSwing.setEnabled (any && sameGrid);
        applyQuantize.setEnabled (any);
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
        const auto mixedText = dash();
        const auto pitch = common ([] (const model::Note& n) { return n.pitch; });
        notePitch.setText (pitch ? juce::String (commands::noteName (*pitch)) : mixedText, juce::dontSendNotification);
        // Velocity shows the range of a mixed selection, like "64–110" (W-4 step 9).
        const auto [lowest, highest] = std::minmax_element (found.begin(), found.end(), [] (const model::Note* a, const model::Note* b) { return a->velocity < b->velocity; });
        noteVelocity.setText ((*lowest)->velocity == (*highest)->velocity
                                  ? juce::String ((*lowest)->velocity)
                                  : juce::String ((*lowest)->velocity) + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x93")) + juce::String ((*highest)->velocity),
                              juce::dontSendNotification);
        const auto length = common ([] (const model::Note& n) { return n.length; });
        noteLength.setText (length ? juce::String (*length) : mixedText, juce::dontSendNotification);
        const auto channel = common ([] (const model::Note& n) { return n.channel; });
        noteChannel.setText (channel ? juce::String (*channel) : mixedText, juce::dontSendNotification);
        std::optional<Ticks> position;
        for (std::size_t i = 0; i < found.size(); ++i)
        {
            const auto at = commands::noteTimelineTick (*studio.model().region (selected[i].regionId), found[i]->pos);
            position = i == 0 || (position && *position == at) ? std::optional (at) : std::nullopt;
            if (! position)
                break;
        }
        notePosition.setText (position ? juce::String (formatBarsBeats (*position, studio.model().signatures)) : mixedText, juce::dontSendNotification);
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
    label (regionDelay, "Delay");
    label (fadeIn, "Fade in");
    label (fadeOut, "Fade out");
    label (regionQuantize, "Quantize");
    label (quantizeStrength, "Strength");
    label (quantizeSwing, "Swing");
    label (regionTranspose, "Transpose");
    label (regionVelocity, "Velocity");
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
        regionDelay.setBounds (withLabel (row()));
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
            regionTranspose.setBounds (withLabel (row()));
            regionVelocity.setBounds (withLabel (row()));
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
