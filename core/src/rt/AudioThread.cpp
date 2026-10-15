#include "weft/rt/AudioThread.h"

#include <atomic>

namespace weft::rt
{
namespace
{
thread_local bool inAudioCallback = false;
std::atomic<std::int64_t> allocationCount { 0 };
std::atomic<std::int64_t> blockingCallCount { 0 };
} // namespace

void noteAllocation() noexcept
{
    if (inAudioCallback)
        allocationCount.fetch_add (1, std::memory_order_relaxed);
}

void noteBlockingCall() noexcept
{
    if (inAudioCallback)
        blockingCallCount.fetch_add (1, std::memory_order_relaxed);
}

Violations violations() noexcept { return { allocationCount.load(), blockingCallCount.load() }; }

void resetViolations() noexcept
{
    allocationCount = 0;
    blockingCallCount = 0;
}

bool isAudioThread() noexcept { return inAudioCallback; }

ScopedAudioThread::ScopedAudioThread() noexcept : previous (inAudioCallback) { inAudioCallback = true; }

ScopedAudioThread::~ScopedAudioThread() { inAudioCallback = previous; }
} // namespace weft::rt
