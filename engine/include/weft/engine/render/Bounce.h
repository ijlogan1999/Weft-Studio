#pragma once

// Bounce (W-9, F-EXP-1 to 5 and 11): renders the master output over a range, offline
// through a private engine or in real time through the live one, then converts it and
// writes WAV and MP3 files. Message thread only; a job renders a slice per step(), so
// the window stays responsive and can cancel.

#include "weft/engine/arrange/OfflineRender.h"
#include "weft/engine/transport/Engine.h"
#include "weft/media/WavFile.h"
#include "weft/model/ProjectModel.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace weft::engine
{
struct BounceRange
{
    Ticks from = 0, to = 0;
};

/** F-EXP-4: the cycle range when cycle is on; else 0 to the end marker; else 0 to the end of the last region. */
BounceRange defaultBounceRange (const model::ProjectModel& model);

enum class DitherMode
{
    automatic, // TPDF below 24 bits
    off,
    on
};

/** Whether a bounce at this bit depth gets dither (32 is float: never). */
bool dithers (DitherMode mode, int bitDepth) noexcept;

struct WavFormat
{
    int bitDepth = 24;      // 16, 24 or 32 (float)
    double sampleRate = 0;  // 0: the project rate
};

struct Mp3Format
{
    bool vbr = false;
    int bitrateKbps = 320;  // CBR: 128, 192, 256 or 320
    int vbrQuality = 2;     // VBR: 0 (best) to 9
    double sampleRate = 0;  // 0: the project rate when it's 44.1 or 48 kHz, else 44.1
    std::string title, artist, album, year;
};

/** The MP3 rate a setting gives for a project rate. */
double mp3SampleRate (const Mp3Format& format, double projectRate) noexcept;

struct BounceSettings
{
    BounceRange range;
    bool includeTail = true;
    bool realtime = false;
    std::optional<WavFormat> wav = WavFormat {};
    std::optional<Mp3Format> mp3;
    DitherMode dither = DitherMode::automatic;
    std::filesystem::path folder;
    std::string name = "Bounce"; // file name without the extension
    bool replace = false;        // an existing file: replace it, or keep both ("name (2).wav")
    std::uint32_t ditherSeed = 0x5745'4654; // fixed, so offline bounces are bit-identical (D-W9-2)
};

/** "name.ext", or with " (2)", " (3)"... when that already exists (Keep both). */
std::filesystem::path keepBothPath (const std::filesystem::path& wanted);
/** The files a bounce would write, before Replace / Keep both. */
std::vector<std::filesystem::path> bounceTargets (const BounceSettings& settings);

/** The tail rule (F-EXP-11): output stays below -90 dBFS for 200 ms, at most 10 s past the range end. */
class TailDetector
{
public:
    static constexpr double thresholdDb = -90.0, quietSeconds = 0.2, maxSeconds = 10.0;
    explicit TailDetector (double sampleRate) noexcept;
    /** Feeds the frames rendered after the range end. */
    void add (const float* left, const float* right, std::int64_t frames) noexcept;
    /** The tail is over (quiet long enough, or at the limit). */
    bool done() const noexcept { return quietRun >= quietFrames || fed >= maxFrames; }
    /** Frames of tail to keep: up to the start of the quiet run (or the limit). */
    std::int64_t length() const noexcept;
    std::int64_t limit() const noexcept { return maxFrames; }

private:
    float threshold;
    std::int64_t quietFrames, maxFrames;
    std::int64_t fed = 0, quietRun = 0, lastLoud = 0;
};

/** TPDF dither of +-1 LSB at `bitDepth`, seeded, added in place (before the writer rounds). */
void addDither (media::AudioData& audio, int bitDepth, std::uint32_t seed);

/** "Bounce clipped (peak +2.0 dBFS)". */
std::string clipMessage (double peakDb);

struct BounceResult
{
    bool ok = false;
    bool cancelled = false;
    std::vector<std::filesystem::path> files;
    std::int64_t frames = 0;      // rendered, tail included (project rate, or the device rate in real time)
    double peakDb = -1000;        // of the float mix
    bool clipped = false;
    std::string message;          // the completion notice
    std::string error;
};

/**
    One bounce. Construct, call step() until it returns false (render() does that), then
    finish() writes the files. A real-time job plays the range on the live engine: the
    caller keeps its audio device running between steps.
*/
class BounceJob
{
public:
    struct Context
    {
        MediaPool* media = nullptr;
        Engine::PluginFactory plugins;
        std::filesystem::path projectFolder;
        Engine* live = nullptr; // for real-time bounces
        /** Plug-ins that report they can't render offline (VST3 "OnlyRT"). */
        std::function<bool (const model::PluginInstance&)> realtimeOnly;
    };

    BounceJob (const model::ProjectModel& model, BounceSettings settings, Context context);
    ~BounceJob();
    BounceJob (const BounceJob&) = delete;
    BounceJob& operator= (const BounceJob&) = delete;

    const BounceSettings& settings() const noexcept { return config; }
    bool realtime() const noexcept { return config.realtime; }
    /** Why a bounce had to run in real time, or empty. */
    const std::string& notice() const noexcept { return why; }

    /** Renders a slice (offline) or takes what the live engine played (real time). False once done. */
    bool step();
    /** Runs step() to the end (offline). */
    void render();
    double progress() const noexcept;
    void cancel();
    bool cancelled() const noexcept { return stopped; }
    /** The rendered mix (range plus kept tail): at the project rate offline, at the device rate in real time. */
    const media::AudioData& mix() const noexcept { return audio; }

    /** Converts and writes each format. */
    BounceResult finish();

private:
    bool stepOffline();
    bool stepRealtime();
    void append (const float* left, const float* right, std::int64_t frames);
    void startRealtime();
    void endRealtime();

    model::ProjectModel project;
    BounceSettings config;
    Context ctx;
    std::string why;
    std::int64_t rangeFrames = 0, latency = 0;
    std::unique_ptr<OfflineRenderer> renderer;
    std::shared_ptr<Engine::Capture> capture;
    std::int64_t taken = 0; // capture frames consumed (latency included)
    bool playing = false, rolled = false;
    TailDetector tail;
    media::AudioData audio;
    bool complete = false, stopped = false;
};

/** Regions whose media isn't available yet (D-W9-1: "N regions have media not yet downloaded"). */
int regionsMissingMedia (const model::ProjectModel& model, const MediaPool& media);

/** VST3 plug-ins with the "OnlyRT" sub-category can't render offline. */
bool isRealtimeOnlyCategory (const std::string& category);
} // namespace weft::engine
