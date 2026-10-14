#pragma once

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
} // namespace weft::rt
