#include "weft/arrange/RegionDrag.h"

namespace weft::arrange
{
RegionDrag::RegionDrag (commands::Session& s, std::vector<Id> regionIds, Id anchorId, Snap snapSetting, double pxPerTick)
    : session (s), ids (std::move (regionIds)), snap (snapSetting), pixelsPerTick (pxPerTick)
{
    clip = commands::RegionClipboard::of (session.model(), ids);
    if (const auto* r = session.model().region (anchorId))
        anchorStartTick = r->start;
    else if (const auto* a = clip.anchor())
        anchorStartTick = a->start;
    plan = commands::planMove (session.model(), clip, 0, 0);
}

void RegionDrag::update (Ticks rawDeltaTicks, int deltaRows, bool alt, bool ctrl)
{
    if (! active)
        return;
    const auto target = snapTick (anchorStartTick + rawDeltaTicks, snap, session.model().signatures, pixelsPerTick, alt);
    rows = deltaRows;
    copy = ctrl;
    plan = commands::planMove (session.model(), clip, target - anchorStartTick, deltaRows);
}

std::vector<Id> RegionDrag::commit()
{
    if (! active)
        return {};
    active = false;
    if (copy)
        return session.copyRegions (ids, plan.deltaTicks, rows);
    session.moveRegions (ids, plan.deltaTicks, rows);
    return {};
}
} // namespace weft::arrange
