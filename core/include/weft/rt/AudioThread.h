#pragma once

#include <cstdint>

namespace weft::rt
{
/** True while the calling thread is inside an audio callback. Debug checks for
    allocation, locks and I/O on the audio thread (AT-RT-1) key off this flag. */
bool isAudioThread() noexcept;

/** Marks the current thread as the audio thread for the scope's lifetime. Nests. */
class ScopedAudioThread
{
public:
    ScopedAudioThread() noexcept;
    ~ScopedAudioThread();
    ScopedAudioThread (const ScopedAudioThread&) = delete;
    ScopedAudioThread& operator= (const ScopedAudioThread&) = delete;

private:
    bool previous;
};

/** Real-time rule violations seen on the audio thread (AT-RT-1). Allocation is counted
    by test builds that replace operator new; blocking calls (file I/O, locks) are
    reported by Weft's own I/O and locking code. */
struct Violations
{
    std::int64_t allocations = 0;
    std::int64_t blockingCalls = 0;
};

/** Records an allocation if the calling thread is the audio thread. */
void noteAllocation() noexcept;
/** Records a blocking call (file I/O, a lock) if the calling thread is the audio thread. */
void noteBlockingCall() noexcept;
Violations violations() noexcept;
void resetViolations() noexcept;
} // namespace weft::rt
