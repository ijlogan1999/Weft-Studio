#include "weft/engine/media/Mp3Decoder.h"

#include <weft_codec.h>

#include <array>

namespace weft::engine
{
namespace
{
class Mp3Decoder final : public media::AudioDecoder
{
public:
    std::vector<std::string> extensions() const override { return { ".mp3" }; }

    std::optional<media::AudioData> decode (const std::filesystem::path& file, std::string* error) const override
    {
        WeftMp3Decoded d {};
        std::array<char, 256> err {};
        const auto path = file.u8string();
        if (weft_mp3_decode (reinterpret_cast<const char*> (path.c_str()), &d, err.data(), err.size()) != 0)
        {
            if (error != nullptr)
                *error = "The file couldn't be decoded.";
            return std::nullopt;
        }
        media::AudioData out;
        out.sampleRate = d.sample_rate;
        const auto channels = static_cast<std::size_t> (d.channels > 0 ? d.channels : 0);
        out.channels.assign (channels, std::vector<float> (d.frames));
        for (std::size_t i = 0; i < d.frames; ++i)
            for (std::size_t ch = 0; ch < channels; ++ch)
                out.channels[ch][i] = d.samples[i * channels + ch];
        weft_mp3_free (&d);
        return out;
    }
};
} // namespace

std::shared_ptr<media::AudioDecoder> makeMp3Decoder() { return std::make_shared<Mp3Decoder>(); }
} // namespace weft::engine
