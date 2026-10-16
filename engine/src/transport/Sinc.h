#pragma once

// Windowed-sinc interpolation for resampled playback (D-ARCH-15, D-ARCH-50).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace weft::engine::sinc
{
/** Zero crossings each side of the kernel centre, at the kernel's cutoff. */
inline constexpr int halfWidth = 8;
/** Lowest cutoff used when reading faster than real time; caps the tap count. */
inline constexpr double minCutoff = 0.25;
/** Taps each side for the widest kernel; the streamer preloads this margin. */
inline constexpr int maxReach = static_cast<int> (halfWidth / minCutoff) + 1;

/** The Kaiser-windowed sinc kernel, tabulated once at static initialisation. */
class Kernel
{
public:
    static constexpr int perUnit = 1024;
    static constexpr int size = halfWidth * perUnit + 2;

    Kernel() noexcept
    {
        constexpr double beta = 8.0;
        const double norm = besselI0 (beta);
        for (int i = 0; i < size; ++i)
        {
            const double u = static_cast<double> (i) / perUnit;
            if (u >= halfWidth)
            {
                table[static_cast<std::size_t> (i)] = 0.0f;
                continue;
            }
            const double r = u / halfWidth;
            const double window = besselI0 (beta * std::sqrt (1.0 - r * r)) / norm;
            const double arg = std::numbers::pi * u;
            table[static_cast<std::size_t> (i)] = static_cast<float> ((u == 0.0 ? 1.0 : std::sin (arg) / arg) * window);
        }
    }

    /** The kernel at distance u (in zero crossings), linearly interpolated; 0 outside. */
    float operator() (double u) const noexcept
    {
        u = std::abs (u) * perUnit;
        const auto i = static_cast<std::int64_t> (u);
        if (i >= size - 1)
            return 0.0f;
        const auto f = static_cast<float> (u - static_cast<double> (i));
        const float a = table[static_cast<std::size_t> (i)], b = table[static_cast<std::size_t> (i + 1)];
        return a + (b - a) * f;
    }

private:
    static double besselI0 (double x) noexcept
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 50; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
            if (term < sum * 1e-17)
                break;
        }
        return sum;
    }

    std::array<float, size> table {};
};

/** Cutoff (fraction of the source Nyquist) for reading `step` source frames per output frame. */
inline double cutoff (double step) noexcept
{
    return step <= 1.0 ? 1.0 : std::max (minCutoff, 1.0 / step);
}

/** Taps each side of the read position for a cutoff. */
inline int reach (double cutoff) noexcept
{
    return static_cast<int> (std::ceil (halfWidth / cutoff));
}
} // namespace weft::engine::sinc
