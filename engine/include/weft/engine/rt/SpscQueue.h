#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>

namespace weft::engine::rt
{
/** Fixed-capacity single-producer single-consumer queue. push() and pop() never
    allocate, lock or block, so either side may be the audio thread. */
template <typename T, std::size_t Capacity>
class SpscQueue
{
    static_assert (std::is_trivially_copyable_v<T>, "queue items are copied on the audio thread");
    static_assert (Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

public:
    /** Returns false (and drops the item) when the queue is full. */
    bool push (const T& item) noexcept
    {
        const auto w = writeIndex.load (std::memory_order_relaxed);
        if (w - readIndex.load (std::memory_order_acquire) == Capacity)
            return false;
        items[w & (Capacity - 1)] = item;
        writeIndex.store (w + 1, std::memory_order_release);
        return true;
    }

    std::optional<T> pop() noexcept
    {
        const auto r = readIndex.load (std::memory_order_relaxed);
        if (r == writeIndex.load (std::memory_order_acquire))
            return std::nullopt;
        T item = items[r & (Capacity - 1)];
        readIndex.store (r + 1, std::memory_order_release);
        return item;
    }

    bool empty() const noexcept { return readIndex.load (std::memory_order_acquire) == writeIndex.load (std::memory_order_acquire); }

private:
    std::array<T, Capacity> items {};
    alignas (64) std::atomic<std::size_t> writeIndex { 0 };
    alignas (64) std::atomic<std::size_t> readIndex { 0 };
};
} // namespace weft::engine::rt
