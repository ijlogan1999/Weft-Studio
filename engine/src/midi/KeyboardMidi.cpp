#include "weft/engine/midi/KeyboardMidi.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace weft::engine
{
namespace
{
// White keys on the home row, black keys on the row above (D-W2-3).
constexpr std::string_view noteKeys = "awsedftgyhujkolp;";

char normalize (char key) noexcept { return static_cast<char> (std::tolower (static_cast<unsigned char> (key))); }
} // namespace

std::optional<int> KeyboardMidi::noteFor (char key) const noexcept
{
    const auto i = noteKeys.find (normalize (key));
    if (i == std::string_view::npos)
        return std::nullopt;
    const int note = 60 + 12 * octave + static_cast<int> (i);
    if (note < 0 || note > 127)
        return std::nullopt;
    return note;
}

void KeyboardMidi::setEnabled (bool enabled, std::vector<Message>& out)
{
    if (! enabled)
        for (auto& h : held)
            if (h != 0)
            {
                out.push_back ({ 0x80, static_cast<std::uint8_t> (h - 1), 64 });
                h = 0;
            }
    on = enabled;
}

bool KeyboardMidi::keyDown (char key, bool repeat, std::vector<Message>& out)
{
    if (! on)
        return false;
    const char k = normalize (key);
    if (k == 'z' || k == 'x')
    {
        if (! repeat)
            octave = std::clamp (octave + (k == 'x' ? 1 : -1), -5, 5);
        return true;
    }
    if (k == 'c' || k == 'v')
    {
        if (! repeat)
            vel = std::clamp (vel + (k == 'v' ? 20 : -20), 1, 127);
        return true;
    }
    const auto note = noteFor (k);
    if (! note)
        return false;
    auto& h = held[static_cast<std::size_t> (static_cast<unsigned char> (k)) & 127];
    if (repeat || h != 0)
        return true; // key repeat doesn't retrigger
    h = *note + 1;
    out.push_back ({ 0x90, static_cast<std::uint8_t> (*note), static_cast<std::uint8_t> (vel) });
    return true;
}

bool KeyboardMidi::keyUp (char key, std::vector<Message>& out)
{
    const char k = normalize (key);
    auto& h = held[static_cast<std::size_t> (static_cast<unsigned char> (k)) & 127];
    if (h != 0)
    {
        // The note the key started, even if the octave changed meanwhile.
        out.push_back ({ 0x80, static_cast<std::uint8_t> (h - 1), 64 });
        h = 0;
        return true;
    }
    return on && (noteFor (k) || k == 'z' || k == 'x' || k == 'c' || k == 'v');
}
} // namespace weft::engine
