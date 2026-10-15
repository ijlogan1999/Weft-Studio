#include "weft/arrange/Grid.h"

#include <algorithm>
#include <cmath>

namespace weft::arrange
{
const std::vector<SnapChoice>& snapChoices()
{
    static const std::vector<SnapChoice> choices {
        { "Smart", Snap::smart() },       { "Bar", Snap::bar() },           { "Beat", Snap::beat() },
        { "1/4", Snap::ticks (960) },     { "1/8", Snap::ticks (480) },     { "1/16", Snap::ticks (240) },
        { "1/32", Snap::ticks (120) },    { "1/4T", Snap::ticks (640) },    { "1/8T", Snap::ticks (320) },
        { "1/16T", Snap::ticks (160) },   { "Off", Snap::off() },
    };
    return choices;
}

std::string snapLabel (const Snap& snap)
{
    for (const auto& c : snapChoices())
        if (c.snap == snap)
            return c.label;
    return std::to_string (snap.division) + " ticks";
}

namespace
{
Ticks beatTicks (const SignaturePoint& sig) { return 4 * ticksPerQuarter / sig.denominator; }

/** D-ARR-5: the finest of bar, beat, 1/8, 1/16, 1/32 whose lines are at least 16 px apart. */
Ticks smartTicks (const SignatureMap& sigs, Ticks at, double pixelsPerTick)
{
    const auto bar = sigs.barAt (at).bar;
    const auto barLength = sigs.barLength (bar);
    if (pixelsPerTick <= 0.0)
        return barLength;
    const Ticks candidates[] = { 120, 240, 480, beatTicks (sigs.signatureAtBar (bar)) };
    Ticks best = barLength;
    for (const auto c : candidates)
        if (c < best && static_cast<double> (c) * pixelsPerTick >= smartGridMinPixels)
            best = c;
    return best;
}
} // namespace

Ticks gridTicks (const Snap& snap, const SignatureMap& sigs, Ticks at, double pixelsPerTick)
{
    switch (snap.kind)
    {
        case Snap::Kind::off: return 0;
        case Snap::Kind::bar: return 0;
        case Snap::Kind::beat: return beatTicks (sigs.signatureAtBar (sigs.barAt (std::max<Ticks> (at, 0)).bar));
        case Snap::Kind::ticks: return std::max<Ticks> (snap.division, 1);
        case Snap::Kind::smart:
        {
            const auto t = smartTicks (sigs, std::max<Ticks> (at, 0), pixelsPerTick);
            return t == sigs.barLength (sigs.barAt (std::max<Ticks> (at, 0)).bar) ? 0 : t;
        }
    }
    return 0;
}

bool gridIsBars (const Snap& snap, const SignatureMap& sigs, Ticks at, double pixelsPerTick)
{
    return snap.kind == Snap::Kind::bar || (snap.kind == Snap::Kind::smart && gridTicks (snap, sigs, at, pixelsPerTick) == 0);
}

Ticks snapTick (Ticks t, const Snap& snap, const SignatureMap& sigs, double pixelsPerTick, bool bypass)
{
    if (bypass || snap.kind == Snap::Kind::off)
        return t;
    if (t <= 0)
        return 0;
    const auto pos = sigs.barAt (t);
    const auto barStart = sigs.barStart (pos.bar);
    const auto nextBar = sigs.barStart (pos.bar + 1);
    if (gridIsBars (snap, sigs, t, pixelsPerTick))
        return (t - barStart) * 2 >= (nextBar - barStart) ? nextBar : barStart;
    const auto grid = gridTicks (snap, sigs, t, pixelsPerTick);
    // Nearest point of the bar's grid, or the next bar line when that is nearer.
    const auto below = barStart + (pos.offset / grid) * grid;
    const auto above = std::min (below + grid, nextBar);
    return (t - below) * 2 >= (above - below) ? above : below;
}

Ticks gridStep (Ticks from, int direction, const Snap& snap, const SignatureMap& sigs, double pixelsPerTick)
{
    const auto dir = direction < 0 ? -1 : 1;
    if (snap.kind == Snap::Kind::off)
        return from + dir;
    if (gridIsBars (snap, sigs, from, pixelsPerTick))
    {
        const auto pos = sigs.barAt (std::max<Ticks> (from, 0));
        if (dir > 0)
            return from + sigs.barLength (pos.bar);
        // Back by the length of the previous bar keeps a bar-aligned region on bar lines.
        return from - (pos.bar > 0 ? sigs.barLength (pos.bar - 1) : sigs.barLength (0));
    }
    return from + dir * gridTicks (snap, sigs, std::max<Ticks> (from, 0), pixelsPerTick);
}

void TimelineView::zoomAround (double factor, double anchorX) noexcept
{
    if (! (factor > 0.0))
        return;
    const auto anchorTick = xToTick (anchorX);
    pixelsPerTick = std::clamp (pixelsPerTick * factor, minPixelsPerTick, maxPixelsPerTick);
    originTick = std::max (0.0, anchorTick - anchorX / pixelsPerTick);
}

void TimelineView::fit (double startTick, double endTick, double width, double fill) noexcept
{
    if (width <= 0.0)
        return;
    const auto span = std::max (endTick - startTick, 1.0);
    pixelsPerTick = std::clamp (width * fill / span, minPixelsPerTick, maxPixelsPerTick);
    const auto visible = width / pixelsPerTick;
    originTick = std::max (0.0, startTick - (visible - span) / 2.0);
}
} // namespace weft::arrange
