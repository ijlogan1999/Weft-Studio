#pragma once

// The immutable render description the audio thread plays from (ARCHITECTURE §3).
// Built on the message thread by compileSnapshot(); published by atomic pointer
// swap; freed on the message thread once the audio thread has moved past it.

#include "weft/engine/graph/Instrument.h"
#include "weft/engine/media/MediaPool.h"
#include "weft/model/ProjectModel.h"
#include "weft/time/SignatureMap.h"
#include "weft/time/TempoMap.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace weft::engine
{
enum class FadeCurve : std::uint8_t
{
    linear,
    equalPower,
    sCurve
};

/** Gain of a fade at x in [0, 1] (0 = silent end, 1 = full). */
float fadeGain (FadeCurve curve, double x) noexcept;

/** A visible piece of an audio region on the timeline. All positions are device-rate samples. */
struct AudioSegment
{
    std::int64_t start = 0, end = 0;           // visible part [start, end)
    std::int64_t regionStart = 0, regionEnd = 0; // the whole region instance, for fades
    const media::AudioData* media = nullptr;
    double sourceAtRegionStart = 0;            // media frame at regionStart
    double sourceStep = 1;                     // media frames per device sample (resampling, D-W0-1)
    float gain = 1;                            // region gain
    std::int64_t fadeIn = 0, fadeOut = 0;      // lengths in device samples
    FadeCurve fadeInCurve = FadeCurve::equalPower, fadeOutCurve = FadeCurve::equalPower;
};

/** A note with absolute device-rate timeline positions. */
struct ScheduledNote
{
    std::int64_t on = 0, off = 0;
    std::uint8_t channel = 1, pitch = 60, velocity = 100;
};

struct TrackPlan
{
    Id trackId;
    bool muted = false;
    float gain = 1; // track volume (pan arrives with the mixer, M6)
    std::vector<AudioSegment> segments; // sorted by start, non-overlapping
    std::vector<ScheduledNote> notes;   // sorted by on
    std::shared_ptr<InstrumentSlot> instrument;
};

struct EngineSnapshot
{
    std::uint64_t serial = 0; // set when published
    double sampleRate = 48000; // device rate the positions are in
    TempoMap tempo;            // at the device rate
    SignatureMap signatures;
    bool cycleEnabled = false;
    std::int64_t cycleStart = 0, cycleEnd = 0;
    float masterGain = 1;
    std::vector<TrackPlan> tracks;
    std::vector<MediaPool::Audio> media; // keeps the decoded audio alive while this snapshot exists

    std::int64_t tickToSample (Ticks t) const noexcept;
};

/** Builds a snapshot of the projected model at the device sample rate. Missing media plays as silence. */
std::unique_ptr<EngineSnapshot> compileSnapshot (const model::ProjectModel& model, double deviceRate, const MediaPool& media,
                                                 const InstrumentRack& instruments);

/** Linear gain for a decibel value; minusInfinityDb and below are silent. */
float dbToGain (double db) noexcept;
} // namespace weft::engine
