// TestGain: VST3 effect with one gain parameter in dB, zero latency.

#include "FixtureEditor.h"

class TestGain final : public juce::AudioProcessor
{
public:
    TestGain()
        : AudioProcessor (BusesProperties()
                              .withInput ("Input", juce::AudioChannelSet::stereo())
                              .withOutput ("Output", juce::AudioChannelSet::stereo()))
    {
        addParameter (gainDb = new juce::AudioParameterFloat (juce::ParameterID { "gain", 1 }, "Gain",
                                                              juce::NormalisableRange<float> (-60.0f, 12.0f), 0.0f,
                                                              juce::AudioParameterFloatAttributes().withLabel ("dB")));
    }

    WEFT_FIXTURE_NO_EDITOR

    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        buffer.applyGain (juce::Decibels::decibelsToGain (gainDb->get(), -1000.0f));
    }

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        juce::MemoryOutputStream (dest, false).writeFloat (gainDb->get());
    }

    void setStateInformation (const void* data, int size) override
    {
        if (size >= static_cast<int> (sizeof (float)))
            *gainDb = juce::MemoryInputStream (data, static_cast<size_t> (size), false).readFloat();
    }

private:
    juce::AudioParameterFloat* gainDb = nullptr;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TestGain)
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TestGain(); }
