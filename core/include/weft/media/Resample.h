#pragma once

// Sample-rate conversion for imported audio (D-DM-4): a Kaiser-windowed sinc filter
// in polyphase form. Offline only; not for the audio thread.

#include "weft/media/WavFile.h"

namespace weft::media
{
/** Output length for `frames` input frames: round(frames x to / from). */
std::int64_t resampledLength (std::int64_t frames, double fromRate, double toRate) noexcept;

/**
    Converts audio to `toRate`. Passband is flat to 90% of the lower Nyquist frequency;
    stopband attenuation is about 100 dB. Same rate returns a copy.
*/
AudioData resample (const AudioData& audio, double toRate);
} // namespace weft::media
