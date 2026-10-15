#pragma once

// Drag-and-drop import into the arrange view (F-ARR-26, F-ARR-27, W-3 and D-DM-4):
// audio becomes project-rate WAV in Media/ under its hash; MIDI files become
// instrument tracks. A drop is one undo step "Import".

#include "weft/commands/Session.h"
#include "weft/engine/media/MediaPool.h"
#include "weft/media/Decoder.h"

#include <filesystem>
#include <string>
#include <vector>

namespace weft::engine
{
struct DropTarget
{
    Ticks at = 0;
    Id trackId; // the track under the pointer; empty = below the last track
};

struct ImportOutcome
{
    std::vector<Id> regions;
    std::vector<Id> tracks;          // tracks the drop created
    std::vector<std::string> rejected; // file names that couldn't be imported
    std::string message;             // status bar text naming the rejected files
};

bool isMidiFile (const std::filesystem::path& file);

/**
    Audio dropped on an audio track lands there at `at`, several files one after another;
    dropped anywhere else, each file gets a new audio track below the last one (D-ARR-7).
    Each track of a MIDI file with notes becomes a new instrument track; an empty project
    adopts the file's tempo map (D-AT-1).
*/
ImportOutcome importFiles (commands::Session& session, MediaPool& media, const std::filesystem::path& mediaDir,
                           const media::DecoderSet& decoders, const std::vector<std::filesystem::path>& files, DropTarget target);
} // namespace weft::engine
