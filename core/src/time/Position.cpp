#include "weft/time/Position.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <vector>

namespace weft
{
namespace
{
Ticks beatLength (const SignaturePoint& sig) noexcept { return 4 * ticksPerQuarter / sig.denominator; }

std::vector<std::string_view> split (std::string_view text, std::string_view separators)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= text.size())
    {
        const auto end = text.find_first_of (separators, start);
        const auto part = text.substr (start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (! part.empty())
            parts.push_back (part);
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return parts;
}

std::optional<long long> parseInt (std::string_view s)
{
    long long v = 0;
    const auto [ptr, ec] = std::from_chars (s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || ptr != s.data() + s.size())
        return std::nullopt;
    return v;
}

std::optional<double> parseNumber (std::string_view s)
{
    if (s.empty())
        return std::nullopt;
    std::string copy (s);
    char* end = nullptr;
    const double v = std::strtod (copy.c_str(), &end);
    if (end != copy.c_str() + copy.size() || ! std::isfinite (v))
        return std::nullopt;
    return v;
}
} // namespace

BarsBeats toBarsBeats (Ticks t, const SignatureMap& signatures) noexcept
{
    BarsBeats out;
    const auto bp = signatures.barAt (t);
    const auto& sig = signatures.signatureAtBar (bp.bar);
    const auto beat = beatLength (sig);
    const auto division = std::min (divisionTicks, beat);
    out.bar = bp.bar + 1;
    out.beat = static_cast<int> (bp.offset / beat) + 1;
    const auto inBeat = bp.offset % beat;
    out.division = static_cast<int> (inBeat / division) + 1;
    out.tick = static_cast<int> (inBeat % division);
    return out;
}

Ticks fromBarsBeats (const BarsBeats& p, const SignatureMap& signatures) noexcept
{
    const int bar = p.bar - 1;
    const auto& sig = signatures.signatureAtBar (bar);
    const auto beat = beatLength (sig);
    const auto division = std::min (divisionTicks, beat);
    return signatures.barStart (bar) + static_cast<Ticks> (p.beat - 1) * beat + static_cast<Ticks> (p.division - 1) * division + p.tick;
}

std::string formatBarsBeats (Ticks t, const SignatureMap& signatures)
{
    const auto p = toBarsBeats (t, signatures);
    return std::to_string (p.bar) + "." + std::to_string (p.beat) + "." + std::to_string (p.division) + "." + std::to_string (p.tick);
}

std::string formatTime (double seconds)
{
    const bool negative = seconds < 0;
    auto ms = static_cast<long long> (std::llround (std::abs (seconds) * 1000.0));
    const auto h = ms / 3'600'000;
    ms %= 3'600'000;
    const auto m = ms / 60'000;
    ms %= 60'000;
    char buf[48];
    std::snprintf (buf, sizeof buf, "%s%lld:%02lld:%02lld.%03lld", negative ? "-" : "", h, m, ms / 1000, ms % 1000);
    return buf;
}

std::optional<Ticks> parseBarsBeats (std::string_view text, const SignatureMap& signatures)
{
    const auto parts = split (text, " .:");
    if (parts.empty() || parts.size() > 4)
        return std::nullopt;
    int values[4] = { 1, 1, 1, 0 };
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        const auto v = parseInt (parts[i]);
        if (! v || *v < (i == 3 ? 0 : 1) || *v > 100000)
            return std::nullopt;
        values[i] = static_cast<int> (*v);
    }
    return fromBarsBeats ({ values[0], values[1], values[2], values[3] }, signatures);
}

std::optional<double> parseTime (std::string_view text)
{
    const auto parts = split (text, ":");
    if (parts.empty() || parts.size() > 3 || std::count (text.begin(), text.end(), ':') + 1 != static_cast<std::ptrdiff_t> (parts.size()))
        return std::nullopt;
    double total = 0;
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        const bool last = i + 1 == parts.size();
        std::optional<double> v;
        if (last)
            v = parseNumber (parts[i]);
        else if (const auto n = parseInt (parts[i]))
            v = static_cast<double> (*n);
        if (! v || *v < 0 || (i > 0 && *v >= 60))
            return std::nullopt;
        total = total * 60 + *v;
    }
    return total;
}

Ticks nextBar (Ticks t, const SignatureMap& signatures) noexcept { return signatures.barStart (signatures.barAt (t).bar + 1); }

Ticks previousBar (Ticks t, const SignatureMap& signatures) noexcept
{
    if (t <= 0)
        return 0;
    const auto bp = signatures.barAt (t);
    if (bp.offset > 0)
        return signatures.barStart (bp.bar);
    return bp.bar > 0 ? signatures.barStart (bp.bar - 1) : 0;
}
} // namespace weft
