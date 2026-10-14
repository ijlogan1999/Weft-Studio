#include "weft/time/Quantize.h"

#include <algorithm>
#include <cmath>

namespace weft
{
Ticks quantizeTarget (Ticks t, Ticks grid, double swingPercent) noexcept
{
    if (grid <= 0)
        return t;
    // Nearest multiple of grid; exact halves go to the earlier point.
    auto index = t / grid;
    if (t % grid < 0)
        --index;
    const auto below = index * grid;
    if (2 * (t - below) > grid)
        ++index;

    const double swing = std::clamp (swingPercent, 0.0, 100.0) / 100.0 / 3.0;
    const bool odd = (index % 2) != 0;
    return index * grid + (odd ? static_cast<Ticks> (std::llround (swing * static_cast<double> (grid))) : 0);
}

Ticks quantizeTick (Ticks t, Ticks grid, double strength, double swingPercent) noexcept
{
    const auto q = quantizeTarget (t, grid, swingPercent);
    const double s = std::clamp (strength, 0.0, 1.0);
    return t + static_cast<Ticks> (std::llround (s * static_cast<double> (q - t)));
}
} // namespace weft
