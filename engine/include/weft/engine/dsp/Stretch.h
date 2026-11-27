#pragma once

// Pitch shifting and time stretching of audio regions (D-UI-51).

#include "weft/engine/media/MediaPool.h"
#include "weft/media/WavFile.h"
#include "weft/model/ProjectModel.h"

#include <string>

namespace weft::engine
{
/**
    The media pool key for the audio a region plays when it is stretched or pitch shifted: the file's hash with the
    stretch and the pitch shift. Empty when the region plays its file as it is (speed alone is a resampled read).
*/
std::string derivedMediaKey (const model::AudioRegionData& audio);

/** The audio stretched to `stretch` times its length at the same pitch, and shifted by `cents`. */
media::AudioData stretchAudio (const media::AudioData& in, double stretch, double cents);

/**
    Renders the stretched or pitch-shifted audio each audio region needs that the pool doesn't hold yet, and drops
    derived audio no region uses any more. A file that isn't here (or is still downloading) is skipped until it is.
    Message thread; Engine::refresh calls it before compiling a snapshot.
*/
void prepareDerivedMedia (const model::ProjectModel& model, MediaPool& pool);
} // namespace weft::engine
