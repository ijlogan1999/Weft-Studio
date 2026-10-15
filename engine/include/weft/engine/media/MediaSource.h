#pragma once

#include "weft/media/WavFile.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace weft::engine
{
/**
    One media file as fixed-size blocks of planar float audio (D-ARCH-17). The audio
    thread reads loaded blocks through atomic pointers; a block that isn't loaded
    reads as missing (the caller plays silence and counts an underrun). Short files
    and in-memory audio are resident: every block loaded up front and never evicted.
    Longer files are streamed: the MediaStreamer loads blocks ahead of the playhead
    and evicts the rest.
*/
class MediaSource
{
public:
    static constexpr std::int64_t blockFrames = 32768;
    using Block = float; // a block is blockFrames x channels floats, channel-major

    /** Resident audio from memory (tests, generated media). */
    explicit MediaSource (const media::AudioData& audio);
    /** Opens a WAV file. Files up to `residentFrames` long are read whole now. */
    static std::shared_ptr<MediaSource> open (const std::filesystem::path& file, std::int64_t residentFrames, std::string* error = nullptr);
    ~MediaSource();
    MediaSource (const MediaSource&) = delete;
    MediaSource& operator= (const MediaSource&) = delete;

    double sampleRate() const noexcept { return rate; }
    int numChannels() const noexcept { return channels; }
    std::int64_t numFrames() const noexcept { return frames; }
    std::int64_t numBlocks() const noexcept { return blocks; }
    bool resident() const noexcept { return isResident; }

    // Audio thread ---------------------------------------------------------------
    /** The block's samples for one channel, or null when the block isn't loaded. */
    const float* block (int channel, std::int64_t index) const noexcept
    {
        const auto* b = slots[static_cast<std::size_t> (index)].load (std::memory_order_acquire);
        return b != nullptr ? b + static_cast<std::int64_t> (channel) * blockFrames : nullptr;
    }
    /** One sample; 0 outside the file. `missing` is set when the block isn't loaded. */
    float sample (int channel, std::int64_t frame, bool& missing) const noexcept;

    // Disk thread ----------------------------------------------------------------
    bool isLoaded (std::int64_t index) const noexcept { return slots[static_cast<std::size_t> (index)].load (std::memory_order_acquire) != nullptr; }
    /** Reads a block from the file. */
    bool load (std::int64_t index);
    /** Detaches a loaded block; the caller frees it once the audio thread can't be reading it. */
    std::unique_ptr<Block[]> evict (std::int64_t index) noexcept;

private:
    MediaSource() = default;
    void allocateSlots();

    double rate = 48000;
    int channels = 0;
    std::int64_t frames = 0, blocks = 0;
    bool isResident = true;
    std::unique_ptr<std::atomic<Block*>[]> slots;
    std::mutex fileLock;
    media::WavReader reader;
};
} // namespace weft::engine
