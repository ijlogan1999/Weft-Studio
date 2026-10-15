#pragma once

// Region selection and marker navigation in the arrange view (W-3a, F-ARR-18, F-ARR-28).
// Selection is local view state; it never enters the document.

#include "weft/core/Id.h"
#include "weft/model/ProjectModel.h"

#include <optional>
#include <set>
#include <vector>

namespace weft::arrange
{
class Selection
{
public:
    const std::set<Id>& regions() const noexcept { return selected; }
    std::vector<Id> ids() const { return { selected.begin(), selected.end() }; }
    bool contains (const Id& id) const { return selected.contains (id); }
    bool empty() const noexcept { return selected.empty(); }

    /** Click: this region only. Shift+click: toggle it. */
    void click (const Id& regionId, bool shift);
    void clear() { selected.clear(); }
    /** Ctrl+A. */
    void selectAll (const model::ProjectModel& m);
    void selectTrack (const model::ProjectModel& m, const Id& trackId);
    /** Rubber band over rows [firstRow, lastRow] and ticks [from, to): every region it touches. Shift adds. */
    void rubberBand (const model::ProjectModel& m, int firstRow, int lastRow, double from, double to, bool shift);
    void set (std::vector<Id> ids) { selected = { ids.begin(), ids.end() }; }
    /** Forgets regions that no longer exist (deleted here or by a collaborator). */
    void prune (const model::ProjectModel& m);

    /** Span of the selection in ticks, for Z (F-ARR-21). */
    std::optional<std::pair<double, double>> span (const model::ProjectModel& m) const;

private:
    std::set<Id> selected;
};

/** Markers sorted by position, then id (the numbering 1-9 uses, F-ARR-28). */
std::vector<const model::Marker*> markersInOrder (const model::ProjectModel& m);
/** Alt+, : the nearest marker before the playhead. */
std::optional<Ticks> previousMarker (const model::ProjectModel& m, Ticks playhead);
/** Alt+. : the nearest marker after the playhead. */
std::optional<Ticks> nextMarker (const model::ProjectModel& m, Ticks playhead);
/** Number keys: marker n (1-based) by position, or nothing. */
std::optional<Ticks> markerNumber (const model::ProjectModel& m, int n);

/** Span of all regions (Z with nothing selected), or nothing for an empty project. */
std::optional<std::pair<double, double>> projectSpan (const model::ProjectModel& m);
} // namespace weft::arrange
