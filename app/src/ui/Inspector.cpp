#include "Inspector.h"

#include "PianoRoll.h"

#include "weft/engine/dsp/BuiltinEffects.h"
#include "weft/engine/midi/JuceMidiInputs.h"
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
        if (refreshing)
            return;
        const bool ticks = regionDelayUnit.getSelectedId() == 2;
        const auto v = regionDelay.getValue();
        forRegions ("Region Delay", Kind::any, [&] (const model::Region& r) {
            return ticks ? studio.session().setRegionDelayTicks (r.id, static_cast<Ticks> (std::llround (v))) : studio.session().setRegionDelay (r.id, v);
        });
    };
    regionDelay.textFromValueFunction = [this] (double v) {
        if (mixed.contains (&regionDelay))
            return dash();
        return regionDelayUnit.getSelectedId() == 2 ? juce::String (juce::roundToInt (v)) : juce::String (v, 1);
    };
    regionDelayUnit.addItem ("ms", 1);
    regionDelayUnit.addItem ("ticks", 2);
    describe (regionDelayUnit, "Region delay unit", "Delay in milliseconds or in ticks (960 to a quarter note)");
    regionDelayUnit.onChange = [this] {
        if (refreshing || regionDelayUnit.getSelectedId() == 0)
            return;
        const bool ticks = regionDelayUnit.getSelectedId() == 2;
        // Switching the unit starts the delay at zero.
        forRegions ("Region Delay", Kind::any, [&] (const model::Region& r) {
            return ticks ? studio.session().setRegionDelayTicks (r.id, 0) : studio.session().setRegionDelay (r.id, 0.0);
        });
    };
    describe (regionReverse, Cmd::reverseRegions);
    regionReverse.onClick = [this] {
        studio.perform (Cmd::reverseRegions);
        refresh();
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
    setupSlider (quantizeRange, -960.0, 960.0, 1.0, "");
    setupSlider (quantizeFlam, -960.0, 960.0, 1.0, "");
    describe (quantizeRange, "Q-Range", "Positive: only notes this close to the grid move. Negative: only notes further away. 0: all (ticks)");
    describe (quantizeFlam, "Q-Flam", "Spreads notes that quantize together this many ticks apart, upwards (or downwards when negative)");
    for (auto* sl : { &quantizeRange, &quantizeFlam })
    {
        sl->setDoubleClickReturnValue (true, 0.0);
        sl->onDragStart = [this] { studio.session().beginGesture ("Region Quantize"); };
        sl->onDragEnd = [this] {
            studio.session().endGesture();
            studio.edited();
        };
        sl->onValueChange = [this] { setRegionQuantize(); };
    }
    for (const auto& [value, name] : dynamicsValues())
        regionDynamics.addItem (name, regionDynamics.getNumItems() + 1);
    regionDynamics.setTextWhenNothingSelected (dash());
    describe (regionDynamics, "Region dynamics", "Widens or narrows the differences between velocities; Fixed plays them all alike");
    regionDynamics.onChange = [this] {
        const auto i = regionDynamics.getSelectedItemIndex();
        if (refreshing || i < 0)
            return;
        const auto v = dynamicsValues()[static_cast<std::size_t> (i)].first;
        forRegions ("Region Dynamics", Kind::midi, [&] (const model::Region& r) { return studio.session().setRegionDynamics (r.id, v); });
    };
    for (const auto& [value, name] : gateValues())
        regionGate.addItem (name, regionGate.getNumItems() + 1);
    regionGate.setTextWhenNothingSelected (dash());
    describe (regionGate, "Region gate time", "Plays notes shorter or longer by a percent; Fixed plays them short, Legato ties each to the next");
    regionGate.onChange = [this] {
        const auto i = regionGate.getSelectedItemIndex();
        if (refreshing || i < 0)
            return;
        const auto v = gateValues()[static_cast<std::size_t> (i)].first;
        forRegions ("Region Gate Time", Kind::midi, [&] (const model::Region& r) { return studio.session().setRegionGate (r.id, v); });
    };
    describe (regionClipLength, "Clip Length", "Notes stop at the region's end; off, they play their whole length");
    regionClipLength.onClick = [this] {
        const bool on = regionClipLength.getToggleState();
        forRegions ("Region Clip Length", Kind::midi, [&] (const model::Region& r) { return studio.session().setRegionClipLength (r.id, on); });
    };
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

    // Track parameters (P-TRI-9..19, D-W3-13).
    describe (midiIn, "MIDI input", "The MIDI input this track plays and records from; the computer keyboard always reaches it");
    midiIn.onChange = [this] { setMidiInput(); };
    midiInChannel.addItem ("All", 1);
    for (int c = 1; c <= 16; ++c)
        midiInChannel.addItem (juce::String (c), c + 1);
    describe (midiInChannel, "MIDI input channel", "The MIDI channel this track listens to, or All");
    midiInChannel.onChange = [this] { setMidiInput(); };
    midiOutChannel.addItem ("As played", 1);
    for (int c = 1; c <= 16; ++c)
        midiOutChannel.addItem (juce::String (c), c + 1);
    describe (midiOutChannel, "MIDI output channel", "Sends every note and controller to the instrument on this channel, or on each one's own");
    midiOutChannel.onChange = [this] {
        if (! refreshing && midiOutChannel.getSelectedId() > 0)
            setPlayback ([this] (model::TrackPlayback& p) { p.outChannel = midiOutChannel.getSelectedId() - 1; });
    };
    delayUnit.addItem ("ms", 1);
    delayUnit.addItem ("ticks", 2);
    describe (delayUnit, "Track delay unit", "Delay in milliseconds or in ticks (960 to a quarter note)");
    delayUnit.onChange = [this] {
        if (refreshing || delayUnit.getSelectedId() == 0)
            return;
        // Changing the unit keeps the delay at zero rather than reading milliseconds as ticks.
        setPlayback ([this] (model::TrackPlayback& p) {
            p.delayInTicks = delayUnit.getSelectedId() == 2;
            p.delay = 0;
        });
    };
    setupSlider (trackDelay, -model::TrackPlayback::maxDelayMs, model::TrackPlayback::maxDelayMs, 0.1, "");
    trackDelay.textFromValueFunction = [this] (double v) {
        return delayUnit.getSelectedId() == 2 ? juce::String (juce::roundToInt (v)) : juce::String (v, 1);
    };
    describe (trackDelay, "Track delay", "Plays this track's MIDI later, or earlier when negative (double-click resets)");
    setupSlider (trackTranspose, -96.0, 96.0, 1.0, " st");
    describe (trackTranspose, "Track transpose", "Plays the track's notes this many semitones up or down (double-click resets)");
    setupSlider (trackVelocity, -99.0, 99.0, 1.0, "");
    describe (trackVelocity, "Track velocity", "Adds to every note's velocity on this track (double-click resets)");
    for (auto* sl : { &keyLow, &keyHigh })
    {
        setupSlider (*sl, 0.0, 127.0, 1.0, "");
        sl->setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, size::control);
        sl->textFromValueFunction = [] (double v) { return juce::String (commands::noteName (juce::roundToInt (v))); };
        sl->valueFromTextFunction = [] (const juce::String& t) {
            if (t.trim().containsOnly ("0123456789"))
                return t.getDoubleValue();
            for (int p = 0; p < 128; ++p)
                if (t.trim().equalsIgnoreCase (juce::String (commands::noteName (p))))
                    return static_cast<double> (p);
            return 0.0;
        };
    }
    describe (keyLow, "Key limit low", "Notes below this one don't play");
    describe (keyHigh, "Key limit high", "Notes above this one don't play");
    for (auto* sl : { &velocityLow, &velocityHigh })
    {
        setupSlider (*sl, 1.0, 127.0, 1.0, "");
        sl->setTextBoxStyle (juce::Slider::TextBoxRight, false, 36, size::control);
    }
    describe (velocityLow, "Velocity limit low", "Notes played softer than this don't play");
    describe (velocityHigh, "Velocity limit high", "Notes played harder than this don't play");
    trackDelay.setDoubleClickReturnValue (true, 0.0);
    trackTranspose.setDoubleClickReturnValue (true, 0.0);
    trackVelocity.setDoubleClickReturnValue (true, 0.0);
    keyLow.setDoubleClickReturnValue (true, 0.0);
    keyHigh.setDoubleClickReturnValue (true, 127.0);
    velocityLow.setDoubleClickReturnValue (true, 1.0);
    velocityHigh.setDoubleClickReturnValue (true, 127.0);
    for (auto* sl : { &trackDelay, &trackTranspose, &trackVelocity, &keyLow, &keyHigh, &velocityLow, &velocityHigh })
    {
        sl->onDragStart = [this] { studio.session().beginGesture ("Track Parameters"); };
        sl->onDragEnd = [this] {
            studio.session().endGesture();
            studio.edited();
        };
        sl->onValueChange = [this, sl] {
            if (refreshing)
                return;
            const auto v = sl->getValue();
            const auto i = static_cast<int> (std::lround (v));
            setPlayback ([&] (model::TrackPlayback& p) {
                if (sl == &trackDelay)
                    p.delay = p.delayInTicks ? std::round (v) : v;
                else if (sl == &trackTranspose)
                    p.transpose = i;
                else if (sl == &trackVelocity)
                    p.velocityOffset = i;
                // A limit pushed past its partner takes the partner along.
                else if (sl == &keyLow)
                    p.keyHigh = std::max (p.keyHigh, p.keyLow = i);
                else if (sl == &keyHigh)
                    p.keyLow = std::min (p.keyLow, p.keyHigh = i);
                else if (sl == &velocityLow)
                    p.velocityHigh = std::max (p.velocityHigh, p.velocityLow = i);
                else
                    p.velocityLow = std::min (p.velocityLow, p.velocityHigh = i);
            });
        };
    }
    describe (noTranspose, "No Transpose", "The regions' own transpose doesn't apply to this track (for drum kits)");
    noTranspose.onClick = [this] {
        const bool on = noTranspose.getToggleState();
        setPlayback ([on] (model::TrackPlayback& p) { p.noTranspose = on; });
    };

    for (juce::Component* c : std::initializer_list<juce::Component*> { &regionName, &regionMute, &regionLoop, &regionGain, &regionDelay, &fadeIn, &fadeOut,
                                                                          &fadeInCurve, &fadeOutCurve, &regionQuantize, &quantizeStrength, &quantizeSwing,
                                                                          &regionDelayUnit, &regionReverse, &quantizeRange, &quantizeFlam, &regionDynamics,
                                                                          &regionGate, &regionClipLength,
                                                                          &applyQuantize, &regionTranspose, &regionVelocity, &notePitch, &noteVelocity, &noteLength, &notePosition,
                                                                          &noteChannel, &trackName, &trackColour, &monitor, &midiIn, &midiInChannel, &midiOutChannel,
                                                                          &delayUnit, &trackDelay, &trackTranspose, &trackVelocity, &keyLow, &keyHigh,
                                                                          &velocityLow, &velocityHigh, &noTranspose })
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

const std::vector<std::pair<int, juce::String>>& Inspector::dynamicsValues()
{
    static const std::vector<std::pair<int, juce::String>> values { { 0, "Fixed" },    { 33, "3:1" },     { 50, "2:1" },   { 67, "1.5:1" },
                                                                    { 80, "1.25:1" }, { 100, "Off" },    { 125, "1:1.25" }, { 150, "1:1.5" },
                                                                    { 200, "1:2" },   { 300, "1:3" } };
    return values;
}

const std::vector<std::pair<int, juce::String>>& Inspector::gateValues()
{
    using M = model::MidiRegionData;
    static const std::vector<std::pair<int, juce::String>> values { { 0, "Off" },   { M::gateFixed, "Fixed" }, { 25, "25%" },  { 50, "50%" },
                                                                    { 75, "75%" },  { 90, "90%" },             { 110, "110%" }, { 125, "125%" },
                                                                    { 150, "150%" }, { 200, "200%" },          { M::gateLegato, "Legato" } };
    return values;
}

void Inspector::setRegionQuantize()
{
    if (refreshing || regionQuantize.getSelectedId() <= 0)
        return;
    const auto index = regionQuantize.getSelectedId() - 1;
    std::optional<model::RegionQuantize> q;
    if (index > 0)
        q = model::RegionQuantize { gridValues()[static_cast<std::size_t> (index)].ticks, quantizeStrength.getValue() / 100.0, quantizeSwing.getValue() / 100.0,
                                    static_cast<Ticks> (std::lround (quantizeRange.getValue())), static_cast<Ticks> (std::lround (quantizeFlam.getValue())) };
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
    for (juce::Component* c : std::initializer_list<juce::Component*> { &regionName, &regionMute, &regionLoop, &regionDelay, &regionDelayUnit })
        c->setVisible (r != nullptr);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &regionGain, &fadeIn, &fadeOut, &fadeInCurve, &fadeOutCurve, &regionReverse })
        c->setVisible (audio);
    if (r != nullptr)
    {
        regionName.setText (all.size() == 1 ? juce::String (r->name) : juce::String (static_cast<int> (all.size())) + " regions", juce::dontSendNotification);
        regionMute.setToggleState (std::all_of (all.begin(), all.end(), [] (const model::Region* x) { return x->muted; }), juce::dontSendNotification);
        regionLoop.setToggleState (std::all_of (all.begin(), all.end(), [] (const model::Region* x) { return x->loop && x->loop->enabled; }), juce::dontSendNotification);
        const bool ticks = r->delayInTicks;
        const bool sameUnit = same ([] (const model::Region& x) { return x.delayInTicks; });
        regionDelayUnit.setSelectedId (sameUnit ? (ticks ? 2 : 1) : 0, juce::dontSendNotification);
        if (ticks)
            regionDelay.setRange (-static_cast<double> (model::Region::maxDelayTicks), static_cast<double> (model::Region::maxDelayTicks), 1.0);
        else
            regionDelay.setRange (-500.0, 500.0, 0.1);
        show (regionDelay, ticks ? static_cast<double> (r->delayTicks) : r->delayMs,
              sameUnit && same ([] (const model::Region& x) { return x.delayInTicks ? static_cast<double> (x.delayTicks) : x.delayMs; }));
    }
    if (audio)
    {
        const auto rate = static_cast<double> (studio.model().info.sampleRate);
        show (regionGain, r->audio.gainDb, same ([] (const model::Region& x) { return x.audio.gainDb; }));
        show (fadeIn, static_cast<double> (r->audio.fadeIn.length) * 1000.0 / rate, same ([] (const model::Region& x) { return x.audio.fadeIn.length; }));
        show (fadeOut, static_cast<double> (r->audio.fadeOut.length) * 1000.0 / rate, same ([] (const model::Region& x) { return x.audio.fadeOut.length; }));
        fadeInCurve.setSelectedItemIndex (same ([] (const model::Region& x) { return x.audio.fadeIn.curve; }) ? curveIndex (r->audio.fadeIn.curve) : -1, juce::dontSendNotification);
        fadeOutCurve.setSelectedItemIndex (same ([] (const model::Region& x) { return x.audio.fadeOut.curve; }) ? curveIndex (r->audio.fadeOut.curve) : -1, juce::dontSendNotification);
        regionReverse.setToggleState (std::all_of (all.begin(), all.end(), [] (const model::Region* x) { return x->audio.reversed; }), juce::dontSendNotification);
    }
    for (juce::Component* c : std::initializer_list<juce::Component*> { &regionQuantize, &quantizeStrength, &quantizeSwing, &quantizeRange, &quantizeFlam,
                                                                          &applyQuantize, &regionTranspose, &regionVelocity, &regionDynamics, &regionGate,
                                                                          &regionClipLength })
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
        show (quantizeRange, q ? static_cast<double> (q->range) : 0.0, same ([] (const model::Region& x) { return x.midi.quantize ? x.midi.quantize->range : Ticks (0); }));
        show (quantizeFlam, q ? static_cast<double> (q->flam) : 0.0, same ([] (const model::Region& x) { return x.midi.quantize ? x.midi.quantize->flam : Ticks (0); }));
        quantizeStrength.setEnabled (any && sameGrid);
        quantizeSwing.setEnabled (any && sameGrid);
        quantizeRange.setEnabled (any && sameGrid);
        quantizeFlam.setEnabled (any && sameGrid);
        const auto indexOf = [] (const std::vector<std::pair<int, juce::String>>& values, int v) {
            for (std::size_t i = 0; i < values.size(); ++i)
                if (values[i].first == v)
                    return static_cast<int> (i);
            return -1;
        };
        regionDynamics.setSelectedItemIndex (same ([] (const model::Region& x) { return x.midi.dynamics; }) ? indexOf (dynamicsValues(), r->midi.dynamics) : -1,
                                             juce::dontSendNotification);
        regionGate.setSelectedItemIndex (same ([] (const model::Region& x) { return x.midi.gate; }) ? indexOf (gateValues(), r->midi.gate) : -1, juce::dontSendNotification);
        regionClipLength.setToggleState (std::all_of (all.begin(), all.end(), [] (const model::Region* x) { return x->midi.clipLength; }), juce::dontSendNotification);
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
    const bool instrument = t != nullptr && t->kind == model::TrackKind::instrument;
    for (auto* c : trackParameters())
        c->setVisible (instrument);
    if (instrument)
        refreshTrackParameters (*t);
    refreshStrips();
    resized();
    body.repaint();
}

std::vector<juce::Component*> Inspector::trackParameters()
{
    return { &midiIn, &midiInChannel, &midiOutChannel, &delayUnit, &trackDelay, &trackTranspose, &trackVelocity,
             &keyLow, &keyHigh, &velocityLow, &velocityHigh, &noTranspose };
}

void Inspector::refreshTrackParameters (const model::Track& t)
{
    const juce::ScopedValueSetter<bool> busy (refreshing, true);
    // The device list is read when another track is selected, not on every edit.
    const auto device = t.input.kind == model::InputAssignment::Kind::midi ? t.input.device : std::string ("all");
    if (devicesFor != t.id || (device != "all" && std::find (midiDevices.begin(), midiDevices.end(), device) == midiDevices.end()))
    {
        devicesFor = t.id;
        midiDevices = midiDeviceNames ? midiDeviceNames() : engine::JuceMidiInputs::deviceNames();
        // A device that isn't connected now stays in the menu while a track uses it.
        if (device != "all" && std::find (midiDevices.begin(), midiDevices.end(), device) == midiDevices.end())
            midiDevices.push_back (device);
        midiIn.clear (juce::dontSendNotification);
        midiIn.addItem ("All", 1);
        for (std::size_t i = 0; i < midiDevices.size(); ++i)
            midiIn.addItem (juce::String (midiDevices[i]), static_cast<int> (i) + 2);
    }
    const auto at = std::find (midiDevices.begin(), midiDevices.end(), device);
    midiIn.setSelectedId (at == midiDevices.end() ? 1 : static_cast<int> (at - midiDevices.begin()) + 2, juce::dontSendNotification);
    midiInChannel.setSelectedId (t.input.kind == model::InputAssignment::Kind::midi ? t.input.channel + 1 : 1, juce::dontSendNotification);
    const auto& p = t.playback;
    midiOutChannel.setSelectedId (p.outChannel + 1, juce::dontSendNotification);
    delayUnit.setSelectedId (p.delayInTicks ? 2 : 1, juce::dontSendNotification);
    if (p.delayInTicks)
        trackDelay.setRange (-model::TrackPlayback::maxDelayTicks, model::TrackPlayback::maxDelayTicks, 1.0);
    else
        trackDelay.setRange (-model::TrackPlayback::maxDelayMs, model::TrackPlayback::maxDelayMs, 0.1);
    trackDelay.setValue (p.delay, juce::dontSendNotification);
    trackDelay.updateText();
    trackTranspose.setValue (p.transpose, juce::dontSendNotification);
    trackVelocity.setValue (p.velocityOffset, juce::dontSendNotification);
    keyLow.setValue (p.keyLow, juce::dontSendNotification);
    keyHigh.setValue (p.keyHigh, juce::dontSendNotification);
    velocityLow.setValue (p.velocityLow, juce::dontSendNotification);
    velocityHigh.setValue (p.velocityHigh, juce::dontSendNotification);
    noTranspose.setToggleState (p.noTranspose, juce::dontSendNotification);
}

void Inspector::setPlayback (const std::function<void (model::TrackPlayback&)>& change)
{
    const auto* t = studio.model().track (studio.selectedTrack());
    if (t == nullptr || t->kind != model::TrackKind::instrument)
        return;
    const auto id = t->id;
    auto p = t->playback;
    change (p);
    if (! (p == t->playback))
        studio.check (studio.session().setTrackPlayback (id, p));
}

void Inspector::setMidiInput()
{
    const auto* t = studio.model().track (studio.selectedTrack());
    if (refreshing || t == nullptr || t->kind != model::TrackKind::instrument || midiIn.getSelectedId() == 0)
        return;
    model::InputAssignment in;
    in.kind = model::InputAssignment::Kind::midi;
    const auto index = midiIn.getSelectedId() - 2;
    in.device = index >= 0 && index < static_cast<int> (midiDevices.size()) ? midiDevices[static_cast<std::size_t> (index)] : std::string ("all");
    in.channel = std::max (0, midiInChannel.getSelectedId() - 1);
    const auto id = t->id;
    studio.check (studio.session().setTrackInput (id, in));
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
    label (quantizeRange, "Q-Range");
    label (quantizeFlam, "Q-Flam");
    label (regionDynamics, "Dynamics");
    label (regionGate, "Gate time");
    label (notePitch, "Pitch");
    label (noteVelocity, "Velocity");
    label (noteLength, "Length");
    label (notePosition, "Position");
    label (noteChannel, "Channel");
    label (trackColour, "Colour");
    label (monitor, "Monitor");
    label (midiIn, "MIDI in");
    label (midiInChannel, "In channel");
    label (midiOutChannel, "Out channel");
    label (trackDelay, "Delay");
    label (trackTranspose, "Transpose");
    label (trackVelocity, "Velocity");
    label (keyLow, "Key limit");
    label (velocityLow, "Vel. limit");
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
        {
            auto delay = withLabel (row());
            regionDelayUnit.setBounds (delay.removeFromRight (64));
            regionDelay.setBounds (delay.withTrimmedRight (space::half));
        }
        if (regionGain.isVisible())
        {
            regionReverse.setBounds (withLabel (row()));
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
            quantizeRange.setBounds (withLabel (row()));
            quantizeFlam.setBounds (withLabel (row()));
            applyQuantize.setBounds (withLabel (row()));
            regionTranspose.setBounds (withLabel (row()));
            regionVelocity.setBounds (withLabel (row()));
            regionDynamics.setBounds (withLabel (row()));
            regionGate.setBounds (withLabel (row()));
            regionClipLength.setBounds (withLabel (row()));
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
    if (midiIn.isVisible())
    {
        midiIn.setBounds (withLabel (row()));
        midiInChannel.setBounds (withLabel (row()));
        midiOutChannel.setBounds (withLabel (row()));
        auto delay = withLabel (row());
        delayUnit.setBounds (delay.removeFromRight (64));
        trackDelay.setBounds (delay.withTrimmedRight (space::half));
        trackTranspose.setBounds (withLabel (row()));
        trackVelocity.setBounds (withLabel (row()));
        // The low and high limits side by side.
        const auto pair = [&] (juce::Slider& lo, juce::Slider& hi) {
            auto b = withLabel (row());
            const auto half = (b.getWidth() - space::half) / 2;
            lo.setBounds (b.removeFromLeft (half));
            hi.setBounds (b.withTrimmedLeft (space::half));
        };
        pair (keyLow, keyHigh);
        pair (velocityLow, velocityHigh);
        noTranspose.setBounds (withLabel (row()));
    }
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
