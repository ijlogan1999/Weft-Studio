#include "weft/media/MidiFile.h"

#include <algorithm>
#include <deque>
#include <fstream>
#include <cmath>
#include <map>
#include <tuple>

namespace weft::media
{
namespace
{
struct Reader
{
    std::span<const std::uint8_t> bytes;
    std::size_t pos = 0;
    bool bad = false;

    bool has (std::size_t n) const noexcept { return pos + n <= bytes.size(); }
    std::uint8_t u8()
    {
        if (! has (1))
        {
            bad = true;
            return 0;
        }
        return bytes[pos++];
    }
    std::uint32_t be (int n)
    {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i)
            v = (v << 8) | u8();
        return v;
    }
    std::uint32_t varLen()
    {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
        {
            const auto b = u8();
            v = (v << 7) | (b & 0x7f);
            if ((b & 0x80) == 0)
                return v;
        }
        bad = true;
        return v;
    }
};

std::optional<MidiFileData> failWith (std::string* error, const char* why)
{
    if (error != nullptr)
        *error = why;
    return std::nullopt;
}
} // namespace

std::optional<MidiFileData> parseMidiFile (std::span<const std::uint8_t> bytes, std::string* error)
{
    Reader in { bytes };
    if (in.be (4) != 0x4d546864) // "MThd"
        return failWith (error, "Not a Standard MIDI File.");
    const auto headerLength = in.be (4);
    MidiFileData data;
    data.format = static_cast<int> (in.be (2));
    const auto trackCount = in.be (2);
    const auto division = in.be (2);
    if (in.bad || headerLength < 6 || data.format > 2)
        return failWith (error, "The MIDI file header is damaged.");
    if (data.format == 2)
        return failWith (error, "Type 2 MIDI files aren't supported.");
    if ((division & 0x8000) != 0 || division == 0)
        return failWith (error, "MIDI files with SMPTE timing aren't supported.");
    data.ticksPerQuarter = static_cast<int> (division);
    in.pos = 8 + headerLength;

    for (std::uint32_t t = 0; t < trackCount && in.has (8); ++t)
    {
        const auto id = in.be (4);
        const auto length = in.be (4);
        const auto end = std::min<std::size_t> (in.pos + length, bytes.size());
        if (id != 0x4d54726b) // "MTrk": skip unknown chunks
        {
            in.pos = end;
            --t;
            continue;
        }
        MidiFileTrack track;
        std::int64_t tick = 0;
        std::uint8_t status = 0;
        std::map<std::pair<int, int>, std::deque<std::size_t>> held; // (channel, pitch) -> open notes
        auto closeNote = [&] (int channel, int pitch, int releaseVelocity) {
            auto it = held.find ({ channel, pitch });
            if (it == held.end() || it->second.empty())
                return;
            auto& n = track.notes[it->second.front()];
            n.length = std::max<std::int64_t> (1, tick - n.tick);
            n.releaseVelocity = releaseVelocity;
            it->second.pop_front();
        };
        while (in.pos < end && ! in.bad)
        {
            tick += in.varLen();
            auto b = in.u8();
            if (b == 0xff)
            {
                const auto type = in.u8();
                const auto len = in.varLen();
                if (! in.has (len))
                    break;
                const auto* p = bytes.data() + in.pos;
                if (type == 0x2f)
                {
                    in.pos += len;
                    break;
                }
                if (type == 0x03 && track.name.empty())
                    track.name.assign (reinterpret_cast<const char*> (p), len);
                else if (type == 0x51 && len == 3)
                {
                    const auto usPerQuarter = (std::uint32_t { p[0] } << 16) | (std::uint32_t { p[1] } << 8) | p[2];
                    if (usPerQuarter > 0)
                        data.tempos.push_back ({ tick, 60'000'000.0 / static_cast<double> (usPerQuarter) });
                }
                else if (type == 0x58 && len >= 2)
                    data.signatures.push_back ({ tick, p[0], 1 << std::min<int> (p[1], 6) });
                else if (type == 0x59 && len == 2)
                    data.keys.push_back ({ tick, static_cast<std::int8_t> (p[0]), p[1] != 0 });
                in.pos += len;
                continue;
            }
            if (b == 0xf0 || b == 0xf7)
            {
                in.pos += in.varLen();
                continue;
            }
            std::uint8_t d1;
            if (b & 0x80)
            {
                status = b;
                d1 = in.u8();
            }
            else
            {
                if (status == 0)
                    return failWith (error, "The MIDI file has a damaged track.");
                d1 = b;
            }
            const int kind = status & 0xf0;
            const int channel = (status & 0x0f) + 1;
            const bool twoBytes = kind != 0xc0 && kind != 0xd0;
            const std::uint8_t d2 = twoBytes ? in.u8() : 0;
            switch (kind)
            {
                case 0x90:
                    if (d2 > 0)
                    {
                        held[{ channel, d1 }].push_back (track.notes.size());
                        track.notes.push_back ({ tick, 0, channel, d1, d2, 64 });
                        break;
                    }
                    closeNote (channel, d1, 64);
                    break;
                case 0x80: closeNote (channel, d1, d2); break;
                case 0xa0: track.controllers.push_back ({ tick, "polyPressure", d1, d2, channel }); break;
                case 0xb0: track.controllers.push_back ({ tick, "cc", d1, d2, channel }); break;
                case 0xc0: track.controllers.push_back ({ tick, "programChange", 0, d1, channel }); break;
                case 0xd0: track.controllers.push_back ({ tick, "channelPressure", 0, d1, channel }); break;
                case 0xe0: track.controllers.push_back ({ tick, "pitchBend", 0, ((d2 << 7) | d1) - 8192, channel }); break;
                default: break;
            }
        }
        for (auto& [key, open] : held)
            for (const auto i : open)
                track.notes[i].length = std::max<std::int64_t> (1, tick - track.notes[i].tick);
        std::erase_if (track.notes, [] (const MidiFileNote& n) { return n.length <= 0; });
        data.tracks.push_back (std::move (track));
        in.pos = end;
    }
    if (in.bad && data.tracks.empty())
        return failWith (error, "The MIDI file is damaged.");
    auto byTick = [] (const auto& a, const auto& b) { return a.tick < b.tick; };
    std::stable_sort (data.tempos.begin(), data.tempos.end(), byTick);
    std::stable_sort (data.signatures.begin(), data.signatures.end(), byTick);
    return data;
}

std::optional<MidiFileData> readMidiFile (const std::filesystem::path& file, std::string* error)
{
    std::ifstream in (file, std::ios::binary);
    if (! in)
        return failWith (error, "Couldn't open the file.");
    std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    return parseMidiFile (bytes, error);
}

// Writing ------------------------------------------------------------------------------

namespace
{
struct TimedEvent
{
    std::int64_t tick = 0;
    int order = 0; // at one tick: meta, then note-offs, controllers, note-ons
    std::vector<std::uint8_t> bytes;
};

void putVarLen (std::vector<std::uint8_t>& out, std::uint64_t v)
{
    std::uint8_t buf[10];
    int n = 0;
    buf[n++] = static_cast<std::uint8_t> (v & 0x7f);
    while ((v >>= 7) != 0)
        buf[n++] = static_cast<std::uint8_t> (0x80 | (v & 0x7f));
    while (n > 0)
        out.push_back (buf[--n]);
}

void put32 (std::vector<std::uint8_t>& out, std::uint32_t v)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back (static_cast<std::uint8_t> ((v >> shift) & 0xff));
}

std::uint8_t data7 (int v) { return static_cast<std::uint8_t> (std::clamp (v, 0, 127)); }
std::uint8_t status (int high, int channel) { return static_cast<std::uint8_t> (high | ((std::clamp (channel, 1, 16) - 1) & 0x0f)); }

TimedEvent meta (std::int64_t tick, std::uint8_t type, std::vector<std::uint8_t> body)
{
    TimedEvent e { tick, 0, { 0xff, type } };
    putVarLen (e.bytes, body.size());
    e.bytes.insert (e.bytes.end(), body.begin(), body.end());
    return e;
}

void addConductor (std::vector<TimedEvent>& events, const MidiFileData& data)
{
    for (const auto& t : data.tempos)
    {
        const auto us = static_cast<std::uint32_t> (std::llround (60'000'000.0 / std::max (t.bpm, 1.0)));
        events.push_back (meta (t.tick, 0x51, { static_cast<std::uint8_t> (us >> 16), static_cast<std::uint8_t> (us >> 8), static_cast<std::uint8_t> (us) }));
    }
    for (const auto& s : data.signatures)
    {
        std::uint8_t power = 0;
        while ((1 << (power + 1)) <= s.denominator)
            ++power;
        events.push_back (meta (s.tick, 0x58, { static_cast<std::uint8_t> (s.numerator), power, 24, 8 }));
    }
    for (const auto& k : data.keys)
        events.push_back (meta (k.tick, 0x59, { static_cast<std::uint8_t> (static_cast<std::int8_t> (std::clamp (k.fifths, -7, 7))),
                                                static_cast<std::uint8_t> (k.minor ? 1 : 0) }));
}

void addTrack (std::vector<TimedEvent>& events, const MidiFileTrack& track)
{
    if (! track.name.empty())
        events.push_back (meta (0, 0x03, std::vector<std::uint8_t> (track.name.begin(), track.name.end())));
    for (const auto& n : track.notes)
    {
        events.push_back ({ n.tick, 3, { status (0x90, n.channel), data7 (n.pitch), static_cast<std::uint8_t> (std::clamp (n.velocity, 1, 127)) } });
        events.push_back ({ n.tick + std::max<std::int64_t> (1, n.length), 1, { status (0x80, n.channel), data7 (n.pitch), data7 (n.releaseVelocity) } });
    }
    for (const auto& c : track.controllers)
    {
        TimedEvent e { c.tick, 2, {} };
        if (c.kind == "pitchBend")
        {
            const auto v = std::clamp (c.value + 8192, 0, 16383);
            e.bytes = { status (0xe0, c.channel), static_cast<std::uint8_t> (v & 0x7f), static_cast<std::uint8_t> (v >> 7) };
        }
        else if (c.kind == "programChange")
            e.bytes = { status (0xc0, c.channel), data7 (c.value) };
        else if (c.kind == "channelPressure")
            e.bytes = { status (0xd0, c.channel), data7 (c.value) };
        else if (c.kind == "polyPressure")
            e.bytes = { status (0xa0, c.channel), data7 (c.number), data7 (c.value) };
        else
            e.bytes = { status (0xb0, c.channel), data7 (c.number), data7 (c.value) };
        events.push_back (std::move (e));
    }
}

void putTrackChunk (std::vector<std::uint8_t>& out, std::vector<TimedEvent> events)
{
    std::stable_sort (events.begin(), events.end(), [] (const TimedEvent& a, const TimedEvent& b) { return std::tie (a.tick, a.order) < std::tie (b.tick, b.order); });
    std::vector<std::uint8_t> body;
    std::int64_t at = 0;
    for (const auto& e : events)
    {
        const auto tick = std::max<std::int64_t> (e.tick, 0);
        putVarLen (body, static_cast<std::uint64_t> (tick - at));
        at = tick;
        body.insert (body.end(), e.bytes.begin(), e.bytes.end());
    }
    body.insert (body.end(), { 0x00, 0xff, 0x2f, 0x00 }); // end of track
    out.insert (out.end(), { 'M', 'T', 'r', 'k' });
    put32 (out, static_cast<std::uint32_t> (body.size()));
    out.insert (out.end(), body.begin(), body.end());
}
} // namespace

std::vector<std::uint8_t> encodeMidiFile (const MidiFileData& data)
{
    const bool single = data.format == 0;
    std::vector<std::uint8_t> out { 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, static_cast<std::uint8_t> (single ? 0 : 1) };
    const auto chunks = single ? 1 : 1 + data.tracks.size();
    out.push_back (static_cast<std::uint8_t> (chunks >> 8));
    out.push_back (static_cast<std::uint8_t> (chunks & 0xff));
    const auto ppq = std::clamp (data.ticksPerQuarter, 1, 0x7fff);
    out.push_back (static_cast<std::uint8_t> (ppq >> 8));
    out.push_back (static_cast<std::uint8_t> (ppq & 0xff));
    std::vector<TimedEvent> conductor;
    addConductor (conductor, data);
    if (single)
    {
        for (const auto& t : data.tracks)
            addTrack (conductor, t);
        putTrackChunk (out, std::move (conductor));
        return out;
    }
    putTrackChunk (out, std::move (conductor));
    for (const auto& t : data.tracks)
    {
        std::vector<TimedEvent> events;
        addTrack (events, t);
        putTrackChunk (out, std::move (events));
    }
    return out;
}

bool writeMidiFile (const std::filesystem::path& file, const MidiFileData& data, std::string* error)
{
    const auto bytes = encodeMidiFile (data);
    std::ofstream out (file, std::ios::binary | std::ios::trunc);
    if (! out || ! out.write (reinterpret_cast<const char*> (bytes.data()), static_cast<std::streamsize> (bytes.size())))
    {
        if (error != nullptr)
            *error = "Couldn't write " + file.filename().string() + ".";
        return false;
    }
    return true;
}
} // namespace weft::media
