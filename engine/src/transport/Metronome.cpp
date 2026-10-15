#include "weft/engine/transport/Metronome.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace weft::engine
{
namespace
{
std::vector<float> makeClick (double sampleRate, double frequency)
{
    // 30 ms tone with a fast exponential decay. It starts at its peak, so the
    // first sample of a click is its onset.
    const auto length = static_cast<std::size_t> (std::lround (0.030 * sampleRate));
    std::vector<float> click (length);
    const double tau = 0.006 * sampleRate;
    for (std::size_t i = 0; i < length; ++i)
    {
        const double t = static_cast<double> (i);
        click[i] = static_cast<float> (Metronome::peakAtZeroDb * std::cos (2.0 * std::numbers::pi * frequency * t / sampleRate) * std::exp (-t / tau));
    }
    return click;
}
} // namespace

void Metronome::prepare (double sampleRate)
{
    voice = nullptr;
    normal = makeClick (sampleRate, 1000.0);
    accent = makeClick (sampleRate, 1500.0);
}

int Metronome::clicksIn (const EngineSnapshot& snap, std::int64_t from, std::int64_t to, Click* out, int maxClicks) noexcept
{
    if (to <= from || maxClicks <= 0)
        return 0;
    // Start one beat early so rounding between ticks and samples can't skip a click.
    const auto startTick = static_cast<Ticks> (std::floor (snap.tempo.sampleToTick (static_cast<double> (from)))) - 1;
    auto bp = snap.signatures.barAt (startTick);
    // A pointer, not a copy: SignaturePoint holds a string, and copying one may allocate.
    const SignaturePoint* sig = &snap.signatures.signatureAtBar (bp.bar);
    auto beatLen = 4 * ticksPerQuarter / sig->denominator;
    auto beat = static_cast<int> (bp.offset / beatLen);
    auto barStart = snap.signatures.barStart (bp.bar);

    int count = 0;
    for (int guard = 0; guard < 100000 && count < maxClicks; ++guard)
    {
        if (beat >= sig->numerator)
        {
            ++bp.bar;
            sig = &snap.signatures.signatureAtBar (bp.bar);
            beatLen = 4 * ticksPerQuarter / sig->denominator;
            barStart = snap.signatures.barStart (bp.bar);
            beat = 0;
        }
        const auto sample = snap.tickToSample (barStart + beat * beatLen);
        if (sample >= to)
            break;
        if (sample >= from)
            out[count++] = { sample, beat == 0 };
        ++beat;
    }
    return count;
}

void Metronome::render (float* const* outputs, int numChannels, int offset, int frames, const int* clickOffsets, const bool* accents,
                        int numClicks, float gain) noexcept
{
    int next = 0;
    for (int i = 0; i < frames; ++i)
    {
        while (next < numClicks && clickOffsets[next] <= i)
        {
            voice = accents[next] ? &accent : &normal;
            voicePos = 0;
            ++next;
        }
        if (voice == nullptr)
        {
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
