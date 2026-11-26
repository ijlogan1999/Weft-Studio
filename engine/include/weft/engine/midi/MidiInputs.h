#pragma once

// Live MIDI from several sources into the engine (F-UI-8): MIDI devices call from their own
// threads and the computer keyboard from the message thread, but Engine::sendMidi takes one
// producer, so a mutex serializes them (none of them is the audio thread). Inputs the user
// disabled in preferences are dropped here.

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

namespace weft::engine
{
class Engine;

/** A device's number for a track's MIDI input filter (P-TRI-9): a stable hash of its name, never 0
    (0 means the computer keyboard when sending, and every device when filtering). */
constexpr std::uint32_t midiSourceId (std::string_view deviceName) noexcept
{
    if (deviceName.empty() || deviceName == "all")
        return 0;
    std::uint32_t h = 2166136261u;
    for (const char c : deviceName)
        h = (h ^ static_cast<std::uint8_t> (c)) * 16777619u;
    return h == 0 ? 1 : h;
}

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
