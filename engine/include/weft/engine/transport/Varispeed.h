#pragma once

// Varispeed (D-ARCH-55): the whole project played faster and higher, or slower and lower, like a tape
// machine, without changing the project. A rate r from 0.5 (-50%) to 2 (+100%), shown in one of four units.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace weft::engine::varispeed
{
inline constexpr double minRate = 0.5, maxRate = 2.0;

enum class Unit
{
    percent,   // (r - 1) x 100
    tempo,     // the tempo played: tempo x r
    semitones, // 12 log2 r
    tuning     // the A above middle C: 440 x r Hz
};

inline double clampRate (double r) noexcept { return std::isfinite (r) ? std::clamp (r, minRate, maxRate) : 1.0; }

/** The rate in a unit; `tempo` is the project tempo (bpm) for Unit::tempo. */
inline double toUnit (double r, Unit unit, double tempo) noexcept
{
    switch (unit)
    {
        case Unit::percent: return (r - 1.0) * 100.0;
        case Unit::tempo: return tempo * r;
        case Unit::semitones: return 12.0 * std::log2 (r);
        case Unit::tuning: return 440.0 * r;
    }
    return r;
}

/** The rate a value in a unit stands for, clamped to the range. */
inline double fromUnit (double value, Unit unit, double tempo) noexcept
{
    double r = 1.0;
    switch (unit)
    {
        case Unit::percent: r = 1.0 + value / 100.0; break;
        case Unit::tempo: r = tempo > 0.0 ? value / tempo : 1.0; break;
        case Unit::semitones: r = std::exp2 (value / 12.0); break;
        case Unit::tuning: r = value / 440.0; break;
    }
    return clampRate (r);
}

/** The display text: "+15.0%", "138.00 bpm", "-2.50 st", "466.2 Hz". */
inline std::string text (double r, Unit unit, double tempo)
{
    const double v = toUnit (r, unit, tempo);
    char buf[32] = {};
    switch (unit)
    {
        case Unit::percent: std::snprintf (buf, sizeof (buf), "%+.1f%%", std::abs (v) < 0.05 ? 0.0 : v); break;
        case Unit::tempo: std::snprintf (buf, sizeof (buf), "%.2f bpm", v); break;
        case Unit::semitones: std::snprintf (buf, sizeof (buf), "%+.2f st", std::abs (v) < 0.005 ? 0.0 : v); break;
        case Unit::tuning: std::snprintf (buf, sizeof (buf), "%.1f Hz", v); break;
    }
    return buf;
}
} // namespace weft::engine::varispeed
