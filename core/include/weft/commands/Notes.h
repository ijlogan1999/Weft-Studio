#pragma once

// Piano roll editing (spec 01 F-PRL, spec 02 W-5): notes are addressed by region and
// id, because the piano roll can show several regions of one track at once.

#include "weft/model/ProjectModel.h"

#include <compare>
#include <vector>

namespace weft::commands
{
struct NoteRef
{
    Id regionId, noteId;
    friend auto operator<=> (const NoteRef&, const NoteRef&) = default;
};

/** Ctrl+C in the piano roll: notes with their timeline positions (F-PRL-7). */
struct NoteClipboard
{
    std::vector<model::Note> notes; // pos is the timeline tick
    bool empty() const noexcept { return notes.empty(); }
    static NoteClipboard of (const model::ProjectModel& m, const std::vector<NoteRef>& refs);
};

/** Where a note in a region plays on the timeline, and back. */
inline Ticks noteTimelineTick (const model::Region& r, Ticks sequencePos) noexcept { return r.start + sequencePos - r.midi.sequenceOffset; }
inline Ticks noteSequenceTick (const model::Region& r, Ticks timelineTick) noexcept { return timelineTick - r.start + r.midi.sequenceOffset; }

/** The note a ref names, or null. */
const model::Note* findNote (const model::ProjectModel& m, const NoteRef& ref);

/** The MIDI region on a track that covers a timeline tick (topmost when stacked), or null. */
const model::Region* midiRegionAt (const model::ProjectModel& m, const Id& trackId, Ticks at);

/** "C3" for MIDI 60 (D-W4-1); sharps for black keys ("C#3"). */
std::string noteName (int pitch);
inline bool isBlackKey (int pitch) noexcept
{
    const auto k = ((pitch % 12) + 12) % 12;
    return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
}

/** A note's place on the timeline while it is being resized. */
struct NoteSpan
{
    Ticks start = 0, length = 0;
    Ticks end() const noexcept { return start + length; }
    friend bool operator== (const NoteSpan&, const NoteSpan&) = default;
};

/** Dragging a note edge to `pointer` (F-PRL-5). The moved edge snaps to `grid` (0 = snap off) and the
    other edge stays put. The shortest note is one grid step, or one tick with snap off. */
NoteSpan resizeNote (NoteSpan note, bool rightEdge, Ticks pointer, Ticks grid) noexcept;

/** Brightness 0..1 of a note drawn at this velocity: rises with velocity (F-PRL-8). */
inline float velocityBrightness (int velocity) noexcept { return 0.35f + 0.65f * static_cast<float> (velocity < 1 ? 1 : velocity > 127 ? 127 : velocity) / 127.0f; }
} // namespace weft::commands
