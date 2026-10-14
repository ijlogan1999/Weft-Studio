// TestSynth: VST3 instrument. Each held note outputs a sine at the note's
// equal-tempered frequency (A4 = 440 Hz) with amplitude velocity / 127.

#include "FixtureEditor.h"

#include <array>

class TestSynth final : public juce::AudioProcessor
{
public:
    TestSynth() : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo())) {}

    WEFT_FIXTURE_NO_EDITOR

    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }

    void prepareToPlay (double rate, int) override
    {
        sampleRate = rate;
        for (auto& v : voices)
            v = {};
    }

    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        buffer.clear();
        int pos = 0;
        for (const auto meta : midi)
        {
            render (buffer, pos, meta.samplePosition);
            pos = meta.samplePosition;
            handle (meta.getMessage());
        }
        render (buffer, pos, buffer.getNumSamples());
    }

    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    struct Voice
    {
        int note = -1;
        double phase = 0.0, increment = 0.0;
        float amplitude = 0.0f;
    };

    void handle (const juce::MidiMessage& m)
    {
        if (m.isNoteOn())
        {
            for (auto& v : voices)
                if (v.note < 0)
                {
                    v.note = m.getNoteNumber();
                    v.phase = 0.0;
                    v.increment = juce::MathConstants<double>::twoPi * juce::MidiMessage::getMidiNoteInHertz (v.note) / sampleRate;
                    v.amplitude = static_cast<float> (m.getVelocity()) / 127.0f;
                    return;
                }
        }
        else if (m.isNoteOff())
        {
            for (auto& v : voices)
                if (v.note == m.getNoteNumber())
                    v.note = -1;
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            for (auto& v : voices)
                v.note = -1;
        }
    }

    void render (juce::AudioBuffer<float>& buffer, int start, int end)
    {
        for (auto& v : voices)
        {
            if (v.note < 0)
                continue;
            for (int i = start; i < end; ++i)
            {
                const auto s = v.amplitude * static_cast<float> (std::sin (v.phase));
                v.phase += v.increment;
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    buffer.addSample (ch, i, s);
            }
        }
    }

    double sampleRate = 48000.0;
    std::array<Voice, 16> voices {};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TestSynth)
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TestSynth(); }
