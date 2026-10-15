#pragma once

// Builds Standard MIDI File bytes for import tests.

#include <cstdint>
#include <string>
#include <vector>

namespace weft::test
{
struct SmfEvent
{
    std::int64_t tick;
    std::vector<std::uint8_t> bytes; // status and data, or 0xff type len... for meta
};

inline void putVarLen (std::vector<std::uint8_t>& out, std::uint32_t v)
{
    std::uint8_t buf[4];
    int n = 0;
    buf[n++] = v & 0x7f;
    while ((v >>= 7) != 0)
        buf[n++] = static_cast<std::uint8_t> ((v & 0x7f) | 0x80);
    while (n > 0)
        out.push_back (buf[--n]);
}

inline void putBe (std::vector<std::uint8_t>& out, std::uint32_t v, int bytes)
{
    for (int i = bytes - 1; i >= 0; --i)
        out.push_back (static_cast<std::uint8_t> ((v >> (8 * i)) & 0xff));
}

inline std::vector<std::uint8_t> smfTrack (std::vector<SmfEvent> events)
{
    std::vector<std::uint8_t> body;
    std::int64_t last = 0;
    for (const auto& e : events)
    {
        putVarLen (body, static_cast<std::uint32_t> (e.tick - last));
        last = e.tick;
        body.insert (body.end(), e.bytes.begin(), e.bytes.end());
    }
    putVarLen (body, 0);
    body.insert (body.end(), { 0xff, 0x2f, 0x00 });
    std::vector<std::uint8_t> out { 'M', 'T', 'r', 'k' };
    putBe (out, static_cast<std::uint32_t> (body.size()), 4);
    out.insert (out.end(), body.begin(), body.end());
    return out;
}

inline std::vector<std::uint8_t> smfFile (int format, int ppq, const std::vector<std::vector<std::uint8_t>>& tracks)
{
    std::vector<std::uint8_t> out { 'M', 'T', 'h', 'd', 0, 0, 0, 6 };
    putBe (out, static_cast<std::uint32_t> (format), 2);
    putBe (out, static_cast<std::uint32_t> (tracks.size()), 2);
    putBe (out, static_cast<std::uint32_t> (ppq), 2);
    for (const auto& t : tracks)
        out.insert (out.end(), t.begin(), t.end());
    return out;
}

inline SmfEvent smfTempo (std::int64_t tick, double bpm)
{
    const auto us = static_cast<std::uint32_t> (60'000'000.0 / bpm + 0.5);
    return { tick, { 0xff, 0x51, 0x03, static_cast<std::uint8_t> (us >> 16), static_cast<std::uint8_t> (us >> 8), static_cast<std::uint8_t> (us) } };
}

inline SmfEvent smfName (const std::string& name)
{
    SmfEvent e { 0, { 0xff, 0x03, static_cast<std::uint8_t> (name.size()) } };
    e.bytes.insert (e.bytes.end(), name.begin(), name.end());
    return e;
}
} // namespace weft::test
