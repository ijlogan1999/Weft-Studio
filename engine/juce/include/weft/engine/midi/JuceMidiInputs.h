#pragma once

// MIDI input devices through JUCE (F-UI-8): every connected input is opened and feeds a
// MidiInputRouter, which drops the ones the user disabled. Devices plugged in later appear
// without a restart (AT-UI-6). Message thread only, apart from the devices' own callbacks.

#include "weft/engine/midi/MidiInputs.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace weft::engine
{
class JuceMidiInputs
{
public:
    /** Names of the MIDI inputs connected now. */
    static std::vector<std::string> deviceNames();

    explicit JuceMidiInputs (MidiInputRouter& router);
    ~JuceMidiInputs();

    /** Opens inputs that appeared and closes ones that went away. Called when the device list changes. */
    void refresh();
    /** Called on the message thread after the device list changed. */
    std::function<void()> onDevicesChanged;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace weft::engine
