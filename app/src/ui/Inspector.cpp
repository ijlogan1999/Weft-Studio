#include "Inspector.h"

#include "PianoRoll.h"

#include "weft/engine/dsp/BuiltinEffects.h"
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

// EQ thumbnail ------------------------------------------------------------------------------

EqThumbnail::EqThumbnail (Studio& s) : studio (s)
{
    setTitle ("EQ");
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void EqThumbnail::setTrack (const Id& trackId)
{
    track = trackId;
    shownValues.clear();
    repaint();
}

Id EqThumbnail::eqId() const
{
    if (const auto* t = studio.model().track (track))
        for (const auto& p : t->strip.inserts)
            if (p.pluginUid == engine::WeftEq::pluginUid)
                return p.id;
    return {};
}

std::vector<double> EqThumbnail::parameterValues() const
{
    std::vector<double> out;
    if (const auto id = eqId(); ! id.empty())
        if (const auto proc = studio.processor (id))
            for (std::size_t i = 0; i < proc->parameters().size(); ++i)
                out.push_back (proc->parameter (static_cast<int> (i)));
    return out;
}

std::vector<double> EqThumbnail::curve (int points) const
{
    std::vector<double> out (static_cast<std::size_t> (std::max (points, 0)), 0.0);
    const auto id = eqId();
    const auto eqProc = id.empty() ? nullptr : std::dynamic_pointer_cast<engine::WeftEq> (studio.processor (id));
    const auto* t = studio.model().track (track);
    bool bypassed = false;
    if (t != nullptr)
        for (const auto& p : t->strip.inserts)
            if (p.id == id)
                bypassed = p.bypass;
    if (eqProc == nullptr || bypassed)
        return out;
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        const auto x = out.size() == 1 ? 0.0 : static_cast<double> (i) / static_cast<double> (out.size() - 1);
        out[i] = eqProc->responseDb (lowHz * std::pow (highHz / lowHz, x));
    }
    return out;
}

void EqThumbnail::open()
{
    const auto* t = studio.model().track (track);
    if (t == nullptr)
        return;
    if (const auto id = eqId(); ! id.empty())
    {
        if (studio.openPluginEditor)
            studio.openPluginEditor (id);
        return;
    }
    if (t->strip.inserts.size() >= static_cast<std::size_t> (model::maxInserts))
    {
        studio.setStatus ("No free insert slot for an EQ.");
        return;
    }
    for (const auto& choice : studio.pluginChoices (false))
        if (choice.plugin.pluginUid == engine::WeftEq::pluginUid)
        {
            studio.addPlugin (track, choice);
            return;
        }
}

void EqThumbnail::poll()
{
    if (parameterValues() != shownValues)
        repaint();
}

void EqThumbnail::mouseUp (const juce::MouseEvent& e)
{
    if (isEnabled() && getLocalBounds().contains (e.getPosition()))
        open();
}

void EqThumbnail::paint (juce::Graphics& g)
{
    shownValues = parameterValues();
    const auto area = getLocalBounds().toFloat().reduced (0.5f);
    // An inset well, like the meters: the curve is a readout, not a control surface.
    depth::well (g, area, radius::r2);
    const auto plot = area.reduced (space::half, space::half);
    const auto yFor = [&plot] (double db) {
        return plot.getCentreY() - static_cast<float> (std::clamp (db, -rangeDb, rangeDb) / rangeDb) * plot.getHeight() / 2.0f;
    };
    const auto xFor = [&plot] (double hz) { return plot.getX() + plot.getWidth() * static_cast<float> (std::log (hz / lowHz) / std::log (highHz / lowHz)); };
    // Grid: 100 Hz, 1 kHz and 10 kHz, and the 0 dB line.
    g.setColour (colour (Token::lineSubtle));
    for (const auto hz : { 100.0, 1000.0, 10000.0 })
        g.fillRect (juce::Rectangle<float> (std::round (xFor (hz)), plot.getY(), 1.0f, plot.getHeight()));
    g.fillRect (juce::Rectangle<float> (plot.getX(), std::round (yFor (0.0)), plot.getWidth(), 1.0f));

    const auto id = eqId();
    if (id.empty())
    {
        g.setColour (colour (Token::textDim));
        g.setFont (font (Type::small));
        g.drawText ("EQ  +", getLocalBounds().reduced (space::s1, 0), juce::Justification::centred);
        setTooltip ("EQ: click to add a Weft EQ to this track");
        return;
    }
    const auto points = std::max (2, static_cast<int> (plot.getWidth()));
    const auto values = curve (points);
    juce::Path line;
    for (int i = 0; i < points; ++i)
    {
        const auto x = plot.getX() + plot.getWidth() * static_cast<float> (i) / static_cast<float> (points - 1);
        const auto y = yFor (values[static_cast<std::size_t> (i)]);
        if (i == 0)
            line.startNewSubPath (x, y);
        else
            line.lineTo (x, y);
    }
    auto fill = line;
    fill.lineTo (plot.getRight(), yFor (0.0));
    fill.lineTo (plot.getX(), yFor (0.0));
    fill.closeSubPath();
    const auto* t = studio.model().track (track);
    const auto tint = t != nullptr ? colourFor (t->color.palette, t->color.rgb) : colour (Token::accent);
    g.setColour (tint.withAlpha (0.25f));
    g.fillPath (fill);
    g.setColour (tint);
    g.strokePath (line, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (colour (Token::textDim));
    g.setFont (font (Type::caption));
    g.drawText ("EQ", area.reduced (space::half + 2.0f, 2.0f), juce::Justification::topLeft, false);
    setTooltip ("EQ: click to open");
}

// Inspector ---------------------------------------------------------------------------------

Inspector::Inspector (Studio& s) : eq (s), studio (s)
{
    setTitle ("Inspector");
    viewport.setViewedComponent (&body, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (space::s1);
    addAndMakeVisible (viewport);
    body.addChildComponent (eq);
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
    setupSlider (regionGain, -30.0, 24.0, 0.1, " dB");
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
    setupSlider (regionDelay, -500.0, 500.0, 0.1, " ms");
    regionDelay.setDoubleClickReturnValue (true, 0.0);
    describe (regionDelay, "Region delay", "Plays the region this much later, or earlier when negative (double-click resets)");
    regionDelay.onDragStart = [this] { studio.session().beginGesture ("Region Delay"); };
    regionDelay.onDragEnd = [this] {
        studio.session().endGesture();
        studio.edited();
    };
    regionDelay.onValueChange = [this] {
        if (const auto* r = region(); r != nullptr && ! refreshing)
            studio.check (studio.session().setRegionDelay (r->id, regionDelay.getValue()));
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
        sl->onValueChange = [this, sl] {
            const auto* r = region();
            if (r == nullptr || refreshing || ! r->isMidi)
                return;
            const auto v = static_cast<int> (std::lround (sl->getValue()));
            studio.check (sl == &regionTranspose ? studio.session().setRegionTranspose (r->id, v)
                                                 : studio.session().setRegionVelocityOffset (r->id, v));
        };
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
        body.addChildComponent (c);
    }
    studio.addChangeListener (this);
    refresh();
    startTimerHz (30);
}

Inspector::~Inspector()
{
    studio.removeChangeListener (this);
    viewport.setViewedComponent (nullptr, false);
}

void Inspector::timerCallback()
{
    if (! isShowing())
        return;
    const auto now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    for (auto* strip : { trackStrip.get(), outputStrip.get() })
        if (strip != nullptr && strip->isVisible())
        {
            strip->updateMeter (now);
            strip->followAutomation();
        }
    if (eq.isVisible())
        eq.poll();
}

void Inspector::refreshStrips()
{
    const auto& m = studio.model();
    const auto* t = m.track (studio.selectedTrack());
    Id outputId;
    if (t != nullptr && t->kind != model::TrackKind::master)
        outputId = t->output.busId.empty() ? m.info.masterTrackId : t->output.busId;
    if (m.track (outputId) == nullptr)
        outputId.clear();
    const auto place = [this] (std::unique_ptr<ChannelStrip>& strip, const Id& id) {
        if (id.empty())
        {
            strip.reset();
            return;
        }
        if (strip == nullptr || strip->trackId() != id)
        {
            strip = std::make_unique<ChannelStrip> (studio, id);
            body.addAndMakeVisible (*strip);
        }
        strip->insertRows = strip->sendRows = 1;
        strip->refresh();
    };
    place (trackStrip, t != nullptr ? t->id : Id());
    place (outputStrip, outputId);
    if (trackStrip != nullptr)
        trackStrip->highlightSelection = false;
    if (outputStrip != nullptr)
    {
        // Clicking the output's strip edits it without moving the selection off the track.
        outputStrip->selectsOnClick = false;
        outputStrip->highlightSelection = false;
        // The two strips' slot rows match, so their faders line up.
        const auto inserts = std::max (trackStrip->shownInsertRows(), outputStrip->shownInsertRows());
        const auto sendRows = std::max (trackStrip->shownSendRows(), outputStrip->shownSendRows());
        for (auto* strip : { trackStrip.get(), outputStrip.get() })
        {
            strip->insertRows = inserts;
            strip->sendRows = sendRows;
        }
    }
    eq.setVisible (t != nullptr);
    if (t != nullptr && eq.trackId() != t->id)
        eq.setTrack (t->id);
    eq.setEnabled (t != nullptr && ! studio.lockedByOther (t->id));
    eq.repaint();
}

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
    for (juce::Component* c : std::initializer_list<juce::Component*> { &regionName, &regionMute, &regionLoop, &regionDelay })
        c->setVisible (r != nullptr);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &regionGain, &fadeIn, &fadeOut, &fadeInCurve, &fadeOutCurve })
        c->setVisible (audio);
    if (r != nullptr)
    {
        regionName.setText (juce::String (r->name), juce::dontSendNotification);
        regionMute.setToggleState (r->muted, juce::dontSendNotification);
        regionLoop.setToggleState (r->loop && r->loop->enabled, juce::dontSendNotification);
        regionDelay.setValue (r->delayMs, juce::dontSendNotification);
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
    for (juce::Component* c : std::initializer_list<juce::Component*> { &regionQuantize, &quantizeStrength, &quantizeSwing, &applyQuantize,
                                                                          &regionTranspose, &regionVelocity })
        c->setVisible (midi);
    if (midi)
    {
        regionTranspose.setValue (r->midi.transpose, juce::dontSendNotification);
        regionVelocity.setValue (r->midi.velocityOffset, juce::dontSendNotification);
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
    refreshStrips();
    resized();
    body.repaint();
}

namespace
{
constexpr int labelX = space::s2, labelWidth = 72, controlX = labelX + labelWidth + space::s1;
}

void Inspector::paintBody (juce::Graphics& g)
{
    g.fillAll (colour (Token::surface));
    // Section headers: a caption on a slightly raised band, so Region, Note, Track and Channel read as
    // separate groups at a glance.
    auto header = [&] (int top, const juce::String& text) {
        const auto band = juce::Rectangle<int> (0, top, body.getWidth(), size::panelHeader);
        g.setColour (colour (Token::surfaceRaised));
        g.fillRect (band);
        depth::highlight (g, band.toFloat(), 0.0f);
        g.setColour (colour (Token::lineSubtle));
        g.fillRect (band.withTop (band.getBottom() - 1));
        g.setColour (colour (Token::textDim));
        g.setFont (font (Type::caption));
        g.drawText (captionText (text), labelX, top, body.getWidth() - 2 * labelX, size::panelHeader, juce::Justification::centredLeft);
    };
    if (regionName.isVisible())
        header (regionTop, "Region");
    if (notePitch.isVisible())
        header (noteTop, "Note");
    if (trackName.isVisible())
        header (trackTop, "Track");
    if (channelTop >= 0)
        header (channelTop, "Channel");
    if (! regionName.isVisible() && ! trackName.isVisible())
    {
        g.setColour (colour (Token::textDim));
        g.setFont (font (Type::body));
        g.drawText ("Select a track or region.", body.getLocalBounds().reduced (space::s2), juce::Justification::centredTop);
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
    viewport.setBounds (getLocalBounds());
    const auto visible = getHeight();
    auto width = getWidth();
    auto height = layout (width, visible);
    if (height > visible)
        height = layout (width -= viewport.getScrollBarThickness(), visible);
    body.setSize (width, height);
}

int Inspector::layout (int width, int visibleHeight)
{
    auto r = juce::Rectangle<int> (0, 0, width, 1 << 20);
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
    if (monitor.isVisible())
        monitor.setBounds (withLabel (row()));
    if (trackStrip == nullptr)
    {
        channelTop = -1;
        return std::max (visibleHeight, r.getY());
    }
    // Channel: the EQ curve, then the two strips side by side filling the rest of the panel
    // (taller than that when their slots need it; the panel scrolls).
    r.removeFromTop (space::s1);
    channelTop = header();
    eq.setBounds (r.removeFromTop (space::s6).reduced (space::s1, 0));
    r.removeFromTop (space::s1);
    const auto preferred = std::max (trackStrip->preferredHeight(), outputStrip != nullptr ? outputStrip->preferredHeight() : 0);
    const auto stripHeight = std::max (preferred, visibleHeight - r.getY() - space::s1);
    auto strips = r.removeFromTop (stripHeight).reduced (space::s1, 0);
    const auto stripWidth = (strips.getWidth() - space::s1) / 2;
    trackStrip->setBounds (strips.removeFromLeft (stripWidth));
    strips.removeFromLeft (space::s1);
    if (outputStrip != nullptr)
        outputStrip->setBounds (strips.withWidth (stripWidth));
    return r.getY() + space::s1;
}
} // namespace weft::ui
