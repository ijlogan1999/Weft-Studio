#include "weft/model/Automation.h"

#include "weft/time/Time.h"

#include <algorithm>
#include <cmath>

namespace weft::automation
{
double toDb (double normalized) noexcept
{
    if (normalized <= 0.0)
        return minusInfinityDb;
    return std::max (minusInfinityDb, 6.0 + 60.0 * std::log10 (std::min (normalized, 1.0)));
}

double fromDb (double db) noexcept
{
    if (db <= minusInfinityDb)
        return 0.0;
    return std::clamp (std::pow (10.0, (db - 6.0) / 60.0), 0.0, 1.0);
}

double shape (double t, double curve) noexcept
{
    t = std::clamp (t, 0.0, 1.0);
    const auto c = std::clamp (curve, -1.0, 1.0);
    if (std::abs (c) < 1e-9)
        return t;
    return std::pow (t, std::pow (4.0, c));
}

std::vector<model::AutoPoint> sortedPoints (const model::AutomationLane& lane)
{
    std::vector<model::AutoPoint> out;
    out.reserve (lane.points.size());
    for (const auto& [id, p] : lane.points)
        out.push_back (p);
    std::sort (out.begin(), out.end(), [] (const auto& a, const auto& b) { return a.pos != b.pos ? a.pos < b.pos : a.id < b.id; });
    return out;
}

double valueAt (const std::vector<model::AutoPoint>& points, double tick) noexcept
{
    if (tick <= static_cast<double> (points.front().pos))
        return points.front().value;
    if (tick >= static_cast<double> (points.back().pos))
        return points.back().value;
    const auto next = std::upper_bound (points.begin(), points.end(), tick, [] (double t, const auto& p) { return t < static_cast<double> (p.pos); });
    const auto& b = *next;
    const auto& a = *std::prev (next);
    const auto span = static_cast<double> (b.pos - a.pos);
    if (span <= 0)
        return b.value;
    return a.value + (b.value - a.value) * shape ((tick - static_cast<double> (a.pos)) / span, a.curve);
}

namespace
{
const model::Track* trackOf (const model::ProjectModel& m, const Id& trackId) { return m.track (trackId); }

std::vector<const model::PluginInstance*> pluginsOf (const model::Track& t)
{
    std::vector<const model::PluginInstance*> out;
    if (t.strip.instrument)
        out.push_back (&*t.strip.instrument);
    for (const auto& p : t.strip.inserts)
        out.push_back (&p);
    return out;
}
} // namespace

std::vector<TargetChoice> targetsFor (const model::ProjectModel& m, const Id& trackId, const ParamLister& params)
{
    std::vector<TargetChoice> out;
    const auto* t = trackOf (m, trackId);
    if (t == nullptr)
        return out;
    for (const auto* kind : { "volume", "pan", "mute" })
    {
        model::AutomationTarget target;
        target.kind = kind;
        out.push_back ({ target, targetName (m, trackId, target, params) });
    }
    for (const auto& s : t->strip.sends)
    {
        model::AutomationTarget target;
        target.kind = "sendLevel";
        target.sendId = s.id;
        out.push_back ({ target, targetName (m, trackId, target, params) });
    }
    for (const auto* p : pluginsOf (*t))
        if (params)
            for (const auto& [paramId, name] : params (*p))
            {
                model::AutomationTarget target;
                target.kind = "pluginParam";
                target.pluginId = p->id;
                target.paramId = paramId;
                out.push_back ({ target, p->name + ": " + name });
            }
    return out;
}

std::string targetName (const model::ProjectModel& m, const Id& trackId, const model::AutomationTarget& target, const ParamLister& params)
{
    if (target.kind == "volume")
        return "Volume";
    if (target.kind == "pan")
        return "Pan";
    if (target.kind == "mute")
        return "Mute";
    const auto* t = trackOf (m, trackId);
    if (target.kind == "sendLevel")
    {
        if (t != nullptr)
            for (const auto& s : t->strip.sends)
                if (s.id == target.sendId)
                {
                    const auto* bus = m.track (s.targetBusId);
                    return "Send to " + (bus != nullptr ? bus->name : std::string ("bus"));
                }
        return "Send level";
    }
    if (t != nullptr)
        for (const auto* p : pluginsOf (*t))
            if (p->id == target.pluginId)
            {
                if (params)
                    for (const auto& [paramId, name] : params (*p))
                        if (paramId == target.paramId)
                            return p->name + ": " + name;
                return p->name + ": " + target.paramId;
            }
    return target.paramId;
}

bool targetExists (const model::ProjectModel& m, const Id& trackId, const model::AutomationTarget& target)
{
    const auto* t = trackOf (m, trackId);
    if (t == nullptr)
        return false;
    if (target.kind == "volume" || target.kind == "pan" || target.kind == "mute")
        return true;
    if (target.kind == "sendLevel")
        return std::any_of (t->strip.sends.begin(), t->strip.sends.end(), [&] (const auto& s) { return s.id == target.sendId; });
    if (target.kind == "pluginParam")
    {
        const auto plugins = pluginsOf (*t);
        return std::any_of (plugins.begin(), plugins.end(), [&] (const auto* p) { return p->id == target.pluginId; });
    }
    return false;
}

double staticValue (const model::Track& t, const model::AutomationTarget& target, double fallback)
{
    if (target.kind == "volume")
        return fromDb (t.strip.volumeDb);
    if (target.kind == "pan")
        return fromPan (t.strip.pan);
    if (target.kind == "mute")
        return t.mute ? 1.0 : 0.0;
    if (target.kind == "sendLevel")
    {
        for (const auto& s : t.strip.sends)
            if (s.id == target.sendId)
                return fromDb (s.levelDb);
        return fallback;
    }
    for (const auto* p : pluginsOf (t))
        if (p->id == target.pluginId)
        {
            const auto it = p->params.find (target.paramId);
            return it != p->params.end() ? it->second : fallback;
        }
    return fallback;
}

// Recorder ------------------------------------------------------------------------------------

void Recorder::add (double seconds, Ticks tick, double value)
{
    if (kept.empty())
    {
        kept.emplace_back (tick, value);
        lastSeconds = seconds;
        pending = false;
        return;
    }
    if (std::abs (value - kept.back().second) > minDelta && seconds - lastSeconds >= minSeconds)
    {
        kept.emplace_back (tick, value);
        lastSeconds = seconds;
        pending = false;
        return;
    }
    latest = { tick, value };
    latestSeconds = seconds;
    pending = true;
}

void Recorder::finish()
{
    if (! pending)
        return;
    pending = false;
    if (latest.first <= kept.back().first)
        return;
    // Keep the end where the control was; never closer than 50 ms to the previous point.
    if (kept.size() > 1 && latestSeconds - lastSeconds < minSeconds)
        kept.back() = latest;
    else
        kept.push_back (latest);
}
} // namespace weft::automation
