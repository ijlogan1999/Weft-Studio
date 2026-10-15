#include "weft/engine/plugins/HostedPlugin.h"

#include <algorithm>

namespace weft::engine
{
HostedPlugin::HostedPlugin (std::unique_ptr<juce::AudioPluginInstance> instance) : plugin (std::move (instance))
{
    params = plugin->getParameters();
}

HostedPlugin::~HostedPlugin()
{
    if (prepared)
        plugin->releaseResources();
}

void HostedPlugin::prepare (double sampleRate, int maxBlockSize)
{
    if (prepared)
        plugin->releaseResources();
    plugin->enableAllBuses();
    plugin->setRateAndBufferSizeDetails (sampleRate, maxBlockSize);
    plugin->prepareToPlay (sampleRate, maxBlockSize);
    const int channels = std::max ({ 2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels() });
    buffer.setSize (channels, std::max (1, maxBlockSize));
    midi.ensureSize (4096);
    prepared = true;
}

void HostedPlugin::process (std::span<const MidiEvent> events, float* const* io, int numChannels, int numFrames) noexcept
{
    if (! prepared || numFrames > buffer.getNumSamples())
        return;
    // A view of the prepared buffer, so nothing is allocated here.
    juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), numFrames);
    const int ours = std::min (numChannels, 2);
    for (int ch = 0; ch < view.getNumChannels(); ++ch)
    {
        if (ch < ours)
            juce::FloatVectorOperations::copy (view.getWritePointer (ch), io[ch], numFrames);
        else
            view.clear (ch, 0, numFrames);
    }
    midi.clear();
    for (const auto& e : events)
        midi.addEvent (juce::MidiMessage (e.status, e.data1, e.data2), std::clamp (e.offset, 0, numFrames - 1));

    plugin->processBlock (view, midi);

    const int outs = std::max (1, plugin->getTotalNumOutputChannels());
    for (int ch = 0; ch < ours; ++ch)
        juce::FloatVectorOperations::copy (io[ch], view.getReadPointer (std::min (ch, outs - 1)), numFrames);
}

int HostedPlugin::latencySamples() const noexcept { return plugin->getLatencySamples(); }

std::vector<ParameterInfo> HostedPlugin::parameters() const
{
    std::vector<ParameterInfo> out;
    for (int i = 0; i < params.size(); ++i)
    {
        auto* p = params[i];
        ParameterInfo info;
        if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (p))
            info.id = hosted->getParameterID().toStdString();
        if (info.id.empty())
            info.id = std::to_string (i);
        info.name = p->getName (64).toStdString();
        info.unit = p->getLabel().toStdString();
        info.defaultValue = p->getDefaultValue();
        info.steps = p->isDiscrete() ? p->getNumSteps() : 0;
        out.push_back (std::move (info));
    }
    return out;
}

void HostedPlugin::setParameter (int index, double normalized) noexcept
{
    if (index >= 0 && index < params.size())
        params[index]->setValue (static_cast<float> (std::clamp (normalized, 0.0, 1.0)));
}

double HostedPlugin::parameter (int index) const noexcept { return index >= 0 && index < params.size() ? params[index]->getValue() : 0.0; }

std::string HostedPlugin::parameterText (int index, double normalized) const
{
    if (index < 0 || index >= params.size())
        return {};
    auto text = params[index]->getText (static_cast<float> (normalized), 32);
    const auto label = params[index]->getLabel();
    if (label.isNotEmpty() && ! text.endsWith (label))
        text << " " << label;
    return text.toStdString();
}

std::vector<std::uint8_t> HostedPlugin::state() const
{
    juce::MemoryBlock block;
    plugin->getStateInformation (block);
    const auto* data = static_cast<const std::uint8_t*> (block.getData());
    return { data, data + block.getSize() };
}

bool HostedPlugin::setState (std::span<const std::uint8_t> bytes)
{
    if (bytes.empty())
        return false;
    plugin->setStateInformation (bytes.data(), static_cast<int> (bytes.size()));
    return true;
}
} // namespace weft::engine
