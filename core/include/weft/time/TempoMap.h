#pragma once

#include "weft/core/Id.h"
#include "weft/time/Time.h"

#include <vector>

namespace weft
{
struct TempoPoint
{
    Id id;
    Ticks pos = 0;
    double bpm = 120.0;
    bool conflicted = false; // another point at the same tick lost to this one (D-DM-9)
};

/**
    Step tempo map (spec 03 §7). Conversion between ticks and samples in float64;
    callers round only at the point of use.
*/
class TempoMap
{
public:
    static constexpr double minBpm = 20.0;
    static constexpr double maxBpm = 999.0;

    /** points need not be sorted; a point at tick 0 is added at 120 BPM if missing. */
    TempoMap (std::vector<TempoPoint> points, double sampleRate);
    TempoMap() : TempoMap ({}, 48000.0) {}

    double sampleRate() const noexcept { return rate; }
    const std::vector<TempoPoint>& points() const noexcept { return segments; }

    double tickToSample (double tick) const noexcept;
    double sampleToTick (double sample) const noexcept;
    double bpmAt (Ticks tick) const noexcept;

    /** Samples between two tick positions. */
    double samplesBetween (double fromTick, double toTick) const noexcept { return tickToSample (toTick) - tickToSample (fromTick); }

private:
    std::size_t segmentForTick (double tick) const noexcept;
    std::vector<TempoPoint> segments;
    std::vector<double> segmentStartSamples;
    double rate;
};
} // namespace weft
