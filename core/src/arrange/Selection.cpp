#include "weft/arrange/Selection.h"

#include <algorithm>

namespace weft::arrange
{
void Selection::click (const Id& regionId, bool shift)
{
    if (! shift)
    {
        selected = { regionId };
        return;
    }
    if (! selected.erase (regionId))
        selected.insert (regionId);
}

void Selection::selectAll (const model::ProjectModel& m)
{
    selected.clear();
    for (const auto& [id, r] : m.regions)
        selected.insert (id);
}

void Selection::selectTrack (const model::ProjectModel& m, const Id& trackId)
{
    selected.clear();
    for (const auto* r : m.regionsOnTrack (trackId))
        selected.insert (r->id);
}

void Selection::rubberBand (const model::ProjectModel& m, int firstRow, int lastRow, double from, double to, bool shift)
{
    if (! shift)
        selected.clear();
    if (firstRow > lastRow)
        std::swap (firstRow, lastRow);
    if (from > to)
        std::swap (from, to);
    const auto rows = static_cast<int> (m.trackOrder.size());
    for (int row = std::max (firstRow, 0); row <= std::min (lastRow, rows - 1); ++row)
        for (const auto* r : m.regionsOnTrack (m.trackOrder[static_cast<std::size_t> (row)]))
            if (static_cast<double> (r->start) < to && m.regionEndTick (*r) > from)
                selected.insert (r->id);
}

void Selection::prune (const model::ProjectModel& m)
{
    std::erase_if (selected, [&] (const Id& id) { return m.region (id) == nullptr; });
}

std::optional<std::pair<double, double>> Selection::span (const model::ProjectModel& m) const
{
    std::optional<std::pair<double, double>> s;
    for (const auto& id : selected)
        if (const auto* r = m.region (id))
        {
            const auto a = static_cast<double> (r->start);
            const auto b = m.regionEndTick (*r);
            s = s ? std::make_pair (std::min (s->first, a), std::max (s->second, b)) : std::make_pair (a, b);
        }
    return s;
}

std::vector<const model::Marker*> markersInOrder (const model::ProjectModel& m)
{
    std::vector<const model::Marker*> out;
    for (const auto& [id, marker] : m.markers)
        out.push_back (marker.get());
    std::sort (out.begin(), out.end(), [] (const model::Marker* a, const model::Marker* b) { return std::tie (a->pos, a->id) < std::tie (b->pos, b->id); });
    return out;
}

std::optional<Ticks> previousMarker (const model::ProjectModel& m, Ticks playhead)
{
    std::optional<Ticks> best;
    for (const auto* marker : markersInOrder (m))
        if (marker->pos < playhead)
            best = marker->pos;
    return best;
}

std::optional<Ticks> nextMarker (const model::ProjectModel& m, Ticks playhead)
{
    for (const auto* marker : markersInOrder (m))
        if (marker->pos > playhead)
            return marker->pos;
    return std::nullopt;
}

std::optional<Ticks> markerNumber (const model::ProjectModel& m, int n)
{
    const auto markers = markersInOrder (m);
    if (n < 1 || static_cast<std::size_t> (n) > markers.size())
        return std::nullopt;
    return markers[static_cast<std::size_t> (n - 1)]->pos;
}

std::optional<std::pair<double, double>> projectSpan (const model::ProjectModel& m)
{
    Selection all;
    all.selectAll (m);
    return all.span (m);
}
} // namespace weft::arrange
