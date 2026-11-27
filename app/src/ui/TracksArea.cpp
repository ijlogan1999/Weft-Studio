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

/** The playhead: a 1 px line (3 px when wide, D-UI-41) with a dark halo, readable over any region colour. */
void drawPlayheadLine (juce::Graphics& g, float x, float top, float bottom)
{
    const auto px = static_cast<float> (juce::roundToInt (x));
    const float half = Theme::get().widePlayhead ? 1.0f : 0.0f;
    g.setColour (depth::shade (0.45f, 0.15f));
    g.fillRect (px - 1.0f - half, top, 3.0f + 2 * half, bottom - top);
    g.setColour (colour (Token::playhead));
    g.fillRect (px - half, top, 1.0f + 2 * half, bottom - top);
}

} // namespace

// Ruler -----------------------------------------------------------------------------------

Ruler::Ruler (Studio& s) : studio (s)
{
    setTitle ("Ruler");
    setTooltip ("Ruler: click to move the playhead; drag the top strip to set the cycle. Marker lane: double-click to add a marker. "
                "Tempo lane: Ctrl+click to add a tempo change, drag a point to change it, Alt-drag to copy it. Signature lane: double-click to change the meter.");
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
    const auto top = laneBounds (laneTempo).value_or (juce::Rectangle<int> (0, tempoTop, 0, tempoLane)).getY();
    return static_cast<float> (top + 5 + (hi - bpm) / std::max (1.0, hi - lo) * inner);
}

double Ruler::bpmAtY (float y) const
{
    const auto [lo, hi] = tempoRange();
    const auto inner = static_cast<double> (tempoLane - 10);
    const auto top = laneBounds (laneTempo).value_or (juce::Rectangle<int> (0, tempoTop, 0, tempoLane)).getY();
    const auto bpm = hi - (static_cast<double> (y) - top - 5) / inner * (hi - lo);
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

int Ruler::laneHeight (GlobalLane lane) noexcept
{
    switch (lane)
    {
        case laneArrangement: return arrangementLane;
        case laneMarkers: return markerLane;
        case laneTempo: return tempoLane;
        case laneSignature: return signatureLane;
        case laneKey: return keyLane;
        default: return 0;
    }
}

std::vector<std::pair<GlobalLane, juce::Rectangle<int>>> Ruler::lanes() const
{
    std::vector<std::pair<GlobalLane, juce::Rectangle<int>>> out;
    int y = rulerHeight;
    for (const auto lane : globalLaneOrder)
        if (studio.laneShown (lane))
        {
            out.emplace_back (lane, juce::Rectangle<int> (0, y, getWidth(), laneHeight (lane)));
            y += laneHeight (lane);
        }
    return out;
}

std::optional<juce::Rectangle<int>> Ruler::laneBounds (GlobalLane lane) const
{
    for (const auto& [l, r] : lanes())
        if (l == lane)
            return r;
    return std::nullopt;
}

std::optional<GlobalLane> Ruler::laneAt (float y) const
{
    for (const auto& [l, r] : lanes())
        if (y >= static_cast<float> (r.getY()) && y < static_cast<float> (r.getBottom()))
            return l;
    return std::nullopt;
}

int Ruler::preferredHeight() const
{
    int h = rulerHeight;
    for (const auto lane : globalLaneOrder)
        if (studio.laneShown (lane))
            h += laneHeight (lane);
    return h;
}

const model::Section* Ruler::sectionAt (double x) const
{
    const auto t = studio.view.xToTick (x);
    for (const auto* section : studio.model().sectionsInOrder())
        if (t >= static_cast<double> (section->start) && t < static_cast<double> (section->end()))
            return section;
    return nullptr;
}

const model::KeyChange* Ruler::keyChangeAt (double x) const
{
    const model::KeyChange* best = nullptr;
    for (const auto* k : studio.model().keyChangesInOrder())
    {
        const auto kx = studio.view.tickToX (static_cast<double> (k->pos));
        if (x >= kx - 3 && x <= kx + 56)
            best = k;
    }
    return best;
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

std::vector<std::pair<const model::Marker*, juce::Rectangle<float>>> Ruler::markerFlags() const
{
    std::vector<std::pair<const model::Marker*, juce::Rectangle<float>>> out;
    if (studio.laneShown (laneMarkers))
        return out;
    const auto f = font (Type::caption);
    for (const auto* marker : arrange::markersInOrder (studio.model()))
    {
        const auto x = static_cast<float> (juce::roundToInt (studio.view.tickToX (static_cast<double> (marker->pos))));
        const auto w = std::min (84.0f, juce::GlyphArrangement::getStringWidth (f, captionText (juce::String (marker->name))) + 10.0f);
        out.emplace_back (marker, juce::Rectangle<float> (x, rulerHeight - 12.0f, w, 11.0f));
    }
    return out;
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
    // Marker flags take the ruler's lower edge (D-UI-20): the bar numbers move up out of their way.
    const bool flagged = ! markerFlags().empty();
    {
        for (std::size_t i = 0; i < labels.size(); ++i)
        {
            const auto [bar, x] = labels[i];
            g.setColour (colour (Token::border));
            g.fillRect (static_cast<float> (juce::roundToInt (x)), static_cast<float> (cycleStrip), 1.0f, static_cast<float> (rulerHeight - cycleStrip));
            g.setColour (colour (Token::textDim));
            g.drawText (juce::String (bar), juce::roundToInt (x) + space::half, flagged ? cycleStrip : cycleStrip + space::half, 40, flagged ? 13 : 16,
                        juce::Justification::left);
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
    // The project end (set from the timeline's right-click menu): a flag at its bar position.
    if (const auto end = m.info.endMarker)
    {
        const auto x = static_cast<float> (juce::roundToInt (v.tickToX (static_cast<double> (*end))));
        g.setColour (colour (Token::text));
        g.fillRect (x, static_cast<float> (cycleStrip), 2.0f, static_cast<float> (rulerHeight - cycleStrip));
        g.setFont (font (Type::caption));
        g.drawText (captionText ("End"), juce::roundToInt (x) - 40, rulerHeight - 14, 37, 12, juce::Justification::centredRight);
    }
    g.setColour (colour (Token::border));
    g.fillRect (0, rulerHeight - 1, getWidth(), 1);
    // The global tracks showing (D-UI-44), each in its band.
    for (const auto& [lane, laneArea] : lanes())
    {
        const auto top = laneArea.getY();
        g.setColour (colour (Token::surface));
        g.fillRect (laneArea);
        if ((studio.protectedLanes & lane) != 0)
        {
            g.setColour (colour (Token::textDim).withAlpha (0.06f)); // protected: a faint veil
            g.fillRect (laneArea);
        }
        if (lane == laneArrangement)
        {
            // Sections: coloured blocks spanning their bars, named.
            g.setFont (font (Type::body));
            for (const auto* section : m.sectionsInOrder())
            {
                const auto x1 = static_cast<float> (v.tickToX (static_cast<double> (section->start)));
                const auto x2 = static_cast<float> (v.tickToX (static_cast<double> (section->end())));
                const auto block = juce::Rectangle<float> (x1, top + 2.0f, std::max (2.0f, x2 - x1 - 1.0f), arrangementLane - 4.0f);
                const auto c = section->color ? colourFor (section->color->palette, section->color->rgb) : colour (Token::accent);
                depth::tinted (g, block, radius::r1, c.withMultipliedAlpha (0.55f));
                g.setColour (colour (Token::text));
                g.drawText (juce::String (section->name), block.withTrimmedLeft (space::s1).withTrimmedRight (space::half), juce::Justification::centredLeft, true);
            }
            if (sectionDrop)
            {
                g.setColour (colour (Token::accent));
                g.fillRect (static_cast<float> (v.tickToX (static_cast<double> (*sectionDrop))) - 1.0f, static_cast<float> (top), 3.0f, static_cast<float> (arrangementLane));
            }
        }
        else if (lane == laneMarkers)
        {
            // Markers: raised chips with their colour on the left edge.
            g.setFont (font (Type::body));
            for (const auto* marker : arrange::markersInOrder (m))
            {
                const auto x = static_cast<float> (v.tickToX (static_cast<double> (marker->pos)));
                const auto c = marker->color ? colourFor (marker->color->palette, marker->color->rgb) : colour (Token::textDim);
                const auto chip = juce::Rectangle<float> (x, top + 3.0f, 72.0f, markerLane - 6.0f);
                depth::raised (g, chip, radius::r1);
                g.setColour (c);
                g.fillRect (chip.withWidth (3.0f));
                g.setColour (colour (Token::text));
                g.drawText (juce::String (marker->name), chip.withTrimmedLeft (space::s1).withTrimmedRight (space::half), juce::Justification::centredLeft, true);
            }
            for (const auto* c : commands::commentThreads (m, commands::CommentFilter::all, studio.session().user()))
                drawCommentPin (g, pinBounds (*c), userColour (studio, c->author), c->resolved);
        }
        else if (lane == laneTempo)
        {
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
                const auto above = y - static_cast<float> (top) > 16.0f;
                g.drawText (bpmText (p.bpm), juce::roundToInt (x) + 6, juce::roundToInt (above ? y - 15.0f : y + 2.0f), 56, 13, juce::Justification::left);
            }
        }
        else if (lane == laneSignature)
        {
            // Signature lane: the meter at each change, at its bar start.
            g.setFont (font (Type::monoValue));
            for (const auto& p : m.signatures.points())
            {
                const auto x = static_cast<float> (v.tickToX (static_cast<double> (m.signatures.barStart (p.bar))));
                g.setColour (colour (Token::border));
                g.fillRect (x, static_cast<float> (top), 1.0f, static_cast<float> (signatureLane));
                g.setColour (p.conflicted ? colour (Token::warning) : colour (Token::text));
                g.drawText (juce::String (p.numerator) + "/" + juce::String (p.denominator), juce::roundToInt (x) + space::half, top, 40, signatureLane,
                            juce::Justification::centredLeft);
            }
        }
        else if (lane == laneKey)
        {
            // Key lane: the project key at the start, then each change.
            g.setFont (font (Type::small));
            const auto drawKey = [&] (Ticks at, const model::KeySignature& key) {
                const auto x = static_cast<float> (v.tickToX (static_cast<double> (at)));
                g.setColour (colour (Token::border));
                g.fillRect (x, static_cast<float> (top), 1.0f, static_cast<float> (keyLane));
                g.setColour (colour (Token::text));
                g.drawText (juce::String (key.name()), juce::roundToInt (x) + space::half, top, 96, keyLane, juce::Justification::centredLeft);
            };
            drawKey (0, m.info.key);
            for (const auto* k : m.keyChangesInOrder())
                drawKey (k->pos, k->key);
        }
        g.setColour (colour (Token::lineSubtle));
        g.fillRect (0, laneArea.getBottom() - 1, getWidth(), 1);
    }
    if (! lanes().empty())
    {
        g.setColour (colour (Token::border));
        g.fillRect (0, getHeight() - 1, getWidth(), 1);
    }
    // Global tracks hidden: the markers stay findable as flags on the ruler (D-UI-20).
    g.setFont (font (Type::caption));
    for (const auto& [marker, flag] : markerFlags())
    {
        const auto c = marker->color ? colourFor (marker->color->palette, marker->color->rgb) : colour (Token::textDim);
        g.setColour (colour (Token::surfaceRaised));
        g.fillRect (flag);
        g.setColour (c);
        g.fillRect (flag.getX(), static_cast<float> (cycleStrip), 2.0f, static_cast<float> (rulerHeight - cycleStrip));
        g.setColour (colour (Token::text));
        g.drawText (captionText (juce::String (marker->name)), flag.withTrimmedLeft (5.0f), juce::Justification::centredLeft, true);
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
    const auto top = laneBounds (laneMarkers).value_or (juce::Rectangle<int> (0, rulerHeight, 0, markerLane)).getY();
    return { x - 1.0f, top + 3.0f, 14.0f, markerLane - 5.0f };
}

const model::Comment* Ruler::commentAt (juce::Point<float> p) const
{
    if (! studio.laneShown (laneMarkers))
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
    const auto lane = p.y >= rulerHeight ? laneAt (p.y) : std::nullopt;
    if (lane)
        studio.layout.singleLane = *lane; // what Show Single Global Track keeps
    if (mods.isPopupMenu() && lane)
    {
        juce::Component::SafePointer<Ruler> self (this);
        contextMenu (p).showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [self, p] (int item) {
            if (self != nullptr)
                self->pickContext (item, p);
        });
        return;
    }
    if (lane == laneSignature)
        return;
    if (lane == laneArrangement)
    {
        // An edge resizes the section; a click inside selects its regions and goes to its start (D-UI-45).
        for (const auto* section : studio.model().sectionsInOrder())
        {
            const auto x1 = studio.view.tickToX (static_cast<double> (section->start)), x2 = studio.view.tickToX (static_cast<double> (section->end()));
            const bool nearStart = std::abs (p.x - x1) <= 4.0, nearEnd = std::abs (p.x - x2) <= 4.0;
            if ((nearStart || nearEnd) && ! studio.laneProtected (laneArrangement))
            {
                drag = nearEnd ? Drag::sectionEnd : Drag::sectionStart;
                draggedSection = section->id;
                studio.session().beginGesture ("Resize Section");
                return;
            }
        }
        if (const auto* section = sectionAt (p.x))
        {
            std::vector<Id> inside;
            for (const auto& [id, r] : studio.model().regions)
                if (r->start >= section->start && r->start < section->end())
                    inside.push_back (id);
            studio.selection.set (inside);
            studio.locate (section->start);
            // Dragging the section moves it and what's in it to another section edge; Ctrl copies (D-UI-45).
            if ((studio.protectedLanes & laneArrangement) == 0)
            {
                drag = Drag::sectionMove;
                draggedSection = section->id;
                dragStart = p;
                copySection = mods.isCommandDown();
                sectionDrop.reset();
            }
        }
        return;
    }
    if (lane == laneKey)
    {
        if (const auto* change = keyChangeAt (p.x))
        {
            studio.locate (change->pos);
            if (! studio.laneProtected (laneKey))
            {
                drag = Drag::keyChange;
                draggedKey = change->id;
                studio.session().beginGesture ("Move Key Change");
            }
        }
        return;
    }
    if (lane == laneTempo)
    {
        if (const auto* point = tempoPointAt (p.x))
        {
            if (studio.laneProtected (laneTempo))
                return;
            dragRange = tempoRange();
            drag = Drag::tempo;
            draggedTempo = point->id;
            dragFrom = point->pos;
            dragBpm = point->bpm;
            dragStart = p;
            copyTempo = mods.isAltDown();
            tempoLock.reset();
            studio.session().beginGesture (copyTempo ? "Copy Tempo Change" : "Tempo Change");
            return;
        }
        if ((mods.isCommandDown() || studio.tool == Tool::pencil) && ! studio.laneProtected (laneTempo))
        {
            if (studio.session().addTempoPoint (t, bpmAtY (p.y)))
                studio.edited();
            else
                studio.setStatus (juce::String (studio.session().lastError()));
        }
        return;
    }
    if (lane == laneMarkers)
    {
        if (const auto* c = commentAt (p))
        {
            studio.showThread (c->id, localAreaToGlobal (pinBounds (*c).toNearestInt()));
            return;
        }
        if (const auto* marker = markerAt (p.x))
        {
            studio.locate (marker->pos);
            if ((studio.protectedLanes & laneMarkers) != 0)
                return; // protected: it only goes there
            drag = Drag::marker;
            draggedMarker = marker->id;
            dragFrom = marker->pos;
            studio.session().beginGesture ("Move Marker");
        }
        return;
    }
    if (p.y >= rulerHeight)
        return; // below the last lane
    // A click on a marker's flag goes to the marker itself, whatever the snap.
    for (const auto& [marker, flag] : markerFlags())
        if (flag.contains (p))
        {
            studio.locate (marker->pos);
            return;
        }
    drag = Drag::playhead;
    studio.locate (t);
}

void Ruler::pointerDrag (juce::Point<float> p, juce::ModifierKeys mods)
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
        case Drag::keyChange:
            if (const auto it = studio.model().keyChanges.find (draggedKey); it != studio.model().keyChanges.end())
            {
                const auto key = it->second->key;
                studio.session().setKeyChange (draggedKey, std::max<Ticks> (1, t), key); // a clash with another change just stays put
            }
            break;
        case Drag::sectionMove:
        {
            if (std::abs (p.x - dragStart.x) < 4.0f)
            {
                sectionDrop.reset();
                break;
            }
            const auto* dragged = studio.model().sections.contains (draggedSection) ? studio.model().sections.at (draggedSection).get() : nullptr;
            if (dragged == nullptr)
                break;
            std::optional<Ticks> best;
            double bestDistance = 0;
            std::vector<Ticks> edges { 0 };
            for (const auto* other : studio.model().sectionsInOrder())
            {
                edges.push_back (other->start);
                edges.push_back (other->end());
            }
            for (const auto e : edges)
            {
                if (e > dragged->start && e < dragged->end())
                    continue;
                if (const auto d = std::abs (studio.view.tickToX (static_cast<double> (e)) - p.x); ! best || d < bestDistance)
                {
                    best = e;
                    bestDistance = d;
                }
            }
            sectionDrop = best;
            repaint();
            break;
        }
        case Drag::sectionStart:
        case Drag::sectionEnd:
            if (const auto it = studio.model().sections.find (draggedSection); it != studio.model().sections.end())
            {
                const auto start = it->second->start, end = it->second->end();
                if (drag == Drag::sectionStart && t < end)
                    studio.session().setSectionRange (draggedSection, t, end - t);
                else if (drag == Drag::sectionEnd && t > start)
                    studio.session().setSectionRange (draggedSection, start, t - start);
            }
            break;
        case Drag::tempo:
        {
            // Vertical: BPM in 0.01 steps over the lane's range at the drag start; horizontal (past a few
            // pixels) moves the change, except the first, which stays at the start (W-11 steps 2-3).
            const auto [lo, hi] = dragRange;
            const auto perPixel = (hi - lo) / static_cast<double> (tempoLane - 10);
            const auto bpm = std::clamp (std::round ((dragBpm - static_cast<double> (p.y - dragStart.y) * perPixel) * 100.0) / 100.0, TempoMap::minBpm,
                                         TempoMap::maxBpm);
            const bool sideways = std::abs (p.x - dragStart.x) >= 4.0f;
            if (copyTempo)
            {
                // Alt-drag copies: nothing changes until the copy leaves the original's place.
                if (! sideways)
                    break;
                const auto copy = studio.session().addTempoPoint (std::max<Ticks> (1, t), dragBpm);
                if (! copy)
                {
                    studio.setStatus (juce::String (studio.session().lastError()));
                    break;
                }
                draggedTempo = *copy;
                copyTempo = false;
            }
            auto pos = draggedTempo == "initial" || ! sideways ? dragFrom : std::max<Ticks> (1, t);
            auto value = bpm;
            // Shift keeps the drag to the direction it set off in once it has gone 4 px (D-UI-30, as D-W3-10).
            if (mods.isShiftDown())
            {
                const auto dx = std::abs (p.x - dragStart.x), dy = std::abs (p.y - dragStart.y);
                if (! tempoLock && std::max (dx, dy) >= 4.0f)
                    tempoLock = dx >= dy ? 'x' : 'y';
                if (tempoLock == 'x')
                    value = dragBpm;
                else
                    pos = dragFrom;
            }
            else
                tempoLock.reset();
            studio.check (studio.session().setTempoPoint (draggedTempo, pos, value));
            break;
        }
        case Drag::none: break;
    }
}

void Ruler::pointerUp()
{
    if (drag == Drag::sectionMove)
    {
        drag = Drag::none;
        const auto target = std::exchange (sectionDrop, std::nullopt);
        if (target)
        {
            if (studio.session().moveSection (draggedSection, *target, copySection))
                studio.edited();
            else
                studio.setStatus (juce::String (studio.session().lastError()));
        }
        repaint();
        return;
    }
    if (drag == Drag::marker || drag == Drag::tempo || drag == Drag::punchStart || drag == Drag::punchEnd || drag == Drag::punch || drag == Drag::keyChange
        || drag == Drag::sectionStart || drag == Drag::sectionEnd)
    {
        studio.session().endGesture();
        studio.edited();
    }
    drag = Drag::none;
}

void Ruler::mouseDown (const juce::MouseEvent& e) { pointerDown (e.position, e.mods); }
void Ruler::mouseDrag (const juce::MouseEvent& e) { pointerDrag (e.position, e.mods); }
void Ruler::mouseUp (const juce::MouseEvent& e)
{
    // A click (no drag) in the cycle strip toggles the cycle.
    if (drag == Drag::cycle && ! e.mouseWasDraggedSinceMouseDown())
        studio.perform (Cmd::cycle);
    pointerUp();
}

juce::PopupMenu Ruler::contextMenu (juce::Point<float> p) const
{
    juce::PopupMenu menu;
    const auto lane = p.y >= rulerHeight ? laneAt (p.y) : std::nullopt;
    if (! lane)
        return menu;
    const bool editable = ! studio.previewing() && (studio.protectedLanes & *lane) == 0;
    if (lane == laneArrangement)
    {
        if (const auto* section = sectionAt (p.x))
        {
            menu.addItem (contextRename, "Rename Section...", editable);
            auto colours = colourMenu (contextColour, section->color && section->color->rgb.empty() ? section->color->palette : -1);
            colours.addSeparator();
            colours.addItem (contextColour, "No Color", true, ! section->color.has_value());
            menu.addSubMenu ("Color", colours, editable);
            menu.addSeparator();
            menu.addItem (contextDelete, "Delete Section", editable);
        }
        else
            menu.addItem (contextAdd, "Add Section Here", editable);
    }
    else if (lane == laneKey)
    {
        const auto* change = keyChangeAt (p.x);
        menu.addItem (contextEdit, change != nullptr ? "Edit Key..." : "Add Key Change Here...", editable);
        if (change != nullptr)
            menu.addItem (contextDelete, "Delete Key Change", editable);
    }
    else if (lane == laneSignature)
    {
        const auto* sig = signatureAt (p.x);
        menu.addItem (contextEdit, sig != nullptr ? "Edit Time Signature..." : "Add Time Signature Here...", editable);
        if (sig != nullptr)
            menu.addItem (contextDelete, "Delete Time Signature", editable && sig->bar > 0);
    }
    else if (lane == laneTempo)
    {
        if (const auto* point = tempoPointAt (p.x))
        {
            menu.addItem (contextEdit, "Set Tempo...", editable);
            menu.addItem (contextDelete, "Delete Tempo Change", editable && point->pos > 0);
        }
        else
            menu.addItem (contextAdd, "Add Tempo Change Here", editable);
    }
    else if (const auto* marker = markerAt (p.x))
    {
        menu.addItem (contextRename, "Rename Marker...", editable);
        auto colours = colourMenu (contextColour, marker->color && marker->color->rgb.empty() ? marker->color->palette : -1);
        colours.addSeparator();
        colours.addItem (contextColour, "No Color", true, ! marker->color.has_value());
        menu.addSubMenu ("Color", colours, editable);
        menu.addSeparator();
        menu.addItem (contextDelete, "Delete Marker", editable);
    }
    else
        menu.addItem (contextAdd, "Add Marker Here", editable);
    return menu;
}

void Ruler::pickContext (int item, juce::Point<float> p)
{
    const auto lane = p.y >= rulerHeight ? laneAt (p.y) : std::nullopt;
    if (item <= 0 || ! lane || studio.laneProtected (*lane))
        return;
    auto& s = studio.session();
    const auto at = std::max<Ticks> (0, studio.snapped (static_cast<Ticks> (studio.view.xToTick (p.x))));
    if (lane == laneArrangement)
    {
        const auto* section = sectionAt (p.x);
        if (section == nullptr)
        {
            if (item == contextAdd)
            {
                // From the bar here to the next section, at most 8 bars.
                const auto& sig = studio.model().signatures;
                const auto start = sig.barStart (sig.barAt (at).bar);
                auto end = start + 8 * sig.barLength (sig.barAt (start).bar);
                for (const auto* other : studio.model().sectionsInOrder())
                    if (other->start > start)
                        end = std::min (end, other->start);
                if (s.addSection (start, end - start))
                    studio.edited();
                else
                    studio.setStatus (juce::String (s.lastError()));
            }
            return;
        }
        const auto id = section->id;
        if (item == contextDelete)
            studio.check (s.deleteSection (id));
        else if (item == contextRename)
            promptForText ("Rename section", juce::String (section->name), [this, id] (juce::String name) {
                if (name.trim().isNotEmpty())
                    studio.check (studio.session().renameSection (id, name.trim().toStdString()));
            });
        else if (item == contextColour)
            studio.check (s.setSectionColor (id, std::nullopt));
        else if (item > contextColour && item <= contextColour + paletteSize)
            studio.check (s.setSectionColor (id, model::Color { item - contextColour - 1, {} }));
        return;
    }
    if (lane == laneKey)
    {
        const auto* change = keyChangeAt (p.x);
        if (item == contextDelete && change != nullptr)
            studio.check (s.deleteKeyChange (change->id));
        else if (item == contextEdit)
        {
            const auto id = change != nullptr ? change->id : Id();
            const auto pos = change != nullptr ? change->pos : std::max<Ticks> (1, at);
            const auto now = studio.model().keyAt (pos);
            promptForText ("Key at bar " + juce::String (studio.model().signatures.barAt (pos).bar + 1), juce::String (now.name()),
                           [this, id, pos] (juce::String text) {
                               const auto key = model::KeySignature::parse (text.toStdString());
                               if (! key)
                               {
                                   studio.setStatus ("Type a key like C, F# minor or Bbm.");
                                   return;
                               }
                               if (id.empty())
                               {
                                   if (studio.session().addKeyChange (pos, *key))
                                       studio.edited();
                                   else
                                       studio.setStatus (juce::String (studio.session().lastError()));
                               }
                               else
                                   studio.check (studio.session().setKeyChange (id, pos, *key));
                           });
        }
        return;
    }
    if (lane == laneSignature)
    {
        const auto* sig = signatureAt (p.x);
        if (item == contextDelete && sig != nullptr)
            studio.check (s.deleteSignaturePoint (sig->id));
        else if (item == contextEdit)
        {
            const auto bar = sig != nullptr ? sig->bar : nearestBar (p.x);
            const auto& now = studio.model().signatures.signatureAtBar (bar);
            promptForText ("Time signature at bar " + juce::String (bar + 1), juce::String (now.numerator) + "/" + juce::String (now.denominator),
                           [this, bar] (juce::String text) { setSignatureFromText (bar, text); });
        }
        return;
    }
    if (lane == laneTempo)
    {
        const auto* point = tempoPointAt (p.x);
        if (item == contextAdd && point == nullptr)
        {
            if (s.addTempoPoint (at, bpmAtY (p.y)))
                studio.edited();
            else
                studio.setStatus (juce::String (s.lastError()));
        }
        else if (item == contextDelete && point != nullptr)
            studio.check (s.deleteTempoPoint (point->id));
        else if (item == contextEdit && point != nullptr)
        {
            const auto id = point->id;
            const auto pos = point->pos;
            promptForText ("Tempo (BPM)", bpmText (point->bpm), [this, id, pos] (juce::String text) {
                studio.check (studio.session().setTempoPoint (id, pos, std::round (text.getDoubleValue() * 100.0) / 100.0));
            });
        }
        return;
    }
    const auto* marker = markerAt (p.x);
    if (marker == nullptr)
    {
        if (item == contextAdd)
        {
            s.addMarker (at, "");
            studio.edited();
        }
        return;
    }
    const auto id = marker->id;
    if (item == contextDelete)
        studio.check (s.deleteMarker (id));
    else if (item == contextRename)
        renameMarker (id);
    else if (item == contextColour)
        studio.check (s.setMarkerColor (id, std::nullopt));
    else if (item > contextColour && item <= contextColour + paletteSize)
        studio.check (s.setMarkerColor (id, model::Color { item - contextColour - 1, {} }));
}

void Ruler::renameMarker (const Id& id)
{
    const auto& markers = studio.model().markers;
    const auto it = markers.find (id);
    if (it == markers.end())
        return;
    promptForText ("Rename marker", juce::String (it->second->name), [this, id] (juce::String name) {
        if (name.isNotEmpty())
            studio.check (studio.session().renameMarker (id, name.toStdString()));
    });
}

void Ruler::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Double-click is the quick way to the menu's first entry: edit what's there, or add a marker.
    const auto p = e.position;
    const auto lane = p.y >= rulerHeight ? laneAt (p.y) : std::nullopt;
    if (! lane)
        return;
    if (lane == laneSignature || lane == laneTempo || lane == laneKey)
        pickContext (contextEdit, p);
    else if (lane == laneArrangement)
        pickContext (sectionAt (p.x) != nullptr ? contextRename : contextAdd, p);
    else
        pickContext (markerAt (p.x) != nullptr ? contextRename : contextAdd, p);
}

void Ruler::createSignatureChange()
{
    // At the bar after the playhead's (or bar 2 at the start), with the meter in effect there.
    const auto& sig = studio.model().signatures;
    const auto bar = sig.barAt (studio.playhead()).bar + (sig.barAt (studio.playhead()).offset > 0 || studio.playhead() == 0 ? 1 : 0);
    const auto& now = sig.signatureAtBar (bar);
    promptForText ("Time signature at bar " + juce::String (bar + 1), juce::String (now.numerator) + "/" + juce::String (now.denominator),
                   [this, bar] (juce::String text) { setSignatureFromText (bar, text); });
}

void Ruler::createKeyChange()
{
    const auto pos = std::max<Ticks> (1, studio.playhead());
    const auto now = studio.model().keyAt (pos);
    promptForText ("Key from here", juce::String (now.name()), [this, pos] (juce::String text) {
        const auto key = model::KeySignature::parse (text.toStdString());
        if (! key)
        {
            studio.setStatus ("Type a key like C, F# minor or Bbm.");
            return;
        }
        if (studio.session().addKeyChange (pos, *key))
            studio.edited();
        else
            studio.setStatus (juce::String (studio.session().lastError()));
    });
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
    describe (mute, "Mute", "Mute track (Ctrl+click: only this; Alt+click: all; drag across tracks to set them too)");
    describe (solo, "Solo", "Solo track (Ctrl+click: only this; Alt+click: all; drag across tracks to set them too)");
    describe (arm, "Record enable", "Record enable (Ctrl+click: only this; Alt+click: all)");
    for (auto [b, which] : { std::pair { &mute, TrackSwitch::mute }, std::pair { &solo, TrackSwitch::solo }, std::pair { &arm, TrackSwitch::arm } })
    {
        b->onClick = [this, which] { studio.clickTrackSwitch (which, trackId, juce::ModifierKeys::currentModifiers); };
        b->setTrackState = [this, which] (bool on) { studio.setTrackSwitch (which, trackId, on); };
        b->onSwipeStart = [this, which] { studio.beginSwitchSwipe (which); };
        b->onSwipeEnd = [this] { studio.endSwitchSwipe(); };
    }
    setRole (mute, ButtonRole::mute);
    setRole (solo, ButtonRole::solo);
    setRole (arm, ButtonRole::record);
    // The optional buttons (D-UI-37): the same click, Ctrl/Alt-click and swipe as Mute.
    describe (onOff, "On/Off", "Turn the track on or off (Ctrl+click: only this one on; Alt+click: all; drag across tracks to set them too)");
    describe (monitor, "Input monitoring", "Input monitoring on, or back to auto (Alt+click: all; drag across tracks to set them too)");
    describe (hide, "Hide", "Hide the track (View > Show Hidden Tracks brings it back)");
    for (auto [b, which] : { std::pair { &onOff, TrackSwitch::on }, std::pair { &monitor, TrackSwitch::monitor } })
    {
        b->setClickingTogglesState (false);
        b->onClick = [this, which] { studio.clickTrackSwitch (which, trackId, juce::ModifierKeys::currentModifiers); };
        b->setTrackState = [this, which] (bool on) { studio.setTrackSwitch (which, trackId, on); };
        b->onSwipeStart = [this, which] { studio.beginSwitchSwipe (which); };
        b->onSwipeEnd = [this] { studio.endSwitchSwipe(); };
    }
    hide.onClick = [this] { pickMenu (itemHide); };
    for (juce::Button* b : { static_cast<juce::Button*> (&onOff), static_cast<juce::Button*> (&monitor), static_cast<juce::Button*> (&hide) })
    {
        b->setWantsKeyboardFocus (true);
        b->setMouseClickGrabsKeyboardFocus (false);
        b->setHasFocusOutline (true);
        addChildComponent (b);
    }
    for (auto* b : { &mute, &solo, &arm })
    {
        b->setWantsKeyboardFocus (true);
        b->setMouseClickGrabsKeyboardFocus (false);
        b->setHasFocusOutline (true);
        addAndMakeVisible (*b);
    }
    // Volume and pan (L-TRH-3): the mixer's fader law and gestures, so one drag is one undo
    // step, and a drag while automation plays touches it like the mixer does (W-8).
    const auto target = [] (const char* kind) {
        model::AutomationTarget t;
        t.kind = kind;
        return t;
    };
    volume.setSliderStyle (juce::Slider::LinearBar);
    volume.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    volume.setNormalisableRange (fader::range());
    volume.setDoubleClickReturnValue (true, 0.0, juce::ModifierKeys::altModifier);
    volume.setVelocityModeParameters (0.2, 1, 0.0, true, juce::ModifierKeys::shiftModifier);
    volume.setPopupDisplayEnabled (true, true, nullptr);
    volume.textFromValueFunction = [] (double v) { return fader::dbText (v <= fader::minDb + 1e-6 ? fader::silentDb : v) + " dB"; };
    describe (volume, "Volume", "Volume (double-click: 0 dB, Shift-drag: fine)");
    volume.onDragStart = [this, target] {
        liveVolume = studio.automationLive (trackId, target ("volume"));
        if (! liveVolume)
            studio.session().beginGesture ("Volume");
    };
    volume.onDragEnd = [this, target] {
        if (liveVolume)
            studio.releaseControl (trackId, target ("volume"));
        else
        {
            studio.session().endGesture();
            studio.edited();
        }
        liveVolume = false;
    };
    volume.onValueChange = [this, target] {
        const auto v = volume.getValue();
        const auto db = v <= fader::minDb + 1e-6 ? fader::silentDb : v;
        if (liveVolume)
            return studio.touchControl (trackId, target ("volume"), automation::fromDb (db));
        studio.check (studio.session().setTrackVolume (trackId, db));
    };
    pan.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    pan.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    pan.setRange (-100.0, 100.0, 0.0);
    pan.setDoubleClickReturnValue (true, 0.0, juce::ModifierKeys::altModifier);
    pan.setVelocityModeParameters (0.2, 1, 0.0, true, juce::ModifierKeys::shiftModifier);
    pan.setLookAndFeel (&knobs.get());
    pan.setPopupDisplayEnabled (true, true, nullptr);
    pan.textFromValueFunction = [] (double v) {
        const auto amount = juce::roundToInt (v);
        return amount == 0 ? juce::String ("C") : juce::String (std::abs (amount)) + (amount < 0 ? "L" : "R");
    };
    describe (pan, "Pan", "Pan (double-click: centre)");
    pan.onDragStart = [this, target] {
        livePan = studio.automationLive (trackId, target ("pan"));
        if (! livePan)
            studio.session().beginGesture ("Pan");
    };
    pan.onDragEnd = [this, target] {
        if (livePan)
            studio.releaseControl (trackId, target ("pan"));
        else
        {
            studio.session().endGesture();
            studio.edited();
        }
        livePan = false;
    };
    pan.onValueChange = [this, target] {
        if (livePan)
            return studio.touchControl (trackId, target ("pan"), automation::fromPan (pan.getValue() / 100.0));
        studio.check (studio.session().setTrackPan (trackId, pan.getValue() / 100.0));
    };
    for (auto* sl : { &volume, &pan })
    {
        makeFocusable (*sl);
        addChildComponent (sl);
    }

    mode.addItemList ({ "Off", "Read", "Touch", "Latch", "Write" }, 1);
    describe (mode, "Automation mode");
    makeFocusable (mode);
    mode.onChange = [this] {
        static const char* const modes[] = { "off", "read", "touch", "latch", "write" };
        const auto i = mode.getSelectedItemIndex();
        if (i >= 0 && i < 5)
        {
            // Alt while choosing: every track takes the mode (D-W8-7).
            if (juce::ModifierKeys::currentModifiers.isAltDown())
                studio.setAllAutomationModes (modes[i]);
            else
                studio.check (studio.session().setTrackAutomationMode (trackId, modes[i]));
        }
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
    if (t != nullptr && (t->kind == model::TrackKind::audio || t->kind == model::TrackKind::instrument))
        m.addItem (itemTrackOff, t->off ? "Turn Track On" : "Turn Track Off", editable);
    if (t != nullptr && ! master)
        m.addItem (itemHide, t->hidden ? "Unhide Track" : "Hide Track", editable);
    m.addSeparator();
    m.addItem (itemLock, lockText.isEmpty() ? "Lock Track" : "Unlock Track", ! other && ! studio.previewing());
    if (other && studio.session().isOwner() && ! studio.previewing())
        m.addItem (itemBreakLock, "Break Lock (" + lockText + ")...");
    m.addSeparator();
    m.addItem (itemComment, "Add Comment", ! studio.previewing());
    m.addSeparator();
    m.addSubMenu ("Configure Track Header", studio.headerOptionsMenu (itemHeaderOption));
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
        case itemTrackOff:
            if (const auto* t = studio.model().track (trackId))
                studio.check (studio.session().setTrackOff (trackId, ! t->off));
            break;
        case itemHide:
            if (const auto* t = studio.model().track (trackId))
            {
                const bool hiding = ! t->hidden;
                studio.check (studio.session().setTrackHidden (trackId, hiding));
                if (hiding && ! studio.model().info.showHiddenTracks)
                    studio.setStatus ("Track hidden. View > Show Hidden Tracks brings it back.");
            }
            break;
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
            if (item > itemHeaderOption)
                studio.toggleHeaderOption (item - itemHeaderOption);
            else if (item > itemColour && item <= itemColour + paletteSize)
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
        removeLane.clear();
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
            auto close = std::make_unique<juce::TextButton> (juce::String (juce::CharPointer_UTF8 ("\xc3\x97")));
            describe (*close, "Close automation lane", "Close this lane; its automation stays");
            makeFocusable (*close);
            close->onClick = [this, i] { studio.removeShownLane (trackId, i); };
            addAndMakeVisible (*close);
            removeLane.push_back (std::move (close));
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
    setSilencedBySolo (mute, studio.silencedBySolo (trackId));
    solo.setToggleState (studio.localView().soloed.contains (trackId), juce::dontSendNotification);
    arm.setToggleState (studio.localView().armed.contains (trackId), juce::dontSendNotification);
    arm.setVisible (commands::LocalView::canArm (*t));
    const auto& options = studio.header;
    const bool switchable = t->kind == model::TrackKind::audio || t->kind == model::TrackKind::instrument;
    onOff.setVisible (options.onOff && switchable);
    onOff.setToggleState (! t->off, juce::dontSendNotification);
    monitor.setVisible (options.monitor && t->kind == model::TrackKind::audio);
    monitor.setToggleState (t->monitor == "on", juce::dontSendNotification);
    hide.setVisible (options.hide && t->kind != model::TrackKind::master);
    hide.setToggleState (t->hidden, juce::dontSendNotification);
    if (! volume.isMouseButtonDown())
        volume.setValue (std::clamp (t->strip.volumeDb, fader::minDb, fader::maxDb), juce::dontSendNotification);
    if (! pan.isMouseButtonDown())
        pan.setValue (t->strip.pan * 100.0, juce::dontSendNotification);
    rebuildAutomation();

    // A lock (04 §6.1, §9.6): the icon shows; someone else's lock disables the shared controls.
    const auto lockTip = studio.lockTooltip (trackId);
    const bool locked = studio.lockedByOther (trackId);
    lock.setVisible (lockTip.isNotEmpty());
    const auto lockColour = locked ? colour (Token::warning) : colour (Token::textDim);
    lock.setColours (lockColour, lockColour.brighter (0.3f), lockColour.darker (0.2f));
    lock.setTooltip (lockTip.isEmpty() ? juce::String() : lockTip + (locked ? juce::String() : juce::String (" (click to unlock)")));
    name.setEditable (false, ! locked, true);
    std::vector<juce::Component*> shared { &name, &mute, &mode, &volume, &pan, &onOff, &monitor, &hide };
    for (auto& p : parameters)
        shared.push_back (p.get());
    for (auto& b : addLane)
        shared.push_back (b.get());
    for (auto& b : removeLane)
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
    const auto& options = studio.header;
    if (t != nullptr && options.colorBars)
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
    const auto row = juce::Rectangle<int> (0, name.getY(), 0, size::control);
    if (options.numbers)
        g.drawText (juce::String (number), space::s1, row.getY(), space::s3 - space::half, size::control, juce::Justification::centredRight);
    if (t != nullptr && options.icons)
    {
        // Track Icons (D-UI-37): the kind's glyph, left of the name.
        const auto box = juce::Rectangle<float> (static_cast<float> (nameLeft() - size::icon - space::half), static_cast<float> (row.getY() + (size::control - size::icon) / 2),
                                                 static_cast<float> (size::icon), static_cast<float> (size::icon));
        const auto glyph = icon::track (static_cast<int> (t->kind));
        g.setColour (colourFor (t->color.palette, t->color.rgb));
        g.fillPath (glyph, glyph.getTransformToScaleToFit (box.reduced (1.0f), true));
    }
    if (const auto box = disclosureArea(); ! box.isEmpty())
    {
        // Open: pointing down; closed: pointing right.
        const auto c = box.toFloat().getCentre();
        juce::Path tri;
        if (t->stackOpen)
            tri.addTriangle (c.x - 4.0f, c.y - 2.0f, c.x + 4.0f, c.y - 2.0f, c.x, c.y + 3.0f);
        else
            tri.addTriangle (c.x - 2.0f, c.y - 4.0f, c.x - 2.0f, c.y + 4.0f, c.x + 3.0f, c.y);
        g.setColour (colour (Token::text));
        g.fillPath (tri);
    }
    if (t != nullptr && trackPart.getHeight() >= 48.0f && ! volume.isVisible() && ! pan.isVisible())
    {
        g.setColour (colour (Token::textDim));
        g.setFont (font (Type::body));
        const auto kind = t->isFolderStack() ? "Folder Stack" : t->isStackMain() ? "Summing Stack" : t->kind == model::TrackKind::bus ? "Bus" : t->kind == model::TrackKind::master ? "Master" : t->kind == model::TrackKind::instrument ? "Instrument" : "Audio";
        g.drawText (kind, nameLeft(), space::s1 + size::control, 120, 16, juce::Justification::centredLeft);
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
    if (t != nullptr && t->off)
    {
        // An off track (L-TRH-7) reads as dimmed, with "Off" where its kind would be.
        g.setColour (colour (Token::surface).withAlpha (0.55f));
        g.fillRect (trackPart);
        g.setColour (colour (Token::textDim));
        g.setFont (font (Type::caption));
        g.drawText ("OFF", getWidth() - space::s1 - 40, static_cast<int> (trackPart.getBottom()) - 16, 40, 14, juce::Justification::centredRight);
    }
    if (t != nullptr && t->hidden)
    {
        // Shown only while Show Hidden Tracks is on (L-TRH-10): marked so it's clear it'll go.
        g.setColour (colour (Token::textDim));
        g.setFont (font (Type::caption));
        g.drawText ("HIDDEN", getWidth() - space::s1 - 56, static_cast<int> (trackPart.getY()) + 2, 56, 14, juce::Justification::centredRight);
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

int TrackHeader::nameLeft() const
{
    // Number, then the icon, then the name (D-UI-37); with neither, the name keeps clear of the colour bar.
    const auto& options = studio.header;
    int left = options.numbers ? space::s4 : space::s1 + space::half;
    if (options.icons)
        left += size::icon + space::half;
    // Track stacks (D-W6-17): members sit indented under their main; the main has its disclosure triangle.
    if (studio.model().stackMain (trackId) != nullptr)
        left += space::s2;
    if (const auto* t = studio.model().track (trackId); t != nullptr && t->isStackMain())
        left += size::icon;
    return left;
}

juce::Rectangle<int> TrackHeader::disclosureArea() const
{
    const auto* t = studio.model().track (trackId);
    if (t == nullptr || ! t->isStackMain())
        return {};
    return { nameLeft() - size::icon - (studio.header.icons ? size::icon + space::half : 0), name.getY(), size::icon, size::control };
}

void TrackHeader::resized()
{
    // Colour edge and number on the left; name, then M S R on the top line (8 px grid).
    const auto trackPart = getLocalBounds().withHeight (studio.trackHeight (trackId));
    auto r = trackPart.withTrimmedLeft (nameLeft()).withTrimmedRight (space::s1);
    auto row = r.withTrimmedTop (std::min (space::s1, std::max (0, (trackPart.getHeight() - size::control) / 2))).withHeight (size::control);
    if (lock.isVisible())
    {
        lock.setBounds (row.removeFromRight (size::icon).withSizeKeepingCentre (size::icon, size::icon));
        row.removeFromRight (space::half);
    }
    // Right to left: Hide, I, R, S, M, On (D-UI-37); a button that doesn't apply leaves no gap.
    for (juce::Button* b : { static_cast<juce::Button*> (&hide), static_cast<juce::Button*> (&monitor), static_cast<juce::Button*> (&arm),
                             static_cast<juce::Button*> (&solo), static_cast<juce::Button*> (&mute), static_cast<juce::Button*> (&onOff) })
    {
        if (! b->isVisible())
            continue;
        b->setBounds (row.removeFromRight (b == &onOff ? size::control + space::half : size::control));
        row.removeFromRight (space::half);
    }
    name.setBounds (row.withTrimmedRight (space::half));
    // Second line: volume, then pan; collaborator badges keep the right end (painted).
    const auto second = juce::Rectangle<int> (r.getX(), space::s1 + size::control + 2, r.getWidth(), 16);
    const bool roomy = trackPart.getHeight() >= 48;
    volume.setVisible (roomy && studio.header.volume);
    pan.setVisible (roomy && studio.header.pan);
    if (roomy)
    {
        auto line = second.withTrimmedRight (static_cast<int> (badges().size()) * 20);
        if (pan.isVisible())
        {
            pan.setBounds (line.removeFromRight (18).withSizeKeepingCentre (18, 18));
            line.removeFromRight (space::half);
        }
        volume.setBounds (line.withSizeKeepingCentre (line.getWidth(), 12));
    }
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
        removeLane[i]->setBounds (top.removeFromRight (size::control));
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
    if (disclosureArea().expanded (2).contains (e.getPosition()))
    {
        // Alt+click opens or closes every stack (M-MIX-31).
        studio.toggleStack (trackId, e.mods.isAltDown());
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
        // Ctrl+drag sets every track's height (D-UI-38).
        if (e.mods.isCommandDown())
            studio.setAllTrackHeights (resizeFrom + e.getDistanceFromDragStartY());
        else
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

int Timeline::rows() const { return static_cast<int> (studio.model().rows.size()); }

juce::Rectangle<float> Timeline::rowBounds (int row) const
{
    float y = static_cast<float> (-scrollY);
    const auto& order = studio.model().rows;
    for (int i = 0; i < row && i < static_cast<int> (order.size()); ++i)
        y += static_cast<float> (studio.rowHeight (order[static_cast<std::size_t> (i)]));
    const auto h = row < static_cast<int> (order.size()) ? studio.rowHeight (order[static_cast<std::size_t> (row)]) : defaultTrackHeight;
    if (row >= static_cast<int> (order.size()))
        y += static_cast<float> ((row - static_cast<int> (order.size())) * defaultTrackHeight);
    return { 0.0f, y, static_cast<float> (getWidth()), static_cast<float> (h) };
}

juce::Rectangle<float> Timeline::laneBounds (int row) const
{
    const auto& order = studio.model().rows;
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
    const auto trackId = studio.model().rows[static_cast<std::size_t> (row)];
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
    for (const auto* r : m.regionsOnTrack (m.rows[static_cast<std::size_t> (row)]))
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
        if (m.rows[static_cast<std::size_t> (row)] == studio.selectedTrack())
        {
            g.setColour (colour (Token::laneAlt));
            g.fillRect (lane);
        }
    }
    if (studio.gridLines) // View › Grid Lines (D-W3-11)
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
            const auto trackId = m.rows[static_cast<std::size_t> (row)];
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
    if (drag == Drag::rubberBand || drag == Drag::zoomBox)
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
    const bool editable = ! studio.previewing();
    const auto add = [&] (Cmd c, bool enabled) {
        const auto& info = commandInfo (c);
        juce::PopupMenu::Item item (info.name);
        item.itemID = contextCommand + static_cast<int> (c) + 1;
        item.shortcutKeyDescription = info.shortcut;
        item.isEnabled = enabled;
        menu.addItem (item);
    };
    if (r != nullptr)
    {
        add (Cmd::cut, editable);
        add (Cmd::copy, true);
        add (Cmd::paste, editable);
        add (Cmd::deleteSelection, editable);
        menu.addSeparator();
        menu.addItem (contextSplitHere, "Split Here", editable);
        for (auto c : { Cmd::join, Cmd::muteRegions, Cmd::repeat, Cmd::loopRegion, Cmd::crossfade })
            add (c, editable);
        if (r->isMidi)
            add (Cmd::applyQuantize, editable);
        else
            add (Cmd::reverseRegions, editable);
        add (Cmd::exportRegions, true);
        menu.addSeparator();
        auto colours = colourMenu (contextTrackColour, r->color && r->color->rgb.empty() ? r->color->palette : -1);
        colours.addSeparator();
        colours.addItem (contextTrackColour, "Track Color", true, ! r->color.has_value());
        menu.addSubMenu ("Color", colours, editable);
    }
    else
    {
        // D-W3-9: an empty lane offers what its track holds, a one-bar MIDI region or an audio file, where it was clicked.
        const auto row = rowAt (p.y);
        const auto* t = row >= 0 && row < rows() ? studio.model().track (studio.model().rows[static_cast<std::size_t> (row)]) : nullptr;
        if (t != nullptr && t->kind == model::TrackKind::instrument)
            menu.addItem (contextCreateMidi, "Create MIDI Region", editable);
        if (t != nullptr && t->kind == model::TrackKind::audio)
            add (Cmd::importAudio, editable);
        add (Cmd::paste, editable);
        menu.addSeparator();
        menu.addItem (contextSetEnd, "Set Project End Here", editable);
        if (studio.model().info.endMarker)
            menu.addItem (contextClearEnd, "Clear Project End", editable);
    }
    menu.addSeparator();
    menu.addItem (contextComment, r != nullptr ? "Add Comment on \"" + juce::String (r->name) + "\"" : juce::String ("Add Comment"), editable);
    return menu;
}

void Timeline::createMidiRegionAt (const Id& trackId, float x)
{
    const auto& m = studio.model();
    const auto start = std::max<Ticks> (0, studio.snapped (tickAt (x)));
    const auto id = studio.session().createMidiRegion (trackId, start, m.signatures.barLength (m.signatures.barAt (start).bar));
    if (id)
        studio.selection.click (*id, false);
    else
        studio.setStatus (juce::String (studio.session().lastError()));
    studio.edited();
}

void Timeline::pickContext (int item, juce::Point<float> p)
{
    const auto at = std::max<Ticks> (0, studio.snapped (tickAt (p.x)));
    if (item > contextCommand)
    {
        if (const auto c = static_cast<Cmd> (item - contextCommand - 1); c == Cmd::paste || c == Cmd::importAudio)
            studio.locate (at); // paste or import where the menu was opened
        studio.perform (static_cast<Cmd> (item - contextCommand - 1));
        return;
    }
    if (item >= contextTrackColour && item <= contextTrackColour + paletteSize)
    {
        // One undo step for the whole selection.
        std::optional<model::Color> c;
        if (item > contextTrackColour)
            c = model::Color { item - contextTrackColour - 1, {} };
        studio.session().beginGesture ("Region Color");
        for (const auto& id : studio.selection.ids())
            studio.session().setRegionColor (id, c);
        studio.session().endGesture();
        studio.edited();
        return;
    }
    switch (item)
    {
        case contextSplitHere:
            if (studio.session().splitRegionsAt (studio.selection.ids(), at).empty())
                studio.setStatus (juce::String (studio.session().lastError()));
            studio.edited();
            return;
        case contextSetEnd: studio.check (studio.session().setEndMarker (at)); return;
        case contextClearEnd: studio.check (studio.session().setEndMarker (std::nullopt)); return;
        case contextCreateMidi:
            if (const auto row = rowAt (p.y); row >= 0 && row < rows())
                createMidiRegionAt (studio.model().rows[static_cast<std::size_t> (row)], p.x);
            return;
        case contextComment: break;
        default: return;
    }
    const auto& m = studio.model();
    commands::CommentAnchor a;
    a.pos = std::max<Ticks> (0, studio.snapped (tickAt (p.x)));
    const auto row = rowAt (p.y);
    if (row >= 0 && row < rows())
        a.trackId = m.rows[static_cast<std::size_t> (row)];
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

void Timeline::resized()
{
    studio.timelineWidth = getWidth();
    studio.timelineHeight = getHeight();
}

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
    dragLock = Lock::none;
    auto& s = studio.session();
    const auto& m = studio.model();
    if (const auto* c = commentPinAt (p); c != nullptr && ! mods.isPopupMenu())
    {
        studio.showThread (c->id, localAreaToGlobal (pinBounds (*c)->toNearestInt()));
        return;
    }
    const auto* r = regionAt (p);
    const auto row = rowAt (p.y);
    const auto trackId = row >= 0 && row < rows() ? m.rows[static_cast<std::size_t> (row)] : Id {};
    if (! trackId.empty())
        studio.selectTrack (trackId);
    if (mods.isPopupMenu())
    {
        if (r != nullptr && ! studio.selection.contains (r->id))
        {
            studio.selection.click (r->id, false);
            studio.sendChangeMessage();
        }
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
                createMidiRegionAt (trackId, p.x);
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
        case Tool::mute:
            if (r != nullptr)
            {
                // D-W3-8: clicked on one of several selected regions, the tool acts on all of them.
                const auto targets = studio.selection.contains (r->id) ? studio.selection.ids() : std::vector<Id> { r->id };
                studio.check (studio.tool == Tool::eraser ? s.deleteRegions (targets) : s.toggleRegionsMuted (targets));
            }
            return;
        case Tool::text:
            if (r != nullptr)
                renameRegion (r->id);
            return;
        case Tool::zoom:
            // D-UI-36: drag a box to fit it; click zooms in around the pointer, Alt+click out.
            if (mods.isAltDown())
            {
                studio.view.zoomAround (0.5, p.x);
                studio.sendChangeMessage();
                return;
            }
            drag = Drag::zoomBox;
            return;
        case Tool::fade:
            if (r != nullptr && ! r->isMidi)
            {
                // The half of the region clicked: its fade in or fade out, set by the drag.
                if (! studio.selection.contains (r->id))
                    studio.selection.click (r->id, false);
                startRegionDrag (*r, p.x < regionBounds (*r).getCentreX() ? Zone::fadeIn : Zone::fadeOut, p, mods);
                pointerDrag (p, mods);
                studio.sendChangeMessage();
            }
            return;
        case Tool::gain:
            if (r != nullptr && ! r->isMidi)
            {
                // Dragging up or down changes the gain of the region, or of every selected audio region when it's one of them.
                const auto targets = studio.selection.contains (r->id) ? studio.selection.ids() : std::vector<Id> { r->id };
                gainFrom.clear();
                for (const auto& id : targets)
                    if (const auto* t = m.region (id); t != nullptr && ! t->isMidi)
                        gainFrom[id] = t->audio.gainDb;
                dragRegion = r->id;
                drag = Drag::gain;
                s.beginGesture ("Region Gain");
                gestureOpen = true;
            }
            return;
        case Tool::quantize:
            if (r != nullptr && r->isMidi)
            {
                const auto targets = studio.selection.contains (r->id) ? studio.selection.ids() : std::vector<Id> { r->id };
                std::vector<Id> midi;
                for (const auto& id : targets)
                    if (const auto* t = m.region (id); t != nullptr && t->isMidi)
                        midi.push_back (id);
                studio.check (s.quantizeNotes (midi, {}, studio.piano.quantizeGrid, studio.piano.strength / 100.0, studio.piano.swing));
            }
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
    {
        studio.selection.click (r->id, false);
        studio.linkSelection();
    }
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
            auto rawDelta = tickAt (p.x) - tickAt (downAt.x);
            auto rowDelta = rowAt (p.y) - rowAt (downAt.y);
            // D-W3-10: with Shift the drag keeps to the direction it set off in, along the track or across tracks.
            if (! mods.isShiftDown())
                dragLock = Lock::none;
            else if (dragLock == Lock::none && p.getDistanceFrom (downAt) >= 4.0f)
                dragLock = std::abs (p.x - downAt.x) >= std::abs (p.y - downAt.y) ? Lock::time : Lock::rows;
            if (dragLock == Lock::time)
                rowDelta = 0;
            else if (dragLock == Lock::rows)
                rawDelta = 0;
            regionDrag->update (rawDelta, rowDelta, mods.isAltDown(), mods.isCommandDown());
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
        case Drag::zoomBox: break;
        case Drag::gain:
        {
            // 0.25 dB a pixel, up is louder; -60 dB and below is silence, +24 dB at most.
            const auto delta = static_cast<double> (downAt.y - p.y) * 0.25;
            for (const auto& [id, from] : gainFrom)
            {
                auto to = std::min (24.0, from + delta);
                if (to <= -60.0)
                    to = -200.0;
                s.setRegionGain (id, to);
            }
            break;
        }
    }
    if (drag != Drag::move && drag != Drag::rubberBand && drag != Drag::none && drag != Drag::zoomBox)
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
    if (drag == Drag::zoomBox)
    {
        const auto band = juce::Rectangle<float> (downAt, p);
        if (band.getWidth() >= 4.0f)
            studio.view.fit (studio.view.xToTick (band.getX()), studio.view.xToTick (band.getRight()), getWidth(), 1.0);
        else
            studio.view.zoomAround (2.0, p.x);
        studio.sendChangeMessage();
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

void Timeline::renameRegion (const Id& regionId)
{
    const auto* r = studio.model().region (regionId);
    if (r == nullptr)
        return;
    juce::Component::SafePointer<Timeline> self (this);
    auto done = [self, regionId] (juce::String name) {
        if (self != nullptr && name.trim().isNotEmpty())
            self->studio.check (self->studio.session().setRegionName (regionId, name.trim().toStdString()));
    };
    if (promptForText)
        promptForText ("Rename region", juce::String (r->name), std::move (done));
    else
        askForText ("Rename region", juce::String (r->name), std::move (done));
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
        const auto h = studio.model().rows.empty() ? defaultTrackHeight : studio.trackHeight (studio.model().rows.front());
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
        target.trackId = studio.model().rows[static_cast<std::size_t> (row)];
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
                                                       studio.model().rows[static_cast<std::size_t> (row)], -1 });
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

juce::PopupMenu TracksArea::headerAreaMenu() const
{
    juce::PopupMenu menu;
    const auto add = [&menu] (Cmd c) {
        const auto& info = commandInfo (c);
        juce::PopupMenu::Item item (info.name);
        item.itemID = static_cast<int> (c) + 1;
        item.shortcutKeyDescription = info.shortcut;
        menu.addItem (item);
    };
    for (const auto c : { Cmd::newTrack, Cmd::newAudioTrack, Cmd::newInstrumentTrack })
        add (c);
    menu.addSeparator();
    add (Cmd::deleteUnusedTracks);
    menu.addSeparator();
    add (Cmd::toggleAllTracksOff);
    menu.addSubMenu ("Configure Track Header", studio.headerOptionsMenu (headerOptionBase));
    return menu;
}

void TracksArea::pickHeaderAreaMenu (int item)
{
    if (item > headerOptionBase)
        studio.toggleHeaderOption (item - headerOptionBase);
    else if (item > 0)
        studio.perform (static_cast<Cmd> (item - 1));
}

void TracksArea::showHeaderOptions()
{
    juce::Component::SafePointer<TracksArea> self (this);
    const auto at = headerColumn.getScreenBounds().withHeight (size::control).withWidth (headerWidth());
    studio.headerOptionsMenu (headerOptionBase)
        .showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (at), [self] (int item) {
            if (self != nullptr && item > headerOptionBase)
            {
                self->studio.toggleHeaderOption (item - headerOptionBase);
                self->showHeaderOptions(); // stays open while options are ticked, like a popover
            }
        });
}

TracksArea::TracksArea (Studio& s) : studio (s), rulerComponent (s), lanes (s)
{
    addAndMakeVisible (rulerComponent);
    addAndMakeVisible (lanes);
    addAndMakeVisible (headerColumn);
    headerColumn.setTitle ("Track headers");
    addAndMakeVisible (resizer);
    resizer.setTitle ("Track header width");
    resizer.setHelpText ("Drag to resize the track headers; double-click for the default width");
    // The headers take their own clicks, so this is the empty space under the last track (04 §6.1).
    headerColumn.onDoubleClick = [this] { studio.perform (Cmd::newTrack); };
    headerColumn.onPopupMenu = [this] {
        juce::Component::SafePointer<TracksArea> self (this);
        headerAreaMenu().showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [self] (int item) {
            if (self != nullptr)
                self->pickHeaderAreaMenu (item);
        });
    };
    addTrack.setMouseClickGrabsKeyboardFocus (false);
    // Alt+click adds a track like the selected one on the next input, without the dialog (D-UI-34).
    addTrack.onClick = [this] { studio.perform (juce::ModifierKeys::currentModifiers.isAltDown() ? Cmd::newTrackNextInput : Cmd::newTrack); };
    addTrack.setTooltip (addTrack.getTooltip() + "; Alt+click: " + commandInfo (Cmd::newTrackNextInput).name);
    addAndMakeVisible (addTrack);
    addAndMakeVisible (vertical);
    addAndMakeVisible (horizontal);
    vertical.addListener (this);
    horizontal.addListener (this);
    vertical.setTitle ("Vertical scroll");
    horizontal.setTitle ("Horizontal scroll");
    lanes.onScroll = [this] { resized(); };
    // Page Up/Down and the saved zooms reach the vertical scroll through these (D-UI-38).
    studio.verticalScroll = [this] { return lanes.scrollY; };
    studio.setVerticalScroll = [this] (int y) {
        lanes.scrollY = std::clamp (y, 0, std::max (0, lanes.contentHeight() + defaultTrackHeight - lanes.getHeight()));
        resized();
        repaint();
    };
    // The zoom sliders in the corner (D-UI-38): horizontal on a log scale, vertical as every track's height.
    for (auto* sl : { &horizontalZoom, &verticalZoom })
    {
        sl->setSliderStyle (juce::Slider::LinearHorizontal);
        sl->setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        sl->onDragStart = [this] { studio.rememberView(); };
        makeFocusable (*sl);
        addAndMakeVisible (*sl);
    }
    describe (horizontalZoom, "Horizontal zoom");
    describe (verticalZoom, "Vertical zoom", "Every track's height");
    horizontalZoom.setRange (0.0, 1.0);
    verticalZoom.setRange (minTrackHeight, maxTrackHeight, 1.0);
    horizontalZoom.onValueChange = [this] {
        studio.view.pixelsPerTick = pixelsPerTickFor (horizontalZoom.getValue());
        resized();
        repaint();
    };
    verticalZoom.onValueChange = [this] { studio.setAllTrackHeights (juce::roundToInt (verticalZoom.getValue())); };
    studio.addChangeListener (this);
    rebuildHeaders();
    startTimerHz (30);
}

double TracksArea::pixelsPerTickFor (double position)
{
    const auto lo = std::log (arrange::TimelineView::minPixelsPerTick), hi = std::log (arrange::TimelineView::maxPixelsPerTick);
    return std::exp (lo + std::clamp (position, 0.0, 1.0) * (hi - lo));
}

double TracksArea::positionFor (double pixelsPerTick)
{
    const auto lo = std::log (arrange::TimelineView::minPixelsPerTick), hi = std::log (arrange::TimelineView::maxPixelsPerTick);
    return std::clamp ((std::log (pixelsPerTick) - lo) / (hi - lo), 0.0, 1.0);
}

void TracksArea::syncZoomSliders()
{
    if (! horizontalZoom.isMouseButtonDown())
        horizontalZoom.setValue (positionFor (studio.view.pixelsPerTick), juce::dontSendNotification);
    if (! verticalZoom.isMouseButtonDown() && ! studio.model().trackOrder.empty())
        verticalZoom.setValue (studio.trackHeight (studio.model().trackOrder.front()), juce::dontSendNotification);
}

TracksArea::~TracksArea()
{
    studio.removeChangeListener (this);
    studio.verticalScroll = nullptr;
    studio.setVerticalScroll = nullptr;
}

void TracksArea::rebuildHeaders()
{
    const auto& order = studio.model().rows;
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
        trackHeaders[i]->setBounds (lanes.rowBounds (static_cast<int> (i)).withX (0).withWidth (static_cast<float> (headerWidth())).toNearestInt());
}

void TracksArea::changeListenerCallback (juce::ChangeBroadcaster*)
{
    syncZoomSliders();
    rebuildHeaders();
    resized();
    repaint();
}

void TracksArea::followPlayhead()
{
    if (! studio.follow || ! studio.playing())
        return;
    const auto x = studio.view.tickToX (static_cast<double> (studio.playhead()));
    if (x > lanes.getWidth() * 0.9 || x < 0)
        studio.view.originTick = static_cast<double> (studio.playhead()) - lanes.getWidth() * 0.1 / studio.view.pixelsPerTick;
}

void TracksArea::timerCallback()
{
    followPlayhead();
    rulerComponent.repaint();
    lanes.repaint();
    if (! studio.collaborators().empty() || hadCollaborators)
        for (auto& h : trackHeaders)
            h->repaint();
    hadCollaborators = ! studio.collaborators().empty();
}

void TracksArea::revealTrack (const Id& trackId)
{
    const auto& order = studio.model().rows;
    const auto it = std::find (order.begin(), order.end(), trackId);
    if (it == order.end())
        return;
    const auto row = lanes.rowBounds (static_cast<int> (it - order.begin()));
    if (row.getY() < 0.0f)
        lanes.scrollY += juce::roundToInt (row.getY());
    else if (row.getBottom() > static_cast<float> (lanes.getHeight()))
        lanes.scrollY += juce::roundToInt (std::min (row.getY(), row.getBottom() - static_cast<float> (lanes.getHeight())));
    lanes.scrollY = std::max (0, lanes.scrollY);
    resized();
    repaint();
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
    const auto corner = juce::Rectangle<int> (0, 0, headerWidth(), rulerComponent.getHeight());
    g.setGradientFill (juce::ColourGradient::vertical (colour (Token::surfaceRaised), 0.0f, colour (Token::surface), static_cast<float> (Ruler::rulerHeight)));
    g.fillRect (corner);
    depth::highlight (g, corner.toFloat(), 0.0f);
    g.setColour (colour (Token::textDim));
    g.setFont (font (Type::caption));
    g.drawText (captionText ("Tracks"), corner.withHeight (Ruler::rulerHeight).reduced (space::s1 + space::half, 0), juce::Justification::centredLeft);
    // The global tracks' names, level with their lanes, each with its protect lock (D-UI-44).
    g.setFont (font (Type::small));
    for (const auto& [lane, area] : rulerComponent.lanes())
    {
        g.setColour (colour (Token::textDim));
        g.drawText (globalLaneName (lane), corner.withY (area.getY()).withHeight (area.getHeight()).reduced (space::s1 + space::half, 0),
                    juce::Justification::centredLeft);
        const auto lock = protectButtonBounds (lane).toFloat();
        const bool on = (studio.protectedLanes & lane) != 0;
        g.setColour (on ? colour (Token::text) : colour (Token::textDim).withAlpha (0.5f));
        const auto body = juce::Rectangle<float> (lock.getX() + 2.0f, lock.getCentreY() - 1.0f, lock.getWidth() - 4.0f, 6.0f);
        g.fillRoundedRectangle (body, 1.0f);
        juce::Path shackle;
        shackle.addCentredArc (body.getCentreX(), body.getY(), body.getWidth() / 2.0f - 1.0f, 3.5f, 0.0f, on ? -juce::MathConstants<float>::halfPi : -juce::MathConstants<float>::pi * 0.85f,
                               juce::MathConstants<float>::halfPi, true);
        g.strokePath (shackle, juce::PathStrokeType (1.4f));
    }
    g.setColour (colour (Token::border));
    g.fillRect (corner.withTop (corner.getBottom() - 1));
}

juce::Rectangle<int> TracksArea::protectButtonBounds (GlobalLane lane) const
{
    const auto area = rulerComponent.laneBounds (lane);
    if (! area)
        return {};
    return { headerWidth() - space::s1 - 14, area->getCentreY() - 7, 14, 14 };
}

juce::PopupMenu TracksArea::globalTracksMenu() const
{
    juce::PopupMenu menu;
    menu.addSectionHeader ("Show");
    for (const auto lane : globalLaneOrder)
        menu.addItem (globalShowBase + lane, globalLaneName (lane), true, studio.layout.globalTracks && (studio.layout.globalLanes & lane) != 0);
    menu.addSeparator();
    menu.addItem (static_cast<int> (Cmd::showSingleGlobalTrack) + 1, commandInfo (Cmd::showSingleGlobalTrack).name, true, studio.layout.singleGlobalLane);
    menu.addItem (static_cast<int> (Cmd::markerTrackOnly) + 1, "Marker Track Only", true,
                  studio.layout.globalTracks && studio.layout.globalLanes == laneMarkers && ! studio.layout.singleGlobalLane);
    menu.addSeparator();
    juce::PopupMenu protect;
    for (const auto lane : globalLaneOrder)
        protect.addItem (globalProtectBase + lane, globalLaneName (lane), true, (studio.protectedLanes & lane) != 0);
    menu.addSubMenu ("Protect", protect);
    menu.addSeparator();
    for (const auto c : { Cmd::addSection, Cmd::createSignatureChange, Cmd::createKeyChange })
        menu.addItem (static_cast<int> (c) + 1, commandInfo (c).name);
    return menu;
}

void TracksArea::pickGlobalTracksMenu (int item)
{
    if (item > globalProtectBase && item <= globalProtectBase + allGlobalLanes)
        studio.toggleLaneProtect (static_cast<GlobalLane> (item - globalProtectBase));
    else if (item > globalShowBase && item <= globalShowBase + allGlobalLanes)
    {
        const auto lane = static_cast<GlobalLane> (item - globalShowBase);
        studio.setLaneShown (lane, ! (studio.layout.globalTracks && (studio.layout.globalLanes & lane) != 0));
    }
    else if (item > 0 && item < globalShowBase)
        studio.perform (static_cast<Cmd> (item - 1));
}

void TracksArea::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();
    if (p.x >= headerWidth() || p.y < Ruler::rulerHeight || p.y >= rulerComponent.getHeight())
        return;
    if (e.mods.isPopupMenu())
    {
        globalTracksMenu().showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe = juce::Component::SafePointer<TracksArea> (this)] (int item) {
            if (safe != nullptr)
                safe->pickGlobalTracksMenu (item);
        });
        return;
    }
    for (const auto& [lane, area] : rulerComponent.lanes())
        if (protectButtonBounds (lane).expanded (2).contains (p))
            studio.toggleLaneProtect (lane);
}

void TracksArea::paintOverChildren (juce::Graphics& g)
{
    // The header column sits above the canvas: a short shadow falls onto the lanes.
    const auto x = static_cast<float> (headerWidth());
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
    top.removeFromLeft (headerWidth());
    const auto bar = getLookAndFeel().getDefaultScrollbarWidth();
    rulerComponent.setBounds (top.withTrimmedRight (bar));
    horizontal.setBounds (r.removeFromBottom (bar).withTrimmedLeft (headerWidth()).withTrimmedRight (bar));
    vertical.setBounds (r.removeFromRight (bar));
    headerColumn.setBounds (r.removeFromLeft (headerWidth()));
    resizer.setBounds (headerWidth() - space::half, headerColumn.getY(), space::s1, headerColumn.getHeight());
    addTrack.setBounds (headerWidth() - space::s1 - size::control, (Ruler::rulerHeight - size::control) / 2, size::control, size::control);
    // The zoom sliders share the caption row, between "Tracks" and +.
    {
        auto zoomRow = juce::Rectangle<int> (64, (Ruler::rulerHeight - 16) / 2, addTrack.getX() - space::half - 64, 16);
        const auto each = std::max (0, (zoomRow.getWidth() - space::half) / 2);
        horizontalZoom.setBounds (zoomRow.removeFromLeft (each));
        zoomRow.removeFromLeft (space::half);
        verticalZoom.setBounds (zoomRow.removeFromLeft (each));
    }
    lanes.setBounds (r);
    layoutHeaders();
    vertical.setRangeLimits (0.0, std::max<double> (lanes.contentHeight() + defaultTrackHeight, lanes.getHeight()), juce::dontSendNotification);
    vertical.setCurrentRange (lanes.scrollY, lanes.getHeight(), juce::dontSendNotification);
    const auto endTick = std::max (commands::projectEndTick (studio.model()) + 3840 * 8, static_cast<Ticks> (studio.view.xToTick (lanes.getWidth())));
    horizontal.setRangeLimits (0.0, static_cast<double> (endTick) * studio.view.pixelsPerTick, juce::dontSendNotification);
    horizontal.setCurrentRange (studio.view.originTick * studio.view.pixelsPerTick, lanes.getWidth(), juce::dontSendNotification);
}
} // namespace weft::ui
