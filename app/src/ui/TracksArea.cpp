#include "TracksArea.h"

#include "weft/time/Position.h"

#include <cmath>

namespace weft::ui
{
namespace
{
constexpr float nameBar = 15.0f, edge = 8.0f;

juce::Colour regionColour (const model::ProjectModel& m, const model::Region& r)
{
    const auto c = m.regionColor (r);
    return colourFor (c.palette, c.rgb);
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
    setTooltip ("Ruler: click to move the playhead; drag the top strip to set the cycle. Marker lane: double-click to add a marker.");
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
    g.fillAll (colour (Token::surface));
    // Cycle strip.
    const auto& cyc = m.info.cycle;
    const auto cx1 = static_cast<float> (v.tickToX (static_cast<double> (cyc.start)));
    const auto cx2 = static_cast<float> (v.tickToX (static_cast<double> (cyc.end)));
    g.setColour (cyc.enabled ? colour (Token::playhead) : colour (Token::gridStrong));
    g.fillRoundedRectangle (cx1, 1.0f, std::max (2.0f, cx2 - cx1), cycleStrip - 2.0f, 4.0f);
    if (m.info.punch.enabled)
    {
        g.setColour (colour (Token::record).withAlpha (0.8f));
        const auto px1 = static_cast<float> (v.tickToX (static_cast<double> (m.info.punch.start)));
        const auto px2 = static_cast<float> (v.tickToX (static_cast<double> (m.info.punch.end)));
        g.fillRect (px1, cycleStrip - 3.0f, px2 - px1, 3.0f);
    }
    // Bars.
    g.setFont (juce::FontOptions (12.0f));
    for (const auto& [bar, x] : barLabels (m, v, getWidth()))
    {
        g.setColour (colour (Token::gridStrong));
        g.drawVerticalLine (juce::roundToInt (x), static_cast<float> (cycleStrip), static_cast<float> (rulerHeight));
        g.setColour (colour (Token::textDim));
        g.drawText (juce::String (bar), juce::roundToInt (x) + 3, cycleStrip + 2, 40, 18, juce::Justification::left);
    }
    // Markers.
    if (studio.layout.globalTracks)
    {
        const auto lane = juce::Rectangle<int> (0, rulerHeight, getWidth(), markerLane);
        g.setColour (colour (Token::lane));
        g.fillRect (lane);
        for (const auto* marker : arrange::markersInOrder (m))
        {
            const auto x = static_cast<float> (v.tickToX (static_cast<double> (marker->pos)));
            const auto c = marker->color ? colourFor (marker->color->palette, marker->color->rgb) : colour (Token::accent);
            g.setColour (c);
            g.fillRoundedRectangle (x, lane.getY() + 3.0f, 70.0f, markerLane - 6.0f, 6.0f);
            g.setColour (colour (Token::onAccent));
            g.drawText (juce::String (marker->name), juce::Rectangle<float> (x + 5, lane.getY() + 3.0f, 64.0f, markerLane - 6.0f),
                        juce::Justification::centredLeft, true);
        }
    }
    g.setColour (colour (Token::border));
    g.drawHorizontalLine (getHeight() - 1, 0.0f, static_cast<float> (getWidth()));
    // Playhead handle.
    const auto px = static_cast<float> (v.tickToX (static_cast<double> (studio.playhead())));
    g.setColour (colour (Token::playhead));
    juce::Path handle;
    handle.addTriangle (px - 6, cycleStrip, px + 6, cycleStrip, px, cycleStrip + 8);
    g.fillPath (handle);
    g.drawVerticalLine (juce::roundToInt (px), static_cast<float> (cycleStrip), static_cast<float> (getHeight()));
}

void Ruler::pointerDown (juce::Point<float> p, juce::ModifierKeys mods)
{
    const auto t = studio.snapped (static_cast<Ticks> (studio.view.xToTick (p.x)), mods.isAltDown());
    if (p.y < cycleStrip)
    {
        drag = Drag::cycle;
        dragFrom = t;
        return;
    }
    if (p.y >= rulerHeight && studio.layout.globalTracks)
    {
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
        case Drag::marker: studio.check (studio.session().moveMarker (draggedMarker, t)); break;
        case Drag::none: break;
    }
}

void Ruler::pointerUp()
{
    if (drag == Drag::marker)
    {
        studio.session().endGesture();
        studio.edited();
    }
    if (drag == Drag::cycle && studio.model().info.cycle.start == dragFrom && false)
        studio.perform (Cmd::cycle);
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
    name.setFont (juce::FontOptions (14.0f, juce::Font::bold));
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
    for (auto* b : { &mute, &solo, &arm })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (*b);
    }
    setTitle ("Track header");
    refresh();
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
    arm.setColour (juce::TextButton::buttonOnColourId, colour (Token::record));
    arm.setColour (juce::TextButton::textColourOnId, colour (Token::onRecord));
    repaint();
}

void TrackHeader::paint (juce::Graphics& g)
{
    const auto* t = studio.model().track (trackId);
    const bool selected = studio.selectedTrack() == trackId;
    g.fillAll (colour (selected ? Token::surfaceRaised : Token::surface));
    if (t != nullptr)
    {
        g.setColour (colourFor (t->color.palette, t->color.rgb));
        g.fillRoundedRectangle (4.0f, 4.0f, 6.0f, getHeight() - 8.0f, 3.0f);
    }
    g.setColour (colour (Token::textDim));
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (juce::String (number), 14, 4, 22, 20, juce::Justification::centredLeft);
    g.setColour (colour (Token::border));
    g.drawHorizontalLine (getHeight() - 1, 0.0f, static_cast<float> (getWidth()));
}

void TrackHeader::resized()
{
    auto r = getLocalBounds().reduced (4).withTrimmedLeft (32);
    auto row = r.removeFromTop (24);
    for (auto* b : { &arm, &solo, &mute })
    {
        b->setBounds (row.removeFromRight (24));
        row.removeFromRight (3);
    }
    name.setBounds (row);
}

void TrackHeader::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (e.y >= getHeight() - 4 ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
}

void TrackHeader::mouseDown (const juce::MouseEvent& e)
{
    resizeFrom = e.y >= getHeight() - 4 ? getHeight() : -1;
    studio.selectTrack (trackId);
}

void TrackHeader::mouseDrag (const juce::MouseEvent& e)
{
    if (resizeFrom > 0)
        studio.setTrackHeight (trackId, resizeFrom + e.getDistanceFromDragStartY());
}

// Timeline ---------------------------------------------------------------------------------

Timeline::Timeline (Studio& s) : studio (s)
{
    setTitle ("Tracks area");
    setWantsKeyboardFocus (false);
}

int Timeline::rows() const { return static_cast<int> (studio.model().trackOrder.size()); }

juce::Rectangle<float> Timeline::laneBounds (int row) const
{
    float y = static_cast<float> (-scrollY);
    const auto& order = studio.model().trackOrder;
    for (int i = 0; i < row && i < static_cast<int> (order.size()); ++i)
        y += static_cast<float> (studio.trackHeight (order[static_cast<std::size_t> (i)]));
    const auto h = row < static_cast<int> (order.size()) ? studio.trackHeight (order[static_cast<std::size_t> (row)]) : defaultTrackHeight;
    if (row >= static_cast<int> (order.size()))
        y += static_cast<float> ((row - static_cast<int> (order.size())) * defaultTrackHeight);
    return { 0.0f, y, static_cast<float> (getWidth()), static_cast<float> (h) };
}

int Timeline::contentHeight() const { return static_cast<int> (laneBounds (rows()).getY()) + scrollY; }

int Timeline::rowAt (float y) const
{
    if (y < static_cast<float> (-scrollY))
        return -1;
    const auto n = rows();
    for (int i = 0; i < n; ++i)
        if (laneBounds (i).getBottom() > y)
            return i;
    return n + static_cast<int> ((y - laneBounds (n).getY()) / defaultTrackHeight);
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
            g.setColour (t == start ? colour (Token::gridStrong) : colour (Token::gridWeak));
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
        c = c.withSaturation (0.1f).withAlpha (0.55f);
    if (ghost)
        c = c.withAlpha (0.45f);
    // Loop iterations, lighter.
    if (r.loop && r.loop->enabled && ! ghost)
    {
        const auto loopEnd = static_cast<float> (studio.view.tickToX (static_cast<double> (r.loop->endTicks)));
        g.setColour (c.withAlpha (0.35f));
        for (auto x = box.getRight(); x < loopEnd; x += box.getWidth())
            g.fillRoundedRectangle (x, box.getY(), std::min (box.getWidth(), loopEnd - x) - 1.0f, box.getHeight(), 6.0f);
    }
    g.setColour (c);
    g.fillRoundedRectangle (box, 6.0f);
    auto body = box.withTrimmedTop (nameBar);
    // Content overview.
    g.setColour (colour (Token::regionText).withAlpha (r.muted ? 0.35f : 0.6f));
    if (r.isMidi)
    {
        if (const auto* seq = m.sequence (r.midi.sequenceId))
        {
            int lo = 127, hi = 0;
            for (const auto* n : seq->sortedNotes())
                lo = std::min (lo, n->pitch), hi = std::max (hi, n->pitch);
            const auto range = static_cast<float> (std::max (12, hi - lo + 1));
            for (const auto* n : seq->sortedNotes())
            {
                if (n->pos < r.midi.sequenceOffset || n->pos >= r.midi.sequenceOffset + r.length)
                    continue;
                const auto x = static_cast<float> (studio.view.tickToX (static_cast<double> (r.start + n->pos - r.midi.sequenceOffset)));
                const auto w = std::max (2.0f, static_cast<float> (n->length * studio.view.pixelsPerTick));
                const auto y = body.getBottom() - 3.0f - (static_cast<float> (n->pitch - lo) + 1.0f) / range * (body.getHeight() - 6.0f);
                g.fillRect (x, y, std::min (w, box.getRight() - x), 2.0f);
            }
        }
    }
    else if (body.getHeight() > 6)
    {
        const auto& pk = studio.peaks (r.audio.media);
        if (! pk.empty())
        {
            const auto rate = static_cast<double> (m.info.sampleRate);
            const double samplesPerPixel = rate / (m.tempo.bpmAt (r.start) / 60.0 * ticksPerQuarter) / studio.view.pixelsPerTick;
            const auto mid = body.getCentreY();
            const auto half = body.getHeight() * 0.45f;
            const auto gain = static_cast<float> (std::pow (10.0, r.audio.gainDb / 20.0));
            for (int px = 0; px < static_cast<int> (box.getWidth()); ++px)
            {
                const auto from = static_cast<std::int64_t> (r.audio.sourceOffset + px * samplesPerPixel) / Studio::peakFrames;
                const auto to = static_cast<std::int64_t> (r.audio.sourceOffset + (px + 1) * samplesPerPixel) / Studio::peakFrames;
                float peak = 0;
                for (auto i = from; i <= to && i < static_cast<std::int64_t> (pk.size()); ++i)
                    peak = std::max (peak, pk[static_cast<std::size_t> (std::max<std::int64_t> (i, 0))]);
                const auto h = std::min (1.0f, peak * gain) * half;
                g.drawVerticalLine (juce::roundToInt (box.getX()) + px, mid - h, mid + h);
            }
        }
        // Fades.
        auto fadeX = [&] (Samples s) { return static_cast<float> (s) / static_cast<float> (r.length) * box.getWidth(); };
        g.setColour (colour (Token::regionText));
        if (r.audio.fadeIn.length > 0)
            g.drawLine (box.getX(), body.getBottom(), box.getX() + fadeX (r.audio.fadeIn.length), body.getY(), 1.5f);
        if (r.audio.fadeOut.length > 0)
            g.drawLine (box.getRight() - fadeX (r.audio.fadeOut.length), body.getY(), box.getRight(), body.getBottom(), 1.5f);
    }
    // Name bar.
    g.setColour (colour (Token::regionText));
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    auto label = juce::String (r.name);
    if (r.muted)
        label = "(muted) " + label;
    g.drawText (label, box.withHeight (nameBar).reduced (6, 0), juce::Justification::centredLeft, true);
    if (! r.takeFolderId.empty())
        if (const auto f = m.takeFolders.find (r.takeFolderId); f != m.takeFolders.end())
        {
            const auto& takes = f->second->takeIds;
            const auto it = std::find (takes.begin(), takes.end(), r.id);
            const auto text = "Take " + juce::String (static_cast<int> (it - takes.begin()) + 1) + "/" + juce::String (static_cast<int> (takes.size()));
            g.drawText (text, box.withHeight (nameBar).withTrimmedLeft (box.getWidth() - 62).reduced (2, 0), juce::Justification::centredRight, true);
        }
    if (selected)
    {
        g.setColour (colour (Token::accent));
        g.drawRoundedRectangle (box.reduced (1.0f), 6.0f, 2.5f);
    }
}

void Timeline::paint (juce::Graphics& g)
{
    const auto& m = studio.model();
    g.fillAll (colour (Token::lane));
    for (int row = 0; row < rows(); ++row)
    {
        const auto lane = laneBounds (row);
        if (lane.getBottom() < 0 || lane.getY() > getHeight())
            continue;
        g.setColour (colour (row % 2 == 0 ? Token::lane : Token::laneAlt));
        g.fillRect (lane);
        if (m.trackOrder[static_cast<std::size_t> (row)] == studio.selectedTrack())
        {
            g.setColour (colour (Token::accent).withAlpha (0.06f));
            g.fillRect (lane);
        }
    }
    paintGrid (g, getLocalBounds().toFloat());
    if (m.info.cycle.enabled)
    {
        g.setColour (colour (Token::playhead).withAlpha (0.06f));
        const auto x1 = static_cast<float> (studio.view.tickToX (static_cast<double> (m.info.cycle.start)));
        const auto x2 = static_cast<float> (studio.view.tickToX (static_cast<double> (m.info.cycle.end)));
        g.fillRect (x1, 0.0f, x2 - x1, static_cast<float> (getHeight()));
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
        g.setFont (juce::FontOptions (12.0f));
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
        g.setFont (juce::FontOptions (16.0f));
        g.drawText ("Drop audio or MIDI files here, or press R to record.", getLocalBounds().withTrimmedTop (contentHeight()).withHeight (60),
                    juce::Justification::centred);
    }
    const auto px = static_cast<float> (studio.view.tickToX (static_cast<double> (studio.playhead())));
    g.setColour (colour (Token::playhead));
    g.drawVerticalLine (juce::roundToInt (px), 0.0f, static_cast<float> (getHeight()));
}

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
    downAt = lastAt = p;
    auto& s = studio.session();
    const auto& m = studio.model();
    const auto* r = regionAt (p);
    const auto row = rowAt (p.y);
    const auto trackId = row >= 0 && row < rows() ? m.trackOrder[static_cast<std::size_t> (row)] : Id {};
    if (! trackId.empty())
        studio.selectTrack (trackId);

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
    if (mods.isCommandDown() && zone == Zone::move)
    {
        // Ctrl+drag on the gain line: region gain (W-3i). Ctrl+drag elsewhere copies.
    }
    startRegionDrag (*r, zone, p, mods);
    studio.sendChangeMessage();
}

void Timeline::pointerDrag (juce::Point<float> p, juce::ModifierKeys mods)
{
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
    juce::ignoreUnused (p, mods);
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
        trackHeaders[i]->setBounds (lanes.laneBounds (static_cast<int> (i)).withX (0).withWidth (static_cast<float> (headerWidth)).toNearestInt());
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

void TracksArea::paint (juce::Graphics& g) { g.fillAll (colour (Token::surface)); }

void TracksArea::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (rulerComponent.preferredHeight());
    top.removeFromLeft (headerWidth);
    rulerComponent.setBounds (top.withTrimmedRight (12));
    horizontal.setBounds (r.removeFromBottom (12).withTrimmedLeft (headerWidth).withTrimmedRight (12));
    vertical.setBounds (r.removeFromRight (12));
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
