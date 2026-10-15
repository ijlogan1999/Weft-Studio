#include "weft/media/Resample.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>

namespace weft::media
{
namespace
{
double besselI0 (double x)
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

constexpr double kaiserBeta = 10.0;
constexpr int zeroCrossings = 32;
constexpr std::int64_t maxPhases = 4096;
} // namespace

std::int64_t resampledLength (std::int64_t frames, double fromRate, double toRate) noexcept
{
    return static_cast<std::int64_t> (std::llround (static_cast<double> (frames) * toRate / fromRate));
}

AudioData resample (const AudioData& audio, double toRate)
{
    if (audio.sampleRate == toRate || audio.numFrames() == 0 || toRate <= 0 || audio.sampleRate <= 0)
    {
        AudioData copy = audio;
        copy.sampleRate = toRate > 0 ? toRate : audio.sampleRate;
        return copy;
    }
    const double from = audio.sampleRate;
    // Output frame n sits at input position n * step.
    const double step = from / toRate;
    const double cutoff = 0.9 * std::min (1.0, toRate / from); // of the input Nyquist
    const double halfWidth = zeroCrossings / cutoff;           // in input samples
    const int taps = static_cast<int> (std::ceil (halfWidth)) * 2 + 1;

    // Phases: exact for rational ratios of integer rates (44.1k -> 48k uses 160), else 4096.
    std::int64_t phases = maxPhases;
    if (from == std::floor (from) && toRate == std::floor (toRate))
    {
        const auto a = static_cast<std::int64_t> (from), b = static_cast<std::int64_t> (toRate);
        const auto l = b / std::gcd (a, b);
        if (l <= maxPhases)
            phases = l;
    }
    // bank[p][j]: weight of input sample (floor(pos) - taps/2 + j) for fractional position p / phases.
    const int half = taps / 2;
    std::vector<float> bank (static_cast<std::size_t> (phases * taps));
    const double norm = besselI0 (kaiserBeta);
    for (std::int64_t p = 0; p < phases; ++p)
    {
        const double frac = static_cast<double> (p) / static_cast<double> (phases);
        double sum = 0;
        for (int j = 0; j < taps; ++j)
        {
            const double x = static_cast<double> (j - half) - frac; // distance from the output position
            double w = 0;
            if (std::abs (x) < halfWidth)
            {
                const double r = x / halfWidth;
                const double window = besselI0 (kaiserBeta * std::sqrt (1.0 - r * r)) / norm;
                const double arg = std::numbers::pi * x * cutoff;
                w = cutoff * (arg == 0.0 ? 1.0 : std::sin (arg) / arg) * window;
            }
            bank[static_cast<std::size_t> (p * taps + j)] = static_cast<float> (w);
            sum += w;
        }
        // Unity DC gain for every phase.
        for (int j = 0; j < taps; ++j)
            bank[static_cast<std::size_t> (p * taps + j)] = static_cast<float> (bank[static_cast<std::size_t> (p * taps + j)] / sum);
    }

    AudioData out;
    out.sampleRate = toRate;
    const auto inFrames = audio.numFrames();
    const auto outFrames = resampledLength (inFrames, from, toRate);
    for (const auto& in : audio.channels)
    {
        std::vector<float> o (static_cast<std::size_t> (outFrames));
        for (std::int64_t n = 0; n < outFrames; ++n)
        {
            const double pos = static_cast<double> (n) * step;
            const auto base = static_cast<std::int64_t> (std::floor (pos));
            auto phase = static_cast<std::int64_t> (std::llround ((pos - static_cast<double> (base)) * static_cast<double> (phases)));
            auto first = base - half;
            if (phase == phases)
            {
                phase = 0;
                ++first;
            }
            const float* w = bank.data() + phase * taps;
            double acc = 0;
            for (int j = 0; j < taps; ++j)
            {
                const auto k = first + j;
                if (k >= 0 && k < inFrames)
                    acc += static_cast<double> (w[j]) * in[static_cast<std::size_t> (k)];
            }
            o[static_cast<std::size_t> (n)] = static_cast<float> (acc);
        }
        out.channels.push_back (std::move (o));
    }
    return out;
}
} // namespace weft::media
