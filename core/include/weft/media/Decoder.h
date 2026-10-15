#pragma once

// Reading audio files for import (F-ARR-26). WAV is built in; the app adds decoders
// for AIFF, FLAC and MP3 from its platform layer.

#include "weft/media/WavFile.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace weft::media
{
class AudioDecoder
{
public:
    virtual ~AudioDecoder() = default;
    /** Lower-case extensions with the dot, e.g. ".flac". */
    virtual std::vector<std::string> extensions() const = 0;
    virtual std::optional<AudioData> decode (const std::filesystem::path& file, std::string* error) const = 0;
};

class DecoderSet
{
public:
    /** A set that reads WAV. */
    DecoderSet();
    void add (std::shared_ptr<AudioDecoder> decoder) { decoders.insert (decoders.begin(), std::move (decoder)); }

    bool canDecode (const std::filesystem::path& file) const;
    std::optional<AudioData> decode (const std::filesystem::path& file, std::string* error = nullptr) const;

private:
    std::vector<std::shared_ptr<AudioDecoder>> decoders;
};

/** Lower-cased extension with the dot. */
std::string lowerExtension (const std::filesystem::path& file);
} // namespace weft::media
