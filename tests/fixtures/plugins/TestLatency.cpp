// TestLatency: reports 1,000 samples of latency and delays its input by exactly that.

#include "FixtureEditor.h"

class TestLatency final : public juce::AudioProcessor
{
public:
    static constexpr int latency = 1000;

    TestLatency()
        : AudioProcessor (BusesProperties()
                              .withInput ("Input", juce::AudioChannelSet::stereo())
                              .withOutput ("Output", juce::AudioChannelSet::stereo()))
    {
        setLatencySamples (latency);
    }

    WEFT_FIXTURE_NO_EDITOR

    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }

    void prepareToPlay (double, int) override
    {
        setLatencySamples (latency);
        delayLine.setSize (getTotalNumOutputChannels(), latency);
        delayLine.clear();
        writePos = 0;
    }

    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const int channels = juce::jmin (buffer.getNumChannels(), delayLine.getNumChannels());
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                auto* line = delayLine.getWritePointer (ch);
                auto* io = buffer.getWritePointer (ch);
                const float delayed = line[writePos];
                line[writePos] = io[i];
                io[i] = delayed;
            }
            writePos = (writePos + 1) % latency;
        }
    }

    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    juce::AudioBuffer<float> delayLine;
    int writePos = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TestLatency)
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TestLatency(); }
