#pragma once

// Automation values and curves (03 §8.2, W-8): the fader taper that maps normalized lane
// values to decibels, segment curves, the value of a lane at a position, the parameters a
// track can automate, and the thinning of live-written automation (D-W8-2).

#include "weft/model/ProjectModel.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace weft::automation
{
// Weft's fader taper (D-ARCH-28), fixed because saved automation depends on it:
// dB = 6 + 60 * log10 (v), so v = 1 is +6 dB, v ~ 0.794 is 0 dB, v = 0.5 is about -12 dB
// and v = 0 is -inf. Volume and send levels use it.
double toDb (double normalized) noexcept;
double fromDb (double db) noexcept;
/** Pan: 0..1 <-> -1..+1. */
inline double toPan (double normalized) noexcept { return normalized * 2.0 - 1.0; }
inline double fromPan (double pan) noexcept { return (pan + 1.0) * 0.5; }
/** Mute: on at 0.5 and above. */
inline bool toMute (double normalized) noexcept { return normalized >= 0.5; }

/** A segment's shape: t in 0..1 to 0..1, monotonic for any curve in -1..1; 0 is linear,
    +1 starts slow (t^4), -1 starts fast (t^0.25) (D-DM-22). */
double shape (double t, double curve) noexcept;

/** A lane's points sorted by position (ties by id). */
std::vector<model::AutoPoint> sortedPoints (const model::AutomationLane& lane);
/** The lane's value at a tick: held before the first point and after the last (W-8 step 4).
    Points must be sorted and non-empty. */
double valueAt (const std::vector<model::AutoPoint>& points, double tick) noexcept;

/** A parameter a track can automate, as its selector lists it (AT-AUT-2). */
struct TargetChoice
{
    model::AutomationTarget target;
    std::string name;
};
/** A plug-in's automatable parameters as (id, name); the engine knows them. */
using ParamLister = std::function<std::vector<std::pair<std::string, std::string>> (const model::PluginInstance&)>;
/** Volume, Pan, Mute, each send's level, then every parameter of the instrument and each insert. */
std::vector<TargetChoice> targetsFor (const model::ProjectModel& model, const Id& trackId, const ParamLister& params);
/** Display name of a target ("Volume", "Send to Reverb", "Weft EQ: Low Gain"). */
std::string targetName (const model::ProjectModel& model, const Id& trackId, const model::AutomationTarget& target, const ParamLister& params);
/** Whether the target still exists on the track (its send or plug-in may have been removed). */
bool targetExists (const model::ProjectModel& model, const Id& trackId, const model::AutomationTarget& target);
/** The target's static value, normalized: what an empty lane plays (W-8 step 5). Plug-in
    parameters use the model's `params`, or `fallback` when it has none. */
double staticValue (const model::Track& track, const model::AutomationTarget& target, double fallback = 0.0);

/**
    Thins live-written values (D-W8-2): a point is kept when the value has moved more than
    0.1 % of the range since the last kept point and at least 50 ms have passed, so a constant
    value gives a start and an end point and a ramp gives at most one point per 50 ms.
*/
class Recorder
{
public:
    static constexpr double minDelta = 0.001;
    static constexpr double minSeconds = 0.05;
    /** A value at a position; `seconds` is the transport time, used for the 50 ms rule. */
    void add (double seconds, Ticks tick, double value);
    /** Ends the pass: the last value is kept so the written automation ends where the control was. */
    void finish();
    const std::vector<std::pair<Ticks, double>>& points() const noexcept { return kept; }
    bool empty() const noexcept { return kept.empty() && ! pending; }
    std::optional<Ticks> firstTick() const { return kept.empty() ? std::nullopt : std::optional<Ticks> (kept.front().first); }

private:
    std::vector<std::pair<Ticks, double>> kept;
    double lastSeconds = 0, latestSeconds = 0;
    bool pending = false;
    std::pair<Ticks, double> latest {};
};
} // namespace weft::automation
