#pragma once

#include "weft/core/Id.h"
#include "weft/time/Time.h"

#include <vector>

namespace weft
{
struct SignaturePoint
{
    Id id;
    int bar = 0; // 0-based
    int numerator = 4;
    int denominator = 4;
    bool conflicted = false; // another point at the same bar lost to this one (D-DM-9)

    Ticks barLength() const noexcept { return static_cast<Ticks> (numerator) * 4 * ticksPerQuarter / denominator; }
};

struct BarPosition
{
    int bar = 0;         // 0-based
    Ticks offset = 0;    // ticks since bar start
};

/** Meter changes at bar starts (spec 03 §7). Bars are 0-based here and 1-based in the UI. */
class SignatureMap
{
public:
    static bool isValid (int numerator, int denominator) noexcept;

    /** points need not be sorted; a 4/4 point at bar 0 is added if missing. Invalid points are dropped. */
    explicit SignatureMap (std::vector<SignaturePoint> points);
    SignatureMap() : SignatureMap (std::vector<SignaturePoint> {}) {}

    const std::vector<SignaturePoint>& points() const noexcept { return segments; }

    Ticks barStart (int bar) const noexcept;
    Ticks barLength (int bar) const noexcept;
    BarPosition barAt (Ticks tick) const noexcept;
    const SignaturePoint& signatureAtBar (int bar) const noexcept;

private:
    std::vector<SignaturePoint> segments;
    std::vector<Ticks> segmentStartTicks;
};
} // namespace weft
