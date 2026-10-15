#pragma once

// AIFF, FLAC and MP3 decoding for import, backed by the platform layer. No JUCE
// types here: the decoder plugs into media::DecoderSet.

#include "weft/media/Decoder.h"

#include <memory>

namespace weft::engine
{
std::shared_ptr<media::AudioDecoder> makePlatformAudioDecoder();

/** WAV plus everything the platform layer reads. */
media::DecoderSet importDecoders();
} // namespace weft::engine
