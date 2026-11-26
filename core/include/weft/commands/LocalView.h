#pragma once

// Per-user view state that never enters the document (D-DM-2): record enables, solos
// and the selected track. Solo is local by design (F-TRK-10, AT-TRK-9).

#include "weft/core/Id.h"
#include "weft/model/ProjectModel.h"

#include <set>

namespace weft::commands
{
struct LocalView
{
    std::set<Id> soloed;
    std::set<Id> armed;
    Id selectedTrack; // receives live MIDI when it's an instrument track (D-W2-1)

    /** Only audio and instrument tracks can be record-enabled (F-TRK-11). */
    static bool canArm (const model::Track& track) noexcept
    {
        return track.kind == model::TrackKind::audio || track.kind == model::TrackKind::instrument;
    }
    /** Toggles a track's record enable; false (and no change) for buses, the master or unknown tracks. */
    bool toggleArm (const model::ProjectModel& model, const Id& trackId)
    {
        const auto* t = model.track (trackId);
        if (t == nullptr || ! canArm (*t))
            return false;
        if (! armed.erase (trackId))
            armed.insert (trackId);
        return true;
    }
    void toggleSolo (const Id& trackId)
    {
        if (! soloed.erase (trackId))
            soloed.insert (trackId);
    }
    /** Ctrl+click on solo: this track alone (F-TRK-10). */
    void soloOnly (const Id& trackId) { soloed = { trackId }; }
    /** Solo is not exclusive, so several can be on; this sets them all at once (D-UI-25). */
    void setSolo (const Id& trackId, bool on)
    {
        if (on)
            soloed.insert (trackId);
        else
            soloed.erase (trackId);
    }
    /** Sets a track's record enable; false (and no change) for buses, the master or unknown tracks. */
    bool setArm (const model::ProjectModel& model, const Id& trackId, bool on)
    {
        const auto* t = model.track (trackId);
        if (t == nullptr || ! canArm (*t))
            return false;
        if (on)
            armed.insert (trackId);
        else
            armed.erase (trackId);
        return true;
    }
    /** Forgets tracks that no longer exist. */
    void prune (const model::ProjectModel& model)
    {
        std::erase_if (armed, [&] (const Id& id) { return model.track (id) == nullptr; });
        std::erase_if (soloed, [&] (const Id& id) { return model.track (id) == nullptr; });
        if (! selectedTrack.empty() && model.track (selectedTrack) == nullptr)
            selectedTrack.clear();
    }
};

/** The channels a solo leaves audible (D-W6-1): the soloed ones plus every channel that feeds them or that
    they feed, through outputs and sends that aren't disabled. Empty when nothing is soloed (all are audible). */
std::set<Id> audibleWithSolo (const model::ProjectModel& model, const std::set<Id>& soloed);
} // namespace weft::commands
