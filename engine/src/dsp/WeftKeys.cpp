#include "weft/engine/dsp/WeftKeys.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace weft::engine
{
namespace
{
constexpr float silenceLevel = 1.0e-6f; // -120 dBFS: the voice is freed

/** Linear below 0.5, then a smooth curve that approaches but never reaches 1. */
float softLimit (float x) noexcept
{
    const float a = std::abs (x);
    if (a <= 0.5f)
        return x;
    const float y = 0.5f + 0.5f * std::tanh ((a - 0.5f) * 2.0f);
    return x < 0 ? -y : y;
}
} // namespace

// Release time constants stay short so released notes fall below -90 dBFS within 2 s.
const std::array<WeftKeys::Preset, 4>& WeftKeys::presets()
{
    static const std::array<Preset, 4> list { {
        { "Soft Keys", { 1, 2, 3 }, { 1.0f / 1.47f, 0.35f / 1.47f, 0.12f / 1.47f }, 0.004, 1.5, 0.12 },
        { "Glass Bell", { 1, 2.76, 5.40 }, { 0.62f, 0.26f, 0.12f }, 0.002, 2.5, 0.15 },
        { "Reed Organ", { 1, 2, 4 }, { 0.55f, 0.30f, 0.15f }, 0.010, 1.0e6, 0.05 },
        { "Warm Pad", { 1, 1.005, 2 }, { 0.45f, 0.40f, 0.15f }, 0.300, 8.0, 0.14 },
    } };
    return list;
}

WeftKeys::WeftKeys()
    : BuiltinProcessor ({ { "preset", "Preset", "", 0, 3, 0, ParamScale::choice, { "Soft Keys", "Glass Bell", "Reed Organ", "Warm Pad" } },
                          { "level", "Level", "dB", -24, 6, 0, ParamScale::linear, {} } })
{
}

void WeftKeys::applyPreset() noexcept
{
    const auto& p = presets()[static_cast<std::size_t> (std::clamp (static_cast<int> (value (0)), 0, 3))];
    attackStep = static_cast<float> (1.0 / (p.attack * rate));
    decayFactor = static_cast<float> (std::exp (-1.0 / (p.decay * rate)));
    releaseFactor = static_cast<float> (std::exp (-1.0 / (p.release * rate)));
    ratios = p.ratios;
    levels = p.levels;
}

void WeftKeys::prepare (double sampleRate, int)
{
    rate = sampleRate;
    takeChanges();
    applyPreset();
    voices = {};
    sustainPedal = false;
}

int WeftKeys::activeVoices() const noexcept
{
    return static_cast<int> (std::count_if (voices.begin(), voices.end(), [] (const Voice& v) { return v.active; }));
}

bool WeftKeys::sounding (int channel, int pitch) const noexcept
{
    return std::any_of (voices.begin(), voices.end(), [&] (const Voice& v) { return v.active && v.channel == channel && v.pitch == pitch; });
}

void WeftKeys::noteOn (int channel, int pitch, int velocity) noexcept
{
    // Reuse a voice already playing this note, else a free one, else the oldest.
    Voice* target = nullptr;
    for (auto& v : voices)
        if (v.active && v.channel == channel && v.pitch == pitch)
            target = &v;
    if (target == nullptr)
        for (auto& v : voices)
            if (! v.active)
            {
                target = &v;
                break;
            }
    if (target == nullptr)
        target = &*std::min_element (voices.begin(), voices.end(), [] (const Voice& a, const Voice& b) { return a.age < b.age; });

    auto& v = *target;
    v.active = v.held = v.attacking = true;
    v.sustained = false;
    v.channel = channel;
    v.pitch = pitch;
    const double base = 2.0 * std::numbers::pi * 440.0 * std::pow (2.0, (pitch - 69) / 12.0) / rate;
    for (std::size_t k = 0; k < 3; ++k)
    {
        v.phase[k] = 0;
        v.increment[k] = base * ratios[k];
    }
    v.amplitude = 0.25f * static_cast<float> (velocity) / 127.0f;
    v.envelope = 0;
    v.release = 1;
    v.age = ++counter;
}

void WeftKeys::noteOff (int channel, int pitch) noexcept
{
    for (auto& v : voices)
        if (v.active && v.held && v.channel == channel && v.pitch == pitch)
        {
            v.held = false;
            v.sustained = sustainPedal;
        }
}

void WeftKeys::handle (const MidiEvent& e) noexcept
{
    const int channel = (e.status & 0x0f) + 1;
    if (e.isNoteOn())
        noteOn (channel, e.data1, e.data2);
    else if (e.isNoteOff())
        noteOff (channel, e.data1);
    else if ((e.status & 0xf0) == 0xb0)
    {
        if (e.data1 == 64)
        {
            sustainPedal = e.data2 >= 64;
            if (! sustainPedal)
                for (auto& v : voices)
                    v.sustained = false;
        }
        else if (e.data1 == 123 || e.data1 == 120) // all notes off / all sound off
            for (auto& v : voices)
            {
                v.held = v.sustained = false;
                if (e.data1 == 120)
                    v.active = false;
            }
    }
}

float WeftKeys::renderVoice (Voice& v) noexcept
{
    // Attack, then a slow decay while held (or sustained), then a fast release.
    if (v.attacking)
    {
        v.envelope = std::min (1.0f, v.envelope + attackStep);
        v.attacking = v.envelope < 1.0f;
    }
    else if (v.held || v.sustained)
        v.envelope *= decayFactor;
    else
        v.release *= releaseFactor;

    float tone = 0;
    for (std::size_t k = 0; k < 3; ++k)
    {
        tone += levels[k] * static_cast<float> (std::sin (v.phase[k]));
        v.phase[k] += v.increment[k];
        if (v.phase[k] > 2.0 * std::numbers::pi)
            v.phase[k] -= 2.0 * std::numbers::pi;
    }
    const float level = v.amplitude * v.envelope * v.release;
    if (! v.held && ! v.sustained && v.release * v.envelope < silenceLevel)
        v.active = false;
    return tone * level;
}

void WeftKeys::process (std::span<const MidiEvent> events, float* const* outputs, int numChannels, int numFrames) noexcept
{
    if (takeChanges())
        applyPreset();
    const auto level = static_cast<float> (std::pow (10.0, value (1) / 20.0));
    std::size_t next = 0;
    for (int i = 0; i < numFrames; ++i)
    {
        for (; next < events.size() && events[next].offset <= i; ++next)
            handle (events[next]);
        float sum = 0;
        for (auto& v : voices)
            if (v.active)
                sum += renderVoice (v);
        sum = softLimit (sum * level);
        for (int ch = 0; ch < numChannels; ++ch)
            outputs[ch][i] = sum;
    }
}
} // namespace weft::engine
