#pragma once

// Piano roll editing (spec 01 F-PRL, spec 02 W-5): notes are addressed by region and
// id, because the piano roll can show several regions of one track at once.

#include "weft/model/ProjectModel.h"

#include <array>
#include <compare>
#include <map>
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

/** A region note as it plays (D-W4-16), in timeline ticks: after the region's quantize (with Q-Range and Q-Flam),
    gate time and clip length, with its velocity after Dynamics and the velocity offset (1..127). The pitch is the
    stored one: the caller adds the region's transpose, which a track's No Transpose ignores. */
struct PlayedNote
{
    const model::Note* note = nullptr;
    Ticks on = 0, off = 0;
    int velocity = 64;
};

/** The notes one pass of a region plays, from passStart to passEnd (a looped region plays several passes), in
    order. With onlyQuantize, just the quantize applies (Apply Quantize). Muted notes don't play. */
std::vector<PlayedNote> playedNotes (const model::Region& r, const model::MidiSequence& seq, Ticks passStart, Ticks passEnd, bool onlyQuantize = false);

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

/** A note where it plays on the timeline, for the note functions below. */
struct TimedNote
{
    NoteRef ref;
    Ticks start = 0, length = 0;
    int pitch = 60;
};

/** Legato (D-W4-14): each note ends where the next later-starting note of the set starts; notes that
    start together end together; the last ones keep their length. Only changed notes are returned. */
std::map<NoteRef, NoteSpan> legatoSpans (const std::vector<TimedNote>& notes);
/** Remove Overlaps (D-W4-14): each of `notes` ends no later than the next note of the same pitch in
    `all` (which may hold notes outside the set) starts. Only changed notes are returned. */
std::map<NoteRef, NoteSpan> overlapFreeSpans (const std::vector<TimedNote>& notes, const std::vector<TimedNote>& all);
/** Note Start / End to Playhead (D-W4-14): notes that start before `playhead` and end after it get that
    edge at the playhead (start: the end stays put). Only changed notes are returned. */
std::map<NoteRef, NoteSpan> startToPlayheadSpans (const std::vector<TimedNote>& notes, Ticks playhead);
/** Notes that start before `playhead` end at it, longer or shorter. */
std::map<NoteRef, NoteSpan> endToPlayheadSpans (const std::vector<TimedNote>& notes, Ticks playhead);

/** Scales for Scale Quantize (D-W4-15): semitone steps from the root, as bits 0-11. */
struct ScaleInfo
{
    const char* name;
    std::uint16_t degrees; // bit n: the note n semitones above the root is in the scale
};
const std::vector<ScaleInfo>& scales();
/** The nearest pitch in the scale on `root` (0 = C .. 11 = B); halfway between two, the lower one.
    Stays within 0-127. */
int scaleQuantizePitch (int pitch, int root, std::uint16_t degrees) noexcept;

/** Brightness 0..1 of a note drawn at this velocity: rises with velocity (F-PRL-8). */
inline float velocityBrightness (int velocity) noexcept { return 0.35f + 0.65f * static_cast<float> (velocity < 1 ? 1 : velocity > 127 ? 127 : velocity) / 127.0f; }
} // namespace weft::commands
