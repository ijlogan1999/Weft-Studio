#pragma once

// Turning a finished pass into regions (W-1a step 8-9, W-1c, W-1d, W-2 step 6).
// Pure functions of the pass and the time maps, so they're testable on their own.

#include "weft/commands/Recording.h"
#include "weft/engine/record/RecordPass.h"
#include "weft/time/SignatureMap.h"
#include "weft/time/TempoMap.h"

#include <vector>

namespace weft::engine
{
struct AudioLayout
{
    std::vector<commands::RecordedTake> takes; // empty: too short, discard the file
    bool takeFolder = false;
    Ticks folderStart = 0, folderEnd = 0;
};

/** Regions for one audio lane holding `frames` frames. Positions use `tempo` at the device rate. */
AudioLayout layoutAudio (const RecordPass& pass, std::int64_t frames, const TempoMap& tempo, const SignatureMap& signatures);

/** Notes and controllers for one MIDI lane. Linear passes span whole bars around the events; cycle passes
    span the cycle (and any bars before it). No events: an empty result. */
commands::RecordedMidi layoutMidi (const RecordPass& pass, const RecordPass::MidiLane& lane, const TempoMap& tempo, const SignatureMap& signatures,
                                   bool mergeLinear, bool mergeCycle);
} // namespace weft::engine
