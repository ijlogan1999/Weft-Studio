#include "weft/engine/transport/LiveAutomation.h"

#include <algorithm>

namespace weft::engine
{
namespace
{
bool writes (const std::string& mode) noexcept { return mode == "touch" || mode == "latch" || mode == "write"; }
} // namespace

bool LiveAutomation::playing() const noexcept
{
    const auto s = engine.state();
    return s == TransportState::playing || s == TransportState::recording;
}

LiveAutomation::Take* LiveAutomation::find (const Id& trackId, const model::AutomationTarget& target)
{
    const auto key = target.key();
    for (auto& t : takes)
        if (t.trackId == trackId && t.target.key() == key)
            return &t;
    return nullptr;
}

const LiveAutomation::Take* LiveAutomation::find (const Id& trackId, const model::AutomationTarget& target) const
{
    return const_cast<LiveAutomation*> (this)->find (trackId, target);
}

bool LiveAutomation::isLive (const model::ProjectModel& model, const Id& trackId, const model::AutomationTarget& target) const
{
    const auto* t = model.track (trackId);
    if (t == nullptr || t->automationMode == "off")
        return false;
    if (const auto* lane = model.lane (trackId, target); lane != nullptr && ! lane->points.empty())
        return true;
    return writes (t->automationMode) && (playing() || find (trackId, target) != nullptr);
}

bool LiveAutomation::touching (const Id& trackId, const model::AutomationTarget& target) const
{
    const auto* take = find (trackId, target);
    return take != nullptr && take->touched;
}

std::vector<Id> LiveAutomation::writingTracks() const
{
    std::vector<Id> out;
    if (! playing())
        return out;
    for (const auto& t : takes)
        if (writes (t.mode) && t.held && std::find (out.begin(), out.end(), t.trackId) == out.end())
            out.push_back (t.trackId);
    return out;
}

void LiveAutomation::record (Take& take, double value, std::optional<double> at)
{
    const auto tick = static_cast<Ticks> (std::llround (std::max (0.0, at.value_or (engine.positionTicks()))));
    const auto seconds = at ? 0.0 : static_cast<double> (engine.positionSamples()) / engine.sampleRate();
    if (! take.recorder.empty() && tick < take.last)
        return; // the cycle wrapped: the pass keeps what it wrote before the jump (D-W8-4)
    if (take.recorder.empty())
        take.from = tick;
    take.recorder.add (seconds, tick, value);
    take.last = tick;
}

void LiveAutomation::close (Take& take)
{
    take.recorder.finish();
    if (! take.recorder.points().empty())
    {
        commands::Session::AutomationWrite write;
        write.trackId = take.trackId;
        write.target = take.target;
        write.from = take.from;
        write.to = std::max (take.last, take.recorder.points().back().first);
        write.points = take.recorder.points();
        done.push_back (std::move (write));
    }
    take.recorder = {};
}

void LiveAutomation::touch (const model::ProjectModel& model, const Id& trackId, const model::AutomationTarget& target, double value)
{
    const auto* t = model.track (trackId);
    if (t == nullptr)
        return;
    value = std::clamp (value, 0.0, 1.0);
    engine.touchAutomation (trackId, target, value);
    auto* take = find (trackId, target);
    if (take == nullptr)
    {
        takes.push_back ({ trackId, target, t->automationMode, {}, value, false, false, 0, 0 });
        take = &takes.back();
    }
    take->touched = take->held = true;
    take->value = value;
    if (playing() && writes (take->mode))
        record (*take, value);
}

void LiveAutomation::release (const model::ProjectModel&, const Id& trackId, const model::AutomationTarget& target)
{
    auto* take = find (trackId, target);
    if (take == nullptr)
    {
        engine.releaseAutomation (trackId, target, false);
        return;
    }
    take->touched = false;
    const bool rolling = playing();
    if (rolling && (take->mode == "latch" || take->mode == "write"))
        return; // holds the release value until stop (W-8)
    if (rolling && take->mode == "touch")
    {
        record (*take, take->value);
        close (*take);
    }
    // Read snaps back; Touch glides back over 200 ms.
    engine.releaseAutomation (trackId, target, rolling && take->mode == "touch");
    takes.erase (takes.begin() + (take - takes.data()));
}

std::optional<LiveAutomation::Pass> LiveAutomation::tick (const model::ProjectModel& model)
{
    const bool now = playing();
    std::optional<Pass> out;
    if (now && ! wasPlaying)
    {
        // A Write pass writes every lane of its tracks from play start, holding what the control
        // shows there until it is touched (W-8).
        const auto at = engine.playStartTicks();
        writeTracks.clear();
        for (const auto& [id, track] : model.tracks)
            if (track->automationMode == "write")
                writeTracks.push_back (id);
        for (const auto& [laneId, lane] : model.automationLanes)
        {
            if (lane->points.empty() || std::find (writeTracks.begin(), writeTracks.end(), lane->trackId) == writeTracks.end()
                || ! automation::targetExists (model, lane->trackId, lane->target) || find (lane->trackId, lane->target) != nullptr)
                continue;
            const auto value = automation::valueAt (automation::sortedPoints (*lane), at);
            engine.touchAutomation (lane->trackId, lane->target, value);
            takes.push_back ({ lane->trackId, lane->target, "write", {}, value, true, false, 0, 0 });
        }
        for (auto& take : takes)
            if (take.held && take.mode == "write")
                record (take, take.value, at);
    }
    if (now)
    {
        for (auto& take : takes)
            if (take.held && writes (take.mode))
                record (take, take.value);
    }
    if (! now && wasPlaying)
    {
        for (auto& take : takes)
            if (writes (take.mode) && ! take.recorder.empty())
                close (take);
        Pass pass;
        pass.writes = std::move (done);
        pass.toLatch = std::move (writeTracks);
        done.clear();
        writeTracks.clear();
        // Controls no longer held go back to their lanes, which now hold what was written.
        for (auto it = takes.begin(); it != takes.end();)
        {
            if (it->touched)
            {
                ++it;
                continue;
            }
            engine.releaseAutomation (it->trackId, it->target, false);
            it = takes.erase (it);
        }
        if (! pass.writes.empty() || ! pass.toLatch.empty())
            out = std::move (pass);
    }
    wasPlaying = now;
    return out;
}
} // namespace weft::engine
