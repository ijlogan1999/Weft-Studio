#pragma once

// Live MIDI from several sources into the engine (F-UI-8): MIDI devices call from their own
// threads and the computer keyboard from the message thread, but Engine::sendMidi takes one
// producer, so a mutex serializes them (none of them is the audio thread). Inputs the user
// disabled in preferences are dropped here.

#include <cstdint>
#include <mutex>
#include <set>
#include <string>

namespace weft::engine
{
class Engine;

class MidiInputRouter
{
public:
    explicit MidiInputRouter (Engine& engine) : eng (engine) {}

    /** Inputs whose messages are dropped, by device name. Any thread. */
    void setDisabled (std::set<std::string> deviceNames);
    std::set<std::string> disabled() const;
    bool enabled (const std::string& deviceName) const;

    /** A message from a device (empty name: the computer keyboard, always on). Any thread but the audio thread.
        False when it was dropped (disabled input, or the engine's queue is full). */
    bool deliver (const std::string& deviceName, std::uint8_t status, std::uint8_t data1, std::uint8_t data2);

private:
    Engine& eng;
    mutable std::mutex lock;
    std::set<std::string> off;
};
} // namespace weft::engine
