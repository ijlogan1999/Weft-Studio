#include "weft/engine/midi/JuceMidiInputs.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <algorithm>
#include <map>

namespace weft::engine
{
struct JuceMidiInputs::Impl final : juce::MidiInputCallback
{
    MidiInputRouter& router;
    std::map<juce::String, std::unique_ptr<juce::MidiInput>> open; // by device identifier
    juce::MidiDeviceListConnection connection;

    explicit Impl (MidiInputRouter& r) : router (r) {}

    void handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& m) override
    {
        const auto* data = m.getRawData();
        const auto size = m.getRawDataSize();
        if (size < 1 || data[0] < 0x80 || data[0] >= 0xf0)
            return; // channel messages only; system messages aren't used yet
        router.deliver (source->getName().toStdString(), data[0], size > 1 ? data[1] : 0, size > 2 ? data[2] : 0);
    }
};

std::vector<std::string> JuceMidiInputs::deviceNames()
{
    std::vector<std::string> names;
    for (const auto& d : juce::MidiInput::getAvailableDevices())
        names.push_back (d.name.toStdString());
    return names;
}

JuceMidiInputs::JuceMidiInputs (MidiInputRouter& router) : impl (std::make_unique<Impl> (router))
{
    impl->connection = juce::MidiDeviceListConnection::make ([this] {
        refresh();
        if (onDevicesChanged)
            onDevicesChanged();
    });
    refresh();
}

JuceMidiInputs::~JuceMidiInputs()
{
    for (auto& [id, input] : impl->open)
        input->stop();
}

void JuceMidiInputs::refresh()
{
    const auto available = juce::MidiInput::getAvailableDevices();
    for (auto it = impl->open.begin(); it != impl->open.end();)
    {
        const bool present = std::any_of (available.begin(), available.end(), [&] (const auto& d) { return d.identifier == it->first; });
        if (! present)
        {
            it->second->stop();
            it = impl->open.erase (it);
        }
        else
            ++it;
    }
    for (const auto& d : available)
    {
        if (impl->open.contains (d.identifier))
            continue;
        if (auto input = juce::MidiInput::openDevice (d.identifier, impl.get()))
        {
            input->start();
            impl->open[d.identifier] = std::move (input);
        }
    }
}
} // namespace weft::engine
