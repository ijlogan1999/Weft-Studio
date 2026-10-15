#pragma once

// A region drag in the arrange view (W-3b, W-3c). While the pointer moves, the drag
// only computes a preview; the document changes once, on release, as one undo step.
// Esc cancels with no change (AT-ARR-6).

#include "weft/arrange/Grid.h"
#include "weft/commands/Session.h"

namespace weft::arrange
{
class RegionDrag
{
public:
    /** anchorId is the region under the pointer; its start is what snaps (D-W3-1). */
    RegionDrag (commands::Session& session, std::vector<Id> regionIds, Id anchorId, Snap snap, double pixelsPerTick);

    /** Pointer moved by rawDeltaTicks and deltaRows since the press. alt bypasses snap; ctrl copies. */
    void update (Ticks rawDeltaTicks, int deltaRows, bool alt, bool ctrl);

    const commands::MovePlan& preview() const noexcept { return plan; }
    Ticks deltaTicks() const noexcept { return plan.deltaTicks; }
    bool copying() const noexcept { return copy; }
    /** Where the anchor region would start; the drag tooltip shows it (W-3b step 4). */
    Ticks anchorStart() const noexcept { return anchorStartTick + plan.deltaTicks; }

    /** Release: applies the move or copy. Returns the copies' ids when copying. */
    std::vector<Id> commit();
    /** Esc. */
    void cancel() noexcept { active = false; }
    bool isActive() const noexcept { return active; }

private:
    commands::Session& session;
    std::vector<Id> ids;
    commands::RegionClipboard clip;
    Ticks anchorStartTick = 0;
    Snap snap;
    double pixelsPerTick;
    commands::MovePlan plan;
    int rows = 0;
    bool copy = false;
    bool active = true;
};
} // namespace weft::arrange
