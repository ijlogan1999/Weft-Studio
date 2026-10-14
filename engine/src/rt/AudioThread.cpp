#include "weft/engine/rt/AudioThread.h"

namespace weft::rt
{
namespace
{
thread_local bool inAudioCallback = false;
}

bool isAudioThread() noexcept { return inAudioCallback; }

ScopedAudioThread::ScopedAudioThread() noexcept : previous (inAudioCallback) { inAudioCallback = true; }

ScopedAudioThread::~ScopedAudioThread() { inAudioCallback = previous; }
} // namespace weft::rt
