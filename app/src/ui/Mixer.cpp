#include "Mixer.h"
#include "PresenceUi.h"

#include <cmath>

namespace weft::ui
{
namespace
{
constexpr int rowHeight = size::control;
constexpr int gap = space::half;
const char* const automationModes[] = { "off", "read", "touch", "latch", "write" };
const char* const feedbackText = "Would create a feedback loop";

bool isSilent (double db) { return db <= fader::minDb + 1e-6; }

model::AutomationTarget targetOf (const char* kind)
{
    model::AutomationTarget t;
    t.kind = kind;
    return t;
}

} // namespace

void KnobLook::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float start, float end, juce::Slider& s)
{
    Theme::get().lookAndFeel().drawRotarySlider (g, x, y, w, h, pos, start, end, s);
}

namespace
{
void styleSlider (juce::Slider& s, juce::Slider::SliderStyle style, double min, double max, double resetTo, KnobLook& knobs)
{
    s.setSliderStyle (style);
    s.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    s.setRange (min, max, 0.0);
    // Double-click or Alt+click resets (F-MIX-2, F-MIX-3); Shift-drag is fine adjustment.
    s.setDoubleClickReturnValue (true, resetTo, juce::ModifierKeys::altModifier);
    s.setVelocityModeParameters (0.2, 1, 0.0, true, juce::ModifierKeys::shiftModifier);
    s.setWantsKeyboardFocus (true);
    s.setMouseClickGrabsKeyboardFocus (false);
    s.setHasFocusOutline (true);
    if (style != juce::Slider::LinearVertical)
        s.setLookAndFeel (&knobs);
}

void small (juce::TextButton& b)
{
    b.setWantsKeyboardFocus (true);
    b.setMouseClickGrabsKeyboardFocus (false);
    b.setHasFocusOutline (true);
}

void setDbSlider (juce::Slider& s, double db)
{
    if (! s.isMouseButtonDown())
        s.setValue (std::clamp (db, fader::minDb, fader::maxDb), juce::dontSendNotification);
}
} // namespace

// Fader law --------------------------------------------------------------------------------

juce::NormalisableRange<double> fader::range()
{
    // Positions follow the automation taper (D-ARCH-28), so a lane and the fader agree.
    return { minDb, maxDb,
             [] (double, double, double proportion) { return std::clamp (automation::toDb (proportion), minDb, maxDb); },
             [] (double, double, double db) { return db <= minDb + 1e-6 ? 0.0 : automation::fromDb (db); } };
}

juce::String fader::dbText (double db)
{
    if (db <= minDb + 1e-6)
        return "-inf";
    return (db > 0.05 ? "+" : "") + juce::String (db, 1);
}

std::optional<double> fader::parseDb (const juce::String& text)
{
    auto t = text.trim().toLowerCase().replace ("db", "").trim();
    if (t == "-inf" || t == "inf" || t == "-oo")
        return silentDb;
    if (! t.containsOnly ("+-.0123456789") || t.isEmpty())
        return {};
    const auto v = t.getDoubleValue();
    return v <= minDb ? silentDb : std::min (v, maxDb);
}

// Meter --------------------------------------------------------------------------------------

float MeterView::proportion (float db)
{
    if (db <= static_cast<float> (fader::minDb))
        return 0.0f;
    return static_cast<float> (fader::range().convertTo0to1 (std::min (static_cast<double> (db), fader::maxDb)));
}

juce::Rectangle<float> MeterView::barArea() const
{
    return getLocalBounds().toFloat().withTrimmedTop (static_cast<float> (clipHeight + clipGap)).reduced (0.0f, size::faderCap / 2.0f);
}

void MeterView::update (float peak, float rms, bool clip, double now)
{
    const auto before = std::tuple (ballistics.levelDb(), ballistics.holdDb(), ballistics.clip(), rmsDb);
    ballistics.update (peak, clip, now);
    rmsLinear = rms;
    rmsDb = engine::gainToDb (rms);
    if (std::tuple (ballistics.levelDb(), ballistics.holdDb(), ballistics.clip(), rmsDb) != before)
        repaint();
}

void MeterView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    // The clip light: dark until something clips, then lit until clicked.
    const auto led = bounds.withHeight (static_cast<float> (clipHeight));
    if (ballistics.clip())
        depth::tinted (g, led, radius::r1, colour (Token::meterClip));
    else
        depth::well (g, led, radius::r1);
    // The well runs the fader's full travel, cap ends included, so the two look like one unit.
    const auto bars = barArea();
    const auto well = bars.expanded (0.0f, size::faderCap / 2.0f);
    depth::well (g, well, radius::r1);
    const auto inner = bars.reduced (2.0f, 0.0f);
    auto yOf = [&] (float db) { return inner.getBottom() - inner.getHeight() * proportion (db); };
    // Zones are fixed in dB, so the colour says the level as much as the height does.
    const auto hotY = yOf (-18.0f), clipY = yOf (-6.0f);
    auto fillZones = [&] (float top, float alpha) {
        if (top >= inner.getBottom())
            return;
        const juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (inner.withTop (top).toNearestInt());
        g.setColour (colour (Token::meterSafe).withAlpha (alpha));
        g.fillRect (inner.withTop (hotY));
        g.setColour (colour (Token::meterHot).withAlpha (alpha));
        g.fillRect (inner.withTop (clipY).withBottom (hotY));
        g.setColour (colour (Token::meterClip).withAlpha (alpha));
        g.fillRect (inner.withBottom (clipY));
    };
    // Peak: the lighter tone; RMS: the solid bar inside it.
    fillZones (yOf (ballistics.levelDb()), 0.4f);
    fillZones (yOf (rmsDb), 1.0f);
    // Tick lines every 6 dB down to -48, the 0 dB line brighter.
    for (const auto db : { 0.0f, -6.0f, -12.0f, -18.0f, -24.0f, -36.0f, -48.0f })
    {
        g.setColour (db == 0.0f ? colour (Token::text).withAlpha (0.35f) : colour (Token::well).withAlpha (0.6f));
        g.fillRect (inner.getX(), std::round (yOf (db)), inner.getWidth(), 1.0f);
    }
    if (ballistics.holdDb() > static_cast<float> (fader::minDb))
    {
        g.setColour (ballistics.holdDb() > -6.0f ? colour (Token::meterClip) : colour (Token::text));
        g.fillRect (inner.getX(), std::round (yOf (ballistics.holdDb())), inner.getWidth(), 2.0f);
    }
}

void MeterView::mouseDown (const juce::MouseEvent& e)
{
    if (e.y <= clipHeight + clipGap)
    {
        ballistics.resetClip();
        if (onResetClip)
            onResetClip();
        repaint();
    }
}

// Insert slot ------------------------------------------------------------------------------

InsertSlot::InsertSlot (ChannelStrip& strip, std::size_t i, const model::PluginInstance* plugin, bool isPlaceholder)
    : index (i), pluginId (plugin != nullptr ? plugin->id : Id()), label (plugin != nullptr ? juce::String (plugin->name) : juce::String()),
      bypassed (plugin != nullptr && plugin->bypass), placeholder (isPlaceholder), owner (strip)
{
    const auto title = plugin == nullptr ? juce::String ("Empty insert slot") : "Insert " + juce::String (i + 1) + ": " + label;
    setTitle (title);
    setTooltip (placeholder ? label + ": Not installed on this computer" : plugin == nullptr ? juce::String ("Add an effect") : label + " (click to open; right-click for more)");
}

void InsertSlot::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.0f, 1.0f);
    // Empty slots sink into the strip; filled ones sit on it like buttons.
    if (pluginId.empty())
        depth::well (g, r, radius::r1);
    else if (placeholder)
        depth::tinted (g, r, radius::r1, colour (Token::warning));
    else
        depth::raised (g, r, radius::r1, isMouseOver());
    g.setFont (font (Type::small));
    if (pluginId.empty())
    {
        g.setColour (colour (Token::textDim));
        g.drawText ("+", r, juce::Justification::centred);
        return;
    }
    const auto edge = r.removeFromLeft (static_cast<float> (bypassWidth));
    // The bypass dot: lit (meter green) while the effect runs, hollow when bypassed. Not the
    // accent, which is kept for selection and record.
    const auto dot = edge.withSizeKeepingCentre (6.0f, 6.0f);
    if (bypassed)
    {
        g.setColour (colour (Token::textDim));
        g.drawEllipse (dot.reduced (0.5f), 1.0f);
    }
    else
    {
        g.setColour (colour (Token::meterSafe));
        g.fillEllipse (dot);
    }
    g.setColour (placeholder ? colour (Token::onWarning) : bypassed ? colour (Token::textDim) : colour (Token::text));
    g.drawText (label, r.reduced (2.0f, 0.0f), juce::Justification::centredLeft, true);
}

void InsertSlot::mouseDown (const juce::MouseEvent& e)
{
    dragging = false;
    if (pluginId.empty())
    {
        owner.showMenu (owner.insertMenu (index), [s = &owner, i = index] (int item) { s->pickInsert (i, item); });
        return;
    }
    if (e.mods.isPopupMenu())
        owner.showMenu (owner.insertMenu (index), [s = &owner, i = index] (int item) { s->pickInsert (i, item); });
    else if (e.x < bypassWidth)
        owner.pickInsert (index, ChannelStrip::itemBypass);
}

void InsertSlot::mouseDrag (const juce::MouseEvent& e)
{
    if (! pluginId.empty() && ! e.mods.isPopupMenu() && e.getDistanceFromDragStart() > 4)
    {
        dragging = true;
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }
}

void InsertSlot::mouseUp (const juce::MouseEvent& e)
{
    setMouseCursor (juce::MouseCursor::NormalCursor);
    if (pluginId.empty() || e.mods.isPopupMenu())
        return;
    if (dragging)
    {
        dragging = false;
        const auto p = e.getEventRelativeTo (&owner).getPosition();
        for (const auto& slot : owner.inserts)
            if (slot->getBounds().contains (p))
            {
                owner.moveInsert (index, std::min (slot->index, owner.inserts.size() - 1));
                return;
            }
        return;
    }
    if (e.mouseDownPosition.x >= bypassWidth)
        owner.openEditor (pluginId);
}

// Send slot ----------------------------------------------------------------------------------

SendSlot::SendSlot (ChannelStrip& strip, std::size_t i, const model::Send* send, const juce::String& busName, bool disabledEdge)
    : index (i), sendId (send != nullptr ? send->id : Id()), owner (strip), warning (disabledEdge)
{
    target.setButtonText (send != nullptr ? busName : juce::String ("Send"));
    describe (target, send != nullptr ? "Send to " + busName : juce::String ("Add a send"),
              disabledEdge ? juce::String ("Disabled: feedback loop") : send != nullptr ? juce::String ("Send to ") + busName : juce::String ("Send to a bus"));
    small (target);
    target.onClick = [this] { owner.showMenu (owner.sendMenu (index), [s = &owner, i = index] (int item) { s->pickSend (i, item); }); };
    addAndMakeVisible (target);
    if (send == nullptr)
    {
        setRole (target, ButtonRole::flat);
        return;
    }
    if (disabledEdge)
        setRole (target, ButtonRole::warning);
    styleSlider (level, juce::Slider::RotaryHorizontalVerticalDrag, fader::minDb, fader::maxDb, 0.0, *knobs);
    level.setSkewFactor (3.0);
    level.setValue (std::clamp (send->levelDb, fader::minDb, fader::maxDb), juce::dontSendNotification);
    describe (level, "Send level", "Send level (double-click: 0 dB)");
    level.onDragStart = [this] { owner.studio.session().beginGesture ("Send Level"); };
    level.onDragEnd = [this] { owner.studio.session().endGesture(); };
    level.onValueChange = [this] {
        const auto v = level.getValue();
        owner.studio.check (owner.studio.session().setSendLevel (owner.trackId(), sendId, isSilent (v) ? fader::silentDb : v));
        owner.studio.edited();
    };
    level.textFromValueFunction = [] (double v) { return fader::dbText (v) + " dB"; };
    level.setPopupDisplayEnabled (true, true, nullptr);
    addAndMakeVisible (level);

    mode.setButtonText (send->pre ? "pre" : "post");
    describe (mode, "Pre or post fader", send->pre ? juce::String ("Pre-fader send (click for post)") : juce::String ("Post-fader send (click for pre)"));
    small (mode);
    mode.onClick = [this] { owner.pickSend (index, ChannelStrip::itemPre); };
    addAndMakeVisible (mode);

    describe (mute, "Mute send");
    small (mute);
    mute.setClickingTogglesState (false);
    mute.setToggleState (send->muted, juce::dontSendNotification);
    setRole (mute, ButtonRole::mute);
    mute.onClick = [this] { owner.pickSend (index, ChannelStrip::itemMuteSend); };
    addAndMakeVisible (mute);
}

void SendSlot::resized()
{
    auto r = getLocalBounds().reduced (0, 1);
    if (sendId.empty())
    {
        target.setBounds (r);
        return;
    }
    mute.setBounds (r.removeFromRight (16));
    mode.setBounds (r.removeFromRight (28));
    level.setBounds (r.removeFromRight (r.getHeight()));
    target.setBounds (r);
}

void SendSlot::paint (juce::Graphics& g)
{
    if (warning)
    {
        g.setColour (colour (Token::warning));
        g.drawRect (getLocalBounds());
    }
}

// Channel strip ------------------------------------------------------------------------------

ChannelStrip::ChannelStrip (Studio& s, Id trackId) : studio (s), id (std::move (trackId))
{
    name.setJustificationType (juce::Justification::centred);
    name.setFont (font (Type::strong));
    name.setEditable (false, ! isMaster());
    describe (name, "Channel name", isMaster() ? juce::String ("Master") : juce::String ("Double-click to rename"));
    name.onTextChange = [this] {
        const auto text = name.getText().trim();
        if (text.isNotEmpty())
            studio.check (studio.session().renameTrack (id, text.toStdString()));
        studio.edited();
    };
    addAndMakeVisible (name);

    small (input);
    input.onClick = [this] {
        const auto* t = track();
        if (t != nullptr && t->kind == model::TrackKind::instrument)
            showMenu (instrumentMenu(), [this] (int item) { pickInstrument (item); });
        else
            showMenu (inputMenu(), [this] (int item) { pickInput (item); });
    };
    addChildComponent (input);

    small (output);
    output.onClick = [this] { showMenu (outputMenu(), [this] (int item) { pickOutput (item); }); };
    addChildComponent (output);

    automation.addItemList ({ "Off", "Read", "Touch", "Latch", "Write" }, 1);
    describe (automation, "Automation mode");
    automation.onChange = [this] {
        const auto i = automation.getSelectedItemIndex();
        if (i >= 0 && i < 5)
            studio.check (studio.session().setTrackAutomationMode (id, automationModes[i]));
        studio.edited();
    };
    addAndMakeVisible (automation);

    styleSlider (pan, juce::Slider::RotaryHorizontalVerticalDrag, -100.0, 100.0, 0.0, *knobs);
    pan.textFromValueFunction = [] (double v) {
        const auto n = juce::roundToInt (v);
        return n == 0 ? juce::String ("C") : juce::String (std::abs (n)) + (n < 0 ? "L" : "R");
    };
    pan.setPopupDisplayEnabled (true, true, nullptr);
    // Fader, pan and mute gestures write or override automation while it is live (W-8, F-AUT-6).
    pan.onDragStart = [this] {
        livePan = studio.automationLive (id, targetOf ("pan"));
        if (! livePan)
            studio.session().beginGesture ("Pan");
    };
    pan.onDragEnd = [this] {
        if (livePan)
            studio.releaseControl (id, targetOf ("pan"));
        else
            studio.session().endGesture();
        livePan = false;
    };
    pan.onValueChange = [this] {
        if (livePan)
            return studio.touchControl (id, targetOf ("pan"), automation::fromPan (pan.getValue() / 100.0));
        studio.check (studio.session().setTrackPan (id, pan.getValue() / 100.0));
        studio.edited();
    };
    addAndMakeVisible (pan);

    styleSlider (volume, juce::Slider::LinearVertical, fader::minDb, fader::maxDb, 0.0, *knobs);
    volume.setNormalisableRange (fader::range());
    describe (volume, "Volume", "Volume (double-click: 0 dB, Shift-drag: fine)");
    volume.addMouseListener (&faderHover, false);
    volume.textFromValueFunction = [] (double v) { return fader::dbText (v) + " dB"; };
    volume.onDragStart = [this] {
        liveVolume = studio.automationLive (id, targetOf ("volume"));
        if (! liveVolume)
            studio.session().beginGesture ("Volume");
    };
    volume.onDragEnd = [this] {
        if (liveVolume)
            studio.releaseControl (id, targetOf ("volume"));
        else
            studio.session().endGesture();
        liveVolume = false;
    };
    volume.onValueChange = [this] {
        const auto db = isSilent (volume.getValue()) ? fader::silentDb : volume.getValue();
        if (liveVolume)
        {
            readout.setText (fader::dbText (db), juce::dontSendNotification);
            return studio.touchControl (id, targetOf ("volume"), automation::fromDb (db));
        }
        setVolume (db);
    };
    volume.onDragStart = [this, start = volume.onDragStart] {
        start();
        repaint();
    };
    addAndMakeVisible (volume);

    readout.setJustificationType (juce::Justification::centred);
    readout.setFont (font (Type::monoValue));
    readout.setEditable (false, true);
    describe (readout, "Volume in dB", "Double-click to type a level");
    readout.onTextChange = [this] {
        if (const auto db = fader::parseDb (readout.getText()))
            setVolume (*db);
        else
            refresh();
    };
    addAndMakeVisible (readout);

    describe (meter, "Meter", "Level: RMS bar, peak above it (click the light to reset the clip indicator)");
    meter.onResetClip = [this] { studio.engine().resetClip (isMaster() ? Id() : id); };
    addAndMakeVisible (meter);

    for (auto* b : { &mute, &solo, &arm, &monitor })
    {
        small (*b);
        addChildComponent (b);
    }
    describe (mute, "Mute");
    describe (solo, "Solo", "Solo (Ctrl+click: solo this channel only)");
    describe (arm, "Record enable");
    describe (monitor, "Input monitoring", "Input monitoring: auto, on, off");
    setRole (mute, ButtonRole::mute);
    setRole (solo, ButtonRole::solo);
    setRole (arm, ButtonRole::record);
    mute.onClick = [this] {
        const auto* t = track();
        if (t == nullptr)
            return;
        const auto mutes = targetOf ("mute");
        if (studio.automationLive (id, mutes))
        {
            // A click is a touch and a release: Latch and Write hold it until stop.
            const auto now = studio.automationValue (id, mutes).value_or (t->mute ? 1.0 : 0.0);
            studio.touchControl (id, mutes, automation::toMute (now) ? 0.0 : 1.0);
            studio.releaseControl (id, mutes);
            return;
        }
        studio.check (studio.session().setTrackMute (id, ! t->mute));
        studio.edited();
    };
    solo.onClick = [this] { studio.toggleSolo (id, juce::ModifierKeys::currentModifiers.isCommandDown()); };
    arm.onClick = [this] { studio.toggleArm (id); };
    monitor.onClick = [this] {
        if (const auto* t = track())
            studio.check (studio.session().setTrackMonitor (id, t->monitor == "auto" ? "on" : t->monitor == "on" ? "off" : "auto"));
        studio.edited();
    };
    refresh();
}

ChannelStrip::~ChannelStrip()
{
    sends.clear();
    pan.setLookAndFeel (nullptr);
}

bool ChannelStrip::isMaster() const { return id == studio.model().info.masterTrackId; }

const model::Track* ChannelStrip::track() const { return studio.model().track (id); }

void ChannelStrip::setVolume (double db)
{
    studio.check (studio.session().setTrackVolume (id, db));
    studio.edited();
}

juce::String ChannelStrip::busName (const Id& busId) const
{
    if (busId.empty())
        return "Master";
    const auto* b = studio.model().track (busId);
    return b != nullptr ? juce::String (b->name) : juce::String ("Missing bus");
}

std::vector<const model::Track*> ChannelStrip::buses() const
{
    std::vector<const model::Track*> out;
    for (const auto& tid : studio.model().trackOrder)
        if (const auto* t = studio.model().track (tid); t != nullptr && t->kind == model::TrackKind::bus && tid != id)
            out.push_back (t);
    return out;
}

int ChannelStrip::shownInsertRows() const
{
    const auto* t = track();
    const auto n = t == nullptr ? 0 : static_cast<int> (t->strip.inserts.size());
    return std::max (insertRows, std::min (n + 1, model::maxInserts));
}

int ChannelStrip::shownSendRows() const
{
    const auto* t = track();
    if (t == nullptr || isMaster())
        return sendRows;
    return std::max (sendRows, std::min (static_cast<int> (t->strip.sends.size()) + 1, model::maxSends));
}

int ChannelStrip::preferredHeight() const
{
    // name, input, slots, output, automation, pan, fader (min), readout, buttons
    return space::s1 + rowHeight + gap + rowHeight + gap + (shownInsertRows() + 1 + shownSendRows()) * rowHeight + gap + rowHeight + gap + rowHeight + gap
           + space::s4 + gap + 120 + gap + rowHeight + gap + rowHeight + space::s1;
}

void ChannelStrip::rebuildSlots()
{
    const auto* t = track();
    if (t == nullptr)
        return;
    std::string key;
    for (const auto& p : t->strip.inserts)
    {
        const auto proc = studio.processor (p.id);
        key += p.id + p.name + (p.bypass ? "b" : "") + (proc != nullptr && proc->isPlaceholder() ? "p" : "") + ";";
    }
    key += "|";
    for (const auto& snd : t->strip.sends)
        key += snd.id + snd.targetBusId + busName (snd.targetBusId).toStdString() + (snd.pre ? "p" : "") + (snd.muted ? "m" : "")
               + (studio.model().disabledEdges.contains (model::RoutingEdge { id, snd.targetBusId, snd.id }.key()) ? "d" : "") + ";";
    if (key == shownSlots)
    {
        for (std::size_t i = 0; i < sends.size() && i < t->strip.sends.size(); ++i)
            setDbSlider (sends[i]->level, t->strip.sends[i].levelDb);
        return;
    }
    shownSlots = key;
    inserts.clear();
    sends.clear();
    for (std::size_t i = 0; i < t->strip.inserts.size(); ++i)
    {
        const auto& p = t->strip.inserts[i];
        const auto proc = studio.processor (p.id);
        inserts.push_back (std::make_unique<InsertSlot> (*this, i, &p, proc != nullptr && proc->isPlaceholder()));
    }
    if (t->strip.inserts.size() < static_cast<std::size_t> (model::maxInserts))
        inserts.push_back (std::make_unique<InsertSlot> (*this, t->strip.inserts.size(), nullptr, false));
    if (! isMaster())
    {
        for (std::size_t i = 0; i < t->strip.sends.size(); ++i)
        {
            const auto& snd = t->strip.sends[i];
            const auto disabled = studio.model().disabledEdges.contains (model::RoutingEdge { id, snd.targetBusId, snd.id }.key());
            sends.push_back (std::make_unique<SendSlot> (*this, i, &snd, busName (snd.targetBusId), disabled));
        }
        if (t->strip.sends.size() < static_cast<std::size_t> (model::maxSends))
            sends.push_back (std::make_unique<SendSlot> (*this, t->strip.sends.size(), nullptr, juce::String(), false));
    }
    for (auto& c : inserts)
        addAndMakeVisible (*c);
    for (auto& c : sends)
        addAndMakeVisible (*c);
    resized();
}

void ChannelStrip::refresh()
{
    const auto* t = track();
    if (t == nullptr)
        return;
    const auto master = isMaster();
    if (! name.isBeingEdited())
        name.setText (master ? juce::String ("Master") : juce::String (t->name), juce::dontSendNotification);

    const auto kind = t->kind;
    input.setVisible (kind == model::TrackKind::audio || kind == model::TrackKind::instrument);
    setRole (input, ButtonRole::normal);
    if (kind == model::TrackKind::instrument)
    {
        const auto proc = t->strip.instrument ? studio.processor (t->strip.instrument->id) : nullptr;
        const auto missing = proc != nullptr && proc->isPlaceholder();
        const auto label = t->strip.instrument ? juce::String (t->strip.instrument->name) : juce::String ("Weft Keys");
        input.setButtonText (label);
        describe (input, "Instrument", missing ? label + ": Not installed on this computer" : "Instrument: " + label);
        if (missing)
            setRole (input, ButtonRole::warning);
    }
    else if (kind == model::TrackKind::audio)
    {
        juce::String label = "No Input";
        if (t->input.kind == model::InputAssignment::Kind::audio)
            label = t->channels == 1 ? "In " + juce::String (t->input.firstChannel + 1)
                                     : "In " + juce::String (t->input.firstChannel + 1) + "-" + juce::String (t->input.firstChannel + 2);
        input.setButtonText (label);
        describe (input, "Input", "Hardware input: " + label);
    }

    rebuildSlots();

    output.setVisible (! master);
    if (! master)
    {
        const auto disabled = studio.model().disabledEdges.contains (model::RoutingEdge { id, t->output.busId, {} }.key());
        output.setButtonText (busName (t->output.busId));
        describe (output, "Output", disabled ? juce::String ("Disabled: feedback loop") : "Output: " + busName (t->output.busId));
        setRole (output, disabled ? ButtonRole::warning : ButtonRole::normal);
        output.repaint();
    }

    for (int i = 0; i < 5; ++i)
        if (t->automationMode == automationModes[i])
            automation.setSelectedItemIndex (i, juce::dontSendNotification);

    pan.setVisible (! master);
    describe (pan, t->channels == 1 || kind == model::TrackKind::instrument ? juce::String ("Pan") : juce::String ("Balance"),
              "Pan (double-click: centre)");
    if (! pan.isMouseButtonDown())
        pan.setValue (t->strip.pan * 100.0, juce::dontSendNotification);
    setDbSlider (volume, t->strip.volumeDb);
    if (! readout.isBeingEdited())
        readout.setText (fader::dbText (t->strip.volumeDb), juce::dontSendNotification);

    mute.setVisible (! master);
    solo.setVisible (! master);
    arm.setVisible (kind == model::TrackKind::audio || kind == model::TrackKind::instrument);
    monitor.setVisible (kind == model::TrackKind::audio);
    mute.setToggleState (t->mute, juce::dontSendNotification);
    solo.setToggleState (studio.localView().soloed.contains (id), juce::dontSendNotification);
    arm.setToggleState (studio.localView().armed.contains (id), juce::dontSendNotification);
    monitor.setToggleState (t->monitor == "on", juce::dontSendNotification);
    monitor.setButtonText (t->monitor == "off" ? "-" : "I");
    describe (monitor, "Input monitoring", "Input monitoring: " + juce::String (t->monitor));

    // Locked by someone else (05 §7, 04 §9.6): everything but solo and record enable is disabled.
    const bool locked = studio.lockedByOther (id);
    const auto lockTip = studio.lockTooltip (id);
    name.setEditable (false, ! master && ! locked, true);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &name, &input, &output, &automation, &pan, &volume, &readout, &mute, &monitor })
    {
        if (c != &name)
            c->setEnabled (! locked);
        auto* tip = dynamic_cast<juce::SettableTooltipClient*> (c);
        if (tip == nullptr)
            continue;
        if (locked)
        {
            if (! tipsBeforeLock.contains (c))
                tipsBeforeLock[c] = tip->getTooltip();
            tip->setTooltip (lockTip);
        }
        else if (auto it = tipsBeforeLock.find (c); it != tipsBeforeLock.end())
        {
            if (tip->getTooltip().startsWith ("Locked by"))
                tip->setTooltip (it->second);
            tipsBeforeLock.erase (it);
        }
    }
    for (auto& slot : inserts)
        slot->setEnabled (! locked);
    for (auto& slot : sends)
        slot->setEnabled (! locked);
    resized();
    repaint();
}

void ChannelStrip::updateMeter (double now)
{
    const auto m = studio.engine().takeMeter (isMaster() ? Id() : id);
    meter.update (m.output, m.rms, m.clip, now);
}

void ChannelStrip::followAutomation()
{
    // Controls move with the automation the engine plays (W-8 "control follows automation").
    const auto v = studio.automationValue (id, targetOf ("volume"));
    const auto p = studio.automationValue (id, targetOf ("pan"));
    const auto m = studio.automationValue (id, targetOf ("mute"));
    if (v && ! volume.isMouseButtonDown())
    {
        const auto db = automation::toDb (*v);
        setDbSlider (volume, db);
        if (! readout.isBeingEdited())
            readout.setText (fader::dbText (db), juce::dontSendNotification);
    }
    if (p && ! pan.isMouseButtonDown())
        pan.setValue (automation::toPan (*p) * 100.0, juce::dontSendNotification);
    if (m)
        mute.setToggleState (automation::toMute (*m), juce::dontSendNotification);
    const bool following = v || p || m;
    if (followed && ! following)
        refresh(); // back to the static values
    followed = following;
}

void ChannelStrip::paint (juce::Graphics& g)
{
    const auto* t = track();
    const auto selected = ! isMaster() && studio.selectedTrack() == id;
    // Each strip is a raised panel on the mixer's ground, with the track colour along the top.
    const auto panel = getLocalBounds().toFloat().reduced (1.0f, 0.0f);
    depth::shadow (g, panel, radius::r1, depth::Level::panel);
    g.setColour (selected ? colour (Token::hover) : colour (Token::surface));
    g.fillRoundedRectangle (panel, radius::r1);
    depth::highlight (g, panel, radius::r1);
    if (t != nullptr)
    {
        g.setColour (isMaster() ? colour (Token::textDim) : colourFor (t->color.palette, t->color.rgb));
        g.fillRect (panel.withHeight (4.0f));
    }
    if (selected)
    {
        g.setColour (colour (Token::accent));
        g.fillRect (panel.withTrimmedTop (4.0f).withHeight (2.0f));
    }
    if (! isMaster() && ! studio.lockTooltip (id).isEmpty())
        drawLock (g, juce::Rectangle<float> (static_cast<float> (getWidth()) - 16.0f, 9.0f, 10.0f, 13.0f),
                  studio.lockedByOther (id) ? colour (Token::warning) : colour (Token::textDim));
    if (! slotArea.isEmpty())
    {
        g.setColour (colour (Token::lineSubtle));
        g.fillRect (slotArea.getX(), slotArea.getY() - gap / 2 - 1, slotArea.getWidth(), 1);
        g.fillRect (slotArea.getX(), slotArea.getBottom() + gap / 2, slotArea.getWidth(), 1);
    }
    // The dB scale between the fader and the meter: both use the fader's taper.
    if (! scaleArea.isEmpty())
    {
        const auto travel = scaleArea.toFloat().reduced (0.0f, size::faderCap / 2.0f);
        g.setFont (font (Type::monoSmall));
        for (const auto db : { 6.0f, 0.0f, -6.0f, -12.0f, -24.0f, -48.0f, -90.0f })
        {
            const auto y = std::round (travel.getBottom() - travel.getHeight() * MeterView::proportion (db));
            // Numbers only: the meter carries the tick lines, so the scale stays quiet.
            g.setColour (db == 0.0f ? colour (Token::text) : colour (Token::textDim));
            const auto label = db <= -90.0f ? juce::String ("inf") : db > 0.0f ? "+" + juce::String (juce::roundToInt (db)) : juce::String (juce::roundToInt (db));
            g.drawText (label, juce::Rectangle<float> (travel.getX(), y - 6.0f, travel.getWidth() - 2.0f, 12.0f), juce::Justification::centredRight, false);
        }
    }
    if (buttonRow.getWidth() > 0 && mute.isVisible())
        paintGroup (g, buttonRow);
}

void ChannelStrip::paintOverChildren (juce::Graphics& g)
{
    // The fader's value tag: shown while the pointer is on the fader or dragging it, over the scale.
    const bool dragging = volume.isMouseButtonDown();
    if (! (volume.isMouseOver() || dragging) || ! volume.isEnabled())
        return;
    const auto fine = dragging && juce::ModifierKeys::currentModifiers.isShiftDown();
    const auto db = isSilent (volume.getValue()) ? fader::silentDb : volume.getValue();
    const auto text = fader::dbText (db) + (fine ? " fine" : "");
    const auto f = font (Type::monoValue);
    const auto w = static_cast<float> (juce::GlyphArrangement::getStringWidthInt (f, text)) + space::s1;
    const auto travel = volume.getBounds().toFloat().reduced (0.0f, size::faderCap / 2.0f);
    const auto y = travel.getBottom() - travel.getHeight() * static_cast<float> (volume.valueToProportionOfLength (volume.getValue()));
    const auto tag = juce::Rectangle<float> (static_cast<float> (volume.getRight()) - 2.0f, y - 10.0f, w, 20.0f);
    depth::shadow (g, tag, radius::r1, depth::Level::floating);
    g.setColour (colour (Token::surfaceRaised));
    g.fillRoundedRectangle (tag, radius::r1);
    g.setColour (fine ? colour (Token::accent) : colour (Token::border));
    g.drawRoundedRectangle (tag.reduced (0.5f), radius::r1, 1.0f);
    g.setColour (colour (Token::text));
    g.setFont (f);
    g.drawText (text, tag, juce::Justification::centred, false);
}

void ChannelStrip::resized()
{
    auto r = getLocalBounds().reduced (space::half + 1, 0);
    r.removeFromTop (space::s1);
    r.removeFromBottom (space::s1);
    name.setBounds (r.removeFromTop (rowHeight));
    r.removeFromTop (gap);
    input.setBounds (r.removeFromTop (rowHeight));
    r.removeFromTop (gap);
    slotArea = r.removeFromTop ((shownInsertRows() + 1 + shownSendRows()) * rowHeight);
    auto slots = slotArea;
    auto insertRowsArea = slots.removeFromTop (shownInsertRows() * rowHeight);
    for (auto& c : inserts)
        c->setBounds (insertRowsArea.removeFromTop (rowHeight));
    slots.removeFromTop (rowHeight);
    for (auto& c : sends)
        c->setBounds (slots.removeFromTop (rowHeight));
    r.removeFromTop (gap);
    output.setBounds (r.removeFromTop (rowHeight));
    r.removeFromTop (gap);
    automation.setBounds (r.removeFromTop (rowHeight));
    r.removeFromTop (gap);
    pan.setBounds (r.removeFromTop (space::s4).withSizeKeepingCentre (space::s4, space::s4));
    r.removeFromTop (gap);
    buttonRow = r.removeFromBottom (rowHeight);
    auto buttons = buttonRow;
    std::vector<juce::TextButton*> shown;
    for (auto* b : { &mute, &solo, &arm, &monitor })
        if (b->isVisible())
            shown.push_back (b);
    // A segmented group: one frame, the buttons share edges.
    const auto bw = shown.empty() ? 0 : buttons.getWidth() / static_cast<int> (shown.size());
    for (std::size_t i = 0; i < shown.size(); ++i)
    {
        shown[i]->setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0) | (i + 1 < shown.size() ? juce::Button::ConnectedOnRight : 0));
        shown[i]->setBounds (i + 1 < shown.size() ? buttons.removeFromLeft (bw) : buttons);
    }
    for (auto* b : { &mute, &solo, &arm, &monitor })
        if (! b->isVisible())
            b->setBounds ({});
    r.removeFromBottom (gap);
    readout.setBounds (r.removeFromBottom (rowHeight));
    r.removeFromBottom (gap);
    // Fader | scale | meter. The meter's clip light sits above the fader's travel.
    auto faderArea = r;
    meter.setBounds (faderArea.removeFromRight (space::s2));
    faderArea.removeFromTop (MeterView::clipHeight + MeterView::clipGap);
    scaleArea = faderArea.removeFromRight (space::s3 + space::half);
    volume.setBounds (faderArea);
}

void ChannelStrip::mouseDown (const juce::MouseEvent&)
{
    if (! isMaster())
        studio.selectTrack (id);
}

void ChannelStrip::showMenu (juce::PopupMenu menu, std::function<void (int)> picked)
{
    juce::Component::SafePointer<ChannelStrip> self (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [self, picked = std::move (picked)] (int item) {
        if (self != nullptr && item != 0)
            picked (item);
    });
}

void ChannelStrip::openEditor (const Id& pluginId)
{
    if (studio.openPluginEditor)
        studio.openPluginEditor (pluginId);
}

namespace
{
/** Built-ins at the top, then a submenu per vendor (W-6 step 1). */
void addPluginChoices (juce::PopupMenu& menu, const std::vector<Studio::PluginChoice>& choices, int firstItem)
{
    std::map<juce::String, juce::PopupMenu> vendors;
    for (std::size_t i = 0; i < choices.size(); ++i)
    {
        const auto item = firstItem + static_cast<int> (i);
        if (choices[i].plugin.format == "builtin")
            menu.addItem (item, choices[i].label);
        else
            vendors[choices[i].group].addItem (item, choices[i].label);
    }
    if (! vendors.empty())
        menu.addSeparator();
    for (auto& [vendor, sub] : vendors)
        menu.addSubMenu (vendor, sub);
}
} // namespace

juce::PopupMenu ChannelStrip::insertMenu (std::size_t slot) const
{
    juce::PopupMenu menu;
    const auto* t = track();
    if (t == nullptr)
        return menu;
    const auto choices = studio.pluginChoices (false);
    if (slot >= t->strip.inserts.size())
    {
        addPluginChoices (menu, choices, itemPlugin);
        return menu;
    }
    const auto& p = t->strip.inserts[slot];
    menu.addItem (itemOpen, "Open " + juce::String (p.name));
    menu.addItem (itemBypass, "Bypass", true, p.bypass);
    menu.addItem (itemRemove, "Remove");
    juce::PopupMenu replace;
    addPluginChoices (replace, choices, itemPlugin);
    menu.addSubMenu ("Replace", replace);
    return menu;
}

void ChannelStrip::pickInsert (std::size_t slot, int itemId)
{
    const auto* t = track();
    if (t == nullptr)
        return;
    const auto filled = slot < t->strip.inserts.size();
    const auto pluginId = filled ? t->strip.inserts[slot].id : Id();
    if (itemId >= itemPlugin)
    {
        const auto choices = studio.pluginChoices (false);
        const auto i = static_cast<std::size_t> (itemId - itemPlugin);
        if (i >= choices.size())
            return;
        if (filled)
            studio.replacePlugin (id, pluginId, choices[i]);
        else
            studio.addPlugin (id, choices[i], slot);
        return;
    }
    if (! filled)
        return;
    switch (itemId)
    {
        case itemOpen: openEditor (pluginId); break;
        case itemBypass:
            studio.check (studio.session().setInsertBypass (id, pluginId, ! t->strip.inserts[slot].bypass));
            studio.edited();
            break;
        case itemRemove:
            studio.check (studio.session().removeInsert (id, pluginId));
            studio.edited();
            break;
        default: break;
    }
}

void ChannelStrip::moveInsert (std::size_t from, std::size_t to)
{
    const auto* t = track();
    if (t == nullptr || from >= t->strip.inserts.size() || from == to)
        return;
    studio.check (studio.session().moveInsert (id, t->strip.inserts[from].id, std::min (to, t->strip.inserts.size() - 1)));
    studio.edited();
}

juce::PopupMenu ChannelStrip::instrumentMenu() const
{
    juce::PopupMenu menu;
    const auto* t = track();
    if (t == nullptr)
        return menu;
    if (t->strip.instrument)
    {
        menu.addItem (itemOpen, "Open " + juce::String (t->strip.instrument->name));
        menu.addSeparator();
    }
    addPluginChoices (menu, studio.pluginChoices (true), itemPlugin);
    return menu;
}

void ChannelStrip::pickInstrument (int itemId)
{
    const auto* t = track();
    if (t == nullptr)
        return;
    if (itemId == itemOpen && t->strip.instrument)
    {
        openEditor (t->strip.instrument->id);
        return;
    }
    const auto choices = studio.pluginChoices (true);
    const auto i = static_cast<std::size_t> (itemId - itemPlugin);
    if (itemId >= itemPlugin && i < choices.size())
        studio.setInstrument (id, choices[i]);
}

juce::PopupMenu ChannelStrip::inputMenu() const
{
    juce::PopupMenu menu;
    const auto* t = track();
    if (t == nullptr)
        return menu;
    const auto inputs = studio.device().config().numInputChannels;
    const auto current = t->input.kind == model::InputAssignment::Kind::audio ? t->input.firstChannel : -1;
    menu.addItem (itemMaster, "No Input", true, current < 0);
    const auto pair = t->channels == 1 ? 1 : 2;
    for (int ch = 0; ch + pair <= inputs; ch += pair)
        menu.addItem (itemBus + ch, pair == 1 ? "Input " + juce::String (ch + 1) : "Input " + juce::String (ch + 1) + "-" + juce::String (ch + 2), true, current == ch);
    return menu;
}

void ChannelStrip::pickInput (int itemId)
{
    model::InputAssignment in;
    if (itemId >= itemBus)
    {
        in.kind = model::InputAssignment::Kind::audio;
        in.firstChannel = itemId - itemBus;
    }
    else if (itemId != itemMaster)
        return;
    studio.check (studio.session().setTrackInput (id, in));
    studio.edited();
}

juce::PopupMenu ChannelStrip::sendMenu (std::size_t slot) const
{
    juce::PopupMenu menu;
    const auto* t = track();
    if (t == nullptr)
        return menu;
    const auto filled = slot < t->strip.sends.size();
    const auto* send = filled ? &t->strip.sends[slot] : nullptr;
    const auto edgeKey = filled ? model::RoutingEdge { id, send->targetBusId, send->id }.key() : std::string();
    const auto all = buses();
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        const auto* b = all[i];
        const auto current = send != nullptr && send->targetBusId == b->id;
        bool taken = false;
        for (const auto& other : t->strip.sends)
            taken = taken || (other.targetBusId == b->id && (send == nullptr || other.id != send->id));
        const auto loops = ! current && studio.session().wouldCycle (id, b->id, edgeKey);
        juce::String label (b->name);
        if (loops)
            label << " (" << feedbackText << ")";
        else if (taken)
            label << " (already sending)";
        menu.addItem (itemBus + static_cast<int> (i), label, ! loops && ! taken, current);
    }
    if (! filled)
    {
        if (! all.empty())
            menu.addSeparator();
        menu.addItem (itemNewBus, "New Bus");
        return menu;
    }
    menu.addSeparator();
    menu.addItem (itemPre, "Pre Fader", true, send->pre);
    menu.addItem (itemMuteSend, "Mute Send", true, send->muted);
    menu.addItem (itemRemove, "Remove Send");
    return menu;
}

void ChannelStrip::pickSend (std::size_t slot, int itemId)
{
    const auto* t = track();
    if (t == nullptr)
        return;
    auto& s = studio.session();
    const auto filled = slot < t->strip.sends.size();
    const auto sendId = filled ? t->strip.sends[slot].id : Id();
    if (itemId == itemNewBus && ! filled)
    {
        if (! s.newBus (id, true))
            studio.setStatus (juce::String (s.lastError()));
        studio.edited();
        return;
    }
    if (itemId >= itemBus)
    {
        const auto all = buses();
        const auto i = static_cast<std::size_t> (itemId - itemBus);
        if (i >= all.size())
            return;
        if (filled)
            studio.check (s.setSendTarget (id, sendId, all[i]->id));
        else if (! s.addSend (id, all[i]->id))
            studio.setStatus (juce::String (s.lastError()));
        studio.edited();
        return;
    }
    if (! filled)
        return;
    const auto& send = t->strip.sends[slot];
    switch (itemId)
    {
        case itemPre: studio.check (s.setSendPre (id, sendId, ! send.pre)); break;
        case itemMuteSend: studio.check (s.setSendMuted (id, sendId, ! send.muted)); break;
        case itemRemove: studio.check (s.removeSend (id, sendId)); break;
        default: return;
    }
    studio.edited();
}

juce::PopupMenu ChannelStrip::outputMenu() const
{
    juce::PopupMenu menu;
    const auto* t = track();
    if (t == nullptr)
        return menu;
    menu.addItem (itemMaster, "Master", true, t->output.busId.empty());
    const auto all = buses();
    const auto edgeKey = model::RoutingEdge { id, t->output.busId, {} }.key();
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        const auto* b = all[i];
        const auto current = t->output.busId == b->id;
        const auto loops = ! current && studio.session().wouldCycle (id, b->id, edgeKey);
        menu.addItem (itemBus + static_cast<int> (i), loops ? juce::String (b->name) + " (" + feedbackText + ")" : juce::String (b->name), ! loops, current);
    }
    menu.addSeparator();
    menu.addItem (itemNewBus, "New Bus");
    return menu;
}

void ChannelStrip::pickOutput (int itemId)
{
    auto& s = studio.session();
    if (itemId == itemMaster)
        studio.check (s.setTrackOutput (id, {}));
    else if (itemId == itemNewBus)
    {
        if (! s.newBus (id, false))
            studio.setStatus (juce::String (s.lastError()));
    }
    else if (itemId >= itemBus)
    {
        const auto all = buses();
        const auto i = static_cast<std::size_t> (itemId - itemBus);
        if (i >= all.size())
            return;
        studio.check (s.setTrackOutput (id, all[i]->id));
    }
    studio.edited();
}

// Mixer view ---------------------------------------------------------------------------------

MixerView::MixerView (Studio& s) : studio (s)
{
    setTitle ("Mixer");
    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, true);
    addAndMakeVisible (viewport);
    studio.addChangeListener (this);
    refresh();
    startTimerHz (30);
}

MixerView::~MixerView() { studio.removeChangeListener (this); }

ChannelStrip* MixerView::stripFor (const Id& trackId)
{
    if (master != nullptr && master->trackId() == trackId)
        return master.get();
    for (auto& s : channelStrips)
        if (s->trackId() == trackId)
            return s.get();
    return nullptr;
}

void MixerView::refresh()
{
    const auto& m = studio.model();
    std::vector<Id> order;
    for (const auto& tid : m.trackOrder)
        if (const auto* t = m.track (tid); t != nullptr && t->kind != model::TrackKind::bus && t->kind != model::TrackKind::master)
            order.push_back (tid);
    for (const auto& tid : m.trackOrder)
        if (const auto* t = m.track (tid); t != nullptr && t->kind == model::TrackKind::bus)
            order.push_back (tid);
    if (order != shownOrder)
    {
        std::vector<std::unique_ptr<ChannelStrip>> next;
        for (const auto& tid : order)
        {
            auto it = std::find_if (channelStrips.begin(), channelStrips.end(), [&] (const auto& s) { return s != nullptr && s->trackId() == tid; });
            if (it != channelStrips.end())
                next.push_back (std::move (*it));
            else
            {
                next.push_back (std::make_unique<ChannelStrip> (studio, tid));
                content.addAndMakeVisible (*next.back());
            }
        }
        channelStrips = std::move (next);
        shownOrder = order;
    }
    if (master == nullptr || master->trackId() != m.info.masterTrackId)
    {
        master = std::make_unique<ChannelStrip> (studio, m.info.masterTrackId);
        addAndMakeVisible (*master);
    }
    for (auto& s : channelStrips)
        s->refresh();
    master->refresh();
    layoutStrips();
}

void MixerView::layoutStrips()
{
    int inserts = 1, sendRows = 1;
    for (auto& s : channelStrips)
    {
        s->insertRows = s->sendRows = 1;
        inserts = std::max (inserts, s->shownInsertRows());
        sendRows = std::max (sendRows, s->shownSendRows());
    }
    master->insertRows = master->sendRows = 1;
    inserts = std::max (inserts, master->shownInsertRows());
    for (auto& s : channelStrips)
    {
        s->insertRows = inserts;
        s->sendRows = sendRows;
    }
    master->insertRows = inserts;
    master->sendRows = sendRows;
    resized();
}

void MixerView::resized()
{
    auto r = getLocalBounds();
    if (master != nullptr)
        master->setBounds (r.removeFromRight (ChannelStrip::width + space::s1).withTrimmedLeft (space::s1).withHeight (std::max (r.getHeight(), master->preferredHeight())));
    viewport.setBounds (r);
    const auto innerHeight = std::max (viewport.getMaximumVisibleHeight(), channelStrips.empty() ? 0 : channelStrips.front()->preferredHeight());
    content.setSize (static_cast<int> (channelStrips.size()) * ChannelStrip::width, innerHeight);
    for (std::size_t i = 0; i < channelStrips.size(); ++i)
        channelStrips[i]->setBounds (static_cast<int> (i) * ChannelStrip::width, 0, ChannelStrip::width, innerHeight);
}

void MixerView::paint (juce::Graphics& g) { g.fillAll (colour (Token::background)); }

void MixerView::visibilityChanged()
{
    if (isShowing())
        refresh();
}

void MixerView::timerCallback()
{
    if (! isShowing())
        return;
    const auto now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    for (auto& s : channelStrips)
    {
        s->updateMeter (now);
        s->followAutomation();
    }
    if (master != nullptr)
    {
        master->updateMeter (now);
        master->followAutomation();
    }
}
} // namespace weft::ui
