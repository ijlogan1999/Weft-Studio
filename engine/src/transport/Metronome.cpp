#include "weft/engine/transport/Metronome.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace weft::engine
{
namespace
{
std::vector<float> makeClick (double sampleRate, double frequency, float level = 1.0f)
{
    // 30 ms tone with a fast exponential decay. It starts at its peak, so the
    // first sample of a click is its onset.
    const auto length = static_cast<std::size_t> (std::lround (0.030 * sampleRate));
    std::vector<float> click (length);
    const double tau = 0.006 * sampleRate;
    for (std::size_t i = 0; i < length; ++i)
    {
        const double t = static_cast<double> (i);
        click[i] = level * static_cast<float> (Metronome::peakAtZeroDb * std::cos (2.0 * std::numbers::pi * frequency * t / sampleRate) * std::exp (-t / tau));
    }
    return click;
}
} // namespace

void Metronome::prepare (double sampleRate)
{
    voice = nullptr;
    // Tones an octave down to an octave up in half-octave steps; the middle one is the original click.
    static constexpr double scale[numTones] = { 0.5, 0.7071067811865476, 1.0, 1.4142135623730951, 2.0 };
    for (int t = 0; t < numTones; ++t)
    {
        auto& v = voices[static_cast<std::size_t> (t)];
        v[bar] = makeClick (sampleRate, 1500.0 * scale[t]);
        v[beat] = makeClick (sampleRate, 1000.0 * scale[t]);
        v[division] = makeClick (sampleRate, 1000.0 * scale[t], 0.5f); // 6 dB below a beat
    }
}

int Metronome::clicksIn (const EngineSnapshot& snap, std::int64_t from, std::int64_t to, Click* out, int maxClicks, Pattern pattern) noexcept
{
    if (to <= from || maxClicks <= 0 || ! (pattern.bars || pattern.beats || pattern.divisions))
        return 0;
    // Start one beat early so rounding between ticks and samples can't skip a click.
    const auto startTick = static_cast<Ticks> (std::floor (snap.tempo.sampleToTick (static_cast<double> (from)))) - 1;
    auto bp = snap.signatures.barAt (startTick);
    // A pointer, not a copy: SignaturePoint holds a string, and copying one may allocate.
    const SignaturePoint* sig = &snap.signatures.signatureAtBar (bp.bar);
    auto beatLen = 4 * ticksPerQuarter / sig->denominator;
    auto beatIndex = static_cast<int> (bp.offset / beatLen);
    auto barStart = snap.signatures.barStart (bp.bar);
    const int parts = pattern.divisions ? 4 : 1; // a division is a quarter of a beat
    int part = 0;

    int count = 0;
    for (int guard = 0; guard < 400000 && count < maxClicks; ++guard)
    {
        if (part >= parts)
        {
            part = 0;
            ++beatIndex;
        }
        if (beatIndex >= sig->numerator)
        {
            ++bp.bar;
            sig = &snap.signatures.signatureAtBar (bp.bar);
            beatLen = 4 * ticksPerQuarter / sig->denominator;
            barStart = snap.signatures.barStart (bp.bar);
            beatIndex = 0;
        }
        const auto sample = snap.tickToSample (barStart + beatIndex * beatLen + part * beatLen / parts);
        if (sample >= to)
            break;
        // The strongest click this point has among those turned on.
        Level level = division;
        bool sounds = false;
        if (part == 0 && beatIndex == 0 && pattern.bars)
            level = bar, sounds = true;
        else if (part == 0 && pattern.beats)
            level = beat, sounds = true;
        else if (pattern.divisions)
            level = division, sounds = true;
        if (sounds && sample >= from)
            out[count++] = { sample, level == bar, level };
        ++part;
    }
    return count;
}

void Metronome::render (float* const* outputs, int numChannels, int offset, int frames, const int* clickOffsets, const Level* levels,
                        int numClicks, float gain, int tone) noexcept
{
    const auto& set = voices[static_cast<std::size_t> (std::clamp (tone, 0, numTones - 1))];
    int next = 0;
    for (int i = 0; i < frames; ++i)
    {
        while (next < numClicks && clickOffsets[next] <= i)
        {
            voice = &set[static_cast<std::size_t> (std::min<int> (levels[next], division))];
            voicePos = 0;
            ++next;
        }
        if (voice == nullptr || voice->empty())
        {
            voice = nullptr;
            if (next >= numClicks)
                return;
            continue;
        }
        const float v = (*voice)[voicePos] * gain;
        for (int ch = 0; ch < numChannels; ++ch)
            outputs[ch][offset + i] += v;
        if (++voicePos >= voice->size())
            voice = nullptr;
    }
}
} // namespace weft::engine
