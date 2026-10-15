#include "weft/engine/dsp/WeftKeys.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace weft::engine
{
namespace
{
constexpr float silenceLevel = 1.0e-6f; // -120 dBFS: the voice is freed
constexpr double releaseSeconds = 0.12;  // time constant: -90 dB after about 1.25 s
constexpr double decaySeconds = 1.5;     // held notes fade like a struck key
} // namespace

void WeftKeys::prepare (double sampleRate, int)
{
    rate = sampleRate;
    attackStep = static_cast<float> (1.0 / (0.004 * rate)); // 4 ms attack
    decayFactor = static_cast<float> (std::exp (-1.0 / (decaySeconds * rate)));
    releaseFactor = static_cast<float> (std::exp (-1.0 / (releaseSeconds * rate)));
    voices = {};
    sustainPedal = false;
}

int WeftKeys::activeVoices() const noexcept
{
    return static_cast<int> (std::count_if (voices.begin(), voices.end(), [] (const Voice& v) { return v.active; }));
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
    v.phase = 0;
    v.increment = 2.0 * std::numbers::pi * 440.0 * std::pow (2.0, (pitch - 69) / 12.0) / rate;
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

    const auto p = v.phase;
    const float tone = static_cast<float> (std::sin (p) + 0.35 * std::sin (2.0 * p) + 0.12 * std::sin (3.0 * p)) / 1.47f;
    v.phase += v.increment;
    if (v.phase > 2.0 * std::numbers::pi)
        v.phase -= 2.0 * std::numbers::pi;
    const float level = v.amplitude * v.envelope * v.release;
    if (! v.held && ! v.sustained && v.release * v.envelope < silenceLevel)
        v.active = false;
    return tone * level;
}

void WeftKeys::process (std::span<const MidiEvent> events, float* const* outputs, int numChannels, int numFrames) noexcept
{
    std::size_t next = 0;
    for (int i = 0; i < numFrames; ++i)
    {
        for (; next < events.size() && events[next].offset <= i; ++next)
            handle (events[next]);
        float sum = 0;
        for (auto& v : voices)
            if (v.active)
                sum += renderVoice (v);
        for (int ch = 0; ch < numChannels; ++ch)
            outputs[ch][i] = sum;
    }
}
} // namespace weft::engine
