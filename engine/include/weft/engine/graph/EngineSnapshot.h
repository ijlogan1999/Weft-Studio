#pragma once

// The immutable render description the audio thread plays from (ARCHITECTURE §3).
// Built on the message thread by compileSnapshot(); published by atomic pointer
// swap; freed on the message thread once the audio thread has moved past it.

#include "weft/commands/LocalView.h"
#include "weft/engine/graph/Instrument.h"
#include "weft/engine/graph/Mixer.h"
#include "weft/engine/media/MediaPool.h"
#include "weft/model/ProjectModel.h"
#include "weft/time/SignatureMap.h"
#include "weft/time/TempoMap.h"

#include <atomic>
#include <cstdint>
#include <set>
#include <utility>
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
    const MediaSource* media = nullptr;
    double sourceAtRegionStart = 0;            // media frame at regionStart
    double sourceStep = 1;                     // media frames per device sample (resampling, D-W0-1)
    float gain = 1;                            // region gain
    std::int64_t fadeIn = 0, fadeOut = 0;      // lengths in device samples
    FadeCurve fadeInCurve = FadeCurve::equalPower, fadeOutCurve = FadeCurve::equalPower;
    /** Under a higher region's fade: the complementary gain, so the two crossfade (D-W1-5).
        The upper region's fade position u runs 0..1 over [duckFrom, duckFrom + duckLength), rising for
        a fade-in and falling for a fade-out; this segment plays at fadeGain (duckCurve, 1 - u). */
    std::int64_t duckFrom = 0, duckLength = 0;
    bool duckRising = true;
    FadeCurve duckCurve = FadeCurve::equalPower;
};

/** A note with absolute device-rate timeline positions. */
struct ScheduledNote
{
    std::int64_t on = 0, off = 0;
    std::uint8_t channel = 1, pitch = 60, velocity = 100;
};

/** A MIDI message other than a note (controllers, pitch bend, pressure, program) at a device-rate position. */
struct ScheduledEvent
{
    std::int64_t at = 0;
    std::uint8_t status = 0, data1 = 0, data2 = 0;
};

enum class MonitorMode : std::uint8_t
{
    autoMode, // armed and (stopped or recording), D-W1-1
    on,
    off
};

struct SendPlan
{
    int target = -1; // channel index of the bus
    float gain = 1;
    bool pre = false;
    std::shared_ptr<DelayLine> delayLine; // delay compensation for this edge (F-MIX-13)
    int delay = 0;
};

/** An insert that processes (bypassed inserts aren't planned, so bypass is bit-exact). */
struct InsertPlan
{
    std::shared_ptr<EffectSlot> slot;
};

/**
    One mixer channel (W-6 signal flow): source, inserts, pre-fader sends, fader, pan,
    mute/solo, post-fader sends, meter, output, each path delayed so all paths reach
    the master equally late (F-MIX-13).
*/
struct ChannelPlan
{
    Id trackId;
    model::TrackKind kind = model::TrackKind::audio;
    int sourceChannels = 2;    // 1 for mono audio tracks
    float gain = 1;            // fader
    float panLeft = 1, panRight = 1; // constant-power pan (mono) or balance (stereo), F-MIX-3
    bool audible = true;       // false when muted, or silenced by another channel's solo (D-W6-1, D-W6-2)
    int output = -1;           // channel index of the destination bus, or -1 for the master
    std::vector<SendPlan> sends;
    std::vector<InsertPlan> inserts;
    int latency = 0;           // samples from timeline to this channel's output: inputs, instrument, inserts
    std::shared_ptr<DelayLine> outputDelayLine;
    int outputDelay = 0;
    std::shared_ptr<ChannelState> state; // fader ramp and meter

    std::vector<AudioSegment> segments; // sorted by start; overlapping only where regions crossfade
    std::int64_t longestSegment = 0;
    std::vector<ScheduledNote> notes;   // sorted by on
    std::vector<ScheduledEvent> controllers; // sorted by at
    std::shared_ptr<InstrumentSlot> instrument;

    // Live input (F-REC-4, F-REC-14).
    int inputFirst = -1;       // first hardware input, -1 for none
    bool armed = false;
    MonitorMode monitor = MonitorMode::autoMode;
    int midiChannel = 0;       // instrument tracks: live MIDI filter, 0 = all
    bool liveMidi = false;     // receives live MIDI (selected or armed, D-W2-1)
};

struct EngineSnapshot
{
    std::uint64_t serial = 0; // set when published
    double sampleRate = 48000; // device rate the positions are in
    int maxBlock = 0;
    TempoMap tempo;            // at the device rate
    SignatureMap signatures;
    bool cycleEnabled = false;
    std::int64_t cycleStart = 0, cycleEnd = 0;
    bool punchEnabled = false;
    std::int64_t punchStart = 0, punchEnd = 0;
    float masterGain = 1;
    std::vector<InsertPlan> masterInserts; // the master's inserts process the whole mix (F-MIX-11)
    std::shared_ptr<ChannelState> masterState;
    int masterLatency = 0;     // the longest path to the master, compensated on every other path
    int fadeFrames = 480;      // delay-compensation crossfade, 10 ms
    std::vector<ChannelPlan> channels; // processing order: every channel before the buses it feeds
    std::vector<MediaPool::Audio> media; // keeps the media sources alive while this snapshot exists

    // Audio-thread scratch, sized at compile time.
    mutable std::vector<std::vector<float>> buffers; // two per channel

    std::int64_t tickToSample (Ticks t) const noexcept;
    int channelIndex (const Id& trackId) const noexcept;
};

/** Local (per-user) mixer and input state that shapes the snapshot but never enters the document. */
using LocalMixState = commands::LocalView;

/** The stateful things a snapshot points at: instruments, inserts and per-channel mixer state. */
struct MixerRacks
{
    const InstrumentRack& instruments;
    const EffectRack& effects;
    MixerState& mixer;
};

/**
    Builds a snapshot of the projected model at the device sample rate. Missing media plays
    as silence; an insert with no slot in the rack is skipped.
*/
std::unique_ptr<EngineSnapshot> compileSnapshot (const model::ProjectModel& model, double deviceRate, int maxBlock, const MediaPool& media,
                                                 MixerRacks racks, const LocalMixState& local = {});

/** Constant-power pan gains for a mono source at pan -1..+1 (centre = -3 dB each side). */
std::pair<float, float> panGains (double pan) noexcept;
/** Balance gains for a stereo source at pan -1..+1 (centre = unity). */
std::pair<float, float> balanceGains (double pan) noexcept;

/** Linear gain for a decibel value; minusInfinityDb and below are silent. */
float dbToGain (double db) noexcept;
} // namespace weft::engine
