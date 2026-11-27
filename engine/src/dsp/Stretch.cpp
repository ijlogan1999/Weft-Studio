#include "weft/engine/dsp/Stretch.h"

#include <signalsmith-stretch/signalsmith-stretch.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

namespace weft::engine
{
namespace
{
bool unity (double v) { return std::abs (v - 1.0) < 1e-9; }
} // namespace

std::string derivedMediaKey (const model::AudioRegionData& audio)
{
    if (unity (audio.stretch) && audio.cents() == 0)
        return {};
    std::ostringstream key;
    key.precision (9);
    key << audio.media << "@stretch=" << audio.stretch << ",cents=" << audio.cents();
    return key.str();
}

media::AudioData stretchAudio (const media::AudioData& in, double stretch, double cents)
{
    media::AudioData out;
    out.sampleRate = in.sampleRate;
    const auto channels = in.numChannels();
    const auto frames = in.numFrames();
    const auto outFrames = static_cast<std::int64_t> (std::llround (static_cast<double> (frames) * stretch));
    out.channels.assign (static_cast<std::size_t> (channels), std::vector<float> (static_cast<std::size_t> (std::max<std::int64_t> (0, outFrames))));
    if (channels == 0 || frames == 0 || outFrames <= 0)
        return out;

    // A fixed seed, so the same edit always renders the same audio.
    signalsmith::stretch::SignalsmithStretch<float> stretcher (1L);
    stretcher.presetDefault (channels, static_cast<float> (in.sampleRate));
    stretcher.setTransposeSemitones (static_cast<float> (cents / 100.0));

    // The whole buffer in one go. Very short audio is padded with silence first, so the stretcher has enough
    // to work with, and the padding is cut off again afterwards.
    const auto minimum = static_cast<std::int64_t> (stretcher.inputLatency() + stretcher.outputLatency()) * 4;
    const auto padded = std::max (frames, minimum);
    const auto paddedOut = static_cast<std::int64_t> (std::llround (static_cast<double> (padded) * stretch));
    std::vector<std::vector<float>> input (static_cast<std::size_t> (channels), std::vector<float> (static_cast<std::size_t> (padded), 0.0f));
    for (std::size_t c = 0; c < input.size(); ++c)
        std::copy (in.channels[c].begin(), in.channels[c].end(), input[c].begin());
    std::vector<std::vector<float>> output (static_cast<std::size_t> (channels), std::vector<float> (static_cast<std::size_t> (paddedOut), 0.0f));
    if (! stretcher.exact (input, static_cast<int> (padded), output, static_cast<int> (paddedOut)))
        return out; // silence: too short even padded
    for (std::size_t c = 0; c < output.size(); ++c)
        std::copy_n (output[c].begin(), static_cast<std::size_t> (outFrames), out.channels[c].begin());
    return out;
}

void prepareDerivedMedia (const model::ProjectModel& model, MediaPool& pool)
{
    std::set<std::string> used;
    for (const auto& [id, r] : model.regions)
    {
        if (r->isMidi)
            continue;
        const auto key = derivedMediaKey (r->audio);
        if (key.empty())
            continue;
        used.insert (key);
        if (pool.find (key) != nullptr)
            continue;
        const auto source = pool.find (r->audio.media);
        if (source == nullptr || source->partial())
            continue;
        const auto audio = source->readAll();
        if (! audio)
            continue;
        pool.add (key, stretchAudio (*audio, r->audio.stretch, static_cast<double> (r->audio.cents())));
    }
    for (const auto& key : pool.keys())
        if (key.find ("@stretch=") != std::string::npos && ! used.contains (key))
            pool.remove (key);
}
} // namespace weft::engine
