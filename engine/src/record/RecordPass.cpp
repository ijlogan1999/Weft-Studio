#include "weft/engine/record/RecordPass.h"

#include <algorithm>

namespace weft::engine
{
AudioFifo::AudioFifo (int channels, std::int64_t capacityFrames)
    : data (static_cast<std::size_t> (std::max (1, channels)), std::vector<float> (static_cast<std::size_t> (std::max<std::int64_t> (1, capacityFrames)))),
      capacity (std::max<std::int64_t> (1, capacityFrames))
{
}

std::int64_t AudioFifo::available() const noexcept { return writePos.load (std::memory_order_acquire) - readPos.load (std::memory_order_acquire); }

std::int64_t AudioFifo::write (const float* const* src, int offset, std::int64_t frames) noexcept
{
    const auto w = writePos.load (std::memory_order_relaxed);
    const auto n = std::min (frames, capacity - (w - readPos.load (std::memory_order_acquire)));
    for (std::size_t c = 0; c < data.size(); ++c)
        for (std::int64_t i = 0; i < n; ++i)
            data[c][static_cast<std::size_t> ((w + i) % capacity)] = src[c][offset + i];
    writePos.store (w + n, std::memory_order_release);
    return n;
}

std::int64_t AudioFifo::read (float* const* dest, std::int64_t maxFrames) noexcept
{
    const auto r = readPos.load (std::memory_order_relaxed);
    const auto n = std::min (maxFrames, writePos.load (std::memory_order_acquire) - r);
    for (std::size_t c = 0; c < data.size(); ++c)
        for (std::int64_t i = 0; i < n; ++i)
            dest[c][i] = data[c][static_cast<std::size_t> ((r + i) % capacity)];
    readPos.store (r + n, std::memory_order_release);
    return n;
}
} // namespace weft::engine
