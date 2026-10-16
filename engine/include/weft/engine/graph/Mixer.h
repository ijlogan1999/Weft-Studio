#pragma once

// Mixer state that outlives engine snapshots (ARCHITECTURE §3): fader ramps, meters and
// the delay lines of plug-in delay compensation. Snapshots point at it; the engine owns it.

#include "weft/core/Id.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace weft::engine
{
/**
    A stereo delay for plug-in delay compensation (F-MIX-13). When the delay changes it
    crossfades from the old read position to the new one over `fadeFrames` (10 ms, W-6
    step 4) so the change doesn't click.
*/
class DelayLine
{
public:
    explicit DelayLine (int capacity);
    int capacity() const noexcept { return mask; }
    /** Writes l/r and replaces them with the signal `delay` samples ago. Audio thread. */
    void process (float* l, float* r, int frames, int delay, int fadeFrames) noexcept;
    int currentDelay() const noexcept { return current; }

private:
    float read (std::size_t ch, int delay, int back) const noexcept;
    std::vector<float> buffer[2];
    int mask = 0;
    std::size_t write = 0;
    int current = 0, previous = 0, fadePos = 0, fadeLength = 0;
};

/** Peaks since the last read, and a clip flag that stays set until reset (F-MIX-5). */
struct MeterState
{
    std::atomic<float> peak { 0 };
    std::atomic<float> input { 0 };
    std::atomic<bool> clip { false };

    void add (float value) noexcept
    {
        if (value > peak.load (std::memory_order_relaxed))
            peak.store (value, std::memory_order_relaxed);
        if (value > 1.0f)
            clip.store (true, std::memory_order_relaxed);
    }
};

/** One mixer channel's persistent state, keyed by track id. */
struct ChannelState
{
    MeterState meter;
    // Audio thread: the fader x pan gains reached at the end of the last span (AT-MIX-2 ramps).
    float gainLeft = 0, gainRight = 0;
    bool gainSet = false;
    // Delay lines by edge ("out", or "send:<send id>"). Message thread; snapshots hold shared copies.
    std::map<std::string, std::shared_ptr<DelayLine>> delays;

    /** The delay line for an edge, made (or remade bigger) to hold `delay` samples. */
    std::shared_ptr<DelayLine> delayFor (const std::string& edge, int delay);
};

/** Channel states by track id (the master's under its own id). Message thread. */
class MixerState
{
public:
    std::shared_ptr<ChannelState> channel (const Id& trackId);
    std::shared_ptr<ChannelState> find (const Id& trackId) const;
    void prune (const std::vector<Id>& keep);

private:
    std::map<Id, std::shared_ptr<ChannelState>> channels;
};

/**
    Meter display ballistics (F-MIX-5, AT-MIX-4): the bar follows the peak and falls back
    at a fixed rate, the hold mark shows the highest peak for 2 s, and the clip light stays
    on until clicked. Message thread; feed it the engine's peaks on each UI tick.
*/
class MeterBallistics
{
public:
    static constexpr double holdSeconds = 2.0;
    static constexpr double fallDbPerSecond = 26.0;

    /** A new peak (linear) read at `now` seconds; `clipped` from the engine's sticky flag. */
    void update (float peakLinear, bool clipped, double now) noexcept;
    float levelDb() const noexcept { return level; }
    float holdDb() const noexcept { return hold; }
    bool clip() const noexcept { return clipped; }
    void resetClip() noexcept { clipped = false; }

private:
    float level = -144.0f, hold = -144.0f;
    double holdSince = 0, last = 0;
    bool clipped = false;
};

float gainToDb (float gain) noexcept;
} // namespace weft::engine
