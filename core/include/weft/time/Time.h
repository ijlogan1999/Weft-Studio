#pragma once

#include <cstdint>

namespace weft
{
/** Musical time at 960 PPQN from project start (spec 03 §1.1). */
using Ticks = std::int64_t;
/** Absolute time in samples at the project sample rate. */
using Samples = std::int64_t;

inline constexpr Ticks ticksPerQuarter = 960;

/** Decibel sentinel for -infinity (spec 03 §1.1). */
inline constexpr double minusInfinityDb = -200.0;
} // namespace weft
