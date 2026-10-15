#pragma once

// A VST3 plug-in as a Weft processor. JUCE types: only the app's editor windows and
// tests include this; the engine sees a Processor.

#include "weft/engine/graph/Instrument.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace weft::engine
{
class HostedPlugin final : public Processor
{
public:
    explicit HostedPlugin (std::unique_ptr<juce::AudioPluginInstance> instance);
    ~HostedPlugin() override;

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (std::span<const MidiEvent> events, float* const* io, int numChannels, int numFrames) noexcept override;
    int latencySamples() const noexcept override;
    std::vector<ParameterInfo> parameters() const override;
    void setParameter (int index, double normalized) noexcept override;
    double parameter (int index) const noexcept override;
    std::string parameterText (int index, double normalized) const override;
    std::vector<std::uint8_t> state() const override;
    bool setState (std::span<const std::uint8_t> bytes) override;

    juce::AudioPluginInstance& instance() noexcept { return *plugin; }
    bool hasEditor() const { return plugin->hasEditor(); }

private:
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::AudioBuffer<float> buffer;
    juce::MidiBuffer midi;
    juce::Array<juce::AudioProcessorParameter*> params;
    bool prepared = false;
};
} // namespace weft::engine
