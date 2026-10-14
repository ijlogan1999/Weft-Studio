#include "weft/time/TempoMap.h"

#include <algorithm>

namespace weft
{
namespace
{
double samplesPerTick (double sampleRate, double bpm) { return sampleRate * 60.0 / (bpm * static_cast<double> (ticksPerQuarter)); }
} // namespace

TempoMap::TempoMap (std::vector<TempoPoint> points, double sampleRate) : segments (std::move (points)), rate (sampleRate)
{
    std::sort (segments.begin(), segments.end(), [] (const auto& a, const auto& b) { return a.pos < b.pos; });
    if (segments.empty() || segments.front().pos != 0)
        segments.insert (segments.begin(), TempoPoint { "initial", 0, 120.0, false });
    for (auto& p : segments)
        p.bpm = std::clamp (p.bpm, minBpm, maxBpm);

    segmentStartSamples.resize (segments.size());
    segmentStartSamples[0] = 0.0;
    for (std::size_t i = 1; i < segments.size(); ++i)
        segmentStartSamples[i] = segmentStartSamples[i - 1]
                               + static_cast<double> (segments[i].pos - segments[i - 1].pos) * samplesPerTick (rate, segments[i - 1].bpm);
}

std::size_t TempoMap::segmentForTick (double tick) const noexcept
{
    const auto it = std::upper_bound (segments.begin(), segments.end(), tick,
                                      [] (double t, const TempoPoint& p) { return t < static_cast<double> (p.pos); });
    return it == segments.begin() ? 0 : static_cast<std::size_t> (it - segments.begin()) - 1;
}

double TempoMap::tickToSample (double tick) const noexcept
{
    const auto i = segmentForTick (tick);
    return segmentStartSamples[i] + (tick - static_cast<double> (segments[i].pos)) * samplesPerTick (rate, segments[i].bpm);
}

double TempoMap::sampleToTick (double sample) const noexcept
{
    const auto it = std::upper_bound (segmentStartSamples.begin(), segmentStartSamples.end(), sample);
    const auto i = it == segmentStartSamples.begin() ? 0 : static_cast<std::size_t> (it - segmentStartSamples.begin()) - 1;
    return static_cast<double> (segments[i].pos) + (sample - segmentStartSamples[i]) / samplesPerTick (rate, segments[i].bpm);
}

double TempoMap::bpmAt (Ticks tick) const noexcept { return segments[segmentForTick (static_cast<double> (tick))].bpm; }
} // namespace weft
