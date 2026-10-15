#pragma once

// Arrange-view editing of several regions at once (spec 02 W-3b, W-3c, F-ARR-23,
// F-ARR-29): the move/copy plan the drag preview shows, and the clipboard.

#include "weft/model/ProjectModel.h"

#include <map>
#include <vector>

namespace weft::commands
{
/**
    Regions lifted out of the project, with what they need to be placed again: their
    MIDI sequences and take folders. A region in a take folder brings the whole folder,
    so takes stay together (D-ARR-6).
*/
struct RegionClipboard
{
    std::vector<model::Region> regions; // trackId is the track each was shown on
    std::map<Id, model::MidiSequence> sequences;
    std::map<Id, model::TakeFolder> takeFolders;

    bool empty() const noexcept { return regions.empty(); }
    /** Earliest region start; the anchor for paste. */
    Ticks earliestStart() const;
    /** The region pasted at the playhead: the leftmost (then topmost by id). */
    const model::Region* anchor() const;

    static RegionClipboard of (const model::ProjectModel& m, const std::vector<Id>& regionIds);
};

/** A track that a move or paste creates below the last track (W-3b step 3). */
struct NewTrackSpec
{
    model::TrackKind kind = model::TrackKind::audio;
    int channels = 2;
    int below = 0; // 0 = first track below the last one, 1 = the one after...
};

struct Placement
{
    Id regionId;      // the region in the clipboard
    Ticks start = 0;
    Id trackId;       // an existing track, or empty when newTrack >= 0
    int newTrack = -1; // index into MovePlan::newTracks
};

struct MovePlan
{
    Ticks deltaTicks = 0; // after clamping so nothing starts before 0
    std::vector<Placement> placements;
    std::vector<NewTrackSpec> newTracks;
};

/**
    Where regions land when shifted by deltaTicks and deltaTracks (rows in the track
    list). A region whose target row holds the wrong kind of track, or lies above the
    first track, stays on its own track; rows below the last track become new tracks
    of the region's kind. A region whose own track isn't in the list (deleted since it
    was copied) uses fallbackRow as its row.
*/
MovePlan planMove (const model::ProjectModel& m, const RegionClipboard& clip, Ticks deltaTicks, int deltaTracks, int fallbackRow = -1);

/** Row of a track in the tracks area, or -1. */
int trackRow (const model::ProjectModel& m, const Id& trackId);
} // namespace weft::commands
