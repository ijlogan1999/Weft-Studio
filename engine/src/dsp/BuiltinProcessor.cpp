#include "weft/engine/dsp/BuiltinProcessor.h"

#include "weft/doc/Value.h"

using weft::doc::Value;

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace weft::engine
{
double BuiltinParam::toPlain (double n) const noexcept
{
    n = std::clamp (n, 0.0, 1.0);
    switch (scale)
    {
        case ParamScale::logarithmic:
            return min * std::pow (max / min, n);
        case ParamScale::choice:
            return std::round (n * static_cast<double> (std::max<std::size_t> (1, choices.size()) - 1));
        case ParamScale::linear:
            break;
    }
    return min + n * (max - min);
}

double BuiltinParam::toNormalized (double plain) const noexcept
{
    plain = std::clamp (plain, min, max);
    switch (scale)
    {
        case ParamScale::logarithmic:
            return std::log (plain / min) / std::log (max / min);
        case ParamScale::choice:
            return choices.size() > 1 ? plain / static_cast<double> (choices.size() - 1) : 0.0;
        case ParamScale::linear:
            break;
    }
    return max > min ? (plain - min) / (max - min) : 0.0;
}

BuiltinProcessor::BuiltinProcessor (std::vector<BuiltinParam> params)
    : defs (std::move (params)),
      values (std::make_unique<std::atomic<double>[]> (defs.size()))
{
    for (std::size_t i = 0; i < defs.size(); ++i)
    {
        if (defs[i].scale == ParamScale::choice)
            defs[i].min = 0, defs[i].max = static_cast<double> (std::max<std::size_t> (1, defs[i].choices.size()) - 1);
        values[i].store (defs[i].toNormalized (defs[i].defaultValue));
    }
}

std::vector<ParameterInfo> BuiltinProcessor::parameters() const
{
    std::vector<ParameterInfo> out;
    for (const auto& d : defs)
        out.push_back ({ d.id, d.name, d.unit, d.toNormalized (d.defaultValue), d.scale == ParamScale::choice ? static_cast<int> (d.choices.size()) : 0 });
    return out;
}

void BuiltinProcessor::setParameter (int index, double normalized) noexcept
{
    if (index < 0 || static_cast<std::size_t> (index) >= defs.size())
        return;
    values[static_cast<std::size_t> (index)].store (std::clamp (normalized, 0.0, 1.0), std::memory_order_relaxed);
    changes.store (true, std::memory_order_release);
}

double BuiltinProcessor::parameter (int index) const noexcept
{
    if (index < 0 || static_cast<std::size_t> (index) >= defs.size())
        return 0;
    return values[static_cast<std::size_t> (index)].load (std::memory_order_relaxed);
}

std::string BuiltinProcessor::parameterText (int index, double normalized) const
{
    if (index < 0 || static_cast<std::size_t> (index) >= defs.size())
        return {};
    const auto& d = defs[static_cast<std::size_t> (index)];
    const auto v = d.toPlain (normalized);
    if (d.scale == ParamScale::choice)
        return d.choices.empty() ? std::string() : d.choices[static_cast<std::size_t> (v)];
    char text[48];
    if (d.unit == "Hz" && v >= 1000)
        std::snprintf (text, sizeof text, "%.2f kHz", v / 1000.0);
    else if (d.unit == "%")
        std::snprintf (text, sizeof text, "%.0f%%", v);
    else
        std::snprintf (text, sizeof text, std::abs (v) < 10 ? "%.2f %s" : "%.1f %s", v, d.unit.c_str());
    std::string s = text;
    while (! s.empty() && s.back() == ' ')
        s.pop_back();
    return s;
}

std::vector<std::uint8_t> BuiltinProcessor::state() const
{
    Value::Map m;
    for (std::size_t i = 0; i < defs.size(); ++i)
        m[defs[i].id] = Value (values[i].load (std::memory_order_relaxed));
    return doc::encode (Value (std::move (m)));
}

bool BuiltinProcessor::setState (std::span<const std::uint8_t> bytes)
{
    const auto v = doc::decode (bytes);
    if (! v || ! v->isMap())
        return false;
    for (std::size_t i = 0; i < defs.size(); ++i)
        if (const auto* p = v->find (defs[i].id); p != nullptr && p->isNumber())
            setParameter (static_cast<int> (i), p->asDouble());
    return true;
}

void BuiltinProcessor::setValue (const std::string& id, double plain) noexcept
{
    for (std::size_t i = 0; i < defs.size(); ++i)
        if (defs[i].id == id)
            setParameter (static_cast<int> (i), defs[i].toNormalized (plain));
}
} // namespace weft::engine
