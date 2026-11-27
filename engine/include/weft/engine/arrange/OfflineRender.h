#pragma once

// Rendering faster than real time through a private engine and null device: the
// rendered join (D-ARR-3) and bounce (W-9) use it. Message thread only.

#include "weft/media/MidiFile.h"
#include "weft/commands/Session.h"
#include "weft/engine/devices/NullDevice.h"
#include "weft/engine/media/MediaPool.h"
#include "weft/engine/transport/Engine.h"
#include "weft/media/WavFile.h"

#include <filesystem>
#include <optional>
#include <set>
#include <string>

namespace weft::engine
{
struct OfflineRenderOptions
{
    Ticks from = 0;
    std::int64_t frames = 0;
    bool metronome = false;
    int blockSize = 512;
    Engine::PluginFactory plugins;      // hosted plug-ins; null: built-ins and placeholders only
    std::filesystem::path projectFolder; // where plug-in state blobs are
    std::set<Id> soloed;                 // local solos: only these and what they feed are heard
};

/**
    Renders a model a slice at a time, from `options.from`, at the project rate (stereo).
    Each slice continues the last; the compensated plug-in latency is skipped, so frame 0
    is the timeline at `from`. `options.frames` is ignored.
*/
class OfflineRenderer
{
public:
    OfflineRenderer (const model::ProjectModel& model, MediaPool& media, const OfflineRenderOptions& options);
    ~OfflineRenderer();
    OfflineRenderer (const OfflineRenderer&) = delete;
    OfflineRenderer& operator= (const OfflineRenderer&) = delete;

    bool ok() const noexcept { return started; }
    /** The next `frames` frames into left and right (resized to fit). */
    bool render (std::int64_t frames, std::vector<float>& left, std::vector<float>& right);
    std::int64_t latency() const noexcept { return compensation; }

private:
    InstrumentRack rack;
    Engine engine;
    NullDevice device;
    std::int64_t compensation = 0, toSkip = 0;
    bool started = false;
};

/**
    Plays the model from `from` for `frames` at the project rate. Stereo output. Streamed media
    is read as it goes. The output is shifted back by the compensated plug-in latency, so it
    lines up with the timeline.
*/
media::AudioData renderOffline (const model::ProjectModel& model, MediaPool& media, const OfflineRenderOptions& options);

/**
    The rendered join of audio regions on one track (D-ARR-3, D-ARR-4): the regions as they
    play, with their gain and inner fades but without the track's strip, from the first
    region's start to the last one's end. The outer fade-in and fade-out aren't rendered;
    they come back as `fadeIn` and `fadeOut` for the joined region, so it plays the same.
*/
struct RenderedJoin
{
    media::AudioData audio;
    model::Fade fadeIn, fadeOut;
};
std::optional<RenderedJoin> renderJoin (const model::ProjectModel& model, const std::vector<Id>& regionIds, MediaPool& media, std::string* error = nullptr);

/**
    Ctrl+J for any selection: joins in place when the session can (MIDI, or contiguous pieces
    of one file); otherwise renders the audio into Media/ and replaces the regions with it.
*/
commands::EditResult joinRegions (commands::Session& session, const std::vector<Id>& regionIds, MediaPool& media,
                                  const std::filesystem::path& mediaDir);

/**
    Reverse (L-RGN-12): writes each selected audio region's part of its file, backwards, into
    Media/ and points the region at it (one undo step "Reverse"). MIDI regions are skipped.
*/
commands::EditResult reverseRegions (commands::Session& session, const std::vector<Id>& regionIds, MediaPool& media,
                                     const std::filesystem::path& mediaDir);

/**
    Snap Edits to Zero Crossings (L-ATE-3): the frame nearest `frame`, within `radius`, where
    the signal (all channels summed) changes sign or is 0. `frame` itself when there's none.
*/
std::int64_t nearestZeroCrossing (const MediaSource& source, std::int64_t frame, std::int64_t radius);

/** One audio region as it plays (gain and fades, no track strip), mono or stereo like its file. */
std::optional<media::AudioData> renderRegion (const model::ProjectModel& model, const Id& regionId, MediaPool& media, std::string* error = nullptr);

/**
    A MIDI region as a type 1 MIDI file (L-EXP-3): its notes and controllers as they play
    (transpose and velocity offset applied, muted notes left out, cut at the region's end),
    from the region start at 960 PPQN, with the project's tempo and meter from there on.
*/
media::MidiFileData midiRegionFile (const model::ProjectModel& model, const Id& regionId);

/**
    Export Selection as MIDI File (D-W9-10): the selected MIDI regions in one type 1 file, one file
    track per project track (in track order, named after it), from the earliest region's start.
    Empty tracks when no MIDI region is selected.
*/
media::MidiFileData midiSelectionFile (const model::ProjectModel& model, const std::vector<Id>& regionIds);

struct RegionExport
{
    std::vector<std::filesystem::path> files;
    std::string message; // the status line
};
/**
    Export Region as File (L-EXP-2, L-EXP-3): audio regions become WAVs at the project rate
    and `bitDepth`, MIDI regions become .mid files, named after the region in `folder`
    ("name (2)" when taken).
*/
RegionExport exportRegions (const model::ProjectModel& model, const std::vector<Id>& regionIds, MediaPool& media,
                            const std::filesystem::path& folder, int bitDepth);
} // namespace weft::engine
