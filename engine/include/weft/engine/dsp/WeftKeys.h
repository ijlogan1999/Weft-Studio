#pragma once

#include "weft/engine/dsp/BuiltinProcessor.h"

#include <array>

namespace weft::engine
{
/**
    Weft Keys, the built-in instrument (F-PLG-9): a 16-voice synth made of a few sine
    partials with an attack/decay/release envelope, in four original presets. Responds to
    note on/off, velocity, sustain (CC64) and all-notes-off (CC123). Released notes fade
    below -90 dBFS within 2 s. A soft limiter keeps the output under 0 dBFS (AT-PLG-7).
*/
class WeftKeys final : public BuiltinProcessor
{
public:
    static constexpr const char* pluginUid = "weft.keys";
    static constexpr int maxVoices = 16;

    struct Preset
    {
        const char* name;
        std::array<double, 3> ratios;   // partial frequencies relative to the note
        std::array<float, 3> levels;
        double attack, decay, release; // seconds: attack ramp, held decay time constant, release time constant
    };
    static const std::array<Preset, 4>& presets();

    WeftKeys();
    void prepare (double sampleRate, int maxBlockSize) override;
    void process (std::span<const MidiEvent> events, float* const* outputs, int numChannels, int numFrames) noexcept override;

    int activeVoices() const noexcept;
    /** A voice is playing this note (held, sustained or releasing). */
    bool sounding (int channel, int pitch) const noexcept;

private:
    struct Voice
    {
        bool active = false, held = false, sustained = false, attacking = false;
        int channel = 1, pitch = 60;
        std::array<double, 3> phase {}, increment {};
        float amplitude = 0, envelope = 0, release = 0;
        std::int64_t age = 0;
    };

    void applyPreset() noexcept;
    void handle (const MidiEvent& e) noexcept;
    void noteOn (int channel, int pitch, int velocity) noexcept;
    void noteOff (int channel, int pitch) noexcept;
    float renderVoice (Voice& v) noexcept;

    std::array<Voice, maxVoices> voices {};
    double rate = 48000;
    float attackStep = 0, decayFactor = 0, releaseFactor = 0;
    std::array<double, 3> ratios { 1, 2, 3 };
    std::array<float, 3> levels { 1, 0, 0 };
    bool sustainPedal = false;
    std::int64_t counter = 0;
};
} // namespace weft::engine
