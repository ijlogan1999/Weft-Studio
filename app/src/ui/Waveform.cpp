#include "Waveform.h"

#include <cmath>

namespace weft::ui
{
std::int16_t WaveSummary::pack (float v) noexcept { return static_cast<std::int16_t> (std::lround (std::clamp (v, -1.0f, 1.0f) * 32767.0f)); }

void WaveSummary::begin (std::int64_t totalFrames)
{
    length = std::max<std::int64_t> (0, totalFrames);
    const auto buckets = static_cast<std::size_t> ((length + baseFrames - 1) / baseFrames);
    levels.assign (1, std::vector<Packed> (buckets, Packed { 32767, -32767, 0 }));
    power.assign (buckets, 0.0);
    counted.assign (buckets, 0);
}

void WaveSummary::add (const float* const* channels, int numChannels, std::int64_t firstFrame, std::int64_t numFrames)
{
    auto& base = levels.front();
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* data = channels[ch];
        if (data == nullptr)
            continue;
        for (std::int64_t i = 0; i < numFrames; ++i)
        {
            const auto b = static_cast<std::size_t> ((firstFrame + i) / baseFrames);
            if (b >= base.size())
                break;
            const auto v = pack (data[i]);
            base[b].lo = std::min (base[b].lo, v);
            base[b].hi = std::max (base[b].hi, v);
            power[b] += static_cast<double> (data[i]) * data[i];
            ++counted[b];
        }
    }
}

void WaveSummary::finish()
{
    if (levels.empty())
        return;
    auto& base = levels.front();
    for (std::size_t b = 0; b < base.size(); ++b)
    {
        if (counted[b] == 0)
            base[b] = { 0, 0, 0 };
        else
            base[b].rms = pack (static_cast<float> (std::sqrt (power[b] / counted[b])));
    }
    power.clear();
    power.shrink_to_fit();
    counted.clear();
    counted.shrink_to_fit();
    // Coarser levels until a level fits in a few hundred buckets.
    while (levels.back().size() > 256)
    {
        const auto& fine = levels.back();
        std::vector<Packed> coarse ((fine.size() + levelFactor - 1) / levelFactor);
        for (std::size_t c = 0; c < coarse.size(); ++c)
        {
            Packed p { 32767, -32767, 0 };
            double sum = 0;
            int n = 0;
            for (std::size_t k = c * levelFactor; k < std::min (fine.size(), (c + 1) * levelFactor); ++k, ++n)
            {
                p.lo = std::min (p.lo, fine[k].lo);
                p.hi = std::max (p.hi, fine[k].hi);
                const auto r = unpack (fine[k].rms);
                sum += static_cast<double> (r) * r;
            }
            p.rms = pack (static_cast<float> (std::sqrt (sum / std::max (1, n))));
            coarse[c] = p;
        }
        levels.push_back (std::move (coarse));
    }
}

WaveSummary::Bucket WaveSummary::get (std::size_t level, std::int64_t index) const
{
    const auto& l = levels[level];
    if (index < 0 || index >= static_cast<std::int64_t> (l.size()))
        return {};
    const auto& p = l[static_cast<std::size_t> (index)];
    return { unpack (p.lo), unpack (p.hi), unpack (p.rms) };
}

WaveSummary::Bucket WaveSummary::at (double frame) const
{
    if (empty() || frame < 0.0 || frame >= static_cast<double> (length))
        return {};
    // Linear between bucket centres, so a zoomed-in outline is smooth rather than stepped.
    const auto pos = frame / baseFrames - 0.5;
    const auto i = static_cast<std::int64_t> (std::floor (pos));
    const auto t = static_cast<float> (pos - static_cast<double> (i));
    const auto last = static_cast<std::int64_t> (levels.front().size()) - 1;
    const auto a = get (0, std::clamp<std::int64_t> (i, 0, last)), b = get (0, std::clamp<std::int64_t> (i + 1, 0, last));
    return { a.lo + (b.lo - a.lo) * t, a.hi + (b.hi - a.hi) * t, a.rms + (b.rms - a.rms) * t };
}

WaveSummary::Bucket WaveSummary::range (double fromFrame, double toFrame) const
{
    if (empty() || toFrame <= 0.0 || fromFrame >= static_cast<double> (length))
        return {};
    fromFrame = std::max (0.0, fromFrame);
    toFrame = std::min (static_cast<double> (length), toFrame);
    // The coarsest level whose buckets are at most a quarter of the span keeps edges accurate.
    std::size_t level = 0;
    double width = baseFrames;
    while (level + 1 < levels.size() && width * levelFactor * 4.0 <= toFrame - fromFrame)
    {
        ++level;
        width *= levelFactor;
    }
    const auto first = static_cast<std::int64_t> (std::floor (fromFrame / width));
    const auto last = std::max (first, static_cast<std::int64_t> (std::ceil (toFrame / width)) - 1);
    Bucket out { 1.0f, -1.0f, 0.0f };
    double sum = 0;
    int n = 0;
    for (auto i = first; i <= last; ++i, ++n)
    {
        const auto b = get (level, i);
        out.lo = std::min (out.lo, b.lo);
        out.hi = std::max (out.hi, b.hi);
        sum += static_cast<double> (b.rms) * b.rms;
    }
    out.rms = static_cast<float> (std::sqrt (sum / std::max (1, n)));
    if (out.lo > out.hi)
        return {};
    return out;
}

void drawWaveform (juce::Graphics& g, const WaveSummary& s, juce::Rectangle<float> area, double firstFrame, double framesPerPixel, const WaveformStyle& style)
{
    if (s.empty() || area.isEmpty() || framesPerPixel <= 0.0)
        return;
    // Work in device pixels so the outline is exact on high-DPI screens.
    const auto scale = std::max (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    const auto step = 1.0f / scale;
    const auto mid = area.getCentreY();
    const auto half = area.getHeight() * 0.5f - 1.0f;
    const auto x0 = std::max (area.getX(), style.startX), x1 = std::min (area.getRight(), style.endX);
    if (x1 <= x0)
        return;
    auto y = [&] (float v) { return mid - std::clamp (v * style.gain, -1.0f, 1.0f) * half; };
    const bool zoomedIn = framesPerPixel * step < WaveSummary::baseFrames;

    std::vector<juce::Point<float>> top, bottom, rmsTop, rmsBottom;
    for (float x = x0; x <= x1 + step * 0.5f; x += step)
    {
        const auto f1 = firstFrame + static_cast<double> (x - area.getX()) * framesPerPixel;
        WaveSummary::Bucket b;
        if (zoomedIn)
            b = s.at (f1); // a smooth line through the finest buckets
        else
            b = s.range (f1, f1 + framesPerPixel * step);
        auto yHi = y (b.hi), yLo = y (b.lo);
        // Never thinner than a device pixel, so silence still shows as a line.
        if (yLo - yHi < step)
        {
            const auto c = (yHi + yLo) * 0.5f;
            yHi = c - step * 0.5f;
            yLo = c + step * 0.5f;
        }
        top.emplace_back (x, yHi);
        bottom.emplace_back (x, yLo);
        rmsTop.emplace_back (x, std::max (yHi, y (b.rms)));
        rmsBottom.emplace_back (x, std::min (yLo, y (-b.rms)));
    }
    auto outline = [] (const std::vector<juce::Point<float>>& upper, const std::vector<juce::Point<float>>& lower) {
        juce::Path p;
        p.preallocateSpace (static_cast<int> (upper.size() + lower.size()) * 3 + 4);
        p.startNewSubPath (upper.front());
        for (std::size_t i = 1; i < upper.size(); ++i)
            p.lineTo (upper[i]);
        for (auto it = lower.rbegin(); it != lower.rend(); ++it)
            p.lineTo (*it);
        p.closeSubPath();
        return p;
    };
    if (top.empty())
        return;
    g.setColour (style.peak);
    g.fillPath (outline (top, bottom));
    g.setColour (style.rms);
    g.fillPath (outline (rmsTop, rmsBottom));
}
} // namespace weft::ui
