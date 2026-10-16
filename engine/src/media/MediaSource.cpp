#include "weft/engine/media/MediaSource.h"

#include <algorithm>
#include <vector>

namespace weft::engine
{
void MediaSource::allocateSlots()
{
    blocks = (frames + blockFrames - 1) / blockFrames;
    playable.store (frames);
    slots = std::make_unique<std::atomic<Block*>[]> (static_cast<std::size_t> (std::max<std::int64_t> (1, blocks)));
    for (std::int64_t i = 0; i < std::max<std::int64_t> (1, blocks); ++i)
        slots[static_cast<std::size_t> (i)].store (nullptr);
}

MediaSource::MediaSource (const media::AudioData& audio)
{
    rate = audio.sampleRate;
    channels = audio.numChannels();
    frames = audio.numFrames();
    allocateSlots();
    for (std::int64_t b = 0; b < blocks; ++b)
    {
        auto* data = new Block[static_cast<std::size_t> (blockFrames * channels)] {};
        const auto start = b * blockFrames;
        const auto n = std::min (blockFrames, frames - start);
        for (int ch = 0; ch < channels; ++ch)
            std::copy_n (audio.channels[static_cast<std::size_t> (ch)].begin() + start, n, data + ch * blockFrames);
        slots[static_cast<std::size_t> (b)].store (data);
    }
}

std::shared_ptr<MediaSource> MediaSource::open (const std::filesystem::path& file, std::int64_t residentFrames, std::string* error)
{
    std::shared_ptr<MediaSource> source (new MediaSource());
    if (! source->reader.open (file, error))
        return nullptr;
    const auto& info = source->reader.info();
    source->rate = info.sampleRate;
    source->channels = info.channels;
    source->frames = info.frames;
    if (source->channels <= 0)
    {
        if (error != nullptr)
            *error = "The file has no audio channels.";
        return nullptr;
    }
    source->allocateSlots();
    source->isResident = source->frames <= residentFrames;
    if (source->isResident)
        for (std::int64_t b = 0; b < source->blocks; ++b)
            if (! source->load (b))
            {
                if (error != nullptr)
                    *error = "The file couldn't be read.";
                return nullptr;
            }
    return source;
}

std::shared_ptr<MediaSource> MediaSource::openPartial (const std::filesystem::path& file, std::int64_t totalFrames, std::string* error)
{
    std::shared_ptr<MediaSource> source (new MediaSource());
    if (! source->reader.open (file, error))
        return nullptr;
    const auto& info = source->reader.info();
    source->rate = info.sampleRate;
    source->channels = info.channels;
    source->frames = std::max (totalFrames, info.frames);
    if (source->channels <= 0)
    {
        if (error != nullptr)
            *error = "The file has no audio channels.";
        return nullptr;
    }
    source->path = file;
    source->isPartial = true;
    source->allocateSlots();
    source->playable.store (0);
    source->extend();
    return source;
}

void MediaSource::extend()
{
    if (! isPartial)
        return;
    std::int64_t available = 0;
    {
        const std::scoped_lock lock (fileLock);
        if (! reader.open (path))
            return;
        available = std::min (reader.info().frames, frames);
    }
    // Whole blocks only (and the last one once the file is complete): a loaded block never changes.
    const auto ready = available >= frames ? blocks : available / blockFrames;
    std::int64_t b = 0;
    while (b < ready && (isLoaded (b) || load (b)))
        ++b;
    playable.store (b >= blocks ? frames : b * blockFrames, std::memory_order_release);
}

MediaSource::~MediaSource()
{
    if (! slots)
        return;
    for (std::int64_t i = 0; i < blocks; ++i)
        delete[] slots[static_cast<std::size_t> (i)].exchange (nullptr);
}

float MediaSource::sample (int channel, std::int64_t frame, bool& missing) const noexcept
{
    if (frame < 0 || frame >= frames)
        return 0.0f;
    const auto* b = block (std::min (channel, channels - 1), frame / blockFrames);
    if (b == nullptr)
    {
        // Not downloaded yet is silence, not a disk that can't keep up (D-COL-11).
        if (frame < playableFrames())
            missing = true;
        return 0.0f;
    }
    return b[frame % blockFrames];
}

bool MediaSource::load (std::int64_t index)
{
    if (index < 0 || index >= blocks || isLoaded (index))
        return true;
    const std::scoped_lock lock (fileLock);
    auto data = std::make_unique<Block[]> (static_cast<std::size_t> (blockFrames * channels));
    std::vector<float*> dest (static_cast<std::size_t> (channels));
    for (int ch = 0; ch < channels; ++ch)
        dest[static_cast<std::size_t> (ch)] = data.get() + ch * blockFrames;
    const auto start = index * blockFrames;
    const auto n = std::min (blockFrames, frames - start);
    if (reader.read (start, n, dest.data()) != n)
        return false;
    slots[static_cast<std::size_t> (index)].store (data.release(), std::memory_order_release);
    return true;
}

std::unique_ptr<MediaSource::Block[]> MediaSource::evict (std::int64_t index) noexcept
{
    if (isResident || index < 0 || index >= blocks)
        return nullptr;
    return std::unique_ptr<Block[]> (slots[static_cast<std::size_t> (index)].exchange (nullptr, std::memory_order_acq_rel));
}
} // namespace weft::engine
