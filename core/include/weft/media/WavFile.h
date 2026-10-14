#pragma once

// Minimal WAV (RIFF/WAVE) reader and writer for project media: PCM 16/24/32-bit
// integer and 32-bit float, any channel count. Media is stored as project-rate
// WAV (D-DM-4), so this is all playback needs.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace weft::media
{
/** Planar float audio in memory. */
struct AudioData
{
    double sampleRate = 48000.0;
    std::vector<std::vector<float>> channels; // channels[c][frame]

    int numChannels() const noexcept { return static_cast<int> (channels.size()); }
    std::int64_t numFrames() const noexcept { return channels.empty() ? 0 : static_cast<std::int64_t> (channels[0].size()); }
};

enum class SampleFormat
{
    int16,
    int24,
    float32
};

std::optional<AudioData> readWav (const std::filesystem::path& file, std::string* error = nullptr);
bool writeWav (const std::filesystem::path& file, const AudioData& audio, SampleFormat format, std::string* error = nullptr);

/** File name of a media blob in the project's Media/ folder: "b3:<hex>" -> "<hex>.wav" (D-W10-1). */
std::string mediaFileName (const std::string& blobHash);
} // namespace weft::media
