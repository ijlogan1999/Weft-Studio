#include "weft/media/Decoder.h"

#include <algorithm>
#include <cctype>

namespace weft::media
{
namespace
{
class WavDecoder final : public AudioDecoder
{
public:
    std::vector<std::string> extensions() const override { return { ".wav", ".wave", ".bwf" }; }
    std::optional<AudioData> decode (const std::filesystem::path& file, std::string* error) const override { return readWav (file, error); }
};
} // namespace

std::string lowerExtension (const std::filesystem::path& file)
{
    auto ext = file.extension().string();
    std::transform (ext.begin(), ext.end(), ext.begin(), [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    return ext;
}

DecoderSet::DecoderSet() { decoders.push_back (std::make_shared<WavDecoder>()); }

bool DecoderSet::canDecode (const std::filesystem::path& file) const
{
    const auto ext = lowerExtension (file);
    for (const auto& d : decoders)
    {
        const auto exts = d->extensions();
        if (std::find (exts.begin(), exts.end(), ext) != exts.end())
            return true;
    }
    return false;
}

std::optional<AudioData> DecoderSet::decode (const std::filesystem::path& file, std::string* error) const
{
    const auto ext = lowerExtension (file);
    std::string lastError = "Unsupported file type.";
    for (const auto& d : decoders)
    {
        const auto exts = d->extensions();
        if (std::find (exts.begin(), exts.end(), ext) == exts.end())
            continue;
        if (auto audio = d->decode (file, &lastError); audio && audio->numFrames() > 0 && audio->numChannels() > 0)
            return audio;
    }
    if (error != nullptr)
        *error = lastError;
    return std::nullopt;
}
} // namespace weft::media
