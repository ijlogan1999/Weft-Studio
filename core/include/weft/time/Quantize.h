#pragma once

#include "weft/time/Time.h"

namespace weft
{
/**
    Quantize math from spec 02 W-5.
    grid: grid value in ticks (> 0). strength: 0..1. swingPercent: 0..100, where
    0 = straight and 100 = full triplet feel (D-W5-1).
    Returns t' = t + strength x (q_swung - t), rounded to the nearest tick.
*/
Ticks quantizeTick (Ticks t, Ticks grid, double strength, double swingPercent) noexcept;

/** The nearest grid point to t (ties go earlier), with swing applied to odd points. */
Ticks quantizeTarget (Ticks t, Ticks grid, double swingPercent) noexcept;
} // namespace weft
