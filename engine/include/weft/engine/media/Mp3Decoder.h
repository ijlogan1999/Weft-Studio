#pragma once

// MP3 decoding for import over the weft-codec crate. It reads the LAME/Info header and
// trims the encoder delay and padding, so imported MP3s land on the grid at their true length.

#include "weft/media/Decoder.h"

#include <memory>

namespace weft::engine
{
std::shared_ptr<media::AudioDecoder> makeMp3Decoder();
} // namespace weft::engine
