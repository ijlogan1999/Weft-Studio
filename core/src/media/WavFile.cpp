#include "weft/media/WavFile.h"

#include "weft/rt/AudioThread.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace weft::media
{
namespace
{
std::uint32_t le32 (const std::uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t> (p[3]) << 24); }
std::uint16_t le16 (const std::uint8_t* p) { return static_cast<std::uint16_t> (p[0] | (p[1] << 8)); }

void put32 (std::vector<std::uint8_t>& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back (static_cast<std::uint8_t> (v >> (8 * i)));
}
void put16 (std::vector<std::uint8_t>& out, std::uint16_t v)
{
    out.push_back (static_cast<std::uint8_t> (v));
    out.push_back (static_cast<std::uint8_t> (v >> 8));
}
void putText (std::vector<std::uint8_t>& out, const std::string& s, std::size_t width)
{
    for (std::size_t i = 0; i < width; ++i)
        out.push_back (i < s.size() ? static_cast<std::uint8_t> (s[i]) : 0);
}
std::string text (const std::uint8_t* p, std::size_t width)
{
    std::size_t n = 0;
    while (n < width && p[n] != 0)
        ++n;
    return { reinterpret_cast<const char*> (p), n };
}

bool fail (std::string* error, const std::string& message)
{
    if (error != nullptr)
        *error = message;
    return false;
}

constexpr std::uint16_t formatPcm = 1, formatFloat = 3, formatExtensible = 0xFFFE;
constexpr std::size_t bextSize = 602; // version 1 fields, no coding history
} // namespace

SampleFormat formatForBitDepth (int bitDepth) noexcept
{
    return bitDepth == 16 ? SampleFormat::int16 : (bitDepth == 32 ? SampleFormat::float32 : SampleFormat::int24);
}

std::string mediaFileName (const std::string& blobHash)
{
    const auto colon = blobHash.find (':');
    return (colon == std::string::npos ? blobHash : blobHash.substr (colon + 1)) + ".wav";
}

// Reading ------------------------------------------------------------------------------

bool WavReader::open (const std::filesystem::path& file, std::string* error)
{
    rt::noteBlockingCall();
    wavInfo = {};
    in = std::ifstream (file, std::ios::binary);
    std::array<std::uint8_t, 12> riff {};
    if (! in || ! in.read (reinterpret_cast<char*> (riff.data()), 12))
        return fail (error, "Couldn't open " + file.string());
    if (std::memcmp (riff.data(), "RIFF", 4) != 0 || std::memcmp (riff.data() + 8, "WAVE", 4) != 0)
        return fail (error, file.string() + " is not a WAV file");

    std::uint16_t format = 0;
    std::uint64_t dataSize = 0;
    bool haveData = false;
    std::uint64_t pos = 12;
    std::array<std::uint8_t, 8> header {};
    while (in.seekg (static_cast<std::streamoff> (pos)) && in.read (reinterpret_cast<char*> (header.data()), 8))
    {
        const auto size = static_cast<std::uint64_t> (le32 (header.data() + 4));
        const auto body = pos + 8;
        if (std::memcmp (header.data(), "fmt ", 4) == 0 && size >= 16)
        {
            std::vector<std::uint8_t> fmt (std::min<std::uint64_t> (size, 40));
            in.read (reinterpret_cast<char*> (fmt.data()), static_cast<std::streamsize> (fmt.size()));
            format = le16 (fmt.data());
            wavInfo.channels = le16 (fmt.data() + 2);
            wavInfo.sampleRate = le32 (fmt.data() + 4);
            wavInfo.bitsPerSample = le16 (fmt.data() + 14);
            if (format == formatExtensible && fmt.size() >= 26)
                format = le16 (fmt.data() + 24); // first two bytes of the sub-format GUID
        }
        else if (std::memcmp (header.data(), "bext", 4) == 0 && size >= bextSize - 256 - 64)
        {
            std::vector<std::uint8_t> b (std::min<std::uint64_t> (size, bextSize));
            in.read (reinterpret_cast<char*> (b.data()), static_cast<std::streamsize> (b.size()));
            BextInfo bext;
            bext.description = text (b.data(), 256);
            bext.originator = text (b.data() + 256, 32);
            bext.originatorReference = text (b.data() + 288, 32);
            bext.date = text (b.data() + 320, 10);
            bext.time = text (b.data() + 330, 8);
            bext.timeReference = le32 (b.data() + 338) | (static_cast<std::uint64_t> (le32 (b.data() + 342)) << 32);
            wavInfo.bext = bext;
        }
        else if (std::memcmp (header.data(), "data", 4) == 0)
        {
            dataOffset = body;
            dataSize = size;
            haveData = true;
        }
        pos = body + size + (size & 1u);
    }
    in.clear();
    in.seekg (0, std::ios::end);
    const auto fileSize = static_cast<std::uint64_t> (in.tellg());
    wavInfo.isFloat = format == formatFloat;
    const auto bits = wavInfo.bitsPerSample;
    const bool supported = wavInfo.channels > 0 && wavInfo.sampleRate > 0
                        && ((format == formatPcm && (bits == 16 || bits == 24 || bits == 32)) || (format == formatFloat && bits == 32));
    if (! haveData || ! supported)
        return fail (error, file.string() + ": unsupported WAV format");
    dataSize = std::min (dataSize, fileSize - std::min (fileSize, dataOffset));
    wavInfo.frames = static_cast<std::int64_t> (dataSize / (static_cast<std::uint64_t> (bits / 8) * static_cast<std::uint64_t> (wavInfo.channels)));
    return true;
}

std::int64_t WavReader::read (std::int64_t start, std::int64_t count, float* const* dest)
{
    rt::noteBlockingCall();
    if (start < 0 || start >= wavInfo.frames || count <= 0)
        return 0;
    count = std::min (count, wavInfo.frames - start);
    const auto channels = static_cast<std::size_t> (wavInfo.channels);
    const std::size_t bytesPerSample = static_cast<std::size_t> (wavInfo.bitsPerSample / 8);
    const auto frameBytes = bytesPerSample * channels;
    buffer.resize (static_cast<std::size_t> (count) * frameBytes);
    in.clear();
    in.seekg (static_cast<std::streamoff> (dataOffset + static_cast<std::uint64_t> (start) * frameBytes));
    if (! in.read (reinterpret_cast<char*> (buffer.data()), static_cast<std::streamsize> (buffer.size())))
        count = static_cast<std::int64_t> (static_cast<std::size_t> (in.gcount()) / frameBytes);
    for (std::size_t f = 0; f < static_cast<std::size_t> (count); ++f)
        for (std::size_t c = 0; c < channels; ++c)
        {
            const auto* p = buffer.data() + (f * channels + c) * bytesPerSample;
            float v = 0.0f;
            if (wavInfo.isFloat)
                std::memcpy (&v, p, 4);
            else if (bytesPerSample == 2)
                v = static_cast<float> (static_cast<std::int16_t> (le16 (p))) / 32768.0f;
            else if (bytesPerSample == 3)
            {
                const auto raw = static_cast<std::int32_t> (static_cast<std::uint32_t> (p[0] << 8 | p[1] << 16 | p[2] << 24)) >> 8;
                v = static_cast<float> (raw) / 8388608.0f;
            }
            else
                v = static_cast<float> (static_cast<double> (static_cast<std::int32_t> (le32 (p))) / 2147483648.0);
            dest[c][f] = v;
        }
    return count;
}

std::optional<WavInfo> readWavInfo (const std::filesystem::path& file, std::string* error)
{
    WavReader reader;
    if (! reader.open (file, error))
        return std::nullopt;
    return reader.info();
}

std::optional<AudioData> readWav (const std::filesystem::path& file, std::string* error)
{
    WavReader reader;
    if (! reader.open (file, error))
        return std::nullopt;
    const auto& info = reader.info();
    AudioData audio;
    audio.sampleRate = info.sampleRate;
    audio.channels.assign (static_cast<std::size_t> (info.channels), std::vector<float> (static_cast<std::size_t> (info.frames)));
    std::vector<float*> dest;
    for (auto& c : audio.channels)
        dest.push_back (c.data());
    if (reader.read (0, info.frames, dest.data()) != info.frames)
    {
        fail (error, "Couldn't read " + file.string());
        return std::nullopt;
    }
    return audio;
}

// Writing ------------------------------------------------------------------------------

WavWriter::~WavWriter() { close(); }

bool WavWriter::open (const std::filesystem::path& file, double sampleRate, int channels, SampleFormat format, const BextInfo* bext,
                      std::string* error)
{
    rt::noteBlockingCall();
    close();
    out = std::ofstream (file, std::ios::binary | std::ios::trunc);
    if (! out)
        return fail (error, "Couldn't create " + file.string());
    numChannels = std::max (1, channels);
    sampleFormat = format;
    frames = 0;

    const std::uint16_t bits = format == SampleFormat::int16 ? 16 : (format == SampleFormat::int24 ? 24 : 32);
    const auto bytesPerSample = static_cast<std::uint32_t> (bits / 8u);
    const auto ch = static_cast<std::uint16_t> (numChannels);
    std::vector<std::uint8_t> h;
    h.insert (h.end(), { 'R', 'I', 'F', 'F' });
    put32 (h, 0); // patched on close
    h.insert (h.end(), { 'W', 'A', 'V', 'E' });
    if (bext != nullptr)
    {
        h.insert (h.end(), { 'b', 'e', 'x', 't' });
        put32 (h, static_cast<std::uint32_t> (bextSize));
        putText (h, bext->description, 256);
        putText (h, bext->originator, 32);
        putText (h, bext->originatorReference, 32);
        putText (h, bext->date, 10);
        putText (h, bext->time, 8);
        put32 (h, static_cast<std::uint32_t> (bext->timeReference));
        put32 (h, static_cast<std::uint32_t> (bext->timeReference >> 32));
        put16 (h, 1);                // version
        h.insert (h.end(), 64, 0);   // UMID
        h.insert (h.end(), 10, 0);   // loudness fields (version 2, unused)
        h.insert (h.end(), 180, 0);  // reserved
    }
    h.insert (h.end(), { 'f', 'm', 't', ' ' });
    put32 (h, 16);
    put16 (h, format == SampleFormat::float32 ? formatFloat : formatPcm);
    put16 (h, ch);
    put32 (h, static_cast<std::uint32_t> (sampleRate));
    put32 (h, static_cast<std::uint32_t> (sampleRate) * ch * bytesPerSample);
    put16 (h, static_cast<std::uint16_t> (ch * bytesPerSample));
    put16 (h, bits);
    h.insert (h.end(), { 'd', 'a', 't', 'a' });
    dataSizeOffset = h.size();
    put32 (h, 0); // patched on close
    if (! out.write (reinterpret_cast<const char*> (h.data()), static_cast<std::streamsize> (h.size())))
        return fail (error, "Couldn't write " + file.string());
    return true;
}

bool WavWriter::write (const float* const* channels, std::int64_t count)
{
    rt::noteBlockingCall();
    if (! out.is_open() || count <= 0)
        return out.is_open();
    buffer.clear();
    buffer.reserve (static_cast<std::size_t> (count) * static_cast<std::size_t> (numChannels) * 4);
    for (std::int64_t f = 0; f < count; ++f)
        for (int c = 0; c < numChannels; ++c)
        {
            const float v = channels[c][f];
            if (sampleFormat == SampleFormat::float32)
            {
                std::uint32_t u = 0;
                std::memcpy (&u, &v, 4);
                put32 (buffer, u);
                continue;
            }
            const double clamped = std::clamp (static_cast<double> (v), -1.0, 1.0);
            if (sampleFormat == SampleFormat::int16)
                put16 (buffer, static_cast<std::uint16_t> (static_cast<std::int16_t> (std::lround (std::min (clamped * 32768.0, 32767.0)))));
            else
            {
                const auto s = static_cast<std::uint32_t> (static_cast<std::int32_t> (std::lround (std::min (clamped * 8388608.0, 8388607.0))));
                buffer.push_back (static_cast<std::uint8_t> (s));
                buffer.push_back (static_cast<std::uint8_t> (s >> 8));
                buffer.push_back (static_cast<std::uint8_t> (s >> 16));
            }
        }
    frames += count;
    return static_cast<bool> (out.write (reinterpret_cast<const char*> (buffer.data()), static_cast<std::streamsize> (buffer.size())));
}

bool WavWriter::close()
{
    if (! out.is_open())
        return true;
    rt::noteBlockingCall();
    const std::uint32_t bytesPerSample = sampleFormat == SampleFormat::int16 ? 2 : (sampleFormat == SampleFormat::int24 ? 3 : 4);
    const auto dataBytes = static_cast<std::uint64_t> (frames) * static_cast<std::uint64_t> (numChannels) * bytesPerSample;
    if (dataBytes & 1u)
        out.put (0); // pad byte
    const auto total = static_cast<std::uint64_t> (out.tellp());
    std::vector<std::uint8_t> v;
    put32 (v, static_cast<std::uint32_t> (std::min<std::uint64_t> (total - 8, 0xFFFFFFFFu)));
    out.seekp (static_cast<std::streamoff> (riffSizeOffset));
    out.write (reinterpret_cast<const char*> (v.data()), 4);
    v.clear();
    put32 (v, static_cast<std::uint32_t> (std::min<std::uint64_t> (dataBytes, 0xFFFFFFFFu)));
    out.seekp (static_cast<std::streamoff> (dataSizeOffset));
    out.write (reinterpret_cast<const char*> (v.data()), 4);
    const bool good = static_cast<bool> (out);
    out.close();
    return good;
}

bool writeWav (const std::filesystem::path& file, const AudioData& audio, SampleFormat format, std::string* error)
{
    WavWriter writer;
    if (! writer.open (file, audio.sampleRate, audio.numChannels(), format, nullptr, error))
        return false;
    std::vector<const float*> src;
    for (const auto& c : audio.channels)
        src.push_back (c.data());
    if (! writer.write (src.data(), audio.numFrames()) || ! writer.close())
        return fail (error, "Couldn't write " + file.string());
    return true;
}
} // namespace weft::media
