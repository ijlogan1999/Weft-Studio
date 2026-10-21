#include "TracksArea.h"

#include "Commands.h"
#include "PresenceUi.h"
#include "Waveform.h"

#include "weft/time/Position.h"

#include <cmath>

namespace weft::ui
{
namespace
{
constexpr float nameBar = 16.0f, edge = 8.0f;

juce::Colour regionColour (const model::ProjectModel& m, const model::Region& r)
{
    const auto c = m.regionColor (r);
    return colourFor (c.palette, c.rgb);
}

/** The playhead: a 1 px line with a dark halo, readable over any region colour. */
void drawPlayheadLine (juce::Graphics& g, float x, float top, float bottom)
{
    const auto px = static_cast<float> (juce::roundToInt (x));
    g.setColour (depth::shade (0.45f, 0.15f));
    g.fillRect (px - 1.0f, top, 3.0f, bottom - top);
    g.setColour (colour (Token::playhead));
    g.fillRect (px, top, 1.0f, bottom - top);
}

void askForText (const juce::String& title, const juce::String& current, std::function<void (juce::String)> done)
{
    auto* w = new juce::AlertWindow (title, {}, juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("text", current);
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([w, done = std::move (done)] (int result) {
                            if (result == 1)
                                done (w->getTextEditorContents ("text"));
                        }),
                        true);
}
} // namespace

// Ruler -----------------------------------------------------------------------------------

Ruler::Ruler (Studio& s) : studio (s)
{
    setTitle ("Ruler");
    setTooltip ("Ruler: click to move the playhead; drag the top strip to set the cycle. Marker lane: double-click to add a marker. "
                "Tempo lane: Ctrl+click to add a tempo change, drag a point to change it. Signature lane: double-click to change the meter.");
}

namespace
{
/** "120", "92.5", "133.33". */
juce::String bpmText (double bpm)
{
    auto s = juce::String (bpm, 2);
    while (s.endsWithChar ('0'))
        s = s.dropLastCharacters (1);
    return s.trimCharactersAtEnd (".");
}
} // namespace

std::pair<double, double> Ruler::tempoRange() const
{
    if (drag == Drag::tempo)
        return dragRange;
    double lo = TempoMap::maxBpm, hi = TempoMap::minBpm;
    for (const auto& p : studio.model().tempo.points())
    {
        lo = std::min (lo, p.bpm);
        hi = std::max (hi, p.bpm);
    }
    return { std::max (TempoMap::minBpm, lo - 20.0), std::min (TempoMap::maxBpm, hi + 20.0) };
}

float Ruler::tempoY (double bpm) const
{
    const auto [lo, hi] = tempoRange();
    const auto inner = static_cast<double> (tempoLane - 10);
    return static_cast<float> (tempoTop + 5 + (hi - bpm) / std::max (1.0, hi - lo) * inner);
}

double Ruler::bpmAtY (float y) const
{
    const auto [lo, hi] = tempoRange();
    const auto inner = static_cast<double> (tempoLane - 10);
    const auto bpm = hi - (static_cast<double> (y) - tempoTop - 5) / inner * (hi - lo);
    return std::clamp (std::round (bpm * 100.0) / 100.0, TempoMap::minBpm, TempoMap::maxBpm);
}

const TempoPoint* Ruler::tempoPointAt (double x) const
{
    const TempoPoint* best = nullptr;
    double bestDistance = 6.0;
    for (const auto& p : studio.model().tempo.points())
        if (const auto d = std::abs (studio.view.tickToX (static_cast<double> (p.pos)) - x); d <= bestDistance)
        {
            best = &p;
            bestDistance = d;
        }
    return best;
}

const SignaturePoint* Ruler::signatureAt (double x) const
{
    const auto& sigs = studio.model().signatures;
    const SignaturePoint* best = nullptr;
    for (const auto& p : sigs.points())
    {
        const auto sx = studio.view.tickToX (static_cast<double> (sigs.barStart (p.bar)));
        if (x >= sx - 3 && x <= sx + 40)
            best = &p;
    }
    return best;
}

int Ruler::nearestBar (double x) const
{
    const auto& sigs = studio.model().signatures;
    const auto at = sigs.barAt (std::max<Ticks> (0, static_cast<Ticks> (studio.view.xToTick (x))));
    return at.offset * 2 > sigs.barLength (at.bar) ? at.bar + 1 : at.bar;
}

bool Ruler::setSignatureFromText (int bar, const juce::String& text)
{
    const auto numerator = text.upToFirstOccurrenceOf ("/", false, false).trim();
    const auto denominator = text.fromFirstOccurrenceOf ("/", false, false).trim();
    if (! text.contains ("/") || ! numerator.containsOnly ("0123456789") || ! denominator.containsOnly ("0123456789") || numerator.isEmpty()
        || denominator.isEmpty())
    {
        studio.setStatus ("Type a time signature such as 3/4.");
        return false;
    }
    const auto n = numerator.getIntValue(), d = denominator.getIntValue();
    for (const auto& p : studio.model().signatures.points())
        if (p.bar == bar)
            return studio.check (studio.session().setSignaturePoint (p.id, bar, n, d));
    if (studio.session().addSignaturePoint (bar, n, d))
    {
        studio.edited();
        return true;
    }
    studio.setStatus (juce::String (studio.session().lastError()));
    return false;
}

std::vector<std::pair<int, double>> Ruler::barLabels (const model::ProjectModel& m, const arrange::TimelineView& view, double width)
{
    std::vector<std::pair<int, double>> out;
    const auto first = m.signatures.barAt (static_cast<Ticks> (std::max (0.0, view.originTick))).bar;
    // Label every bar when there's room (36 px), else every 2nd, 4th, 8th... bar.
    int every = 1;
    while (static_cast<double> (m.signatures.barLength (first)) * view.pixelsPerTick * every < 36.0 && every < 4096)
        every *= 2;
    for (int bar = first - first % every;; bar += every)
    {
        const auto x = view.tickToX (static_cast<double> (m.signatures.barStart (bar)));
        if (x > width)
            break;
        if (x >= -1.0)
            out.emplace_back (bar + 1, x);
    }
    return out;
}

const model::Marker* Ruler::markerAt (double x) const
{
    const model::Marker* best = nullptr;
    for (const auto* marker : arrange::markersInOrder (studio.model()))
    {
        const auto mx = studio.view.tickToX (static_cast<double> (marker->pos));
        if (x >= mx - 3 && x <= mx + 70)
            best = marker;
    }
    return best;
}

void Ruler::paint (juce::Graphics& g)
{
    const auto& m = studio.model();
    const auto& v = studio.view;
    // The ruler is raised over the canvas: a lit band with a top highlight.
    const auto band = getLocalBounds().toFloat().withHeight (static_cast<float> (rulerHeight));
    g.setGradientFill (juce::ColourGradient::vertical (colour (Token::surfaceRaised), 0.0f, colour (Token::surface), band.getBottom()));
    g.fillRect (band);
    depth::highlight (g, band, 0.0f);
    // Cycle strip.
    const auto& cyc = m.info.cycle;
    const auto cx1 = static_cast<float> (v.tickToX (static_cast<double> (cyc.start)));
    const auto cx2 = static_cast<float> (v.tickToX (static_cast<double> (cyc.end)));
    const auto cycleBox = juce::Rectangle<float> (cx1, 1.0f, std::max (2.0f, cx2 - cx1), cycleStrip - 2.0f);
    if (cyc.enabled)
    {
        depth::tinted (g, cycleBox, radius::r1, colour (Token::loop));
    }
    else
    {
        // Off: a faint band, so the range can be found and dragged without reading as a rule.
        g.setColour (colour (Token::loop).withAlpha (0.18f));
        g.fillRoundedRectangle (cycleBox, radius::r1);
    }
    if (m.info.punch.enabled)
    {
        g.setColour (colour (Token::record));
        const auto px1 = static_cast<float> (v.tickToX (static_cast<double> (m.info.punch.start)));
        const auto px2 = static_cast<float> (v.tickToX (static_cast<double> (m.info.punch.end)));
        g.fillRect (px1, cycleStrip - 2.0f, px2 - px1, 2.0f);
        // Its ends are handles (W-1d): drag them to move the range.
        g.fillRect (px1, 1.0f, 2.0f, cycleStrip - 1.0f);
        g.fillRect (px2 - 2.0f, 1.0f, 2.0f, cycleStrip - 1.0f);
    }
    // Bars: a full tick and its number; beats as short ticks when there is room.
    g.setFont (font (Type::monoSmall));
    const auto labels = barLabels (m, v, getWidth());
    {
        for (std::size_t i = 0; i < labels.size(); ++i)
        {
            const auto [bar, x] = labels[i];
            g.setColour (colour (Token::border));
            g.fillRect (static_cast<float> (juce::roundToInt (x)), static_cast<float> (cycleStrip), 1.0f, static_cast<float> (rulerHeight - cycleStrip));
            g.setColour (colour (Token::textDim));
            g.drawText (juce::String (bar), juce::roundToInt (x) + space::half, cycleStrip + space::half, 40, 16, juce::Justification::left);
            const auto barTick = m.signatures.barStart (bar - 1);
            const auto beat = 4 * ticksPerQuarter / m.signatures.signatureAtBar (bar - 1).denominator;
            if (beat * v.pixelsPerTick >= 12.0)
                for (Ticks t = barTick + beat; t < barTick + m.signatures.barLength (bar - 1); t += beat)
                {
                    const auto bx = static_cast<float> (juce::roundToInt (v.tickToX (static_cast<double> (t))));
                    g.setColour (colour (Token::gridStrong));
                    g.fillRect (bx, static_cast<float> (rulerHeight - 6), 1.0f, 6.0f);
                    // Zoomed in far enough, beats are numbered too ("2.3").
                    if (beat * v.pixelsPerTick >= 48.0)
                    {
                        g.setColour (colour (Token::textDim));
                        g.drawText (juce::String (bar) + "." + juce::String ((t - barTick) / beat + 1), juce::roundToInt (bx) + space::half,
                                    cycleStrip + space::half, 40, 16, juce::Justification::left);
                    }
                }
        }
    }
    g.setColour (colour (Token::border));
    g.fillRect (0, rulerHeight - 1, getWidth(), 1);
    // Markers: raised chips with their colour on the left edge.
    if (studio.layout.globalTracks)
    {
        const auto lane = juce::Rectangle<int> (0, rulerHeight, getWidth(), markerLane);
        g.setColour (colour (Token::surface));
        g.fillRect (lane);
        g.setFont (font (Type::body));
        for (const auto* marker : arrange::markersInOrder (m))
        {
            const auto x = static_cast<float> (v.tickToX (static_cast<double> (marker->pos)));
            const auto c = marker->color ? colourFor (marker->color->palette, marker->color->rgb) : colour (Token::textDim);
            const auto chip = juce::Rectangle<float> (x, lane.getY() + 3.0f, 72.0f, markerLane - 6.0f);
            depth::raised (g, chip, radius::r1);
            g.setColour (c);
            g.fillRect (chip.withWidth (3.0f));
            g.setColour (colour (Token::text));
            g.drawText (juce::String (marker->name), chip.withTrimmedLeft (space::s1).withTrimmedRight (space::half), juce::Justification::centredLeft, true);
        }
        for (const auto* c : commands::commentThreads (m, commands::CommentFilter::all, studio.session().user()))
            drawCommentPin (g, pinBounds (*c), userColour (studio, c->author), c->resolved);
        g.setColour (colour (Token::lineSubtle));
        g.fillRect (0, tempoTop - 1, getWidth(), 1);

        // Tempo lane: the map as a step graph, a handle and value at each change (W-11).
        const auto& points = m.tempo.points();
        juce::Path steps;
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            const auto x1 = static_cast<float> (v.tickToX (static_cast<double> (points[i].pos)));
            const auto x2 = i + 1 < points.size() ? static_cast<float> (v.tickToX (static_cast<double> (points[i + 1].pos))) : static_cast<float> (getWidth());
            const auto y = tempoY (points[i].bpm);
            if (i == 0)
                steps.startNewSubPath (std::min (x1, 0.0f), y);
            else
                steps.lineTo (x1, y);
            steps.lineTo (x2, y);
        }
        g.setColour (colour (Token::accent));
        g.strokePath (steps, juce::PathStrokeType (1.5f));
        g.setFont (font (Type::monoSmall));
        for (const auto& p : points)
        {
            const auto x = static_cast<float> (v.tickToX (static_cast<double> (p.pos)));
            const auto y = tempoY (p.bpm);
            g.setColour (p.conflicted ? colour (Token::warning) : colour (Token::accent));
            g.fillRect (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ x, y }));
            g.setColour (colour (Token::textDim));
            const auto above = y - static_cast<float> (tempoTop) > 16.0f;
            g.drawText (bpmText (p.bpm), juce::roundToInt (x) + 6, juce::roundToInt (above ? y - 15.0f : y + 2.0f), 56, 13, juce::Justification::left);
        }
        g.setColour (colour (Token::lineSubtle));
        g.fillRect (0, signatureTop - 1, getWidth(), 1);

        // Signature lane: the meter at each change, at its bar start.
        g.setFont (font (Type::monoValue));
        for (const auto& p : m.signatures.points())
        {
            const auto x = static_cast<float> (v.tickToX (static_cast<double> (m.signatures.barStart (p.bar))));
            g.setColour (colour (Token::border));
            g.fillRect (x, static_cast<float> (signatureTop), 1.0f, static_cast<float> (signatureLane));
            g.setColour (p.conflicted ? colour (Token::warning) : colour (Token::text));
            g.drawText (juce::String (p.numerator) + "/" + juce::String (p.denominator), juce::roundToInt (x) + space::half, signatureTop, 40, signatureLane,
                        juce::Justification::centredLeft);
        }
        g.setColour (colour (Token::border));
        g.fillRect (0, getHeight() - 1, getWidth(), 1);
    }
    // Playhead handle.
    const auto px = static_cast<float> (juce::roundToInt (v.tickToX (static_cast<double> (studio.playhead())))) + 0.5f;
    juce::Path handle;
    handle.addTriangle (px - 5.0f, static_cast<float> (cycleStrip), px + 5.0f, static_cast<float> (cycleStrip), px, cycleStrip + 7.0f);
    g.setColour (depth::shade (0.4f, 0.2f));
    g.fillPath (handle, juce::AffineTransform::translation (0.0f, 1.0f));
    g.setColour (colour (Token::playhead));
    g.fillPath (handle);
    drawPlayheadLine (g, px - 0.5f, static_cast<float> (cycleStrip), static_cast<float> (getHeight()));
}

juce::Rectangle<float> Ruler::pinBounds (const model::Comment& thread) const
{
    const auto x = static_cast<float> (studio.view.tickToX (static_cast<double> (thread.pos)));
    return { x - 1.0f, rulerHeight + 3.0f, 14.0f, markerLane - 5.0f };
}

const model::Comment* Ruler::commentAt (juce::Point<float> p) const
{
    if (! studio.layout.globalTracks)
        return nullptr;
    const auto threads = commands::commentThreads (studio.model(), commands::CommentFilter::all, studio.session().user());
    for (auto it = threads.rbegin(); it != threads.rend(); ++it)
        if (pinBounds (**it).expanded (2.0f).contains (p))
            return *it;
    return nullptr;
}

void Ruler::pointerDown (juce::Point<float> p, juce::ModifierKeys mods)
{
    const auto t = studio.snapped (static_cast<Ticks> (studio.view.xToTick (p.x)), mods.isAltDown());
    if (p.y < cycleStrip)
    {
        drag = Drag::cycle;
        if (const auto& punch = studio.model().info.punch; punch.enabled)
        {
            const auto near = [&] (Ticks edge) { return std::abs (studio.view.tickToX (static_cast<double> (edge)) - p.x) <= 4.0; };
            drag = near (punch.start) ? Drag::punchStart : near (punch.end) ? Drag::punchEnd : mods.isShiftDown() ? Drag::punch : Drag::cycle;
            if (drag != Drag::cycle)
                studio.session().beginGesture ("Set Punch");
        }
        dragFrom = t;
        return;
    }
    if (p.y >= signatureTop && studio.layout.globalTracks)
    {
        if (const auto* sig = signatureAt (p.x); sig != nullptr && mods.isPopupMenu())
            studio.check (studio.session().deleteSignaturePoint (sig->id));
        return;
    }
    if (p.y >= tempoTop && studio.layout.globalTracks)
    {
        if (const auto* point = tempoPointAt (p.x))
        {
            if (mods.isPopupMenu())
            {
                studio.check (studio.session().deleteTempoPoint (point->id));
                return;
            }
            dragRange = tempoRange();
            drag = Drag::tempo;
            draggedTempo = point->id;
            dragFrom = point->pos;
            dragBpm = point->bpm;
            dragStart = p;
            studio.session().beginGesture ("Tempo Change");
            return;
        }
        if (mods.isCommandDown() || studio.tool == Tool::pencil)
        {
            if (studio.session().addTempoPoint (t, bpmAtY (p.y)))
                studio.edited();
            else
                studio.setStatus (juce::String (studio.session().lastError()));
        }
        return;
    }
    if (p.y >= rulerHeight && studio.layout.globalTracks)
    {
        if (const auto* c = commentAt (p))
        {
            studio.showThread (c->id, localAreaToGlobal (pinBounds (*c).toNearestInt()));
            return;
        }
        if (const auto* marker = markerAt (p.x))
        {
            if (mods.isPopupMenu())
            {
                studio.check (studio.session().deleteMarker (marker->id));
                return;
            }
            studio.locate (marker->pos);
            drag = Drag::marker;
            draggedMarker = marker->id;
            dragFrom = marker->pos;
            studio.session().beginGesture ("Move Marker");
        }
        return;
    }
    drag = Drag::playhead;
    studio.locate (t);
}

void Ruler::pointerDrag (juce::Point<float> p)
{
    const auto t = studio.snapped (std::max<Ticks> (0, static_cast<Ticks> (studio.view.xToTick (p.x))));
    switch (drag)
    {
        case Drag::cycle:
            if (t != dragFrom)
                studio.check (studio.session().setCycle (true, std::min (t, dragFrom), std::max (t, dragFrom)));
            break;
        case Drag::playhead: studio.locate (t); break;
        case Drag::punchStart:
        case Drag::punchEnd:
        case Drag::punch:
        {
            const auto& punch = studio.model().info.punch;
            auto start = drag == Drag::punchEnd ? punch.start : drag == Drag::punch ? std::min (t, dragFrom) : t;
            auto end = drag == Drag::punchStart ? punch.end : drag == Drag::punch ? std::max (t, dragFrom) : t;
            if (end > start)
                studio.check (studio.session().setPunch (true, start, end));
            break;
        }
        case Drag::marker: studio.check (studio.session().moveMarker (draggedMarker, t)); break;
        case Drag::tempo:
        {
            // Vertical: BPM in 0.01 steps over the lane's range at the drag start; horizontal (past a few
            // pixels) moves the change, except the first, which stays at the start (W-11 steps 2-3).
            const auto [lo, hi] = dragRange;
            const auto perPixel = (hi - lo) / static_cast<double> (tempoLane - 10);
            const auto bpm = std::clamp (std::round ((dragBpm - static_cast<double> (p.y - dragStart.y) * perPixel) * 100.0) / 100.0, TempoMap::minBpm,
                                         TempoMap::maxBpm);
            const auto pos = draggedTempo == "initial" || std::abs (p.x - dragStart.x) < 4.0f ? dragFrom : std::max<Ticks> (1, t);
            studio.check (studio.session().setTempoPoint (draggedTempo, pos, bpm));
            break;
        }
        case Drag::none: break;
    }
}

void Ruler::pointerUp()
{
    if (drag == Drag::marker || drag == Drag::tempo || drag == Drag::punchStart || drag == Drag::punchEnd || drag == Drag::punch)
    {
        studio.session().endGesture();
        studio.edited();
    }
    drag = Drag::none;
}

void Ruler::mouseDown (const juce::MouseEvent& e) { pointerDown (e.position, e.mods); }
void Ruler::mouseDrag (const juce::MouseEvent& e) { pointerDrag (e.position); }
void Ruler::mouseUp (const juce::MouseEvent& e)
{
    // A click (no drag) in the cycle strip toggles the cycle.
    if (drag == Drag::cycle && ! e.mouseWasDraggedSinceMouseDown())
        studio.perform (Cmd::cycle);
    pointerUp();
}

void Ruler::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.position.y < rulerHeight || ! studio.layout.globalTracks)
        return;
    if (e.position.y >= signatureTop)
    {
        const auto* sig = signatureAt (e.position.x);
        const auto bar = sig != nullptr ? sig->bar : nearestBar (e.position.x);
        const auto& now = studio.model().signatures.signatureAtBar (bar);
        askForText ("Time signature at bar " + juce::String (bar + 1), juce::String (now.numerator) + "/" + juce::String (now.denominator),
                    [this, bar] (juce::String text) { setSignatureFromText (bar, text); });
        return;
    }
    if (e.position.y >= tempoTop)
    {
        if (const auto* point = tempoPointAt (e.position.x))
        {
            const auto id = point->id;
            const auto pos = point->pos;
            askForText ("Tempo (BPM)", bpmText (point->bpm), [this, id, pos] (juce::String text) {
                studio.check (studio.session().setTempoPoint (id, pos, std::round (text.getDoubleValue() * 100.0) / 100.0));
            });
        }
        return;
    }
    if (const auto* marker = markerAt (e.position.x))
    {
        const auto id = marker->id;
        askForText ("Rename marker", juce::String (marker->name), [this, id] (juce::String name) {
            if (name.isNotEmpty())
                studio.check (studio.session().renameMarker (id, name.toStdString()));
        });
        return;
    }
    studio.session().addMarker (studio.snapped (static_cast<Ticks> (studio.view.xToTick (e.position.x))), "");
    studio.edited();
}

// Track header ------------------------------------------------------------------------------

TrackHeader::TrackHeader (Studio& s, const Id& id, int n) : trackId (id), studio (s), number (n)
{
    name.setEditable (false, true, true);
    name.setFont (font (Type::strong));
    name.setBorderSize ({});
    describe (name, "Track name", "Track name (double-click to rename)");
    name.onTextChange = [this] {
        const auto text = name.getText().trim();
        if (text.isEmpty())
            refresh(); // an empty name reverts (AT-TRK-6)
        else
            studio.check (studio.session().renameTrack (trackId, text.toStdString()));
    };
    addAndMakeVisible (name);
    describe (mute, "Mute", "Mute track");
    describe (solo, "Solo", "Solo track (Ctrl+click: solo only this)");
    describe (arm, "Record enable", "Record enable");
    mute.onClick = [this] {
        if (const auto* t = studio.model().track (trackId))
            studio.check (studio.session().setTrackMute (trackId, ! t->mute));
    };
    solo.onClick = [this] { studio.toggleSolo (trackId, juce::ModifierKeys::currentModifiers.isCommandDown()); };
    arm.onClick = [this] { studio.toggleArm (trackId); };
    setRole (mute, ButtonRole::mute);
    setRole (solo, ButtonRole::solo);
    setRole (arm, ButtonRole::record);
    for (auto* b : { &mute, &solo, &arm })
    {
        b->setWantsKeyboardFocus (true);
        b->setMouseClickGrabsKeyboardFocus (false);
        b->setHasFocusOutline (true);
        addAndMakeVisible (*b);
    }
    mode.addItemList ({ "Off", "Read", "Touch", "Latch", "Write" }, 1);
    describe (mode, "Automation mode");
    makeFocusable (mode);
    mode.onChange = [this] {
        static const char* const modes[] = { "off", "read", "touch", "latch", "write" };
        const auto i = mode.getSelectedItemIndex();
        if (i >= 0 && i < 5)
            studio.check (studio.session().setTrackAutomationMode (trackId, modes[i]));
    };
    addChildComponent (mode);
    lock.setShape (icon::lock(), false, true, false);
    makeFocusable (lock);
    lock.onClick = [this] { lockClicked(); };
    addChildComponent (lock);
    setTitle ("Track header");
    refresh();
}

void TrackHeader::lockClicked()
{
    if (studio.lockedByOther (trackId))
    {
        if (studio.session().isOwner() && studio.confirmBreakLock)
            studio.confirmBreakLock (trackId);
        else
            studio.setStatus (studio.lockTooltip (trackId) + ".");
        return;
    }
    studio.toggleLock (trackId);
}

juce::PopupMenu TrackHeader::menu() const
{
    juce::PopupMenu m;
    const auto lockText = studio.lockTooltip (trackId);
    const bool other = studio.lockedByOther (trackId);
    const auto* t = studio.model().track (trackId);
    const bool editable = ! other && ! studio.previewing() && t != nullptr;
    const bool master = t != nullptr && t->kind == model::TrackKind::master;
    const auto& order = studio.model().trackOrder;
    const auto at = std::find (order.begin(), order.end(), trackId);
    m.addItem (itemRename, "Rename", editable);
    m.addItem (itemDuplicate, commandInfo (Cmd::duplicateTrack).name, editable && ! master);
    m.addItem (itemDuplicateWithRegions, commandInfo (Cmd::duplicateTrackWithRegions).name, editable && ! master);
    m.addSubMenu ("Color", colourMenu (itemColour, t != nullptr && t->color.rgb.empty() ? t->color.palette : -1), editable);
    m.addItem (itemMoveUp, commandInfo (Cmd::moveTrackUp).name, ! studio.previewing() && at != order.end() && at != order.begin());
    m.addItem (itemMoveDown, commandInfo (Cmd::moveTrackDown).name, ! studio.previewing() && at != order.end() && at + 1 != order.end());
    m.addItem (itemDelete, commandInfo (Cmd::deleteTrack).name, editable && ! master);
    m.addSeparator();
    m.addItem (itemLock, lockText.isEmpty() ? "Lock Track" : "Unlock Track", ! other && ! studio.previewing());
    if (other && studio.session().isOwner() && ! studio.previewing())
        m.addItem (itemBreakLock, "Break Lock (" + lockText + ")...");
    m.addSeparator();
    m.addItem (itemComment, "Add Comment", ! studio.previewing());
    return m;
}

void TrackHeader::pickMenu (int item)
{
    switch (item)
    {
        case itemRename: name.showEditor(); break;
        case itemDuplicate: studio.selectTrack (trackId); studio.perform (Cmd::duplicateTrack); break;
        case itemDuplicateWithRegions: studio.selectTrack (trackId); studio.perform (Cmd::duplicateTrackWithRegions); break;
        case itemMoveUp: studio.selectTrack (trackId); studio.perform (Cmd::moveTrackUp); break;
        case itemMoveDown: studio.selectTrack (trackId); studio.perform (Cmd::moveTrackDown); break;
        case itemDelete: studio.selectTrack (trackId); studio.perform (Cmd::deleteTrack); break;
        case itemLock: studio.toggleLock (trackId); break;
        case itemBreakLock:
            if (studio.confirmBreakLock)
                studio.confirmBreakLock (trackId);
            break;
        case itemComment:
        {
            commands::CommentAnchor a;
            a.pos = studio.playhead();
            a.trackId = trackId;
            studio.startComment (a);
            break;
        }
        default:
            if (item > itemColour && item <= itemColour + paletteSize)
                studio.check (studio.session().setTrackColor (trackId, model::Color { item - itemColour - 1, {} }));
            break;
    }
}

void TrackHeader::rebuildAutomation()
{
    const bool shown = studio.automationView;
    mode.setVisible (shown);
    const auto lanes = shown ? studio.shownLanes (trackId) : std::vector<model::AutomationTarget> {};
    const auto choices = shown ? studio.automationTargets (trackId) : std::vector<automation::TargetChoice> {};
    std::vector<std::string> built;
    for (const auto& c : choices)
        built.push_back (c.target.key() + "=" + c.name);
    for (const auto& l : lanes)
        built.push_back ("lane:" + l.key());
    if (built != shownChoices)
    {
        shownChoices = built;
        parameters.clear();
        addLane.clear();
        for (std::size_t i = 0; i < lanes.size(); ++i)
        {
            auto box = std::make_unique<juce::ComboBox>();
            describe (*box, "Automation parameter");
            makeFocusable (*box);
            for (std::size_t c = 0; c < choices.size(); ++c)
                box->addItem (juce::String (choices[c].name), static_cast<int> (c) + 1);
            for (std::size_t c = 0; c < choices.size(); ++c)
                if (choices[c].target.key() == lanes[i].key())
                    box->setSelectedItemIndex (static_cast<int> (c), juce::dontSendNotification);
            box->onChange = [this, i, choices, b = box.get()] {
                const auto c = b->getSelectedItemIndex();
                if (c >= 0 && static_cast<std::size_t> (c) < choices.size())
                    studio.setShownLane (trackId, i, choices[static_cast<std::size_t> (c)].target);
            };
            addAndMakeVisible (*box);
            parameters.push_back (std::move (box));
            auto plus = std::make_unique<juce::TextButton> ("+");
            describe (*plus, "Add automation lane", "Show another automation lane");
            makeFocusable (*plus);
            plus->onClick = [this] { studio.addShownLane (trackId); };
            addAndMakeVisible (*plus);
            addLane.push_back (std::move (plus));
        }
    }
    static const char* const modes[] = { "off", "read", "touch", "latch", "write" };
    if (const auto* t = studio.model().track (trackId))
        for (int i = 0; i < 5; ++i)
            if (t->automationMode == modes[i])
                mode.setSelectedItemIndex (i, juce::dontSendNotification);
}

void TrackHeader::refresh()
{
    const auto* t = studio.model().track (trackId);
    if (t == nullptr)
        return;
    name.setText (juce::String (t->name), juce::dontSendNotification);
    setTitle ("Track " + juce::String (number) + ": " + juce::String (t->name));
    mute.setToggleState (t->mute, juce::dontSendNotification);
    solo.setToggleState (studio.localView().soloed.contains (trackId), juce::dontSendNotification);
    arm.setToggleState (studio.localView().armed.contains (trackId), juce::dontSendNotification);
    arm.setVisible (commands::LocalView::canArm (*t));
    rebuildAutomation();

    // A lock (04 §6.1, §9.6): the icon shows; someone else's lock disables the shared controls.
    const auto lockTip = studio.lockTooltip (trackId);
    const bool locked = studio.lockedByOther (trackId);
    lock.setVisible (lockTip.isNotEmpty());
    const auto lockColour = locked ? colour (Token::warning) : colour (Token::textDim);
    lock.setColours (lockColour, lockColour.brighter (0.3f), lockColour.darker (0.2f));
    lock.setTooltip (lockTip.isEmpty() ? juce::String() : lockTip + (locked ? juce::String() : juce::String (" (click to unlock)")));
    name.setEditable (false, ! locked, true);
    std::vector<juce::Component*> shared { &name, &mute, &mode };
    for (auto& p : parameters)
        shared.push_back (p.get());
    for (auto& b : addLane)
        shared.push_back (b.get());
    for (auto* c : shared)
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
    setTooltip (locked ? lockTip : juce::String());
    resized();
    repaint();
}

void TrackHeader::paint (juce::Graphics& g)
{
    const auto* t = studio.model().track (trackId);
    const bool selected = studio.selectedTrack() == trackId;
    const auto trackPart = getLocalBounds().withHeight (studio.trackHeight (trackId)).toFloat();
    g.fillAll (colour (Token::surface));
    if (selected)
    {
        g.setColour (colour (Token::hover));
        g.fillRect (trackPart);
    }
    depth::highlight (g, trackPart, 0.0f);
    // The track's colour runs down the left edge; selection adds an accent rule beside it.
    if (t != nullptr)
    {
        g.setColour (colourFor (t->color.palette, t->color.rgb));
        g.fillRect (trackPart.withWidth (static_cast<float> (space::half)));
    }
    if (selected)
    {
        g.setColour (colour (Token::accent));
        g.fillRect (trackPart.withX (static_cast<float> (space::half)).withWidth (2.0f));
    }
    g.setColour (colour (Token::textDim));
    g.setFont (font (Type::monoSmall));
    g.drawText (juce::String (number), space::s1, space::s1, space::s3 - space::half, size::control, juce::Justification::centredRight);
    if (t != nullptr && trackPart.getHeight() >= 48.0f)
    {
        g.setFont (font (Type::body));
        const auto kind = t->kind == model::TrackKind::bus ? "Bus" : t->kind == model::TrackKind::master ? "Master" : t->kind == model::TrackKind::instrument ? "Instrument" : "Audio";
        g.drawText (kind, space::s4, space::s1 + size::control, 120, 16, juce::Justification::centredLeft);
    }
    // Collaborators working on this track (04 §6.1): a badge each, red while they record on it.
    auto badge = juce::Rectangle<float> (static_cast<float> (getWidth() - space::s1 - 16), static_cast<float> (space::s1 + size::control + 2), 16.0f, 16.0f);
    for (const auto* who : badges())
    {
        if (badge.getX() < 96.0f || badge.getBottom() > static_cast<float> (studio.trackHeight (trackId)))
            break;
        drawBadge (g, badge, colourOf (*who), initials (who->displayName), who->state.status == collab::Presence::Status::recording);
        badge.translate (-20.0f, 0.0f);
    }
    g.setColour (colour (Token::background));
    g.fillRect (0, getHeight() - 1, getWidth(), 1);
    if (dropMark != 0) // a dragged track lands here
    {
        g.setColour (colour (Token::accent));
        g.fillRect (0, dropMark < 0 ? 0 : getHeight() - 2, getWidth(), 2);
    }
    g.setColour (colour (Token::lineSubtle));
    for (std::size_t i = 0; i < parameters.size(); ++i)
        g.fillRect (space::s4, studio.trackHeight (trackId) + static_cast<int> (i) * Studio::automationLaneHeight, getWidth() - space::s4, 1);
}

std::vector<const collab::RemotePresence*> TrackHeader::badges() const
{
    std::vector<const collab::RemotePresence*> out;
    for (const auto* who : studio.collaborators())
        if (who->hasState && who->state.focusTrack == trackId)
            out.push_back (who);
    return out;
}

void TrackHeader::resized()
{
    // Colour edge and number on the left; name, then M S R on the top line (8 px grid).
    const auto trackPart = getLocalBounds().withHeight (studio.trackHeight (trackId));
    auto r = trackPart.withTrimmedLeft (space::s4).withTrimmedRight (space::s1);
    auto row = r.withTrimmedTop (std::min (space::s1, std::max (0, (trackPart.getHeight() - size::control) / 2))).withHeight (size::control);
    if (lock.isVisible())
    {
        lock.setBounds (row.removeFromRight (size::icon).withSizeKeepingCentre (size::icon, size::icon));
        row.removeFromRight (space::half);
    }
    for (auto* b : { &arm, &solo, &mute })
    {
        if (b == &arm && ! arm.isVisible())
            continue;
        b->setBounds (row.removeFromRight (size::control));
        row.removeFromRight (space::half);
    }
    name.setBounds (row.withTrimmedRight (space::half));
    // Automation lanes: the parameter and "+" on each; the track's mode on the first (W-8 step 1).
    for (std::size_t i = 0; i < parameters.size(); ++i)
    {
        auto lane = juce::Rectangle<int> (0, trackPart.getBottom() + static_cast<int> (i) * Studio::automationLaneHeight, getWidth(),
                                          Studio::automationLaneHeight)
                        .reduced (space::s1, space::half)
                        .withTrimmedLeft (space::s3);
        auto top = lane.removeFromTop (size::control);
        addLane[i]->setBounds (top.removeFromRight (size::control));
        top.removeFromRight (space::half);
        parameters[i]->setBounds (top);
        if (i == 0)
            mode.setBounds (lane.removeFromTop (size::control).withWidth (std::min (lane.getWidth(), 88)));
    }
}

void TrackHeader::mouseMove (const juce::MouseEvent& e)
{
    const auto bottom = studio.trackHeight (trackId);
    setMouseCursor (e.y >= bottom - 4 && e.y <= bottom ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
}

void TrackHeader::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        studio.selectTrack (trackId);
        juce::Component::SafePointer<TrackHeader> self (this);
        menu().showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [self] (int item) {
            if (self != nullptr)
                self->pickMenu (item);
        });
        return;
    }
    const auto bottom = studio.trackHeight (trackId);
    resizeFrom = e.y >= bottom - 4 && e.y <= bottom ? bottom : -1;
    studio.selectTrack (trackId);
}

void TrackHeader::mouseDrag (const juce::MouseEvent& e)
{
    if (resizeFrom > 0)
    {
        studio.setTrackHeight (trackId, resizeFrom + e.getDistanceFromDragStartY());
        return;
    }
    // Dragging a header reorders the tracks (F-TRK-7); the master stays where it is.
    const auto* t = studio.model().track (trackId);
    if (! reordering && (std::abs (e.getDistanceFromDragStartY()) < 6 || t == nullptr || t->kind == model::TrackKind::master || studio.previewing()))
        return;
    reordering = true;
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    showDropTarget (headerAt (e.getPosition() + getPosition()));
}

void TrackHeader::showDropTarget (TrackHeader* over)
{
    if (over == this)
        over = nullptr;
    if (reorderOver != nullptr)
    {
        reorderOver->dropMark = 0;
        reorderOver->repaint();
    }
    reorderOver = over;
    if (over != nullptr)
    {
        over->dropMark = over->getY() < getY() ? -1 : 1;
        over->repaint();
    }
}

void TrackHeader::mouseUp (const juce::MouseEvent& e)
{
    if (! std::exchange (reordering, false))
        return;
    setMouseCursor (juce::MouseCursor::NormalCursor);
    showDropTarget (nullptr);
    if (const auto* over = headerAt (e.getPosition() + getPosition()); over != nullptr && over != this)
    {
        const auto& order = studio.model().trackOrder;
        const auto to = std::find (order.begin(), order.end(), over->trackId);
        if (to != order.end())
            studio.moveTrackTo (trackId, static_cast<std::size_t> (to - order.begin()));
    }
}

TrackHeader* TrackHeader::headerAt (juce::Point<int> inParent) const
{
    // Above the first header picks the first; below the last picks the last.
    const auto* parent = getParentComponent();
    if (parent == nullptr)
        return nullptr;
    TrackHeader *first = nullptr, *last = nullptr;
    for (auto* c : parent->getChildren())
        if (auto* h = dynamic_cast<TrackHeader*> (c); h != nullptr && h->isVisible())
        {
            if (h->getBounds().contains (inParent.withX (h->getX())))
                return h;
            if (first == nullptr || h->getY() < first->getY())
                first = h;
            if (last == nullptr || h->getBottom() > last->getBottom())
                last = h;
        }
    if (first != nullptr && inParent.y < first->getY())
        return first;
    if (last != nullptr && inParent.y >= last->getBottom())
        return last;
    return nullptr;
}

// Timeline ---------------------------------------------------------------------------------

Timeline::Timeline (Studio& s) : studio (s)
{
    setTitle ("Tracks area");
    setWantsKeyboardFocus (false);
}

int Timeline::rows() const { return static_cast<int> (studio.model().trackOrder.size()); }

juce::Rectangle<float> Timeline::rowBounds (int row) const
{
    float y = static_cast<float> (-scrollY);
    const auto& order = studio.model().trackOrder;
    for (int i = 0; i < row && i < static_cast<int> (order.size()); ++i)
        y += static_cast<float> (studio.rowHeight (order[static_cast<std::size_t> (i)]));
    const auto h = row < static_cast<int> (order.size()) ? studio.rowHeight (order[static_cast<std::size_t> (row)]) : defaultTrackHeight;
    if (row >= static_cast<int> (order.size()))
        y += static_cast<float> ((row - static_cast<int> (order.size())) * defaultTrackHeight);
    return { 0.0f, y, static_cast<float> (getWidth()), static_cast<float> (h) };
}

juce::Rectangle<float> Timeline::laneBounds (int row) const
{
    const auto& order = studio.model().trackOrder;
    auto r = rowBounds (row);
    if (row >= 0 && row < static_cast<int> (order.size()))
        r.setHeight (static_cast<float> (studio.trackHeight (order[static_cast<std::size_t> (row)])));
    return r;
}

juce::Rectangle<float> Timeline::automationBounds (int row, std::size_t index) const
{
    const auto lane = laneBounds (row);
    return lane.withY (lane.getBottom() + static_cast<float> (index * Studio::automationLaneHeight)).withHeight (static_cast<float> (Studio::automationLaneHeight));
}

std::optional<Timeline::AutomationHit> Timeline::automationLaneAt (juce::Point<float> p) const
{
    if (! studio.automationView)
        return std::nullopt;
    const auto row = rowAt (p.y);
    if (row < 0 || row >= rows())
        return std::nullopt;
    const auto trackId = studio.model().trackOrder[static_cast<std::size_t> (row)];
    const auto lanes = studio.shownLanes (trackId);
    for (std::size_t i = 0; i < lanes.size(); ++i)
        if (const auto box = automationBounds (row, i); box.contains (p))
            return AutomationHit { trackId, lanes[i], box };
    return std::nullopt;
}

int Timeline::contentHeight() const { return static_cast<int> (rowBounds (rows()).getY()) + scrollY; }

int Timeline::rowAt (float y) const
{
    if (y < static_cast<float> (-scrollY))
        return -1;
    const auto n = rows();
    for (int i = 0; i < n; ++i)
        if (rowBounds (i).getBottom() > y)
            return i;
    return n + static_cast<int> ((y - rowBounds (n).getY()) / defaultTrackHeight);
}

// Automation lanes (W-8) ----------------------------------------------------------------

namespace
{
constexpr float lanePad = 6.0f, pointRadius = 4.0f, pointReach = 7.0f;
} // namespace

double Timeline::valueAtY (juce::Rectangle<float> box, float y)
{
    const auto h = std::max (1.0f, box.getHeight() - 2 * lanePad);
    return std::clamp (1.0 - static_cast<double> (y - box.getY() - lanePad) / h, 0.0, 1.0);
}

juce::Point<float> Timeline::pointPosition (const AutomationHit& lane, const model::AutoPoint& point) const
{
    const auto h = lane.box.getHeight() - 2 * lanePad;
    return { static_cast<float> (studio.view.tickToX (static_cast<double> (point.pos))),
             lane.box.getY() + lanePad + static_cast<float> (1.0 - point.value) * h };
}

std::optional<model::AutoPoint> Timeline::pointNear (const AutomationHit& lane, juce::Point<float> p) const
{
    const auto* l = studio.model().lane (lane.trackId, lane.target);
    if (l == nullptr)
        return std::nullopt;
    std::optional<model::AutoPoint> best;
    float bestDistance = pointReach;
    for (const auto& point : automation::sortedPoints (*l))
        if (const auto d = pointPosition (lane, point).getDistanceFrom (p); d <= bestDistance)
        {
            bestDistance = d;
            best = point;
        }
    return best;
}

std::pair<Ticks, double> Timeline::pointDelta (juce::Point<float> p, juce::ModifierKeys mods) const
{
    auto dx = p.x - downAt.x, dy = p.y - downAt.y;
    if (mods.isShiftDown()) // Shift constrains to one axis (W-8 step 3)
    {
        if (std::abs (dx) >= std::abs (dy))
            dy = 0;
        else
            dx = 0;
    }
    const auto raw = static_cast<Ticks> (std::llround (dx / studio.view.pixelsPerTick));
    const auto to = std::abs (dx) < 0.5f ? grabbed.pos : std::max<Ticks> (0, studio.snapped (grabbed.pos + raw, mods.isAltDown()));
    const auto h = std::max (1.0f, autoLane.box.getHeight() - 2 * lanePad);
    return { to - grabbed.pos, -static_cast<double> (dy) / h };
}

std::vector<model::AutoPoint> Timeline::previewPoints (const AutomationHit& lane) const
{
    const auto* l = studio.model().lane (lane.trackId, lane.target);
    if (l == nullptr)
        return {};
    auto points = automation::sortedPoints (*l);
    const bool mine = autoLane.trackId == lane.trackId && autoLane.target.key() == lane.target.key();
    if (mine && autoDrag == AutoDrag::move && lastAt != downAt)
    {
        const auto [dt, dv] = pointDelta (lastAt, dragMods);
        for (auto& point : points)
            if (studio.pointSelection.points.contains (point.id))
            {
                point.pos = std::max<Ticks> (0, point.pos + dt);
                point.value = std::clamp (point.value + dv, 0.0, 1.0);
            }
        std::stable_sort (points.begin(), points.end(), [] (const auto& a, const auto& b) { return a.pos < b.pos; });
    }
    if (mine && autoDrag == AutoDrag::bend)
        for (auto& point : points)
            if (point.id == grabbed.id)
                point.curve = bendCurve;
    return points;
}

void Timeline::paintAutomation (juce::Graphics& g, const AutomationHit& lane)
{
    const auto& m = studio.model();
    const auto* track = m.track (lane.trackId);
    if (track == nullptr)
        return;
    g.setColour (colour (Token::surface).withAlpha (0.7f));
    g.fillRect (lane.box);
    g.setColour (colour (Token::border));
    g.drawHorizontalLine (juce::roundToInt (lane.box.getY()), 0.0f, lane.box.getRight());
    const auto points = previewPoints (lane);
    const auto line = colourFor (track->color.palette, track->color.rgb);
    const bool active = track->automationMode != "off" && ! points.empty();
    const auto h = lane.box.getHeight() - 2 * lanePad;
    const auto yFor = [&] (double v) { return lane.box.getY() + lanePad + static_cast<float> (1.0 - v) * h; };
    juce::Path path;
    if (points.empty())
    {
        // An empty lane plays the static value (W-8 step 5).
        double fallback = 0;
        if (lane.target.kind == "pluginParam")
            if (const auto proc = studio.processor (lane.target.pluginId))
                if (const auto i = proc->parameterIndex (lane.target.paramId); i >= 0)
                    fallback = proc->parameter (i);
        const auto y = yFor (automation::staticValue (*track, lane.target, fallback));
        path.startNewSubPath (lane.box.getX(), y);
        path.lineTo (lane.box.getRight(), y);
    }
    else
        for (float x = lane.box.getX(); x <= lane.box.getRight(); x += 2.0f)
        {
            const auto y = yFor (automation::valueAt (points, studio.view.xToTick (x)));
            if (x <= lane.box.getX())
                path.startNewSubPath (x, y);
            else
                path.lineTo (x, y);
        }
    g.setColour (active ? line : colour (Token::textDim));
    g.strokePath (path, juce::PathStrokeType (active ? 2.0f : 1.0f));
    for (const auto& point : points)
    {
        const auto c = pointPosition (lane, point);
        if (c.x < -pointRadius || c.x > lane.box.getRight() + pointRadius)
            continue;
        const bool selected = studio.pointSelection.points.contains (point.id);
        const auto dot = juce::Rectangle<float> (pointRadius * 2, pointRadius * 2).withCentre (c);
        g.setColour (selected ? colour (Token::accent) : colour (Token::lane));
        g.fillEllipse (dot);
        g.setColour (selected ? colour (Token::text) : line);
        g.drawEllipse (dot, 1.5f);
    }
    g.setColour (colour (Token::textDim));
    g.setFont (font (Type::small));
    g.drawText (studio.automationTargetName (lane.trackId, lane.target), lane.box.reduced (6.0f, 2.0f), juce::Justification::topLeft);
    if (autoDrag == AutoDrag::band && autoLane.box == lane.box)
    {
        const auto band = juce::Rectangle<float> (downAt, lastAt);
        g.setColour (colour (Token::accent).withAlpha (0.15f));
        g.fillRect (band);
        g.setColour (colour (Token::accent));
        g.drawRect (band, 1.0f);
    }
}

void Timeline::automationDown (const AutomationHit& hit, juce::Point<float> p, juce::ModifierKeys mods)
{
    autoLane = hit;
    dragMods = mods;
    toggleOnUp = false;
    const auto* lane = studio.model().lane (hit.trackId, hit.target);
    auto& sel = studio.pointSelection;
    if (lane == nullptr || sel.laneId != lane->id)
        sel = { lane != nullptr ? lane->id : Id {}, {} };
    if (const auto near = pointNear (hit, p))
    {
        grabbed = *near;
        if (mods.isShiftDown() && sel.points.contains (near->id))
            toggleOnUp = true;
        else if (mods.isShiftDown())
            sel.points.insert (near->id);
        else if (! sel.points.contains (near->id))
            sel.points = { near->id };
        autoDrag = AutoDrag::move;
    }
    else if (mods.isAltDown() && lane != nullptr)
    {
        // Alt+drag on a segment bends it (W-8 step 6).
        const auto points = automation::sortedPoints (*lane);
        const auto tick = studio.view.xToTick (p.x);
        autoDrag = AutoDrag::none;
        for (std::size_t i = 0; i + 1 < points.size(); ++i)
            if (static_cast<double> (points[i].pos) <= tick && tick < static_cast<double> (points[i + 1].pos))
            {
                grabbed = points[i];
                bendCurve = points[i].curve;
                bendSign = points[i + 1].value >= points[i].value ? -1.0 : 1.0;
                autoDrag = AutoDrag::bend;
            }
    }
    else
    {
        if (! mods.isShiftDown())
            sel.points.clear();
        autoDrag = AutoDrag::add;
    }
    repaint();
}

void Timeline::automationDrag (juce::Point<float> p, juce::ModifierKeys mods)
{
    lastAt = p;
    dragMods = mods;
    switch (autoDrag)
    {
        case AutoDrag::add:
            if (p.getDistanceFrom (downAt) > 4.0f)
                autoDrag = AutoDrag::band;
            break;
        case AutoDrag::bend:
            bendCurve = std::clamp (grabbed.curve + bendSign * static_cast<double> (downAt.y - p.y) / 60.0, -1.0, 1.0);
            break;
        case AutoDrag::band:
        case AutoDrag::move:
        case AutoDrag::none:
            break;
    }
    repaint();
}

void Timeline::automationUp (juce::Point<float> p, juce::ModifierKeys mods)
{
    lastAt = p;
    dragMods = mods;
    auto& s = studio.session();
    auto& sel = studio.pointSelection;
    const auto* lane = s.model().lane (autoLane.trackId, autoLane.target);
    switch (autoDrag)
    {
        case AutoDrag::add:
        {
            const auto at = std::max<Ticks> (0, studio.snapped (tickAt (p.x), mods.isAltDown()));
            if (const auto id = s.addAutomationPoint (autoLane.trackId, autoLane.target, at, valueAtY (autoLane.box, p.y)))
            {
                const auto* added = s.model().lane (autoLane.trackId, autoLane.target);
                sel = { added != nullptr ? added->id : Id {}, { *id } };
            }
            else
                studio.setStatus (juce::String (s.lastError()));
            studio.edited();
            break;
        }
        case AutoDrag::band:
            if (lane != nullptr)
            {
                const auto band = juce::Rectangle<float> (downAt, p);
                sel.laneId = lane->id;
                for (const auto& point : automation::sortedPoints (*lane))
                    if (band.contains (pointPosition (autoLane, point)))
                        sel.points.insert (point.id);
            }
            break;
        case AutoDrag::move:
            if (lane != nullptr && p.getDistanceFrom (downAt) > 1.0f)
            {
                const auto [dt, dv] = pointDelta (p, mods);
                std::vector<commands::Session::PointMove> moves;
                for (const auto& point : automation::sortedPoints (*lane))
                    if (sel.points.contains (point.id))
                        moves.push_back ({ point.id, std::max<Ticks> (0, point.pos + dt), std::clamp (point.value + dv, 0.0, 1.0) });
                studio.check (s.moveAutomationPoints (lane->id, moves));
            }
            else if (toggleOnUp)
                sel.points.erase (grabbed.id);
            break;
        case AutoDrag::bend:
            if (lane != nullptr)
                studio.check (s.setAutomationCurve (lane->id, grabbed.id, bendCurve));
            break;
        case AutoDrag::none:
            break;
    }
    autoDrag = AutoDrag::none;
    repaint();
}

juce::Rectangle<float> Timeline::regionBounds (const model::Region& r) const
{
    const auto& m = studio.model();
    const auto row = commands::trackRow (m, m.effectiveTrackId (r));
    const auto lane = laneBounds (row);
    const auto x1 = static_cast<float> (studio.view.tickToX (static_cast<double> (r.start)));
    const auto x2 = static_cast<float> (studio.view.tickToX (m.regionEndTick (r)));
    return { x1, lane.getY() + 2.0f, std::max (3.0f, x2 - x1), lane.getHeight() - 4.0f };
}

const model::Region* Timeline::regionAt (juce::Point<float> p) const
{
    const auto& m = studio.model();
    const auto row = rowAt (p.y);
    if (row < 0 || row >= rows())
        return nullptr;
    const model::Region* best = nullptr;
    for (const auto* r : m.regionsOnTrack (m.trackOrder[static_cast<std::size_t> (row)]))
    {
        if (! r->takeFolderId.empty())
            if (const auto f = m.takeFolders.find (r->takeFolderId); f != m.takeFolders.end() && f->second->activeTakeId != r->id)
                continue;
        if (regionBounds (*r).contains (p) && (best == nullptr || r->z >= best->z))
            best = r;
    }
    return best;
}

Timeline::Zone Timeline::zoneAt (const model::Region& r, juce::Point<float> p) const
{
    const auto b = regionBounds (r);
    if (! b.contains (p))
        return Zone::none;
    const bool top = p.y < b.getCentreY();
    const bool left = p.x < b.getX() + edge, right = p.x > b.getRight() - edge;
    // D-UI-4: lower corners trim; upper-left fades in (audio); upper-right in the name bar fades out (audio),
    // below it loops. MIDI regions trim from the upper-left too and loop from the whole upper-right.
    if (! r.takeFolderId.empty() && p.y < b.getY() + nameBar && p.x > b.getRight() - 60 && ! right)
        return Zone::takes;
    if (left)
        return ! top || r.isMidi ? Zone::trimStart : Zone::fadeIn;
    if (right)
    {
        if (! top)
            return Zone::trimEnd;
        if (! r.isMidi && p.y < b.getY() + nameBar)
            return Zone::fadeOut;
        return Zone::loop;
    }
    return Zone::move;
}

void Timeline::paintGrid (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto& m = studio.model();
    const auto& v = studio.view;
    const auto firstTick = static_cast<Ticks> (std::max (0.0, v.xToTick (0)));
    const auto lastTick = static_cast<Ticks> (v.xToTick (area.getRight()));
    for (auto bar = m.signatures.barAt (firstTick).bar;; ++bar)
    {
        const auto start = m.signatures.barStart (bar);
        if (start > lastTick)
            break;
        const auto len = m.signatures.barLength (bar);
        const auto beat = 4 * ticksPerQuarter / m.signatures.signatureAtBar (bar).denominator;
        const auto sub = arrange::gridTicks (studio.activeSnap(), m.signatures, start, v.pixelsPerTick);
        const auto step = sub > 0 && sub * v.pixelsPerTick >= 8.0 ? std::min<Ticks> (sub, beat) : beat;
        for (Ticks t = start; t < start + len; t += step)
        {
            const auto x = static_cast<float> (v.tickToX (static_cast<double> (t)));
            if (beat * v.pixelsPerTick < 6.0 && t != start)
                continue;
            g.setColour (t == start ? colour (Token::gridStrong) : (t - start) % beat == 0 ? colour (Token::gridWeak) : colour (Token::gridSub));
            g.drawVerticalLine (juce::roundToInt (x), area.getY(), area.getBottom());
        }
    }
}

void Timeline::paintRegion (juce::Graphics& g, const model::Region& r, juce::Rectangle<float> box, bool ghost)
{
    const auto& m = studio.model();
    const bool selected = studio.selection.contains (r.id);
    auto c = regionColour (m, r);
    if (r.muted)
        c = c.withSaturation (c.getSaturation() * 0.2f);
    const auto alpha = ghost ? 0.5f : r.muted ? 0.6f : 1.0f;
    const auto bodyColour = regionBody (c).withAlpha (alpha);
    // Loop iterations: the body only, with a dashed start edge.
    if (r.loop && r.loop->enabled && ! ghost)
    {
        const auto loopEnd = static_cast<float> (studio.view.tickToX (static_cast<double> (r.loop->endTicks)));
        for (auto x = box.getRight(); x < loopEnd; x += box.getWidth())
        {
            const auto it = juce::Rectangle<float> (x, box.getY(), std::min (box.getWidth(), loopEnd - x) - 1.0f, box.getHeight());
            g.setColour (bodyColour.withMultipliedAlpha (0.6f));
            g.fillRoundedRectangle (it, radius::r1);
            g.setColour (c.withAlpha (0.6f));
            g.fillRect (it.withHeight (2.0f));
        }
    }
    if (! ghost)
        depth::shadow (g, box, radius::r1, depth::Level::raised);
    g.setColour (bodyColour);
    g.fillRoundedRectangle (box, radius::r1);
    // The name bar carries the track colour at full strength; the body stays dark so content reads.
    const auto bar = box.withHeight (static_cast<float> (nameBar));
    {
        const juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (bar.toNearestInt());
        g.setColour (c.withAlpha (alpha));
        g.fillRoundedRectangle (box, radius::r1);
    }
    g.setColour (colour (Token::regionText).withAlpha (0.08f));
    g.fillRect (bar.withHeight (1.0f));
    auto body = box.withTrimmedTop (static_cast<float> (nameBar));
    const auto content = c.brighter (0.6f).withAlpha (alpha);
    if (r.isMidi)
    {
        if (const auto* seq = m.sequence (r.midi.sequenceId))
        {
            int lo = 127, hi = 0;
            for (const auto* n : seq->sortedNotes())
                lo = std::min (lo, n->pitch), hi = std::max (hi, n->pitch);
            const auto range = static_cast<float> (std::max (12, hi - lo + 1));
            const auto noteH = std::clamp ((body.getHeight() - 8.0f) / range, 2.0f, 4.0f);
            g.setColour (content);
            for (const auto* n : seq->sortedNotes())
            {
                if (n->pos < r.midi.sequenceOffset || n->pos >= r.midi.sequenceOffset + r.length)
                    continue;
                const auto x = static_cast<float> (studio.view.tickToX (static_cast<double> (r.start + n->pos - r.midi.sequenceOffset)));
                const auto w = std::max (2.0f, static_cast<float> (n->length * studio.view.pixelsPerTick) - 1.0f);
                const auto y = body.getBottom() - 4.0f - (static_cast<float> (n->pitch - lo) + 1.0f) / range * (body.getHeight() - 8.0f);
                g.fillRoundedRectangle (x, y, std::min (w, box.getRight() - x), noteH, 1.0f);
            }
        }
    }
    else if (body.getHeight() > 6)
    {
        const auto& wave = studio.waveform (r.audio.media);
        if (! wave.empty())
        {
            const auto rate = static_cast<double> (m.info.sampleRate);
            const double framesPerPixel = rate / (m.tempo.bpmAt (r.start) / 60.0 * ticksPerQuarter) / studio.view.pixelsPerTick;
            WaveformStyle style;
            style.peak = content.withMultipliedAlpha (0.55f);
            style.rms = content;
            style.gain = static_cast<float> (std::pow (10.0, r.audio.gainDb / 20.0));
            const juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (body.toNearestInt());
            drawWaveform (g, wave, body.reduced (0.0f, 2.0f), static_cast<double> (r.audio.sourceOffset), framesPerPixel, style);
        }
        // Fades.
        auto fadeX = [&] (Samples s) { return static_cast<float> (s) / static_cast<float> (r.length) * box.getWidth(); };
        g.setColour (colour (Token::regionText).withAlpha (0.8f));
        if (r.audio.fadeIn.length > 0)
            g.drawLine (box.getX(), body.getBottom(), box.getX() + fadeX (r.audio.fadeIn.length), body.getY(), 1.0f);
        if (r.audio.fadeOut.length > 0)
            g.drawLine (box.getRight() - fadeX (r.audio.fadeOut.length), body.getY(), box.getRight(), body.getBottom(), 1.0f);
    }
    // Audio still on its way (F-COL-10): the part not here yet is hatched, with how far it got.
    juce::String transfer;
    if (! r.isMidi && ! ghost)
        if (const auto d = studio.downloadState (r.audio.media))
        {
            const auto* ref = m.mediaRef (r.audio.media);
            const auto total = ref != nullptr ? static_cast<double> (ref->lengthSamples) : 0.0;
            // Where the downloaded prefix ends, in the region's own coordinates.
            const auto have = d->fraction * total - static_cast<double> (r.audio.sourceOffset);
            const auto split = total > 0 && r.length > 0 ? std::clamp (static_cast<float> (have / static_cast<double> (r.length)), 0.0f, 1.0f) : 0.0f;
            const auto missing = body.withTrimmedLeft (body.getWidth() * split);
            const juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (missing.toNearestInt());
            g.setColour (colour (Token::lane).withAlpha (0.45f));
            g.fillRect (missing);
            g.setColour (colour (Token::regionText).withAlpha (0.25f));
            for (auto x = missing.getX() - missing.getHeight(); x < missing.getRight(); x += 8.0f)
                g.drawLine (x, missing.getBottom(), x + missing.getHeight(), missing.getY(), 1.0f);
            transfer = d->failed ? "Download failed" : d->waiting ? "Waiting for upload" : "Downloading " + juce::String (juce::roundToInt (d->fraction * 100.0)) + "%";
        }
    // Name bar.
    g.setColour (colour (Token::regionText).withAlpha (alpha));
    g.setFont (font (Type::caption).withExtraKerningFactor (0.0f));
    auto label = juce::String (r.name);
    if (transfer.isNotEmpty())
        label << "  (" << transfer << ")";
    if (r.muted)
        label = "(muted) " + label;
    // Keep the name readable when the region starts left of the view.
    auto nameArea = bar.reduced (static_cast<float> (space::half), 0.0f);
    nameArea.setLeft (std::max (nameArea.getX(), static_cast<float> (space::half)));
    g.drawText (label, nameArea, juce::Justification::centredLeft, true);
    if (! r.takeFolderId.empty())
        if (const auto f = m.takeFolders.find (r.takeFolderId); f != m.takeFolders.end())
        {
            const auto& takes = f->second->takeIds;
            const auto it = std::find (takes.begin(), takes.end(), r.id);
            const auto text = "Take " + juce::String (static_cast<int> (it - takes.begin()) + 1) + "/" + juce::String (static_cast<int> (takes.size()));
            g.drawText (text, bar.withTrimmedLeft (box.getWidth() - 64.0f).reduced (static_cast<float> (space::half), 0.0f), juce::Justification::centredRight, true);
        }
    if (selected)
    {
        g.setColour (colour (Token::accent));
        g.drawRoundedRectangle (box.reduced (1.0f), radius::r1, 2.0f);
    }
    if (ghost)
        return;
    // Collaborators' selections, in their colours; several stack 1.5 px apart (05 §5). The first
    // sits just inside the region's edge so it covers whole pixels against the dark body.
    float offset = -0.75f;
    for (const auto* who : studio.collaborators())
        if (std::find (who->state.regions.begin(), who->state.regions.end(), r.id) != who->state.regions.end())
        {
            g.setColour (colourOf (*who));
            g.drawRoundedRectangle (box.expanded (offset), std::max (0.0f, radius::r1 + offset), 1.5f);
            offset += 1.5f;
        }
}

void Timeline::paint (juce::Graphics& g)
{
    const auto& m = studio.model();
    g.fillAll (colour (Token::lane));
    for (int row = 0; row < rows(); ++row)
    {
        const auto lane = rowBounds (row);
        if (lane.getBottom() < 0 || lane.getY() > getHeight())
            continue;
        if (m.trackOrder[static_cast<std::size_t> (row)] == studio.selectedTrack())
        {
            g.setColour (colour (Token::laneAlt));
            g.fillRect (lane);
        }
    }
    paintGrid (g, getLocalBounds().toFloat());
    // Lane separators over the grid.
    g.setColour (colour (Token::background).withAlpha (0.8f));
    for (int row = 0; row < rows(); ++row)
    {
        const auto lane = rowBounds (row);
        if (lane.getBottom() >= 0 && lane.getY() <= getHeight())
            g.fillRect (0.0f, lane.getBottom() - 1.0f, static_cast<float> (getWidth()), 1.0f);
    }
    // Below the last track the canvas recedes, so the end of the track list is plain.
    const auto end = rows() > 0 ? rowBounds (rows() - 1).getBottom() : 0.0f;
    if (end < static_cast<float> (getHeight()))
    {
        g.setColour (colour (Token::background).withAlpha (0.7f));
        g.fillRect (0.0f, end, static_cast<float> (getWidth()), static_cast<float> (getHeight()) - end);
    }
    if (studio.automationView)
        for (int row = 0; row < rows(); ++row)
        {
            const auto trackId = m.trackOrder[static_cast<std::size_t> (row)];
            const auto lanes = studio.shownLanes (trackId);
            for (std::size_t i = 0; i < lanes.size(); ++i)
            {
                const auto box = automationBounds (row, i);
                if (box.getBottom() >= 0 && box.getY() <= getHeight())
                    paintAutomation (g, { trackId, lanes[i], box });
            }
        }
    if (m.info.cycle.enabled)
    {
        // The loop region: a light wash with its edges marked, so it reads across every lane.
        const auto x1 = static_cast<float> (juce::roundToInt (studio.view.tickToX (static_cast<double> (m.info.cycle.start))));
        const auto x2 = static_cast<float> (juce::roundToInt (studio.view.tickToX (static_cast<double> (m.info.cycle.end))));
        g.setColour (depth::wash (0.045f, 0.04f));
        g.fillRect (x1, 0.0f, x2 - x1, static_cast<float> (getHeight()));
        g.setColour (colour (Token::loop).withAlpha (0.7f));
        g.fillRect (x1, 0.0f, 1.0f, static_cast<float> (getHeight()));
        g.fillRect (x2 - 1.0f, 0.0f, 1.0f, static_cast<float> (getHeight()));
    }

    // Regions: lowest first, so the top one is drawn last. Inactive takes are hidden.
    std::vector<const model::Region*> regions;
    for (const auto& [id, r] : m.regions)
    {
        if (! r->takeFolderId.empty())
            if (const auto f = m.takeFolders.find (r->takeFolderId); f != m.takeFolders.end() && f->second->activeTakeId != id)
                continue;
        regions.push_back (r.get());
    }
    std::stable_sort (regions.begin(), regions.end(), [] (const model::Region* a, const model::Region* b) { return a->z < b->z; });
    for (const auto* r : regions)
    {
        const auto box = regionBounds (*r);
        if (box.getRight() >= 0 && box.getX() <= getWidth() && box.getBottom() >= 0 && box.getY() <= getHeight())
            paintRegion (g, *r, box, false);
    }

    // Drag preview.
    if (regionDrag && regionDrag->isActive() && drag == Drag::move)
    {
        const auto clip = commands::RegionClipboard::of (m, studio.selection.ids());
        for (const auto& p : regionDrag->preview().placements)
            for (const auto& r : clip.regions)
                if (r.id == p.regionId)
                {
                    auto moved = r;
                    moved.start = p.start;
                    auto box = regionBounds (moved);
                    const auto row = p.newTrack >= 0 ? rows() + regionDrag->preview().newTracks[static_cast<std::size_t> (p.newTrack)].below
                                                     : commands::trackRow (m, p.trackId);
                    const auto lane = laneBounds (row);
                    box.setY (lane.getY() + 2.0f);
                    box.setHeight (lane.getHeight() - 4.0f);
                    paintRegion (g, moved, box, true);
                }
        g.setColour (colour (Token::text));
        g.setFont (font (Type::monoValue));
        g.drawText (juce::String (formatBarsBeats (regionDrag->anchorStart(), m.signatures)) + (regionDrag->copying() ? "  (copy)" : ""),
                    juce::Rectangle<float> (lastAt.x + 12, lastAt.y - 22, 160, 18), juce::Justification::left);
    }
    if (drag == Drag::rubberBand)
    {
        g.setColour (colour (Token::accent).withAlpha (0.15f));
        const auto band = juce::Rectangle<float> (downAt, lastAt);
        g.fillRect (band);
        g.setColour (colour (Token::accent));
        g.drawRect (band, 1.0f);
    }
    if (m.regions.empty())
    {
        g.setColour (colour (Token::textDim));
        g.setFont (font (Type::strong));
        g.drawText ("Drop audio or MIDI files here, or press R to record.", getLocalBounds().withTrimmedTop (contentHeight()).withHeight (60),
                    juce::Justification::centred);
    }
    for (const auto* c : commands::commentThreads (m, commands::CommentFilter::all, studio.session().user()))
        if (const auto box = pinBounds (*c))
            drawCommentPin (g, *box, userColour (studio, c->author), c->resolved);
    paintCollaborators (g);
    drawPlayheadLine (g, static_cast<float> (studio.view.tickToX (static_cast<double> (studio.playhead()))), 0.0f, static_cast<float> (getHeight()));
}

std::optional<juce::Rectangle<float>> Timeline::pinBounds (const model::Comment& thread) const
{
    const auto& m = studio.model();
    const auto track = commands::commentTrack (m, thread);
    if (track.empty())
        return std::nullopt;
    const auto row = commands::trackRow (m, track);
    if (row < 0)
        return std::nullopt;
    const auto lane = laneBounds (row);
    const auto x = static_cast<float> (studio.view.tickToX (static_cast<double> (thread.pos)));
    return juce::Rectangle<float> (x - 1.0f, lane.getY() + 2.0f, 12.0f, 14.0f);
}

const model::Comment* Timeline::commentPinAt (juce::Point<float> p) const
{
    const auto threads = commands::commentThreads (studio.model(), commands::CommentFilter::all, studio.session().user());
    for (auto it = threads.rbegin(); it != threads.rend(); ++it)
        if (const auto box = pinBounds (**it); box && box->expanded (2.0f).contains (p))
            return *it;
    return nullptr;
}

juce::PopupMenu Timeline::contextMenu (juce::Point<float> p) const
{
    juce::PopupMenu menu;
    const auto* r = regionAt (p);
    menu.addItem (1, r != nullptr ? "Add Comment on \"" + juce::String (r->name) + "\"" : juce::String ("Add Comment"), ! studio.previewing());
    return menu;
}

void Timeline::pickContext (int item, juce::Point<float> p)
{
    if (item != 1)
        return;
    const auto& m = studio.model();
    commands::CommentAnchor a;
    a.pos = std::max<Ticks> (0, studio.snapped (tickAt (p.x)));
    const auto row = rowAt (p.y);
    if (row >= 0 && row < rows())
        a.trackId = m.trackOrder[static_cast<std::size_t> (row)];
    if (const auto* r = regionAt (p))
        a.regionId = r->id;
    studio.startComment (a);
}

void Timeline::paintCollaborators (juce::Graphics& g)
{
    const auto& m = studio.model();
    for (const auto* who : studio.collaborators())
    {
        if (! who->hasState)
            continue;
        const auto c = colourOf (*who);
        // Playheads only while that collaborator plays.
        if (who->state.playhead.playing)
        {
            const auto x = static_cast<float> (studio.view.tickToX (static_cast<double> (studio.collaboratorPlayhead (*who))));
            g.setColour (c.withAlpha (0.8f));
            g.drawLine (x, 0.0f, x, static_cast<float> (getHeight()), 1.5f);
        }
        // Pointers over the arrange view, at their bar and track whatever our zoom (D-COL-5).
        const auto& ptr = who->state.pointer;
        if (! ptr || ptr->area != collab::Presence::View::arrange)
            continue;
        const auto row = commands::trackRow (m, ptr->track);
        if (row < 0 || row >= rows())
            continue;
        const auto lane = laneBounds (row);
        const auto tip = juce::Point<float> (static_cast<float> (studio.view.tickToX (static_cast<double> (ptr->pos))), lane.getCentreY() - 7.0f);
        if (getLocalBounds().toFloat().contains (tip))
            drawPointer (g, tip, c, juce::String::fromUTF8 (who->displayName.c_str()), pointerAlpha (studio, *who));
    }
}

std::optional<juce::Point<float>> Timeline::collaboratorPointer (const std::string& clientId) const
{
    for (const auto* who : studio.collaborators())
        if (who->clientId == clientId && who->state.pointer && who->state.pointer->area == collab::Presence::View::arrange)
        {
            const auto row = commands::trackRow (studio.model(), who->state.pointer->track);
            if (row < 0 || row >= rows())
                return std::nullopt;
            return juce::Point<float> (static_cast<float> (studio.view.tickToX (static_cast<double> (who->state.pointer->pos))), laneBounds (row).getCentreY());
        }
    return std::nullopt;
}

void Timeline::mouseExit (const juce::MouseEvent&) { studio.setPointer (std::nullopt); }

void Timeline::resized() { studio.timelineWidth = getWidth(); }

void Timeline::startRegionDrag (const model::Region& r, Zone zone, juce::Point<float> p, juce::ModifierKeys mods)
{
    dragRegion = r.id;
    auto& s = studio.session();
    switch (zone)
    {
        case Zone::move:
            drag = Drag::move;
            regionDrag = std::make_unique<arrange::RegionDrag> (s, studio.selection.ids(), r.id, studio.activeSnap(), studio.view.pixelsPerTick);
            regionDrag->update (0, 0, mods.isAltDown(), mods.isCommandDown());
            break;
        case Zone::trimStart: drag = Drag::trimStart; break;
        case Zone::trimEnd: drag = Drag::trimEnd; break;
        case Zone::fadeIn: drag = Drag::fadeIn; break;
        case Zone::fadeOut: drag = Drag::fadeOut; break;
        case Zone::loop: drag = Drag::loop; break;
        case Zone::none:
        case Zone::takes: break;
    }
    if (drag != Drag::move && drag != Drag::none)
    {
        static const char* names[] = { "", "", "Trim Region", "Trim Region", "Fade", "Fade", "Loop Region" };
        s.beginGesture (names[static_cast<int> (drag)]);
        gestureOpen = true;
    }
    juce::ignoreUnused (p);
}

void Timeline::pointerDown (juce::Point<float> p, juce::ModifierKeys mods, int clicks)
{
    studio.piano.focus = false; // keys act on regions again
    downAt = lastAt = p;
    auto& s = studio.session();
    const auto& m = studio.model();
    if (const auto* c = commentPinAt (p); c != nullptr && ! mods.isPopupMenu())
    {
        studio.showThread (c->id, localAreaToGlobal (pinBounds (*c)->toNearestInt()));
        return;
    }
    const auto* r = regionAt (p);
    const auto row = rowAt (p.y);
    const auto trackId = row >= 0 && row < rows() ? m.trackOrder[static_cast<std::size_t> (row)] : Id {};
    if (! trackId.empty())
        studio.selectTrack (trackId);
    if (mods.isPopupMenu())
    {
        juce::Component::SafePointer<Timeline> self (this);
        contextMenu (p).showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [self, p] (int item) {
            if (self != nullptr)
                self->pickContext (item, p);
        });
        return;
    }
    // Regions on a track someone else locked can't be picked up for editing (04 §9.6).
    if (r != nullptr && clicks < 2 && studio.lockedByOther (m.effectiveTrackId (*r)))
    {
        studio.setStatus (studio.lockTooltip (m.effectiveTrackId (*r)) + ".");
        return;
    }
    if (const auto hit = automationLaneAt (p))
    {
        automationDown (*hit, p, mods);
        return;
    }
    studio.pointSelection = {};

    if (clicks >= 2 && r != nullptr)
    {
        // Double-click opens the matching editor (spec 04 §7).
        studio.editorRegion = r->id;
        studio.layout.bottom = r->isMidi ? BottomTab::pianoRoll : BottomTab::audioEditor;
        studio.selection.click (r->id, false);
        studio.sendChangeMessage();
        return;
    }

    switch (studio.tool)
    {
        case Tool::pencil:
            if (r == nullptr && ! trackId.empty() && m.track (trackId)->kind == model::TrackKind::instrument)
            {
                const auto start = studio.snapped (tickAt (p.x));
                const auto at = m.signatures.barAt (start);
                const auto id = s.createMidiRegion (trackId, start, m.signatures.barLength (at.bar));
                if (id)
                    studio.selection.click (*id, false);
                studio.edited();
            }
            return;
        case Tool::scissors:
            if (r != nullptr)
            {
                const auto at = studio.snapped (tickAt (p.x), mods.isAltDown());
                if (s.splitRegion (r->id, at))
                    studio.edited();
                else
                    studio.setStatus (juce::String (s.lastError()));
            }
            return;
        case Tool::glue:
            if (r != nullptr)
            {
                auto ids = studio.selection.ids();
                if (std::find (ids.begin(), ids.end(), r->id) == ids.end())
                    ids.push_back (r->id);
                studio.check (engine::joinRegions (s, ids, studio.media(), studio.mediaDir()));
            }
            return;
        case Tool::eraser:
            if (r != nullptr)
                studio.check (s.deleteRegions ({ r->id }));
            return;
        case Tool::mute:
            if (r != nullptr)
                studio.check (s.toggleRegionsMuted ({ r->id }));
            return;
        case Tool::pointer: break;
    }

    if (r == nullptr)
    {
        if (! mods.isShiftDown())
            studio.selection.clear();
        drag = Drag::rubberBand;
        studio.sendChangeMessage();
        return;
    }
    const auto zone = zoneAt (*r, p);
    if (zone == Zone::takes)
    {
        nextTake (r->id);
        return;
    }
    if (mods.isShiftDown())
    {
        studio.selection.click (r->id, true);
        studio.sendChangeMessage();
        if (! studio.selection.contains (r->id))
            return;
    }
    else if (! studio.selection.contains (r->id))
        studio.selection.click (r->id, false);
    startRegionDrag (*r, zone, p, mods);
    studio.sendChangeMessage();
}

void Timeline::pointerDrag (juce::Point<float> p, juce::ModifierKeys mods)
{
    if (autoDrag != AutoDrag::none)
        return automationDrag (p, mods);
    lastAt = p;
    auto& s = studio.session();
    const auto& m = studio.model();
    const auto* r = m.region (dragRegion);
    const auto t = studio.snapped (std::max<Ticks> (0, tickAt (p.x)), mods.isAltDown());
    switch (drag)
    {
        case Drag::none: break;
        case Drag::move:
        {
            const auto rawDelta = tickAt (p.x) - tickAt (downAt.x);
            const auto downRow = rowAt (downAt.y), row = rowAt (p.y);
            regionDrag->update (rawDelta, row - downRow, mods.isAltDown(), mods.isCommandDown());
            break;
        }
        case Drag::rubberBand:
        {
            const auto band = juce::Rectangle<float> (downAt, p);
            studio.selection.rubberBand (m, rowAt (band.getY()), rowAt (band.getBottom()), studio.view.xToTick (band.getX()),
                                         studio.view.xToTick (band.getRight()), mods.isShiftDown());
            studio.sendChangeMessage();
            break;
        }
        case Drag::trimStart:
            if (r != nullptr)
                s.trimRegionStart (r->id, t);
            break;
        case Drag::trimEnd:
            if (r != nullptr)
            {
                if (r->isMidi)
                    s.trimRegionLength (r->id, std::max<Ticks> (t - r->start, 1));
                else
                {
                    const auto len = std::llround (m.tempo.tickToSample (static_cast<double> (t)) - m.tempo.tickToSample (static_cast<double> (r->start)));
                    s.trimRegionLength (r->id, std::max<std::int64_t> (len, model::minAudioRegionLength));
                }
            }
            break;
        case Drag::fadeIn:
        case Drag::fadeOut:
            if (r != nullptr && ! r->isMidi)
            {
                const auto box = regionBounds (*r);
                const auto frac = drag == Drag::fadeIn ? (p.x - box.getX()) / box.getWidth() : (box.getRight() - p.x) / box.getWidth();
                const auto len = static_cast<Samples> (std::clamp (frac, 0.0f, 1.0f) * static_cast<float> (r->length));
                if (drag == Drag::fadeIn)
                    s.setRegionFades (r->id, model::Fade { std::min (len, r->length - r->audio.fadeOut.length), r->audio.fadeIn.curve }, std::nullopt);
                else
                    s.setRegionFades (r->id, std::nullopt, model::Fade { std::min (len, r->length - r->audio.fadeIn.length), r->audio.fadeOut.curve });
            }
            break;
        case Drag::loop:
            if (r != nullptr)
                s.setRegionLoop (r->id, t);
            break;
    }
    if (drag != Drag::move && drag != Drag::rubberBand && drag != Drag::none)
        studio.edited();
    repaint();
}

void Timeline::pointerUp (juce::Point<float> p, juce::ModifierKeys mods)
{
    if (autoDrag != AutoDrag::none)
        return automationUp (p, mods);
    if (drag == Drag::move && regionDrag)
    {
        const bool moved = regionDrag->deltaTicks() != 0 || ! regionDrag->preview().newTracks.empty()
                           || std::any_of (regionDrag->preview().placements.begin(), regionDrag->preview().placements.end(), [&] (const auto& pl) {
                                  const auto* r = studio.model().region (pl.regionId);
                                  return r != nullptr && pl.trackId != studio.model().effectiveTrackId (*r);
                              });
        if (moved)
        {
            const auto copies = regionDrag->commit();
            if (! copies.empty())
                studio.selection.set (copies);
            studio.edited();
        }
        regionDrag.reset();
    }
    if (gestureOpen)
    {
        studio.session().endGesture();
        gestureOpen = false;
        studio.edited();
    }
    drag = Drag::none;
    repaint();
}

bool Timeline::cancelDrag()
{
    if (autoDrag != AutoDrag::none)
    {
        autoDrag = AutoDrag::none;
        repaint();
        return true;
    }
    if (drag == Drag::none)
        return false;
    if (regionDrag)
    {
        regionDrag->cancel();
        regionDrag.reset();
    }
    if (gestureOpen)
    {
        studio.session().cancelGesture();
        gestureOpen = false;
        studio.edited();
    }
    drag = Drag::none;
    repaint();
    return true;
}

void Timeline::wheel (juce::Point<float> p, juce::ModifierKeys mods, float deltaY)
{
    if (mods.isCommandDown() && mods.isShiftDown())
    {
        const auto h = studio.model().trackOrder.empty() ? defaultTrackHeight : studio.trackHeight (studio.model().trackOrder.front());
        studio.setAllTrackHeights (h + juce::roundToInt (deltaY * 40.0f));
    }
    else if (mods.isCommandDown())
        studio.view.zoomAround (std::pow (2.0, static_cast<double> (deltaY) * 2.0), p.x);
    else if (mods.isShiftDown())
        studio.view.originTick = std::max (0.0, studio.view.originTick - static_cast<double> (deltaY) * 200.0 / studio.view.pixelsPerTick);
    else
        scrollY = std::clamp (scrollY - juce::roundToInt (deltaY * 200.0f), 0, std::max (0, contentHeight() - getHeight() + defaultTrackHeight));
    if (onScroll)
        onScroll();
    studio.sendChangeMessage();
}

void Timeline::nextTake (const Id& regionId)
{
    const auto& m = studio.model();
    const auto* r = m.region (regionId);
    if (r == nullptr)
        return;
    const auto f = m.takeFolders.find (r->takeFolderId);
    if (f == m.takeFolders.end() || f->second->takeIds.empty())
        return;
    const auto& takes = f->second->takeIds;
    auto it = std::find (takes.begin(), takes.end(), f->second->activeTakeId);
    const auto next = it == takes.end() || it + 1 == takes.end() ? takes.front() : *(it + 1);
    studio.check (studio.session().setActiveTake (f->first, next));
}

engine::ImportOutcome Timeline::dropFiles (const std::vector<std::filesystem::path>& files, juce::Point<float> p)
{
    const auto row = rowAt (p.y);
    engine::DropTarget target;
    target.at = studio.snapped (std::max<Ticks> (0, tickAt (p.x)));
    if (row >= 0 && row < rows())
        target.trackId = studio.model().trackOrder[static_cast<std::size_t> (row)];
    return studio.importFiles (files, target);
}

void Timeline::filesDropped (const juce::StringArray& files, int x, int y)
{
    std::vector<std::filesystem::path> paths;
    for (const auto& f : files)
        paths.emplace_back (f.toStdString());
    dropFiles (paths, { static_cast<float> (x), static_cast<float> (y) });
}

void Timeline::itemDropped (const SourceDetails& details)
{
    std::vector<std::filesystem::path> paths;
    if (auto* tree = dynamic_cast<juce::FileTreeComponent*> (details.sourceComponent.get()))
        for (int i = 0; i < tree->getNumSelectedFiles(); ++i)
            paths.emplace_back (tree->getSelectedFile (i).getFullPathName().toStdString());
    else if (details.description.isString())
        paths.emplace_back (details.description.toString().toStdString());
    if (! paths.empty())
        dropFiles (paths, details.localPosition.toFloat());
}

void Timeline::mouseMove (const juce::MouseEvent& e)
{
    // Shared with collaborators in musical coordinates: the bar and the track under the pointer.
    const auto row = rowAt (e.position.y);
    if (row >= 0 && row < rows())
        studio.setPointer (collab::Presence::Pointer { collab::Presence::View::arrange, tickAt (e.position.x),
                                                       studio.model().trackOrder[static_cast<std::size_t> (row)], -1 });
    else
        studio.setPointer (std::nullopt);
    const auto* r = studio.tool == Tool::pointer ? regionAt (e.position) : nullptr;
    auto shape = juce::MouseCursor::NormalCursor;
    if (r != nullptr)
        switch (zoneAt (*r, e.position))
        {
            case Zone::trimStart:
            case Zone::trimEnd: shape = juce::MouseCursor::LeftRightResizeCursor; break;
            case Zone::fadeIn: shape = juce::MouseCursor::TopLeftCornerResizeCursor; break;
            case Zone::fadeOut: shape = juce::MouseCursor::TopRightCornerResizeCursor; break;
            case Zone::loop: shape = juce::MouseCursor::CopyingCursor; break;
            case Zone::takes: shape = juce::MouseCursor::PointingHandCursor; break;
            case Zone::none:
            case Zone::move: break;
        }
    else if (studio.tool != Tool::pointer)
        shape = juce::MouseCursor::CrosshairCursor;
    setMouseCursor (shape);
}

void Timeline::mouseDown (const juce::MouseEvent& e) { pointerDown (e.position, e.mods, 1); }
void Timeline::mouseDrag (const juce::MouseEvent& e) { pointerDrag (e.position, e.mods); }
void Timeline::mouseUp (const juce::MouseEvent& e) { pointerUp (e.position, e.mods); }
void Timeline::mouseDoubleClick (const juce::MouseEvent& e) { pointerDown (e.position, e.mods, 2); }
void Timeline::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) { wheel (e.position, e.mods, w.deltaY); }

// Tracks area -------------------------------------------------------------------------------

TracksArea::TracksArea (Studio& s) : studio (s), rulerComponent (s), lanes (s)
{
    addAndMakeVisible (rulerComponent);
    addAndMakeVisible (lanes);
    addAndMakeVisible (headerColumn);
    headerColumn.setTitle ("Track headers");
    addAndMakeVisible (vertical);
    addAndMakeVisible (horizontal);
    vertical.addListener (this);
    horizontal.addListener (this);
    vertical.setTitle ("Vertical scroll");
    horizontal.setTitle ("Horizontal scroll");
    lanes.onScroll = [this] { resized(); };
    studio.addChangeListener (this);
    rebuildHeaders();
    startTimerHz (30);
}

TracksArea::~TracksArea() { studio.removeChangeListener (this); }

void TracksArea::rebuildHeaders()
{
    const auto& order = studio.model().trackOrder;
    if (order != shownTracks)
    {
        trackHeaders.clear();
        int n = 1;
        for (const auto& id : order)
        {
            trackHeaders.push_back (std::make_unique<TrackHeader> (studio, id, n++));
            headerColumn.addAndMakeVisible (*trackHeaders.back());
        }
        shownTracks = order;
    }
    for (auto& h : trackHeaders)
        h->refresh();
    layoutHeaders();
}

void TracksArea::layoutHeaders()
{
    for (std::size_t i = 0; i < trackHeaders.size(); ++i)
        trackHeaders[i]->setBounds (lanes.rowBounds (static_cast<int> (i)).withX (0).withWidth (static_cast<float> (headerWidth)).toNearestInt());
}

void TracksArea::changeListenerCallback (juce::ChangeBroadcaster*)
{
    rebuildHeaders();
    resized();
    repaint();
}

void TracksArea::timerCallback()
{
    // Follow playhead: page when it leaves the view.
    if (studio.follow && studio.playing())
    {
        const auto x = studio.view.tickToX (static_cast<double> (studio.playhead()));
        if (x > lanes.getWidth() * 0.9 || x < 0)
            studio.view.originTick = static_cast<double> (studio.playhead()) - lanes.getWidth() * 0.1 / studio.view.pixelsPerTick;
    }
    rulerComponent.repaint();
    lanes.repaint();
    if (! studio.collaborators().empty() || hadCollaborators)
        for (auto& h : trackHeaders)
            h->repaint();
    hadCollaborators = ! studio.collaborators().empty();
}

void TracksArea::scrollBarMoved (juce::ScrollBar* bar, double start)
{
    if (bar == &vertical)
        lanes.scrollY = juce::roundToInt (start);
    else
        studio.view.originTick = start / studio.view.pixelsPerTick;
    resized();
    repaint();
}

void TracksArea::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::surface));
    // The corner over the track headers: the panel's caption, level with the ruler.
    const auto corner = juce::Rectangle<int> (0, 0, headerWidth, rulerComponent.getHeight());
    g.setGradientFill (juce::ColourGradient::vertical (colour (Token::surfaceRaised), 0.0f, colour (Token::surface), static_cast<float> (Ruler::rulerHeight)));
    g.fillRect (corner);
    depth::highlight (g, corner.toFloat(), 0.0f);
    g.setColour (colour (Token::textDim));
    g.setFont (font (Type::caption));
    g.drawText (captionText ("Tracks"), corner.withHeight (Ruler::rulerHeight).reduced (space::s1 + space::half, 0), juce::Justification::centredLeft);
    if (studio.layout.globalTracks)
    {
        // The global tracks' names, level with their lanes.
        g.setFont (font (Type::small));
        const auto label = [&] (const char* text, int top, int height) {
            g.drawText (text, corner.withY (top).withHeight (height).reduced (space::s1 + space::half, 0), juce::Justification::centredLeft);
        };
        label ("Markers", Ruler::rulerHeight, Ruler::markerLane);
        label ("Tempo", Ruler::tempoTop, Ruler::tempoLane);
        label ("Signature", Ruler::signatureTop, Ruler::signatureLane);
    }
    g.setColour (colour (Token::border));
    g.fillRect (corner.withTop (corner.getBottom() - 1));
}

void TracksArea::paintOverChildren (juce::Graphics& g)
{
    // The header column sits above the canvas: a short shadow falls onto the lanes.
    const auto x = static_cast<float> (headerWidth);
    const auto a = depth::shade (0.3f, 0.1f);
    g.setGradientFill (juce::ColourGradient::horizontal (a, x, a.withAlpha (0.0f), x + 5.0f));
    g.fillRect (x, static_cast<float> (rulerComponent.getY()), 5.0f, static_cast<float> (getHeight()));
    g.setColour (colour (Token::border));
    g.fillRect (x - 1.0f, 0.0f, 1.0f, static_cast<float> (getHeight()));
}

void TracksArea::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (rulerComponent.preferredHeight());
    top.removeFromLeft (headerWidth);
    const auto bar = getLookAndFeel().getDefaultScrollbarWidth();
    rulerComponent.setBounds (top.withTrimmedRight (bar));
    horizontal.setBounds (r.removeFromBottom (bar).withTrimmedLeft (headerWidth).withTrimmedRight (bar));
    vertical.setBounds (r.removeFromRight (bar));
    headerColumn.setBounds (r.removeFromLeft (headerWidth));
    lanes.setBounds (r);
    layoutHeaders();
    vertical.setRangeLimits (0.0, std::max<double> (lanes.contentHeight() + defaultTrackHeight, lanes.getHeight()), juce::dontSendNotification);
    vertical.setCurrentRange (lanes.scrollY, lanes.getHeight(), juce::dontSendNotification);
    const auto endTick = std::max (commands::projectEndTick (studio.model()) + 3840 * 8, static_cast<Ticks> (studio.view.xToTick (lanes.getWidth())));
    horizontal.setRangeLimits (0.0, static_cast<double> (endTick) * studio.view.pixelsPerTick, juce::dontSendNotification);
    horizontal.setCurrentRange (studio.view.originTick * studio.view.pixelsPerTick, lanes.getWidth(), juce::dontSendNotification);
}
} // namespace weft::ui
