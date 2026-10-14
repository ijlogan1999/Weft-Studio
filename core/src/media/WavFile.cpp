#include "weft/media/WavFile.h"

#include "weft/rt/AudioThread.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

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

bool fail (std::string* error, const std::string& message)
{
    if (error != nullptr)
        *error = message;
    return false;
}

constexpr std::uint16_t formatPcm = 1, formatFloat = 3, formatExtensible = 0xFFFE;
} // namespace

std::string mediaFileName (const std::string& blobHash)
{
    const auto colon = blobHash.find (':');
    return (colon == std::string::npos ? blobHash : blobHash.substr (colon + 1)) + ".wav";
}

std::optional<AudioData> readWav (const std::filesystem::path& file, std::string* error)
{
    rt::noteBlockingCall();
    std::ifstream in (file, std::ios::binary);
    if (! in)
    {
        fail (error, "Couldn't open " + file.string());
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (in)), {});
    if (bytes.size() < 12 || std::memcmp (bytes.data(), "RIFF", 4) != 0 || std::memcmp (bytes.data() + 8, "WAVE", 4) != 0)
    {
        fail (error, file.string() + " is not a WAV file");
        return std::nullopt;
    }

    std::uint16_t format = 0, channels = 0, bits = 0;
    std::uint32_t rate = 0;
    const std::uint8_t* data = nullptr;
    std::size_t dataSize = 0;
    for (std::size_t pos = 12; pos + 8 <= bytes.size();)
    {
        const auto* chunk = bytes.data() + pos;
        const auto size = static_cast<std::size_t> (le32 (chunk + 4));
        const auto body = pos + 8;
        const auto available = std::min (size, bytes.size() - body);
        if (std::memcmp (chunk, "fmt ", 4) == 0 && available >= 16)
        {
            format = le16 (chunk + 8);
            channels = le16 (chunk + 10);
            rate = le32 (chunk + 12);
            bits = le16 (chunk + 22);
            if (format == formatExtensible && available >= 26)
                format = le16 (chunk + 8 + 24); // first two bytes of the sub-format GUID
        }
        else if (std::memcmp (chunk, "data", 4) == 0)
        {
            data = chunk + 8;
            dataSize = available;
        }
        pos = body + size + (size & 1u);
    }
    const bool supported = channels > 0 && rate > 0 && ((format == formatPcm && (bits == 16 || bits == 24 || bits == 32)) || (format == formatFloat && bits == 32));
    if (data == nullptr || ! supported)
    {
        fail (error, file.string() + ": unsupported WAV format");
        return std::nullopt;
    }

    const std::size_t bytesPerSample = bits / 8u;
    const std::size_t frames = dataSize / (bytesPerSample * channels);
    AudioData audio;
    audio.sampleRate = rate;
    audio.channels.assign (channels, std::vector<float> (frames));
    for (std::size_t f = 0; f < frames; ++f)
        for (std::size_t c = 0; c < channels; ++c)
        {
            const auto* p = data + (f * channels + c) * bytesPerSample;
            float v = 0.0f;
            if (format == formatFloat)
                std::memcpy (&v, p, 4);
            else if (bits == 16)
                v = static_cast<float> (static_cast<std::int16_t> (le16 (p))) / 32768.0f;
            else if (bits == 24)
            {
                const auto raw = static_cast<std::int32_t> (static_cast<std::uint32_t> (p[0] << 8 | p[1] << 16 | p[2] << 24)) >> 8;
                v = static_cast<float> (raw) / 8388608.0f;
            }
            else
                v = static_cast<float> (static_cast<double> (static_cast<std::int32_t> (le32 (p))) / 2147483648.0);
            audio.channels[c][f] = v;
        }
    return audio;
}

bool writeWav (const std::filesystem::path& file, const AudioData& audio, SampleFormat format, std::string* error)
{
    rt::noteBlockingCall();
    const auto channels = static_cast<std::uint16_t> (audio.numChannels());
    const auto frames = static_cast<std::size_t> (audio.numFrames());
    const std::uint16_t bits = format == SampleFormat::int16 ? 16 : (format == SampleFormat::int24 ? 24 : 32);
    const std::uint32_t bytesPerSample = bits / 8u;
    const auto dataSize = static_cast<std::uint32_t> (frames * channels * bytesPerSample);

    std::vector<std::uint8_t> out;
    out.reserve (44 + dataSize);
    out.insert (out.end(), { 'R', 'I', 'F', 'F' });
    put32 (out, 36 + dataSize);
    out.insert (out.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
    put32 (out, 16);
    put16 (out, format == SampleFormat::float32 ? formatFloat : formatPcm);
    put16 (out, channels);
    put32 (out, static_cast<std::uint32_t> (audio.sampleRate));
    put32 (out, static_cast<std::uint32_t> (audio.sampleRate) * channels * bytesPerSample);
    put16 (out, static_cast<std::uint16_t> (channels * bytesPerSample));
    put16 (out, bits);
    out.insert (out.end(), { 'd', 'a', 't', 'a' });
    put32 (out, dataSize);
    for (std::size_t f = 0; f < frames; ++f)
        for (std::size_t c = 0; c < channels; ++c)
        {
            const float v = audio.channels[c][f];
            if (format == SampleFormat::float32)
            {
                std::uint32_t u = 0;
                std::memcpy (&u, &v, 4);
                put32 (out, u);
                continue;
            }
            const double clamped = std::clamp (static_cast<double> (v), -1.0, 1.0);
            if (format == SampleFormat::int16)
                put16 (out, static_cast<std::uint16_t> (static_cast<std::int16_t> (std::lround (std::min (clamped * 32768.0, 32767.0)))));
            else
            {
                const auto s = static_cast<std::uint32_t> (static_cast<std::int32_t> (std::lround (std::min (clamped * 8388608.0, 8388607.0))));
                out.push_back (static_cast<std::uint8_t> (s));
                out.push_back (static_cast<std::uint8_t> (s >> 8));
                out.push_back (static_cast<std::uint8_t> (s >> 16));
            }
        }

    std::ofstream file_ (file, std::ios::binary);
    if (! file_ || ! file_.write (reinterpret_cast<const char*> (out.data()), static_cast<std::streamsize> (out.size())))
        return fail (error, "Couldn't write " + file.string());
    return true;
}
} // namespace weft::media
