#pragma once

// Presence (spec 05 §5): where each collaborator is and what they're doing. Ephemeral:
// it goes through the relay to everyone else in the project and is never stored. The
// relay stamps who sent it; everything else is this state, encoded as a codec map.

#include "weft/core/Id.h"
#include "weft/doc/Value.h"
#include "weft/time/TempoMap.h"
#include "weft/time/Time.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace weft::collab
{
struct Presence
{
    enum class Status
    {
        active,
        idle,          // no input for 2 minutes
        recording,
        offlineEditing // reserved: shown for a client that edits offline (never sent while offline)
    };
    enum class View
    {
        arrange,
        pianoRoll,
        audioEditor,
        mixer
    };
    struct Playhead
    {
        Ticks pos = 0;
        bool playing = false;
        std::int64_t at = 0; // sender's clock, Unix ms, when it was at pos
        friend bool operator== (const Playhead&, const Playhead&) = default;
    };
    /** In musical coordinates, so it lands right whatever the viewer's zoom (D-COL-5). */
    struct Pointer
    {
        View area = View::arrange; // arrange or pianoRoll
        Ticks pos = 0;
        Id track;      // arrange: the track under the pointer
        int pitch = -1; // piano roll: the note row
        friend bool operator== (const Pointer&, const Pointer&) = default;
    };

    Status status = Status::active;
    View view = View::arrange;
    Id focusTrack, focusRegion;
    Playhead playhead;
    std::optional<Pointer> pointer;
    /** Selected ids, at most 500 of each; beyond that only the count (05 §5). */
    std::vector<Id> regions, notes, points;
    std::int64_t selectionCount = 0;
    Id writingAutomationOn; // track id, or empty

    static constexpr std::size_t maxSelectionIds = 500;

    doc::Value toValue() const;
    static Presence fromValue (const doc::Value& v);
    friend bool operator== (const Presence&, const Presence&) = default;
};

const char* statusName (Presence::Status s);

/** Where a collaborator's playhead is now: their last position moved on by the time since
    (05 §5), through the project's tempo map. A sender clock far from ours (over 5 s) counts
    from when the update arrived instead. */
Ticks extrapolatePlayhead (const Presence::Playhead& p, const TempoMap& tempo, std::int64_t nowMs, std::int64_t receivedAtMs);

/** A collaborator connected to the project right now (one per client). */
struct RemotePresence
{
    std::string clientId, userId, displayName;
    int color = 0;
    Presence state;
    bool hasState = false;            // joined, but nothing received yet
    std::int64_t receivedAt = 0;      // our clock, Unix ms
    std::int64_t pointerMovedAt = 0;  // our clock: pointers fade 5 s after this
};
} // namespace weft::collab
