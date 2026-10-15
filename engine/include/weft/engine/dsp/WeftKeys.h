#pragma once

#include "weft/engine/graph/Instrument.h"

#include <array>

namespace weft::engine
{
/**
    Weft Keys, the built-in instrument (F-PLG-9): a 16-voice keyboard voice made of
    a few decaying sine partials. Responds to note on/off, velocity, sustain (CC64)
    and all-notes-off (CC123). Released notes fade below -90 dBFS within 2 s.
*/
class WeftKeys final : public Instrument
{
public:
    static constexpr const char* pluginUid = "weft.keys";
    static constexpr int maxVoices = 16;

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (std::span<const MidiEvent> events, float* const* outputs, int numChannels, int numFrames) noexcept override;

    int activeVoices() const noexcept;

private:
    struct Voice
    {
        bool active = false, held = false, sustained = false, attacking = false;
        int channel = 1, pitch = 60;
        double phase = 0, increment = 0;
        float amplitude = 0, envelope = 0, release = 0;
        std::int64_t age = 0;
    };

    void handle (const MidiEvent& e) noexcept;
    void noteOn (int channel, int pitch, int velocity) noexcept;
    void noteOff (int channel, int pitch) noexcept;
    float renderVoice (Voice& v) noexcept;

    std::array<Voice, maxVoices> voices {};
    double rate = 48000;
    float attackStep = 0, decayFactor = 0, releaseFactor = 0;
    bool sustainPedal = false;
    std::int64_t counter = 0;
};
} // namespace weft::engine
