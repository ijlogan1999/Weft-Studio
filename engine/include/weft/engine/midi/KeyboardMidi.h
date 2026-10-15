#pragma once

// Computer keyboard as a MIDI input (F-REC-17): one row of letter keys plays an octave
// and a bit, Z/X shift the octave, C/V change the velocity. The UI feeds key events in
// and sends the resulting messages to Engine::sendMidi.

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace weft::engine
{
class KeyboardMidi
{
public:
    struct Message
    {
        std::uint8_t status = 0, data1 = 0, data2 = 0;
    };

    bool enabled() const noexcept { return on; }
    /** Ctrl+K. Turning the mode off releases held notes. */
    void setEnabled (bool enabled, std::vector<Message>& out);
    void toggle (std::vector<Message>& out) { setEnabled (! on, out); }

    /** A key went down (`key` is the character, case-insensitive; `repeat` for auto-repeat). Returns
        true when the mode used the key, so it must not run its normal command. */
    bool keyDown (char key, bool repeat, std::vector<Message>& out);
    bool keyUp (char key, std::vector<Message>& out);

    int octaveShift() const noexcept { return octave; } // -2 .. +3 relative to C3 = 60 on A
    int velocity() const noexcept { return vel; }
    /** The note a key plays at the current octave, if it's a note key. */
    std::optional<int> noteFor (char key) const noexcept;

private:
    bool on = false;
    int octave = 0;
    int vel = 100;
    std::array<int, 128> held {}; // per key character: note + 1 while held, else 0
};
} // namespace weft::engine
