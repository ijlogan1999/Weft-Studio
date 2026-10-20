#include "PianoRoll.h"

#include "PresenceUi.h"

#include "weft/time/Position.h"
#include "weft/time/Quantize.h"

#include <cmath>

namespace weft::ui
{
using commands::NoteRef;

const std::vector<GridValue>& gridValues()
{
    static const std::vector<GridValue> values { { "Off", 0 },     { "1/1", 3840 },  { "1/2", 1920 }, { "1/4", 960 },   { "1/8", 480 },
                                                 { "1/16", 240 },  { "1/32", 120 },  { "1/64", 60 },  { "1/8T", 320 },  { "1/16T", 160 } };
    return values;
}

namespace
{
const juce::StringArray toolNames { "Pointer", "Pencil", "Eraser", "Velocity", "Scissors" };

int gridIndex (Ticks ticks)
{
    const auto& v = gridValues();
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i].ticks == ticks)
            return static_cast<int> (i);
    return 5;
}

juce::String laneName (const LaneKind& k)
{
    switch (k.type)
    {
        case LaneKind::Type::velocity: return "Velocity";
        case LaneKind::Type::pitchBend: return "Pitch Bend";
        case LaneKind::Type::pressure: return "Aftertouch";
        case LaneKind::Type::cc: break;
    }
    if (k.number == 1)
        return "Modulation (CC 1)";
    if (k.number == 64)
        return "Sustain (CC 64)";
    return "CC " + juce::String (k.number);
}
} // namespace

PianoRoll::PianoRoll (Studio& s) : studio (s)
{
    setTitle ("Piano Roll");

    toolBox.addItemList (toolNames, 1);
    describe (toolBox, "Piano roll tool", "Piano roll tool: pointer, pencil, eraser, velocity or scissors");
    toolBox.onChange = [this] {
        if (! refreshing && toolBox.getSelectedItemIndex() >= 0)
            setTool (static_cast<NoteTool> (toolBox.getSelectedItemIndex()));
    };

    for (const auto& g : gridValues())
    {
        snapBox.addItem (juce::String ("Snap ") + g.name, snapBox.getNumItems() + 1);
        if (g.ticks > 0)
            quantizeBox.addItem (juce::String ("Q ") + g.name, quantizeBox.getNumItems() + 1);
    }
    describe (snapBox, "Note snap", "Grid that drawn, moved and resized notes snap to");
    snapBox.onChange = [this] {
        if (! refreshing && snapBox.getSelectedItemIndex() >= 0)
            studio.piano.grid = gridValues()[static_cast<std::size_t> (snapBox.getSelectedItemIndex())].ticks;
    };
    describe (quantizeBox, "Quantize grid", "Grid value the Q button quantizes notes to");
    quantizeBox.onChange = [this] {
        if (! refreshing && quantizeBox.getSelectedItemIndex() >= 0)
            studio.piano.quantizeGrid = gridValues()[static_cast<std::size_t> (quantizeBox.getSelectedItemIndex()) + 1].ticks;
    };

    for (auto* sl : { &strength, &swing })
    {
        sl->setSliderStyle (juce::Slider::LinearBar);
        sl->setRange (0.0, 100.0, 1.0);
        sl->setTextBoxIsEditable (true);
    }
    strength.textFromValueFunction = [] (double v) { return "Strength " + juce::String (juce::roundToInt (v)) + "%"; };
    swing.textFromValueFunction = [] (double v) { return "Swing " + juce::String (juce::roundToInt (v)) + "%"; };
    for (auto* sl : { &strength, &swing })
        sl->valueFromTextFunction = [] (const juce::String& t) { return t.retainCharacters ("0123456789.").getDoubleValue(); };
    strength.updateText();
    swing.updateText();
    describe (strength, "Quantize strength", "Quantize strength, 0 to 100%");
    describe (swing, "Quantize swing", "Quantize swing, 0 (straight) to 100% (triplet feel)");
    strength.onValueChange = [this] {
        if (! refreshing)
            studio.piano.strength = strength.getValue();
    };
    swing.onValueChange = [this] {
        if (! refreshing)
            studio.piano.swing = swing.getValue();
    };

    describe (quantizeButton, Cmd::quantize);
    quantizeButton.onClick = [this] {
        studio.piano.focus = true;
        studio.perform (Cmd::quantize);
    };
    describe (auditionToggle, "Audition", "Play notes as they are clicked, drawn or moved");
    auditionToggle.onClick = [this] { studio.piano.audition = auditionToggle.getToggleState(); };
    describe (followToggle, "Follow", "Scroll the piano roll with the playhead");
    followToggle.onClick = [this] { studio.piano.follow = followToggle.getToggleState(); };

    laneBox.addItem ("Velocity", 1);
    laneBox.addItem ("Modulation (CC 1)", 2);
    laneBox.addItem ("Sustain (CC 64)", 3);
    laneBox.addItem ("Pitch Bend", 4);
    laneBox.addItem ("Aftertouch", 6);
    laneBox.addItem ("Hide lane", 5);
    laneBox.addSeparator();
    for (int n = 0; n < 128; ++n)
        laneBox.addItem ("CC " + juce::String (n), 10 + n);
    describe (laneBox, "Controller lane", "What the lane under the notes shows: velocity, a controller, or pitch bend");
    laneBox.onChange = [this] {
        if (refreshing)
            return;
        const auto id = laneBox.getSelectedId();
        laneShown = id != 5;
        if (id == 1)
            lane = {};
        else if (id == 2)
            lane = { LaneKind::Type::cc, 1 };
        else if (id == 3)
            lane = { LaneKind::Type::cc, 64 };
        else if (id == 4)
            lane = { LaneKind::Type::pitchBend, 0 };
        else if (id == 6)
            lane = { LaneKind::Type::pressure, 0 };
        else if (id >= 10)
            lane = { LaneKind::Type::cc, id - 10 };
        resized();
        repaint();
    };

    for (juce::Component* c : std::initializer_list<juce::Component*> { &toolBox, &snapBox, &quantizeBox, &strength, &swing, &quantizeButton,
                                                                          &auditionToggle, &followToggle, &laneBox })
    {
        makeFocusable (*c);
        addAndMakeVisible (c);
    }
    studio.addChangeListener (this);
    refreshToolbar();
}

PianoRoll::~PianoRoll() { studio.removeChangeListener (this); }

void PianoRoll::setTool (NoteTool t)
{
    tool = t;
    const juce::ScopedValueSetter<bool> guard (refreshing, true);
    toolBox.setSelectedItemIndex (static_cast<int> (t), juce::dontSendNotification);
}

void PianoRoll::refreshToolbar()
{
    const juce::ScopedValueSetter<bool> guard (refreshing, true);
    const auto& p = studio.piano;
    toolBox.setSelectedItemIndex (static_cast<int> (tool), juce::dontSendNotification);
    snapBox.setSelectedItemIndex (gridIndex (p.grid), juce::dontSendNotification);
    quantizeBox.setSelectedItemIndex (std::max (0, gridIndex (p.quantizeGrid) - 1), juce::dontSendNotification);
    strength.setValue (p.strength, juce::dontSendNotification);
    swing.setValue (p.swing, juce::dontSendNotification);
    auditionToggle.setToggleState (p.audition, juce::dontSendNotification);
    followToggle.setToggleState (p.follow, juce::dontSendNotification);
    if (! laneShown)
        laneBox.setSelectedId (5, juce::dontSendNotification);
    else if (lane.type == LaneKind::Type::velocity)
        laneBox.setSelectedId (1, juce::dontSendNotification);
    else if (lane.type == LaneKind::Type::pitchBend)
        laneBox.setSelectedId (4, juce::dontSendNotification);
    else if (lane.type == LaneKind::Type::pressure)
        laneBox.setSelectedId (6, juce::dontSendNotification);
    else
        laneBox.setSelectedId (lane.number == 1 ? 2 : lane.number == 64 ? 3 : 10 + lane.number, juce::dontSendNotification);
}

void PianoRoll::changeListenerCallback (juce::ChangeBroadcaster*)
{
    regions = studio.pianoRollRegions();
    std::string key;
    for (const auto& id : regions)
        key += id + ",";
    if (key != shownKey)
    {
        shownKey = key;
        needsFit = true;
    }
    if (needsFit && getWidth() > 0 && isVisible())
        fitRegions();
    refreshToolbar();
    repaint();
}

void PianoRoll::visibilityChanged()
{
    if (isVisible())
    {
        changeListenerCallback (nullptr);
        startTimerHz (30);
    }
    else
        stopTimer();
}

void PianoRoll::timerCallback()
{
    if (! studio.playing())
    {
        // Collaborators' pointers move and fade without anything else changing.
        if (! studio.collaborators().empty())
            repaint();
        return;
    }
    if (studio.piano.follow)
    {
        const auto x = tickToX (static_cast<double> (studio.playhead()));
        const auto g = gridArea();
        if (x > g.getRight() - 10 || x < g.getX())
            originTick = static_cast<double> (studio.playhead()) - 20.0 / pixelsPerTick;
    }
    repaint();
}

// Geometry -------------------------------------------------------------------------------------

juce::Rectangle<int> PianoRoll::keyboardArea() const
{
    return { 0, toolbarHeight + rulerHeight, keyboardWidth, gridArea().getHeight() };
}

juce::Rectangle<int> PianoRoll::rulerArea() const { return { keyboardWidth, toolbarHeight, std::max (0, getWidth() - keyboardWidth), rulerHeight }; }

juce::Rectangle<int> PianoRoll::gridArea() const
{
    const auto top = toolbarHeight + rulerHeight;
    const auto bottom = getHeight() - (laneShown ? laneHeight : 0);
    return { keyboardWidth, top, std::max (0, getWidth() - keyboardWidth), std::max (0, bottom - top) };
}

juce::Rectangle<int> PianoRoll::laneArea() const
{
    if (! laneShown)
        return {};
    return { keyboardWidth, getHeight() - laneHeight, std::max (0, getWidth() - keyboardWidth), laneHeight };
}

int PianoRoll::yToPitch (float y) const
{
    return std::clamp (topPitch - static_cast<int> (std::floor ((y - static_cast<float> (gridArea().getY())) / static_cast<float> (rowHeight))), 0, 127);
}

int PianoRoll::laneValueAt (float y) const
{
    const auto a = laneArea().toFloat().reduced (0, 6);
    const auto f = std::clamp ((a.getBottom() - y) / std::max (1.0f, a.getHeight()), 0.0f, 1.0f);
    return lane.minValue() + juce::roundToInt (f * static_cast<float> (lane.maxValue() - lane.minValue()));
}

int PianoRoll::lineValueAt (float x) const
{
    const auto a = laneValueAt (downAt.y), b = laneValueAt (dragAt.y);
    if (std::abs (dragAt.x - downAt.x) < 1.0f)
        return b;
    const auto f = std::clamp ((x - downAt.x) / (dragAt.x - downAt.x), 0.0f, 1.0f);
    return juce::roundToInt (static_cast<float> (a) + f * static_cast<float> (b - a));
}

float PianoRoll::laneY (int value) const
{
    const auto a = laneArea().toFloat().reduced (0, 6);
    const auto f = static_cast<float> (value - lane.minValue()) / static_cast<float> (lane.maxValue() - lane.minValue());
    return a.getBottom() - f * a.getHeight();
}

void PianoRoll::resized()
{
    auto bar = getLocalBounds().removeFromTop (toolbarHeight).reduced (space::s1, space::half);
    const auto place = [&bar] (juce::Component& c, int w) {
        c.setBounds (bar.removeFromLeft (w));
        bar.removeFromLeft (space::s1);
    };
    place (toolBox, 96);
    place (snapBox, 96);
    place (quantizeBox, 88);
    place (strength, 112);
    place (swing, 96);
    place (quantizeButton, 32);
    place (auditionToggle, 88);
    place (followToggle, 72);
    place (laneBox, std::max (88, std::min (152, bar.getWidth())));
    if (needsFit && getWidth() > 0 && isVisible())
        fitRegions();
}

void PianoRoll::fitRegions()
{
    needsFit = false;
    const auto& m = studio.model();
    if (regions.empty())
        return;
    double from = 1e18, to = -1e18;
    int lo = 128, hi = -1;
    for (const auto& id : regions)
        if (const auto* r = m.region (id))
        {
            from = std::min (from, static_cast<double> (r->start));
            to = std::max (to, static_cast<double> (r->start + r->length));
        }
    for (const auto& n : shownNotes())
    {
        lo = std::min (lo, n.pitch);
        hi = std::max (hi, n.pitch);
    }
    const auto w = std::max (100, gridArea().getWidth());
    const auto range = std::max (960.0, to - from);
    pixelsPerTick = w * 0.95 / range;
    originTick = from - range * 0.025;
    const auto rows = std::max (1, gridArea().getHeight() / rowHeight);
    const auto centre = hi < 0 ? 60 : (lo + hi) / 2;
    topPitch = std::clamp (centre + rows / 2, std::min (127, rows - 1), 127);
}

// Notes ----------------------------------------------------------------------------------------

std::vector<PianoRoll::Shown> PianoRoll::shownNotes() const
{
    std::vector<Shown> out;
    const auto& m = studio.model();
    for (const auto& id : regions)
    {
        const auto* r = m.region (id);
        const auto* seq = r != nullptr ? m.sequence (r->midi.sequenceId) : nullptr;
        if (seq == nullptr)
            continue;
        for (const auto* n : seq->sortedNotes())
        {
            if (n->pos < r->midi.sequenceOffset || n->pos >= r->midi.sequenceOffset + r->length)
                continue;
            Shown s;
            s.ref = { id, n->id };
            s.stored = commands::noteTimelineTick (*r, n->pos);
            s.start = s.stored;
            if (const auto& q = r->midi.quantize) // drawn where it plays (W-5)
                s.start = std::max (r->start, quantizeTick (s.stored, q->grid, q->strength, q->swing * 100.0));
            s.length = n->length;
            s.pitch = n->pitch;
            s.velocity = n->velocity;
            s.channel = n->channel;
            s.muted = n->muted;
            s.bounds = noteBounds (s);
            out.push_back (s);
        }
    }
    return out;
}

juce::Rectangle<float> PianoRoll::noteBounds (const Shown& n) const
{
    const auto x = static_cast<float> (tickToX (static_cast<double> (n.start)));
    const auto w = std::max (3.0f, static_cast<float> (static_cast<double> (n.length) * pixelsPerTick));
    return { x, pitchToY (n.pitch) + 1.0f, w, static_cast<float> (rowHeight) - 1.0f };
}

std::optional<PianoRoll::Shown> PianoRoll::noteAt (juce::Point<float> p) const
{
    const auto notes = shownNotes();
    for (auto it = notes.rbegin(); it != notes.rend(); ++it) // the one drawn on top
        if (it->bounds.expanded (0.0f, 0.5f).contains (p))
            return *it;
    return std::nullopt;
}

Ticks PianoRoll::snapTick (double tick, bool floor) const
{
    const auto g = studio.piano.grid;
    const auto t = static_cast<Ticks> (std::llround (tick));
    if (g <= 0)
        return t;
    if (floor)
        return static_cast<Ticks> (std::floor (tick / static_cast<double> (g))) * g;
    return quantizeTick (t, g, 1.0, 0.0);
}

const model::Region* PianoRoll::regionFor (Ticks tick) const
{
    const model::Region *covering = nullptr, *before = nullptr;
    for (const auto& id : regions)
        if (const auto* r = studio.model().region (id); r != nullptr && r->start <= tick)
        {
            if (tick < r->start + r->length)
                covering = r;
            if (before == nullptr || r->start > before->start)
                before = r;
        }
    return covering != nullptr ? covering : before;
}

// Pointer --------------------------------------------------------------------------------------

void PianoRoll::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;
    pointerDown (e.position, e.mods);
}

void PianoRoll::mouseDrag (const juce::MouseEvent& e) { pointerDrag (e.position, e.mods); }
void PianoRoll::mouseUp (const juce::MouseEvent&) { pointerUp(); }

void PianoRoll::mouseMove (const juce::MouseEvent& e)
{
    auto shape = juce::MouseCursor::NormalCursor;
    if (gridArea().toFloat().contains (e.position) && ! regions.empty())
        studio.setPointer (collab::Presence::Pointer { collab::Presence::View::pianoRoll, static_cast<Ticks> (std::llround (xToTick (e.position.x))),
                                                       shownTrack(), yToPitch (e.position.y) });
    else
        studio.setPointer (std::nullopt);
    if (gridArea().toFloat().contains (e.position))
    {
        if (tool == NoteTool::pencil)
            shape = juce::MouseCursor::CrosshairCursor;
        else if (const auto n = noteAt (e.position); n && tool == NoteTool::pointer)
        {
            const auto edge = std::min (6.0f, n->bounds.getWidth() / 3.0f);
            if (e.position.x >= n->bounds.getRight() - edge || e.position.x <= n->bounds.getX() + edge)
                shape = juce::MouseCursor::LeftRightResizeCursor;
        }
    }
    setMouseCursor (shape);
}

void PianoRoll::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const auto dy = static_cast<double> (w.deltaY);
    if (e.mods.isCommandDown() && e.mods.isAltDown())
        rowHeight = std::clamp (rowHeight + (dy > 0 ? 1 : -1), 5, 24);
    else if (e.mods.isCommandDown())
    {
        const auto anchorTick = xToTick (e.position.x);
        pixelsPerTick = std::clamp (pixelsPerTick * std::pow (2.0, dy * 2.0), 0.002, 2.0);
        originTick = anchorTick - (e.position.x - gridArea().getX()) / pixelsPerTick;
    }
    else if (e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY))
        originTick -= (std::abs (w.deltaX) > 0.0f ? static_cast<double> (w.deltaX) : dy) * 200.0 / pixelsPerTick;
    else
        topPitch = std::clamp (topPitch + juce::roundToInt (dy * 12.0), std::min (127, gridArea().getHeight() / rowHeight - 1), 127);
    repaint();
}

void PianoRoll::pointerDown (juce::Point<float> p, juce::ModifierKeys mods)
{
    studio.piano.focus = true;
    downAt = dragAt = p;
    drag = Drag::none;
    anchor.reset();
    lanePoint.reset();
    deltaTicks = 0;
    deltaPitch = 0;
    velocityDelta = 0;
    if (keyboardArea().toFloat().contains (p))
        keyClicked (yToPitch (p.y), mods.isShiftDown());
    else if (rulerArea().toFloat().contains (p))
    {
        drag = Drag::ruler;
        studio.locate (std::max<Ticks> (0, snapTick (xToTick (p.x))));
    }
    else if (laneShown && laneArea().toFloat().contains (p))
        laneDown (p, mods);
    else if (gridArea().toFloat().contains (p))
    {
        studio.piano.point.reset();
        gridDown (p, mods);
    }
    repaint();
}

void PianoRoll::keyClicked (int pitch, bool add)
{
    studio.piano.focus = true;
    if (! regions.empty())
    {
        const auto was = studio.piano.audition;
        studio.piano.audition = true; // a key always sounds
        studio.auditionNote (regions.front(), pitch, studio.piano.lastVelocity, 1, 960);
        studio.piano.audition = was;
    }
    if (! add)
        studio.piano.selection.clear();
    for (const auto& n : shownNotes())
        if (n.pitch == pitch)
            studio.piano.selection.insert (n.ref);
    studio.sendChangeMessage();
}

void PianoRoll::gridDown (juce::Point<float> p, juce::ModifierKeys mods)
{
    auto& sel = studio.piano.selection;
    const auto hit = noteAt (p);
    const auto audition = [this] (const Shown& n) { studio.auditionNote (n.ref.regionId, n.pitch, n.velocity, n.channel, n.length); };
    switch (tool)
    {
        case NoteTool::eraser:
            if (hit)
                studio.check (studio.session().deleteNotes (std::vector<NoteRef> { hit->ref }));
            return;
        case NoteTool::scissors:
            if (hit)
                splitNoteAt (*hit, p);
            return;
        case NoteTool::velocity:
            if (hit)
            {
                if (! sel.contains (hit->ref))
                    sel = { hit->ref };
                anchor = hit;
                drag = Drag::velocity;
                audition (*hit);
                studio.sendChangeMessage();
            }
            return;
        case NoteTool::pencil:
            if (! hit)
            {
                drawNoteAt (p);
                return;
            }
            break;
        case NoteTool::pointer: break;
    }

    if (! hit)
    {
        if (mods.isCommandDown())
        {
            drawNoteAt (p); // Ctrl+click on empty grid draws (F-PRL-2)
            return;
        }
        rubberBase = mods.isShiftDown() ? sel : std::set<NoteRef> {};
        sel = rubberBase;
        drag = Drag::rubber;
        studio.sendChangeMessage();
        return;
    }

    if (mods.isShiftDown())
    {
        if (sel.contains (hit->ref))
            sel.erase (hit->ref);
        else
            sel.insert (hit->ref);
    }
    else if (! sel.contains (hit->ref))
        sel = { hit->ref };
    anchor = hit;
    const auto edge = std::min (6.0f, hit->bounds.getWidth() / 3.0f);
    if (mods.isCommandDown())
        drag = Drag::copy;
    else if (p.x >= hit->bounds.getRight() - edge)
        drag = Drag::resizeRight;
    else if (p.x <= hit->bounds.getX() + edge)
        drag = Drag::resizeLeft;
    else
        drag = Drag::move;
    span = { hit->stored, hit->length };
    if (! mods.isShiftDown() || sel.contains (hit->ref))
        audition (*hit);
    studio.sendChangeMessage();
}

void PianoRoll::drawNoteAt (juce::Point<float> p)
{
    const auto tick = snapTick (xToTick (p.x), true);
    const auto* r = regionFor (tick);
    if (r == nullptr)
        return;
    const auto regionId = r->id;
    auto& s = studio.session();
    s.beginGesture ("Add Note");
    const auto ref = s.drawNote (regionId, tick, yToPitch (p.y), studio.piano.lastLength, studio.piano.lastVelocity);
    if (! ref)
    {
        s.cancelGesture();
        if (! s.lastError().empty())
            studio.setStatus (juce::String (s.lastError()));
        return;
    }
    studio.piano.selection = { *ref };
    anchor = Shown {};
    anchor->ref = *ref;
    anchor->start = anchor->stored = tick;
    anchor->length = studio.piano.lastLength;
    anchor->pitch = yToPitch (p.y);
    anchor->velocity = studio.piano.lastVelocity;
    span = { tick, studio.piano.lastLength };
    drag = Drag::resizeRight; // dragging on draws a longer note
    studio.auditionNote (regionId, anchor->pitch, anchor->velocity, 1, anchor->length);
    studio.edited();
}

void PianoRoll::splitNoteAt (const Shown& n, juce::Point<float> p)
{
    const auto at = snapTick (xToTick (p.x));
    if (at <= n.stored || at >= n.stored + n.length)
        return;
    const auto* note = commands::findNote (studio.model(), n.ref);
    if (note == nullptr)
        return;
    const auto copy = *note;
    auto& s = studio.session();
    s.beginGesture ("Split Note");
    const bool ok = s.setNoteSpan (n.ref, n.stored, at - n.stored)
                    && s.drawNote (n.ref.regionId, at, copy.pitch, n.stored + n.length - at, copy.velocity, copy.channel).has_value();
    if (ok)
        s.endGesture();
    else
        s.cancelGesture();
    studio.edited();
}

void PianoRoll::pointerDrag (juce::Point<float> p, juce::ModifierKeys mods)
{
    dragAt = p;
    const bool free = mods.isAltDown(); // Alt drags without snap
    switch (drag)
    {
        case Drag::move:
        case Drag::copy:
        {
            const auto raw = xToTick (p.x) - xToTick (downAt.x);
            const auto target = static_cast<double> (anchor->stored) + raw;
            const auto newDelta = (free ? static_cast<Ticks> (std::llround (target)) : snapTick (target)) - anchor->stored;
            const auto newPitch = yToPitch (p.y) - yToPitch (downAt.y);
            if (newPitch != deltaPitch && std::clamp (anchor->pitch + newPitch, 0, 127) == anchor->pitch + newPitch)
                studio.auditionNote (anchor->ref.regionId, anchor->pitch + newPitch, anchor->velocity, anchor->channel, anchor->length);
            deltaTicks = newDelta;
            deltaPitch = newPitch;
            break;
        }
        case Drag::resizeLeft:
        case Drag::resizeRight:
            span = commands::resizeNote ({ anchor->stored, anchor->length }, drag == Drag::resizeRight, static_cast<Ticks> (std::llround (xToTick (p.x))),
                                         free ? 0 : studio.piano.grid);
            break;
        case Drag::rubber:
        {
            const auto box = juce::Rectangle<float> (downAt, p);
            auto sel = rubberBase;
            for (const auto& n : shownNotes())
                if (n.bounds.intersects (box))
                    sel.insert (n.ref);
            studio.piano.selection = sel;
            studio.sendChangeMessage();
            break;
        }
        case Drag::velocity: velocityDelta = juce::roundToInt (downAt.y - p.y); break;
        case Drag::laneVelocity:
        case Drag::laneNew: laneValue = laneValueAt (p.y); break;
        case Drag::laneLine: break;
        case Drag::laneMove:
            laneValue = laneValueAt (p.y);
            laneTick = std::max<Ticks> (0, free ? static_cast<Ticks> (std::llround (xToTick (p.x))) : snapTick (xToTick (p.x)));
            break;
        case Drag::ruler: studio.locate (std::max<Ticks> (0, snapTick (xToTick (p.x)))); break;
        case Drag::none: break;
    }
    repaint();
}

void PianoRoll::pointerUp()
{
    auto& s = studio.session();
    const std::vector<NoteRef> sel (studio.piano.selection.begin(), studio.piano.selection.end());
    const auto d = std::exchange (drag, Drag::none);
    switch (d)
    {
        case Drag::move:
            if (deltaTicks != 0 || deltaPitch != 0)
                studio.check (s.moveNotes (sel, deltaTicks, deltaPitch));
            break;
        case Drag::copy:
        {
            if (deltaTicks == 0 && deltaPitch == 0)
                break;
            auto clip = commands::NoteClipboard::of (studio.model(), sel);
            if (clip.empty())
                break;
            const bool pitchOk = std::all_of (clip.notes.begin(), clip.notes.end(),
                                              [this] (const model::Note& n) { return n.pitch + deltaPitch >= 0 && n.pitch + deltaPitch <= 127; });
            Ticks earliest = clip.notes.front().pos;
            for (auto& n : clip.notes)
            {
                earliest = std::min (earliest, n.pos);
                if (pitchOk)
                    n.pitch += deltaPitch;
            }
            const auto track = studio.model().effectiveTrackId (*studio.model().region (anchor->ref.regionId));
            const auto pasted = s.pasteNotes (clip, track, earliest + deltaTicks);
            if (pasted.empty())
                studio.setStatus (juce::String (s.lastError()));
            else
                studio.piano.selection = { pasted.begin(), pasted.end() };
            studio.edited();
            break;
        }
        case Drag::resizeLeft:
        case Drag::resizeRight:
            if (span.start != anchor->stored || span.length != anchor->length)
            {
                studio.check (s.setNoteSpan (anchor->ref, span.start, span.length));
                studio.piano.lastLength = span.length;
            }
            if (s.inGesture())
                s.endGesture();
            studio.edited();
            break;
        case Drag::velocity:
            if (velocityDelta != 0)
            {
                studio.check (s.offsetNotesVelocity (sel, velocityDelta));
                studio.piano.lastVelocity = std::clamp (anchor->velocity + velocityDelta, 1, 127);
            }
            break;
        case Drag::laneVelocity:
            studio.check (s.setNotesVelocity (std::vector<NoteRef> { anchor->ref }, laneValue));
            break;
        case Drag::laneNew:
            if (const auto* r = regionFor (laneTick))
            {
                model::CcEvent e;
                e.kind = lane.type == LaneKind::Type::pitchBend ? "pitchBend" : lane.type == LaneKind::Type::pressure ? "channelPressure" : "cc";
                e.number = lane.number;
                e.pos = laneTick;
                e.value = laneValue;
                if (! s.addController (r->id, e))
                    studio.setStatus (juce::String (s.lastError()));
                studio.edited();
            }
            break;
        case Drag::laneMove: studio.check (s.moveController (lanePoint->regionId, lanePoint->eventId, laneTick, laneValue)); break;
        case Drag::laneLine:
        {
            const auto x0 = std::min (downAt.x, dragAt.x), x1 = std::max (downAt.x, dragAt.x);
            s.beginGesture ("Velocity");
            for (const auto& n : shownNotes())
                if (const auto x = static_cast<float> (tickToX (static_cast<double> (n.start))); x >= x0 && x <= x1)
                    s.setNotesVelocity ({ n.ref }, std::clamp (lineValueAt (x), 1, 127));
            s.endGesture();
            studio.edited();
            break;
        }
        case Drag::rubber:
        case Drag::ruler:
        case Drag::none: break;
    }
    deltaTicks = 0;
    deltaPitch = 0;
    velocityDelta = 0;
    repaint();
}

// Controller lane -----------------------------------------------------------------------------------

std::vector<PianoRoll::LanePoint> PianoRoll::lanePoints() const
{
    std::vector<LanePoint> out;
    if (lane.type == LaneKind::Type::velocity)
        return out;
    const auto& m = studio.model();
    for (const auto& id : regions)
    {
        const auto* r = m.region (id);
        const auto* seq = r != nullptr ? m.sequence (r->midi.sequenceId) : nullptr;
        if (seq == nullptr)
            continue;
        for (const auto& [eventId, e] : seq->controllers)
        {
            const bool match = lane.type == LaneKind::Type::pitchBend  ? e.kind == "pitchBend"
                               : lane.type == LaneKind::Type::pressure ? e.kind == "channelPressure"
                                                                       : (e.kind == "cc" && e.number == lane.number);
            if (match && e.pos >= r->midi.sequenceOffset && e.pos < r->midi.sequenceOffset + r->length)
                out.push_back ({ id, eventId, commands::noteTimelineTick (*r, e.pos), e.value });
        }
    }
    std::sort (out.begin(), out.end(), [] (const LanePoint& a, const LanePoint& b) { return a.tick < b.tick; });
    return out;
}

std::optional<PianoRoll::LanePoint> PianoRoll::lanePointAt (juce::Point<float> p) const
{
    for (const auto& pt : lanePoints())
        if (juce::Point<float> (static_cast<float> (tickToX (static_cast<double> (pt.tick))), laneY (pt.value)).getDistanceFrom (p) <= 6.0f)
            return pt;
    return std::nullopt;
}

void PianoRoll::laneDown (juce::Point<float> p, juce::ModifierKeys mods)
{
    if (lane.type == LaneKind::Type::velocity && tool == NoteTool::pencil)
    {
        drag = Drag::laneLine; // dragging across bars draws a line of velocities
        return;
    }
    if (lane.type == LaneKind::Type::velocity)
    {
        std::optional<Shown> best;
        float bestDistance = 6.0f;
        for (const auto& n : shownNotes())
        {
            const auto dist = std::abs (static_cast<float> (tickToX (static_cast<double> (n.start))) + 1.5f - p.x);
            if (dist < bestDistance || (best && dist <= bestDistance && studio.piano.selection.contains (n.ref)))
            {
                best = n;
                bestDistance = dist;
            }
        }
        if (best)
        {
            anchor = best;
            drag = Drag::laneVelocity;
            laneValue = laneValueAt (p.y);
        }
        return;
    }
    const auto pt = lanePointAt (p);
    if (tool == NoteTool::eraser || mods.isAltDown())
    {
        if (pt)
            studio.check (studio.session().deleteControllers (pt->regionId, { pt->eventId }));
        studio.piano.point.reset();
        return;
    }
    laneValue = laneValueAt (p.y);
    studio.piano.point.reset();
    if (pt)
    {
        studio.piano.point = std::pair { pt->regionId, pt->eventId };
        lanePoint = pt;
        laneTick = pt->tick;
        drag = Drag::laneMove;
    }
    else
    {
        laneTick = std::max<Ticks> (0, snapTick (xToTick (p.x)));
        drag = Drag::laneNew;
    }
}

// Painting -------------------------------------------------------------------------------------

Id PianoRoll::shownTrack() const
{
    const auto& m = studio.model();
    if (! regions.empty())
        if (const auto* r = m.region (regions.front()))
            return m.effectiveTrackId (*r);
    return {};
}

void PianoRoll::paintCollaborators (juce::Graphics& g)
{
    // Collaborators with this track's notes open: their note selections and pointers (04 §7).
    const auto track = shownTrack();
    const auto notes = shownNotes();
    for (const auto* who : studio.collaborators())
    {
        if (! who->hasState)
            continue;
        const auto c = colourOf (*who);
        for (const auto& n : notes)
            if (std::find (who->state.notes.begin(), who->state.notes.end(), n.ref.noteId) != who->state.notes.end())
            {
                g.setColour (c);
                g.drawRect (n.bounds.expanded (1.0f), 1.5f);
            }
        const auto& ptr = who->state.pointer;
        if (ptr && ptr->area == collab::Presence::View::pianoRoll && ptr->track == track && ptr->pitch >= 0)
            drawPointer (g, { static_cast<float> (tickToX (static_cast<double> (ptr->pos))), pitchToY (ptr->pitch) + rowHeight * 0.5f }, c,
                         juce::String::fromUTF8 (who->displayName.c_str()), pointerAlpha (studio, *who));
    }
}

void PianoRoll::mouseExit (const juce::MouseEvent&) { studio.setPointer (std::nullopt); }

juce::Colour PianoRoll::trackColour() const
{
    const auto& m = studio.model();
    if (! regions.empty())
        if (const auto* r = m.region (regions.front()))
            if (const auto* t = m.track (m.effectiveTrackId (*r)))
                return colourFor (t->color.palette, t->color.rgb);
    return colour (Token::accent);
}

void PianoRoll::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::surface));
    if (regions.empty())
    {
        g.setColour (colour (Token::textDim));
        g.setFont (font (Type::title));
        g.drawText ("Double-click a MIDI region to edit its notes.", getLocalBounds().withTrimmedTop (toolbarHeight), juce::Justification::centred);
        return;
    }
    paintRuler (g);
    {
        const juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (gridArea());
        paintGrid (g);
        paintNotes (g);
        paintCollaborators (g);
    }
    paintKeyboard (g);
    if (laneShown)
    {
        const juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (laneArea().getUnion (laneArea().withX (0).withWidth (keyboardWidth)));
        paintLane (g);
    }
    // Playhead across the ruler, grid and lane.
    const auto x = static_cast<float> (tickToX (static_cast<double> (studio.playhead())));
    if (x >= static_cast<float> (keyboardWidth))
    {
        g.setColour (colour (Token::playhead));
        g.drawLine (x, static_cast<float> (toolbarHeight), x, static_cast<float> (getHeight()), 1.5f);
    }
}

void PianoRoll::paintKeyboard (juce::Graphics& g)
{
    const auto area = keyboardArea();
    const juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (area);
    // White keys in the fader-cap grey rather than white, so the keyboard doesn't glare.
    g.setGradientFill (juce::ColourGradient::horizontal (colour (Token::capBottom), 0.0f, colour (Token::capTop), static_cast<float> (keyboardWidth)));
    g.fillRect (area);
    g.setFont (font (Type::small));
    const auto first = yToPitch (static_cast<float> (area.getBottom() - 1));
    for (int pitch = first; pitch <= std::min (127, topPitch); ++pitch)
    {
        const auto y = pitchToY (pitch);
        const juce::Rectangle<float> row (0.0f, y, static_cast<float> (keyboardWidth), static_cast<float> (rowHeight));
        // A line between neighbouring white keys (B-C and E-F).
        if (pitch % 12 == 0 || pitch % 12 == 5)
        {
            g.setColour (colour (Token::regionText).withAlpha (0.25f));
            g.fillRect (0.0f, row.getBottom() - 1.0f, static_cast<float> (keyboardWidth), 1.0f);
        }
        if (commands::isBlackKey (pitch))
        {
            const auto key = row.withWidth (static_cast<float> (keyboardWidth) * 0.6f).reduced (0.0f, 1.0f);
            g.setColour (colour (Token::well));
            g.fillRoundedRectangle (key, radius::r1);
            g.setColour (colour (Token::text).withAlpha (0.12f));
            g.fillRect (key.withTrimmedRight (2.0f).withTrimmedTop (1.0f).withHeight (1.0f));
        }
        if (pitch % 12 == 0)
        {
            g.setColour (colour (Token::regionText));
            g.drawText (juce::String (commands::noteName (pitch)), row.withTrimmedRight (static_cast<float> (space::half)), juce::Justification::centredRight);
        }
    }
    g.setColour (colour (Token::border));
    g.drawVerticalLine (keyboardWidth - 1, static_cast<float> (area.getY()), static_cast<float> (area.getBottom()));
}

void PianoRoll::paintRuler (juce::Graphics& g)
{
    const auto area = rulerArea();
    const juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (area);
    g.setColour (colour (Token::surfaceRaised));
    g.fillRect (area);
    const auto& m = studio.model();
    // Region boundaries: a strip in the track colour along the bottom of the ruler.
    for (const auto& id : regions)
        if (const auto* r = m.region (id))
        {
            const auto x0 = static_cast<float> (tickToX (static_cast<double> (r->start)));
            const auto x1 = static_cast<float> (tickToX (static_cast<double> (r->start + r->length)));
            g.setColour (trackColour().withAlpha (0.8f));
            g.fillRect (juce::Rectangle<float> (x0, static_cast<float> (area.getBottom() - 5), x1 - x0, 5.0f));
        }
    g.setFont (font (Type::monoSmall));
    const auto start = m.signatures.barAt (std::max<Ticks> (0, static_cast<Ticks> (xToTick (area.getX())))).bar;
    int lastLabel = -1000;
    for (int bar = start;; ++bar)
    {
        const auto x = juce::roundToInt (tickToX (static_cast<double> (m.signatures.barStart (bar))));
        if (x > area.getRight())
            break;
        g.setColour (colour (Token::gridStrong));
        g.drawVerticalLine (x, static_cast<float> (area.getY() + 10), static_cast<float> (area.getBottom()));
        if (x - lastLabel >= 36)
        {
            g.setColour (colour (Token::textDim));
            g.drawText (juce::String (bar + 1), x + 3, area.getY(), 40, 14, juce::Justification::left);
            lastLabel = x;
        }
    }
}

void PianoRoll::paintGrid (juce::Graphics& g)
{
    const auto area = gridArea();
    const auto& m = studio.model();
    for (int pitch = yToPitch (static_cast<float> (area.getBottom() - 1)); pitch <= std::min (127, topPitch); ++pitch)
    {
        g.setColour (colour (commands::isBlackKey (pitch) ? Token::laneAlt : Token::lane));
        g.fillRect (juce::Rectangle<float> (static_cast<float> (area.getX()), pitchToY (pitch), static_cast<float> (area.getWidth()),
                                            static_cast<float> (rowHeight)));
        if (pitch % 12 == 0)
        {
            g.setColour (colour (Token::gridStrong));
            g.drawHorizontalLine (juce::roundToInt (pitchToY (pitch)) + rowHeight - 1, static_cast<float> (area.getX()),
                                  static_cast<float> (area.getRight()));
        }
    }
    // Snap grid (thinned to at least 6 px) and bars.
    const auto left = std::max (0.0, xToTick (area.getX()));
    const auto right = xToTick (area.getRight());
    auto step = static_cast<double> (studio.piano.grid > 0 ? studio.piano.grid : 240);
    while (step * pixelsPerTick < 6.0)
        step *= 2.0;
    g.setColour (colour (Token::gridWeak));
    for (auto t = std::floor (left / step) * step; t <= right; t += step)
        g.drawVerticalLine (juce::roundToInt (tickToX (t)), static_cast<float> (area.getY()), static_cast<float> (area.getBottom()));
    g.setColour (colour (Token::gridStrong));
    for (int bar = m.signatures.barAt (static_cast<Ticks> (left)).bar;; ++bar)
    {
        const auto x = tickToX (static_cast<double> (m.signatures.barStart (bar)));
        if (x > area.getRight())
            break;
        g.drawVerticalLine (juce::roundToInt (x), static_cast<float> (area.getY()), static_cast<float> (area.getBottom()));
    }
    // Outside the shown regions is shaded.
    juce::RectangleList<float> outside (area.toFloat());
    for (const auto& id : regions)
        if (const auto* r = m.region (id))
        {
            const auto x0 = static_cast<float> (tickToX (static_cast<double> (r->start)));
            const auto x1 = static_cast<float> (tickToX (static_cast<double> (r->start + r->length)));
            outside.subtract (juce::Rectangle<float> (x0, static_cast<float> (area.getY()), x1 - x0, static_cast<float> (area.getHeight())));
        }
    g.setColour (colour (Token::background).withAlpha (0.55f));
    g.fillRectList (outside);
}

void PianoRoll::paintNotes (juce::Graphics& g)
{
    const auto base = trackColour();
    const auto& sel = studio.piano.selection;
    const bool moving = (drag == Drag::move || drag == Drag::copy) && (deltaTicks != 0 || deltaPitch != 0);
    const bool resizing = drag == Drag::resizeLeft || drag == Drag::resizeRight;
    g.setFont (font (Type::small));
    for (const auto& n : shownNotes())
    {
        auto b = n.bounds;
        const bool selected = sel.contains (n.ref);
        if (resizing && anchor && n.ref == anchor->ref)
            b = noteBounds ({ n.ref, span.start, span.start, span.length, n.pitch, n.velocity, n.channel, n.muted, {} });
        else if (moving && selected)
        {
            if (drag == Drag::copy) // the original stays; the copy is drawn at the target
            {
                g.setColour (base.withAlpha (0.35f));
                g.fillRect (b);
            }
            b = noteBounds ({ n.ref, n.start + deltaTicks, n.stored, n.length, std::clamp (n.pitch + deltaPitch, 0, 127), n.velocity, n.channel, n.muted, {} });
        }
        if (n.start != n.stored) // region quantize: a faint mark where the note is stored
        {
            g.setColour (base.withAlpha (0.4f));
            g.fillRect (juce::Rectangle<float> (static_cast<float> (tickToX (static_cast<double> (n.stored))), b.getY(), 2.0f, b.getHeight()));
        }
        const auto velocity = drag == Drag::velocity && selected ? std::clamp (n.velocity + velocityDelta, 1, 127) : n.velocity;
        const auto fill = base.withBrightness (commands::velocityBrightness (velocity));
        if (n.muted)
        {
            g.setColour (fill);
            g.drawRect (b, 1.5f);
        }
        else
        {
            g.setColour (fill);
            g.fillRect (b);
        }
        if (selected)
        {
            g.setColour (colour (Token::accent));
            g.drawRect (b.expanded (1.0f), 2.0f);
        }
        if (b.getWidth() > 28.0f && rowHeight >= 9)
        {
            g.setColour (n.muted ? colour (Token::text) : colour (Token::regionText));
            g.drawText (juce::String (commands::noteName (n.pitch)), b.reduced (3.0f, 0.0f), juce::Justification::centredLeft, false);
        }
    }
    if (drag == Drag::rubber)
    {
        const auto box = juce::Rectangle<float> (downAt, dragAt);
        g.setColour (colour (Token::accent).withAlpha (0.2f));
        g.fillRect (box);
        g.setColour (colour (Token::accent));
        g.drawRect (box, 1.0f);
    }
}

void PianoRoll::paintLane (juce::Graphics& g)
{
    const auto area = laneArea();
    g.setColour (colour (Token::laneAlt));
    g.fillRect (area);
    g.setColour (colour (Token::border));
    g.drawHorizontalLine (area.getY(), 0.0f, static_cast<float> (getWidth()));
    g.setColour (colour (Token::surfaceRaised));
    g.fillRect (juce::Rectangle<int> (0, area.getY() + 1, keyboardWidth, area.getHeight()));
    g.setColour (colour (Token::textDim));
    g.setFont (font (Type::small));
    g.drawFittedText (laneName (lane), juce::Rectangle<int> (2, area.getY() + 4, keyboardWidth - 4, 40), juce::Justification::topLeft, 3);

    const auto base = trackColour();
    if (lane.type == LaneKind::Type::velocity)
    {
        for (const auto& n : shownNotes())
        {
            const bool selected = studio.piano.selection.contains (n.ref);
            auto v = n.velocity;
            if (drag == Drag::velocity && selected)
                v = std::clamp (v + velocityDelta, 1, 127);
            if (drag == Drag::laneVelocity && anchor && n.ref == anchor->ref)
                v = laneValue;
            const auto x = static_cast<float> (tickToX (static_cast<double> (n.start)));
            if (drag == Drag::laneLine && x >= std::min (downAt.x, dragAt.x) && x <= std::max (downAt.x, dragAt.x))
                v = std::clamp (lineValueAt (x), 1, 127);
            const auto y = laneY (v);
            g.setColour (selected ? colour (Token::accent) : base.withBrightness (commands::velocityBrightness (v)));
            g.fillRect (juce::Rectangle<float> (x, y, 3.0f, static_cast<float> (area.getBottom()) - y));
            g.fillEllipse (x - 1.5f, y - 3.0f, 6.0f, 6.0f);
        }
        return;
    }
    auto points = lanePoints();
    if (drag == Drag::laneMove && lanePoint)
        for (auto& pt : points)
            if (pt.eventId == lanePoint->eventId)
                pt.tick = laneTick, pt.value = laneValue;
    if (drag == Drag::laneNew)
        points.push_back ({ {}, {}, laneTick, laneValue });
    std::sort (points.begin(), points.end(), [] (const LanePoint& a, const LanePoint& b) { return a.tick < b.tick; });
    if (lane.type == LaneKind::Type::pitchBend) // centre line: no bend
    {
        g.setColour (colour (Token::gridStrong));
        g.drawHorizontalLine (juce::roundToInt (laneY (0)), static_cast<float> (area.getX()), static_cast<float> (area.getRight()));
    }
    if (drag == Drag::laneLine)
    {
        g.setColour (colour (Token::accent));
        g.drawLine (downAt.x, laneY (laneValueAt (downAt.y)), dragAt.x, laneY (laneValueAt (dragAt.y)), 1.5f);
    }
    // Controller values hold until the next point: drawn as steps.
    g.setColour (base);
    for (std::size_t i = 0; i < points.size(); ++i)
    {
        const auto x = static_cast<float> (tickToX (static_cast<double> (points[i].tick)));
        const auto y = laneY (points[i].value);
        const auto next = i + 1 < points.size() ? static_cast<float> (tickToX (static_cast<double> (points[i + 1].tick))) : static_cast<float> (area.getRight());
        g.drawLine (x, y, next, y, 1.5f);
        if (i + 1 < points.size())
            g.drawLine (next, y, next, laneY (points[i + 1].value), 1.0f);
        const bool selected = studio.piano.point && studio.piano.point->second == points[i].eventId;
        g.setColour (selected ? colour (Token::accent) : base);
        g.fillEllipse (x - 3.5f, y - 3.5f, 7.0f, 7.0f);
        g.setColour (base);
    }
}
} // namespace weft::ui
