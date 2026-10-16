#pragma once

// Rendering faster than real time through a private engine and null device: the
// rendered join (D-ARR-3) and bounce (W-9) use it. Message thread only.

#include "weft/commands/Session.h"
#include "weft/engine/devices/NullDevice.h"
#include "weft/engine/media/MediaPool.h"
#include "weft/engine/transport/Engine.h"
#include "weft/media/WavFile.h"

#include <filesystem>
#include <optional>
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
} // namespace weft::engine
