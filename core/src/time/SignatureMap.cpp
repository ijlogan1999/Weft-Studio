#include "weft/time/SignatureMap.h"

#include <algorithm>

namespace weft
{
bool SignatureMap::isValid (int numerator, int denominator) noexcept
{
    const bool denominatorOk = denominator == 1 || denominator == 2 || denominator == 4 || denominator == 8
                            || denominator == 16 || denominator == 32;
    return numerator >= 1 && numerator <= 32 && denominatorOk;
}

SignatureMap::SignatureMap (std::vector<SignaturePoint> points)
{
    std::erase_if (points, [] (const auto& p) { return p.bar < 0 || ! isValid (p.numerator, p.denominator); });
    std::sort (points.begin(), points.end(), [] (const auto& a, const auto& b) { return a.bar < b.bar; });
    if (points.empty() || points.front().bar != 0)
        points.insert (points.begin(), SignaturePoint { "initial", 0, 4, 4, false });
    segments = std::move (points);

    segmentStartTicks.resize (segments.size());
    segmentStartTicks[0] = 0;
    for (std::size_t i = 1; i < segments.size(); ++i)
        segmentStartTicks[i] = segmentStartTicks[i - 1] + static_cast<Ticks> (segments[i].bar - segments[i - 1].bar) * segments[i - 1].barLength();
}

const SignaturePoint& SignatureMap::signatureAtBar (int bar) const noexcept
{
    const auto it = std::upper_bound (segments.begin(), segments.end(), bar, [] (int b, const SignaturePoint& p) { return b < p.bar; });
    return it == segments.begin() ? segments.front() : *(it - 1);
}

Ticks SignatureMap::barStart (int bar) const noexcept
{
    const auto it = std::upper_bound (segments.begin(), segments.end(), bar, [] (int b, const SignaturePoint& p) { return b < p.bar; });
    const auto i = it == segments.begin() ? 0 : static_cast<std::size_t> (it - segments.begin()) - 1;
    return segmentStartTicks[i] + static_cast<Ticks> (bar - segments[i].bar) * segments[i].barLength();
}

Ticks SignatureMap::barLength (int bar) const noexcept { return signatureAtBar (bar).barLength(); }

BarPosition SignatureMap::barAt (Ticks tick) const noexcept
{
    const auto it = std::upper_bound (segmentStartTicks.begin(), segmentStartTicks.end(), tick);
    const auto i = it == segmentStartTicks.begin() ? 0 : static_cast<std::size_t> (it - segmentStartTicks.begin()) - 1;
    const auto len = segments[i].barLength();
    const auto since = tick - segmentStartTicks[i];
    // Floor division so negative ticks map to bars before 0 consistently.
    auto bars = since / len;
    if (since % len < 0)
        --bars;
    return { segments[i].bar + static_cast<int> (bars), since - bars * len };
}
} // namespace weft
