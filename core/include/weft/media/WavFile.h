#pragma once

// WAV (RIFF/WAVE) reading and writing for project media: PCM 16/24/32-bit integer
// and 32-bit float, any channel count, with the Broadcast Wave `bext` chunk for
// recordings (F-REC-15). Media is stored as project-rate WAV (D-DM-4), so this is
// all playback needs.

#include <cstdint>
#include <filesystem>
#include <fstream>
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

/** Recording bit depth (16, 24 or 32 = float) to a sample format. */
SampleFormat formatForBitDepth (int bitDepth) noexcept;

/** Broadcast Wave metadata (EBU Tech 3285, version 1). */
struct BextInfo
{
    std::string description;
    std::string originator = "Weft Studio";
    std::string originatorReference;
    std::string date; // yyyy-mm-dd
    std::string time; // hh:mm:ss
    std::uint64_t timeReference = 0; // first sample's position since midnight, or the timeline
};

struct WavInfo
{
    double sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    bool isFloat = false;
    std::int64_t frames = 0;
    std::optional<BextInfo> bext;
};

/** Random-access reader: parses the header once, then reads any frame range. */
class WavReader
{
public:
    bool open (const std::filesystem::path& file, std::string* error = nullptr);
    const WavInfo& info() const noexcept { return wavInfo; }
    /** Reads frames [start, start + count) into dest[c] for each file channel; returns frames read. */
    std::int64_t read (std::int64_t start, std::int64_t count, float* const* dest);

private:
    std::ifstream in;
    WavInfo wavInfo;
    std::uint64_t dataOffset = 0;
    std::vector<std::uint8_t> buffer;
};

/** Streaming writer: sizes are patched on close(), so recordings can grow on disk. */
class WavWriter
{
public:
    ~WavWriter();
    bool open (const std::filesystem::path& file, double sampleRate, int channels, SampleFormat format, const BextInfo* bext = nullptr,
               std::string* error = nullptr);
    /** Writes planar float frames (one pointer per channel). */
    bool write (const float* const* channels, std::int64_t frames);
    /** Patches the RIFF and data sizes and closes the file. */
    bool close();
    bool isOpen() const noexcept { return out.is_open(); }
    std::int64_t framesWritten() const noexcept { return frames; }

private:
    std::ofstream out;
    int numChannels = 0;
    SampleFormat sampleFormat = SampleFormat::int24;
    std::uint64_t dataSizeOffset = 0, riffSizeOffset = 4;
    std::int64_t frames = 0;
    std::vector<std::uint8_t> buffer;
};

std::optional<WavInfo> readWavInfo (const std::filesystem::path& file, std::string* error = nullptr);
std::optional<AudioData> readWav (const std::filesystem::path& file, std::string* error = nullptr);
bool writeWav (const std::filesystem::path& file, const AudioData& audio, SampleFormat format, std::string* error = nullptr);

/** File name of a media blob in the project's Media/ folder: "b3:<hex>" -> "<hex>.wav" (D-W10-1). */
std::string mediaFileName (const std::string& blobHash);
} // namespace weft::media
