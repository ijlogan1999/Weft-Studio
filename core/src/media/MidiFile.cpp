#include "weft/media/MidiFile.h"

#include <algorithm>
#include <deque>
#include <fstream>
#include <map>

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
} // namespace weft::media
