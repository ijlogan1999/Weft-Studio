#include "weft/engine/dsp/BuiltinEffects.h"

#include "weft/engine/dsp/WeftKeys.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace weft::engine
{
namespace
{
float dbGain (double db) noexcept { return static_cast<float> (std::pow (10.0, db / 20.0)); }
} // namespace

// Biquad ----------------------------------------------------------------------------

void Biquad::design (Type type, double rate, double freq, double q, double gainDb) noexcept
{
    freq = std::clamp (freq, 10.0, 0.49 * rate);
    q = std::max (q, 0.05);
    const double w = 2.0 * std::numbers::pi * freq / rate;
    const double cw = std::cos (w), sw = std::sin (w);
    const double alpha = sw / (2.0 * q);
    const double A = std::pow (10.0, gainDb / 40.0);
    double B0 = 1, B1 = 0, B2 = 0, A0 = 1, A1 = 0, A2 = 0;
    switch (type)
    {
        case Type::highPass:
            B0 = (1 + cw) / 2, B1 = -(1 + cw), B2 = (1 + cw) / 2, A0 = 1 + alpha, A1 = -2 * cw, A2 = 1 - alpha;
            break;
        case Type::lowPass:
            B0 = (1 - cw) / 2, B1 = 1 - cw, B2 = (1 - cw) / 2, A0 = 1 + alpha, A1 = -2 * cw, A2 = 1 - alpha;
            break;
        case Type::peak:
            B0 = 1 + alpha * A, B1 = -2 * cw, B2 = 1 - alpha * A, A0 = 1 + alpha / A, A1 = -2 * cw, A2 = 1 - alpha / A;
            break;
        case Type::lowShelf:
        {
            const double s = 2 * std::sqrt (A) * alpha;
            B0 = A * ((A + 1) - (A - 1) * cw + s), B1 = 2 * A * ((A - 1) - (A + 1) * cw), B2 = A * ((A + 1) - (A - 1) * cw - s);
            A0 = (A + 1) + (A - 1) * cw + s, A1 = -2 * ((A - 1) + (A + 1) * cw), A2 = (A + 1) + (A - 1) * cw - s;
            break;
        }
        case Type::highShelf:
        {
            const double s = 2 * std::sqrt (A) * alpha;
            B0 = A * ((A + 1) + (A - 1) * cw + s), B1 = -2 * A * ((A - 1) + (A + 1) * cw), B2 = A * ((A + 1) + (A - 1) * cw - s);
            A0 = (A + 1) - (A - 1) * cw + s, A1 = 2 * ((A - 1) - (A + 1) * cw), A2 = (A + 1) - (A - 1) * cw - s;
            break;
        }
    }
    b0 = B0 / A0, b1 = B1 / A0, b2 = B2 / A0, a1 = A1 / A0, a2 = A2 / A0;
}

// EQ -------------------------------------------------------------------------------------

WeftEq::WeftEq()
    : BuiltinProcessor ({
          { "hpOn", "Low Cut", "", 0, 1, 0, ParamScale::choice, { "Off", "On" } },
          { "hpFreq", "Low Cut Freq", "Hz", 20, 2000, 80, ParamScale::logarithmic, {} },
          { "lowFreq", "Low Freq", "Hz", 30, 1000, 120, ParamScale::logarithmic, {} },
          { "lowGain", "Low Gain", "dB", -18, 18, 0, ParamScale::linear, {} },
          { "mid1Freq", "Mid 1 Freq", "Hz", 40, 16000, 500, ParamScale::logarithmic, {} },
          { "mid1Gain", "Mid 1 Gain", "dB", -18, 18, 0, ParamScale::linear, {} },
          { "mid1Q", "Mid 1 Q", "", 0.2, 10, 1, ParamScale::logarithmic, {} },
          { "mid2Freq", "Mid 2 Freq", "Hz", 40, 16000, 2500, ParamScale::logarithmic, {} },
          { "mid2Gain", "Mid 2 Gain", "dB", -18, 18, 0, ParamScale::linear, {} },
          { "mid2Q", "Mid 2 Q", "", 0.2, 10, 1, ParamScale::logarithmic, {} },
          { "highFreq", "High Freq", "Hz", 1000, 18000, 8000, ParamScale::logarithmic, {} },
          { "highGain", "High Gain", "dB", -18, 18, 0, ParamScale::linear, {} },
          { "lpOn", "High Cut", "", 0, 1, 0, ParamScale::choice, { "Off", "On" } },
          { "lpFreq", "High Cut Freq", "Hz", 1000, 20000, 18000, ParamScale::logarithmic, {} },
      })
{
}

void WeftEq::prepare (double sampleRate, int)
{
    rate = sampleRate;
    for (auto& b : bands)
        b.reset();
    design();
}

void WeftEq::design() noexcept
{
    constexpr double butterworthQ = 0.70710678118654752;
    using T = Biquad::Type;
    bands[0].design (T::highPass, rate, value (1), butterworthQ, 0);
    bands[1].design (T::lowShelf, rate, value (2), butterworthQ, value (3));
    bands[2].design (T::peak, rate, value (4), value (6), value (5));
    bands[3].design (T::peak, rate, value (7), value (9), value (8));
    bands[4].design (T::highShelf, rate, value (10), butterworthQ, value (11));
    bands[5].design (T::lowPass, rate, value (13), butterworthQ, 0);
    active = { value (0) > 0.5, value (3) != 0.0, value (5) != 0.0, value (8) != 0.0, value (11) != 0.0, value (12) > 0.5 };
}

void WeftEq::process (std::span<const MidiEvent>, float* const* io, int numChannels, int numFrames) noexcept
{
    if (takeChanges())
        design();
    for (std::size_t b = 0; b < bands.size(); ++b)
        if (active[b])
            for (int ch = 0; ch < std::min (numChannels, 2); ++ch)
                for (int i = 0; i < numFrames; ++i)
                    io[ch][i] = bands[b].run (ch, io[ch][i]);
}

// Compressor ------------------------------------------------------------------------------

WeftCompressor::WeftCompressor()
    : BuiltinProcessor ({
          { "threshold", "Threshold", "dB", -60, 0, -20, ParamScale::linear, {} },
          { "ratio", "Ratio", ":1", 1, 20, 4, ParamScale::logarithmic, {} },
          { "attack", "Attack", "ms", 0.1, 200, 10, ParamScale::logarithmic, {} },
          { "release", "Release", "ms", 5, 2000, 100, ParamScale::logarithmic, {} },
          { "makeup", "Makeup", "dB", 0, 24, 0, ParamScale::linear, {} },
      })
{
}

void WeftCompressor::prepare (double sampleRate, int)
{
    rate = sampleRate;
    envelope = reduction = 0;
    takeChanges();
    attackCoef = static_cast<float> (std::exp (-1.0 / (value (2) * 0.001 * rate)));
    releaseCoef = static_cast<float> (std::exp (-1.0 / (value (3) * 0.001 * rate)));
    detectorCoef = releaseCoef;
}

void WeftCompressor::process (std::span<const MidiEvent>, float* const* io, int numChannels, int numFrames) noexcept
{
    if (takeChanges())
    {
        attackCoef = static_cast<float> (std::exp (-1.0 / (value (2) * 0.001 * rate)));
        releaseCoef = static_cast<float> (std::exp (-1.0 / (value (3) * 0.001 * rate)));
        detectorCoef = releaseCoef;
    }
    const auto threshold = static_cast<float> (value (0));
    const auto slope = static_cast<float> (1.0 - 1.0 / value (1));
    const auto makeup = static_cast<float> (value (4));
    const int chans = std::min (numChannels, 2);
    for (int i = 0; i < numFrames; ++i)
    {
        // Peak detector: instant rise, falls at the release rate.
        float level = 0;
        for (int ch = 0; ch < chans; ++ch)
            level = std::max (level, std::abs (io[ch][i]));
        envelope = level > envelope ? level : undenormal (envelope * detectorCoef);
        const float levelDb = envelope > 1.0e-6f ? 20.0f * std::log10 (envelope) : -120.0f;
        const float target = std::max (0.0f, (levelDb - threshold) * slope);
        // The gain reduction moves toward its target at the attack (more) or release (less) rate.
        const float coef = target > reduction ? attackCoef : releaseCoef;
        reduction = undenormal (target + coef * (reduction - target));
        const float gain = dbGain (makeup - reduction);
        for (int ch = 0; ch < chans; ++ch)
            io[ch][i] *= gain;
    }
}

// Delay -----------------------------------------------------------------------------------

WeftDelay::WeftDelay()
    : BuiltinProcessor ({
          { "time", "Time", "ms", 1, 2000, 375, ParamScale::logarithmic, {} },
          { "feedback", "Feedback", "%", 0, 95, 30, ParamScale::linear, {} },
          { "mix", "Mix", "%", 0, 100, 30, ParamScale::linear, {} },
      })
{
}

void WeftDelay::prepare (double sampleRate, int maxBlockSize)
{
    rate = sampleRate;
    const auto size = static_cast<std::size_t> (std::ceil (2.0 * rate)) + static_cast<std::size_t> (std::max (1, maxBlockSize)) + 1;
    for (auto& l : lines)
        l.assign (size, 0.0f);
    write = 0;
}

void WeftDelay::process (std::span<const MidiEvent>, float* const* io, int numChannels, int numFrames) noexcept
{
    if (lines[0].empty())
        return;
    takeChanges();
    const auto size = lines[0].size();
    const auto delay = std::clamp<std::size_t> (static_cast<std::size_t> (std::llround (value (0) * 0.001 * rate)), 1, size - 1);
    const auto feedback = static_cast<float> (value (1) / 100.0);
    const auto mix = static_cast<float> (value (2) / 100.0);
    const int chans = std::min (numChannels, 2);
    for (int i = 0; i < numFrames; ++i)
    {
        const auto read = (write + size - delay) % size;
        for (int ch = 0; ch < chans; ++ch)
        {
            auto& line = lines[static_cast<std::size_t> (ch)];
            const float x = io[ch][i];
            const float wet = line[read];
            line[write] = undenormal (x + wet * feedback);
            io[ch][i] = x * (1.0f - mix) + wet * mix;
        }
        write = (write + 1) % size;
    }
}

// Reverb ----------------------------------------------------------------------------------

WeftReverb::WeftReverb()
    : BuiltinProcessor ({
          { "decay", "Decay", "s", 0.2, 10, 2, ParamScale::logarithmic, {} },
          { "preDelay", "Pre-delay", "ms", 0, 200, 10, ParamScale::linear, {} },
          { "damping", "Damping", "%", 0, 100, 30, ParamScale::linear, {} },
          { "mix", "Mix", "%", 0, 100, 30, ParamScale::linear, {} },
      })
{
}

void WeftReverb::prepare (double sampleRate, int)
{
    rate = sampleRate;
    // Mutually unrelated lengths in milliseconds; the right side is slightly longer for width.
    constexpr std::array<double, 8> combMs { 25.31, 26.93, 28.97, 30.71, 32.23, 33.79, 35.33, 36.67 };
    constexpr std::array<double, 4> allPassMs { 12.61, 10.03, 7.73, 5.11 };
    for (std::size_t side = 0; side < 2; ++side)
    {
        const double spread = side == 0 ? 0.0 : 0.53;
        for (std::size_t i = 0; i < combs[side].size(); ++i)
        {
            auto& c = combs[side][i];
            c.buffer.assign (static_cast<std::size_t> (std::max (1.0, std::round ((combMs[i] + spread) * 0.001 * rate))), 0.0f);
            c.pos = 0;
            c.store = 0;
        }
        for (std::size_t i = 0; i < allPasses[side].size(); ++i)
        {
            auto& a = allPasses[side][i];
            a.buffer.assign (static_cast<std::size_t> (std::max (1.0, std::round ((allPassMs[i] + spread) * 0.001 * rate))), 0.0f);
            a.pos = 0;
        }
    }
    preDelay.assign (static_cast<std::size_t> (std::ceil (0.2 * rate)) + 1, 0.0f);
    preWrite = 0;
    takeChanges();
    design();
}

void WeftReverb::design() noexcept
{
    // Each comb loses 60 dB over the decay time: g = 10^(-3 L / (T fs)).
    const double decay = value (0);
    const auto damp = static_cast<float> (0.4 * value (2) / 100.0);
    for (auto& side : combs)
        for (auto& c : side)
        {
            c.feedback = static_cast<float> (std::pow (10.0, -3.0 * static_cast<double> (c.buffer.size()) / (decay * rate)));
            c.damp = damp;
        }
}

void WeftReverb::process (std::span<const MidiEvent>, float* const* io, int numChannels, int numFrames) noexcept
{
    if (preDelay.empty())
        return;
    if (takeChanges())
        design();
    const auto mix = static_cast<float> (value (3) / 100.0);
    const auto preSamples = std::min (preDelay.size() - 1, static_cast<std::size_t> (std::llround (value (1) * 0.001 * rate)));
    const int chans = std::min (numChannels, 2);
    constexpr float inputGain = 0.06f, allPassGain = 0.5f;
    for (int i = 0; i < numFrames; ++i)
    {
        const float mono = chans > 1 ? 0.5f * (io[0][i] + io[1][i]) : io[0][i];
        preDelay[preWrite] = mono;
        const float in = preDelay[(preWrite + preDelay.size() - preSamples) % preDelay.size()] * inputGain;
        preWrite = (preWrite + 1) % preDelay.size();
        for (std::size_t side = 0; side < 2; ++side)
        {
            float sum = 0;
            for (auto& c : combs[side])
            {
                const float out = c.buffer[c.pos];
                c.store = undenormal (out * (1.0f - c.damp) + c.store * c.damp);
                c.buffer[c.pos] = in + c.store * c.feedback;
                c.pos = c.pos + 1 == c.buffer.size() ? 0 : c.pos + 1;
                sum += out;
            }
            for (auto& a : allPasses[side])
            {
                const float delayed = a.buffer[a.pos];
                const float v = sum + delayed * allPassGain;
                a.buffer[a.pos] = undenormal (v);
                sum = delayed - v * allPassGain;
                a.pos = a.pos + 1 == a.buffer.size() ? 0 : a.pos + 1;
            }
            if (static_cast<int> (side) < chans)
                io[side][i] = io[side][i] * (1.0f - mix) + sum * mix;
        }
    }
}

// Gain ------------------------------------------------------------------------------------

WeftGain::WeftGain()
    : BuiltinProcessor ({
          { "gain", "Gain", "dB", -60, 24, 0, ParamScale::linear, {} },
          { "invert", "Invert Polarity", "", 0, 1, 0, ParamScale::choice, { "Off", "On" } },
      })
{
}

void WeftGain::process (std::span<const MidiEvent>, float* const* io, int numChannels, int numFrames) noexcept
{
    takeChanges();
    const float g = dbGain (value (0)) * (value (1) > 0.5 ? -1.0f : 1.0f);
    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < numFrames; ++i)
            io[ch][i] *= g;
}

// Placeholder -----------------------------------------------------------------------------

void PlaceholderProcessor::process (std::span<const MidiEvent>, float* const* io, int numChannels, int numFrames) noexcept
{
    if (instrument)
        for (int ch = 0; ch < numChannels; ++ch)
            std::fill_n (io[ch], numFrames, 0.0f);
}

// Registry --------------------------------------------------------------------------------

const std::vector<BuiltinInfo>& builtinPlugins()
{
    static const std::vector<BuiltinInfo> list {
        { WeftEq::pluginUid, "Weft EQ", false },
        { WeftCompressor::pluginUid, "Weft Compressor", false },
        { WeftDelay::pluginUid, "Weft Delay", false },
        { WeftReverb::pluginUid, "Weft Reverb", false },
        { WeftGain::pluginUid, "Weft Gain", false },
        { WeftKeys::pluginUid, "Weft Keys", true },
    };
    return list;
}

std::shared_ptr<Processor> createBuiltin (const std::string& uid)
{
    if (uid == WeftEq::pluginUid)
        return std::make_shared<WeftEq>();
    if (uid == WeftCompressor::pluginUid)
        return std::make_shared<WeftCompressor>();
    if (uid == WeftDelay::pluginUid)
        return std::make_shared<WeftDelay>();
    if (uid == WeftReverb::pluginUid)
        return std::make_shared<WeftReverb>();
    if (uid == WeftGain::pluginUid)
        return std::make_shared<WeftGain>();
    if (uid == WeftKeys::pluginUid)
        return std::make_shared<WeftKeys>();
    return nullptr;
}
} // namespace weft::engine
