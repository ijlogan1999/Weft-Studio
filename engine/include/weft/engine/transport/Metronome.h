#pragma once

#include "weft/engine/graph/EngineSnapshot.h"

#include <array>
#include <cstdint>
#include <vector>

namespace weft::engine
{
/**
    The click (F-TRN-7). Sounds are synthesized, not samples: a short decaying
    tone, higher for the accent on each bar's first beat, at the same peak level.
    Beats follow the signature's denominator (6/8 clicks every eighth note).
    D-W1-11: the bar, beat and division clicks can each be turned off (a division is a quarter of a beat,
    softer than a beat), and the tone moves all three in pitch.
*/
class Metronome
{
public:
    /** Peak level of a click at the 0 dB setting. */
    static constexpr float peakAtZeroDb = 0.5f;

    /** Message thread: builds the click sounds for the device rate. */
    void prepare (double sampleRate);

    /** What a click marks: the first beat of a bar (the accent), another beat, or a division of a beat. */
    enum Level : std::uint8_t
    {
        bar,
        beat,
        division
    };
    struct Click
    {
        std::int64_t sample = 0;
        bool accent = false;
        Level level = beat;
    };
    /** Which clicks sound (D-W1-11). With bars off, a bar's first beat clicks as a beat if beats are on. */
    struct Pattern
    {
        bool bars = true, beats = true, divisions = false;
    };
    static constexpr int numTones = 5, middleTone = 2;

    /** Clicks with a timeline sample in [from, to), in order (audio-thread safe). Returns how many were written. */
    static int clicksIn (const EngineSnapshot& snapshot, std::int64_t from, std::int64_t to, Click* out, int maxClicks, Pattern pattern) noexcept;
    static int clicksIn (const EngineSnapshot& snapshot, std::int64_t from, std::int64_t to, Click* out, int maxClicks) noexcept
    {
        return clicksIn (snapshot, from, to, out, maxClicks, Pattern {});
    }

    /** Adds the click sound to outputs[ch][offset .. offset + frames), starting the given clicks at
        their offsets (relative to offset, sorted). `tone` is 0 (lowest) to numTones - 1. Audio thread. */
    void render (float* const* outputs, int numChannels, int offset, int frames, const int* clickOffsets, const Level* levels,
                 int numClicks, float gain, int tone = middleTone) noexcept;

    /** Stops a sounding click (on locate). */
    void silence() noexcept { voice = nullptr; }

private:
    std::array<std::array<std::vector<float>, 3>, numTones> voices; // [tone][level]
    const std::vector<float>* voice = nullptr;
    std::size_t voicePos = 0;
};
} // namespace weft::engine
