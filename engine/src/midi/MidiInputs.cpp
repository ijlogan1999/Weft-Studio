#include "weft/engine/midi/MidiInputs.h"

#include "weft/engine/transport/Engine.h"

namespace weft::engine
{
void MidiInputRouter::setDisabled (std::set<std::string> deviceNames)
{
    const std::scoped_lock l (lock);
    off = std::move (deviceNames);
}

std::set<std::string> MidiInputRouter::disabled() const
{
    const std::scoped_lock l (lock);
    return off;
}

bool MidiInputRouter::enabled (const std::string& deviceName) const
{
    const std::scoped_lock l (lock);
    return deviceName.empty() || ! off.contains (deviceName);
}

bool MidiInputRouter::deliver (const std::string& deviceName, std::uint8_t status, std::uint8_t data1, std::uint8_t data2)
{
    const std::scoped_lock l (lock);
    if (! deviceName.empty() && off.contains (deviceName))
        return false;
    return eng.sendMidi (status, data1, data2);
}
} // namespace weft::engine
