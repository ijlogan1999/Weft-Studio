#pragma once

#include "weft/engine/graph/EngineSnapshot.h"

#include <cstdint>
#include <vector>

namespace weft::engine
{
/**
    The click (F-TRN-7). Sounds are synthesized, not samples: a short decaying
    tone, higher for the accent on each bar's first beat, at the same peak level.
    Beats follow the signature's denominator (6/8 clicks every eighth note).
*/
class Metronome
{
public:
    /** Peak level of a click at the 0 dB setting. */
    static constexpr float peakAtZeroDb = 0.5f;

    /** Message thread: builds the click sounds for the device rate. */
    void prepare (double sampleRate);

    struct Click
    {
        std::int64_t sample = 0;
        bool accent = false;
    };

    /** Clicks with a timeline sample in [from, to), in order (audio-thread safe). Returns how many were written. */
    static int clicksIn (const EngineSnapshot& snapshot, std::int64_t from, std::int64_t to, Click* out, int maxClicks) noexcept;

    /** Adds the click sound to outputs[ch][offset .. offset + frames), starting the given clicks at
        their offsets (relative to offset, sorted). Audio thread. */
    void render (float* const* outputs, int numChannels, int offset, int frames, const int* clickOffsets, const bool* accents,
                 int numClicks, float gain) noexcept;

    /** Stops a sounding click (on locate). */
    void silence() noexcept { voice = nullptr; }

private:
    std::vector<float> normal, accent;
    const std::vector<float>* voice = nullptr;
    std::size_t voicePos = 0;
};
} // namespace weft::engine
