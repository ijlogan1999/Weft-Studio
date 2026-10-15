#pragma once

// What a finished recording pass hands the session (spec 02 W-1, W-2). The engine
// fills these in on stop; Session::commitRecording turns them into media, regions
// and take folders as one undo step named "Record".

#include "weft/core/Id.h"
#include "weft/model/Model.h"

#include <cstdint>
#include <string>
#include <vector>

namespace weft::commands
{
/** One region cut from a recorded file. */
struct RecordedTake
{
    Ticks start = 0;
    Samples sourceOffset = 0;
    Samples length = 0;
    Samples fadeIn = 0, fadeOut = 0; // punch handles (D-W1-5)
};

struct RecordedAudio
{
    Id trackId;
    std::string trackName; // for the recovered track if the track was deleted meanwhile (D-W1-6)
    int trackChannels = 1;
    model::MediaRef media; // the file, already in Media/
    std::vector<RecordedTake> takes;
    /** Cycle recording: the takes go into one folder spanning the cycle; the last take is active. */
    bool takeFolder = false;
    Ticks folderStart = 0, folderEnd = 0;
};

struct RecordedMidi
{
    Id trackId;
    std::string trackName;
    std::vector<model::Note> notes;            // positions in project ticks
    std::vector<model::CcEvent> controllers;   // positions in project ticks
    Ticks regionStart = 0, regionEnd = 0;      // the new region's span (bars)
    /** Merge into the topmost MIDI region overlapping the span, extending it, instead of a new region (F-REC-7). */
    bool merge = false;
};

/** Minutes of recording the free space holds for the armed tracks (F-REC-16). */
double recordingMinutesAvailable (std::uint64_t freeBytes, int sampleRate, int bitDepth, int totalChannels) noexcept;
/** W-1a step 4: warn when less than 10 minutes remain. No armed audio channels: no warning. */
bool needsDiskSpaceWarning (std::uint64_t freeBytes, int sampleRate, int bitDepth, int totalChannels) noexcept;
} // namespace weft::commands
