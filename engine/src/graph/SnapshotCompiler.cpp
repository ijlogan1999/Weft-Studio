#include "weft/engine/graph/EngineSnapshot.h"

#include "weft/model/Automation.h"
#include "weft/time/Quantize.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <numbers>
#include <optional>

namespace weft::engine
{
namespace
{
using Interval = std::pair<std::int64_t, std::int64_t>;

/** The MIDI message for a controller point; pitch bend is 14-bit, centre 8192 (F-PRL-12). */
std::optional<ScheduledEvent> scheduledEvent (const model::CcEvent& e)
{
    const auto ch = static_cast<std::uint8_t> ((std::clamp (e.channel, 1, 16) - 1) & 0x0f);
    const auto b7 = [] (int v) { return static_cast<std::uint8_t> (std::clamp (v, 0, 127)); };
    ScheduledEvent out;
    if (e.kind == "cc")
        out = { 0, static_cast<std::uint8_t> (0xb0 | ch), b7 (e.number), b7 (e.value) };
    else if (e.kind == "pitchBend")
    {
        const auto v = std::clamp (e.value, -8192, 8191) + 8192;
        out = { 0, static_cast<std::uint8_t> (0xe0 | ch), static_cast<std::uint8_t> (v & 0x7f), static_cast<std::uint8_t> ((v >> 7) & 0x7f) };
    }
    else if (e.kind == "channelPressure")
        out = { 0, static_cast<std::uint8_t> (0xd0 | ch), b7 (e.value), 0 };
    else if (e.kind == "polyPressure")
        out = { 0, static_cast<std::uint8_t> (0xa0 | ch), b7 (e.number), b7 (e.value) };
    else if (e.kind == "programChange")
        out = { 0, static_cast<std::uint8_t> (0xc0 | ch), b7 (e.value), 0 };
    else
        return std::nullopt;
    return out;
}

/** Parts of [a, b) not in covered (sorted, disjoint); then adds [coverA, coverB) to covered. */
std::vector<Interval> takeVisible (std::vector<Interval>& covered, std::int64_t a, std::int64_t b, std::int64_t coverA, std::int64_t coverB)
{
    std::vector<Interval> visible;
    auto cursor = a;
    for (const auto& [ca, cb] : covered)
    {
        if (cb <= cursor)
            continue;
        if (ca >= b)
            break;
        if (ca > cursor)
            visible.emplace_back (cursor, std::min (ca, b));
        cursor = std::max (cursor, cb);
        if (cursor >= b)
            break;
    }
    if (cursor < b)
        visible.emplace_back (cursor, b);

    if (coverA < coverB)
        covered.emplace_back (coverA, coverB);
    std::sort (covered.begin(), covered.end());
    std::vector<Interval> merged;
    for (const auto& iv : covered)
    {
        if (! merged.empty() && iv.first <= merged.back().second)
            merged.back().second = std::max (merged.back().second, iv.second);
        else
            merged.push_back (iv);
    }
    covered = std::move (merged);
    return visible;
}

FadeCurve curveFromString (const std::string& s) noexcept
{
    if (s == "linear")
        return FadeCurve::linear;
    if (s == "sCurve")
        return FadeCurve::sCurve;
    return FadeCurve::equalPower;
}

/** One timeline instance of a region (a region loop repeats it). */
struct Instance
{
    const model::Region* region = nullptr;
    Ticks startTick = 0, endTick = 0;     // MIDI
    std::int64_t start = 0, end = 0;      // device samples
    std::int64_t fullLength = 0;          // audio: unclipped instance length in device samples
};
} // namespace

float fadeGain (FadeCurve curve, double x) noexcept
{
    x = std::clamp (x, 0.0, 1.0);
    switch (curve)
    {
        case FadeCurve::linear:
            return static_cast<float> (x);
        case FadeCurve::sCurve:
            return static_cast<float> (0.5 - 0.5 * std::cos (std::numbers::pi * x));
        case FadeCurve::equalPower:
            break;
    }
    return static_cast<float> (std::sin (0.5 * std::numbers::pi * x));
}

float dbToGain (double db) noexcept { return db <= minusInfinityDb ? 0.0f : static_cast<float> (std::pow (10.0, db / 20.0)); }

std::int64_t EngineSnapshot::tickToSample (Ticks t) const noexcept { return std::llround (tempo.tickToSample (static_cast<double> (t))); }

std::pair<float, float> panGains (double pan) noexcept
{
    const double theta = (std::clamp (pan, -1.0, 1.0) + 1.0) * std::numbers::pi / 4.0;
    return { static_cast<float> (std::cos (theta)), static_cast<float> (std::sin (theta)) };
}

std::pair<float, float> balanceGains (double pan) noexcept
{
    pan = std::clamp (pan, -1.0, 1.0);
    return { static_cast<float> (pan > 0 ? 1.0 - pan : 1.0), static_cast<float> (pan < 0 ? 1.0 + pan : 1.0) };
}

int EngineSnapshot::channelIndex (const Id& trackId) const noexcept
{
    for (std::size_t i = 0; i < channels.size(); ++i)
        if (channels[i].trackId == trackId)
            return static_cast<int> (i);
    return -1;
}

namespace
{
void planRegions (ChannelPlan& plan, const model::ProjectModel& model, const model::Track& track, EngineSnapshot& snapshot,
                  const MediaPool& mediaPool, double projectRate)
{
    auto* snap = &snapshot;
    const double deviceRate = snapshot.sampleRate;
    const double projectToDevice = deviceRate / projectRate;
    const auto trackId = track.id;

    // Expand regions into timeline instances. Muted regions neither play nor hide others;
    // in a take folder only the active take plays (03 §4.4).
    std::vector<Instance> instances;
    for (const auto* r : model.regionsOnTrack (trackId))
    {
        if (r->muted || r->length <= 0 || r->isMidi != (track.kind == model::TrackKind::instrument))
            continue;
        if (! r->takeFolderId.empty())
        {
            const auto folder = model.takeFolders.find (r->takeFolderId);
            if (folder != model.takeFolders.end() && folder->second->activeTakeId != r->id)
                continue;
        }
        const auto loopEnd = r->loop && r->loop->enabled ? r->loop->endTicks : Ticks { 0 };
        if (r->isMidi)
        {
            const Ticks stopTick = std::max (r->start + r->length, loopEnd);
            for (Ticks s = r->start; s < stopTick; s += r->length)
            {
                Instance inst { r, s, std::min (s + r->length, stopTick), 0, 0, 0 };
                inst.start = snap->tickToSample (inst.startTick);
                inst.end = snap->tickToSample (inst.endTick);
                instances.push_back (inst);
            }
        }
        else
        {
            const auto start = snap->tickToSample (r->start);
            const auto length = std::max<std::int64_t> (1, std::llround (static_cast<double> (r->length) * projectToDevice));
            const auto stop = std::max (start + length, loopEnd > r->start ? snap->tickToSample (loopEnd) : std::int64_t { 0 });
            for (auto s = start; s < stop; s += length)
                instances.push_back ({ r, 0, 0, s, std::min (s + length, stop), length });
        }
    }
    // Topmost first (D-W3-2): higher z, then later id, wins where regions overlap.
    std::stable_sort (instances.begin(), instances.end(), [] (const Instance& a, const Instance& b) {
        if (a.region->z != b.region->z)
            return a.region->z > b.region->z;
        return a.region->id > b.region->id;
    });

    // Fades of higher audio regions don't hide what's under them: the lower region plays
    // the complementary fade, so they crossfade (D-W1-5).
    struct FadeZone
    {
        std::int64_t start, end;
        bool rising;
        FadeCurve curve;
    };
    std::vector<FadeZone> zones;
    std::vector<Interval> covered;
    for (const auto& inst : instances)
    {
        const auto& r = *inst.region;
        const auto length = inst.end - inst.start;
        std::int64_t fadeIn = 0, fadeOut = 0;
        if (! r.isMidi)
        {
            fadeIn = std::clamp<std::int64_t> (std::llround (static_cast<double> (r.audio.fadeIn.length) * projectToDevice), 0, length);
            fadeOut = std::clamp<std::int64_t> (std::llround (static_cast<double> (r.audio.fadeOut.length) * projectToDevice), 0, length - fadeIn);
        }
        const auto visible = takeVisible (covered, inst.start, inst.end, inst.start + fadeIn, inst.end - fadeOut);
        if (visible.empty())
            continue;

        if (! r.isMidi)
        {
            auto audio = mediaPool.find (r.audio.media);
            if (! audio || audio->numFrames() == 0)
                continue;
            if (std::find (snap->media.begin(), snap->media.end(), audio) == snap->media.end())
                snap->media.push_back (audio);

            // Split the visible parts where higher regions' fade zones start and end.
            std::vector<Interval> pieces;
            for (const auto& [a, b] : visible)
            {
                std::vector<std::int64_t> cuts { a, b, inst.start + fadeIn, inst.end - fadeOut };
                for (const auto& z : zones)
                    for (const auto c : { z.start, z.end })
                        if (c > a && c < b)
                            cuts.push_back (c);
                std::erase_if (cuts, [&] (std::int64_t c) { return c < a || c > b; });
                std::sort (cuts.begin(), cuts.end());
                cuts.erase (std::unique (cuts.begin(), cuts.end()), cuts.end());
                for (std::size_t i = 0; i + 1 < cuts.size(); ++i)
                    pieces.emplace_back (cuts[i], cuts[i + 1]);
            }
            for (const auto& [a, b] : pieces)
            {
                AudioSegment seg;
                // Where this region has its own fade, a higher region's fade is an explicit
                // crossfade: the two curves already complement each other (D-ARCH-23).
                const bool ownFade = a < inst.start + fadeIn || b > inst.end - fadeOut;
                for (const auto& z : zones)
                    if (! ownFade && z.start <= a && b <= z.end)
                    {
                        seg.duckFrom = z.start;
                        seg.duckLength = z.end - z.start;
                        seg.duckRising = z.rising;
                        seg.duckCurve = z.curve;
                        break;
                    }
                seg.start = a;
                seg.end = b;
                seg.regionStart = inst.start;
                seg.regionEnd = inst.end;
                seg.media = audio.get();
                seg.sourceAtRegionStart = static_cast<double> (r.audio.sourceOffset) * audio->sampleRate() / projectRate;
                seg.sourceStep = audio->sampleRate() / deviceRate;
                seg.gain = dbToGain (r.audio.gainDb);
                seg.fadeIn = fadeIn;
                seg.fadeOut = fadeOut;
                seg.fadeInCurve = curveFromString (r.audio.fadeIn.curve);
                seg.fadeOutCurve = curveFromString (r.audio.fadeOut.curve);
                plan.segments.push_back (seg);
                plan.longestSegment = std::max (plan.longestSegment, b - a);
            }
            if (fadeIn > 0)
                zones.push_back ({ inst.start, inst.start + fadeIn, true, curveFromString (r.audio.fadeIn.curve) });
            if (fadeOut > 0)
                zones.push_back ({ inst.end - fadeOut, inst.end, false, curveFromString (r.audio.fadeOut.curve) });
            continue;
        }

        const auto* seq = model.sequence (r.midi.sequenceId);
        if (seq == nullptr)
            continue;
        const auto windowStart = r.midi.sequenceOffset;
        const auto windowEnd = windowStart + r.length;
        for (const auto* note : seq->sortedNotes())
        {
            if (note->muted || note->pos < windowStart || note->pos >= windowEnd)
                continue;
            auto onTick = inst.startTick + (note->pos - windowStart);
            if (const auto& q = r.midi.quantize)
                onTick = quantizeTick (onTick, q->grid, q->strength, q->swing * 100.0);
            if (onTick < inst.startTick || onTick >= inst.endTick)
                continue;
            const auto on = snap->tickToSample (onTick);
            const bool shown = std::any_of (visible.begin(), visible.end(), [on] (const Interval& iv) { return on >= iv.first && on < iv.second; });
            if (! shown)
                continue;
            ScheduledNote n;
            n.on = on;
            n.off = std::max (on + 1, snap->tickToSample (std::min (onTick + std::max<Ticks> (1, note->length), inst.endTick)));
            n.channel = static_cast<std::uint8_t> (std::clamp (note->channel, 1, 16));
            n.pitch = static_cast<std::uint8_t> (std::clamp (note->pitch + r.midi.transpose, 0, 127));
            n.velocity = static_cast<std::uint8_t> (std::clamp (note->velocity + r.midi.velocityOffset, 1, 127));
            plan.notes.push_back (n);
        }
        for (const auto& [id, e] : seq->controllers)
        {
            if (e.pos < windowStart || e.pos >= windowEnd)
                continue;
            const auto atTick = inst.startTick + (e.pos - windowStart);
            if (atTick < inst.startTick || atTick >= inst.endTick)
                continue;
            const auto at = snap->tickToSample (atTick);
            if (! std::any_of (visible.begin(), visible.end(), [at] (const Interval& iv) { return at >= iv.first && at < iv.second; }))
                continue;
            if (auto out = scheduledEvent (e))
            {
                out->at = at;
                plan.controllers.push_back (*out);
            }
        }
    }
    std::sort (plan.segments.begin(), plan.segments.end(), [] (const AudioSegment& a, const AudioSegment& b) { return a.start < b.start; });
    std::stable_sort (plan.notes.begin(), plan.notes.end(), [] (const ScheduledNote& a, const ScheduledNote& b) { return a.on < b.on; });
    std::stable_sort (plan.controllers.begin(), plan.controllers.end(), [] (const ScheduledEvent& a, const ScheduledEvent& b) { return a.at < b.at; });
}

MonitorMode monitorFromString (const std::string& s)
{
    if (s == "on")
        return MonitorMode::on;
    if (s == "off")
        return MonitorMode::off;
    return MonitorMode::autoMode;
}
} // namespace

namespace
{
/** The processing inserts of a strip, and their total latency. */
std::vector<InsertPlan> planInserts (const model::ChannelStrip& strip, const EffectRack& effects, int& latency)
{
    std::vector<InsertPlan> out;
    for (const auto& p : strip.inserts)
    {
        if (p.bypass)
            continue;
        if (auto slot = effects.find (p.id))
        {
            latency += std::max (0, slot->processor->latencySamples());
            out.push_back ({ std::move (slot), p.id });
        }
    }
    return out;
}
} // namespace

bool followsAutomation (const model::Track& track) noexcept { return track.automationMode != "off"; }

double AutomationPlan::laneValue (std::int64_t at) const noexcept
{
    if (nodes.empty())
        return staticValue;
    if (at <= nodes.front().at)
        return nodes.front().value;
    if (at >= nodes.back().at)
        return nodes.back().value;
    const auto next = std::upper_bound (nodes.begin(), nodes.end(), at, [] (std::int64_t t, const AutomationNode& n) { return t < n.at; });
    const auto& b = *next;
    const auto& a = *std::prev (next);
    if (b.at <= a.at)
        return b.value;
    const auto t = static_cast<double> (at - a.at) / static_cast<double> (b.at - a.at);
    return a.value + (b.value - a.value) * automation::shape (t, a.curve);
}

double AutomationPlan::valueAt (std::int64_t at, int frames) const noexcept
{
    auto& s = *slot;
    double v = s.touched.load (std::memory_order_acquire);
    if (std::isnan (v))
    {
        v = laneValue (at);
        if (s.glide.exchange (false, std::memory_order_acq_rel))
        {
            const auto from = s.applied.load (std::memory_order_relaxed);
            if (! std::isnan (from))
            {
                s.glideFrom = from;
                s.glideLength = s.glideLeft = std::max<std::int64_t> (1, glideFrames);
            }
        }
        if (s.glideLeft > 0)
        {
            const auto x = static_cast<double> (s.glideLeft) / static_cast<double> (s.glideLength);
            v += (s.glideFrom - v) * x;
            s.glideLeft = std::max<std::int64_t> (0, s.glideLeft - frames);
        }
    }
    else
        s.glideLeft = 0;
    s.applied.store (v, std::memory_order_relaxed);
    return v;
}

namespace
{
/**
    The automation plans of a channel (W-8): every lane the track's mode follows, plus targets a
    control is touching. Targets that no longer exist (a removed send or plug-in) are skipped.
*/
std::vector<AutomationPlan> planAutomation (const model::ProjectModel& model, const model::Track& track, const EngineSnapshot& snap, MixerState& mixer,
                                            const std::set<std::string>& touched, const std::vector<SendPlan>& sends,
                                            const std::vector<InsertPlan>& inserts, const InstrumentSlot* instrument)
{
    std::map<std::string, std::pair<model::AutomationTarget, const model::AutomationLane*>> targets;
    if (followsAutomation (track))
        for (const auto& [laneId, lane] : model.automationLanes)
            if (lane->trackId == track.id && ! lane->points.empty())
                targets[lane->target.key()] = { lane->target, lane.get() };
    const auto prefix = track.id + "|";
    for (auto it = touched.lower_bound (prefix); it != touched.end() && it->starts_with (prefix); ++it)
    {
        const auto key = it->substr (prefix.size());
        if (! targets.contains (key))
            if (auto target = model::AutomationTarget::fromKey (key))
                targets[key] = { *target, nullptr };
    }

    std::vector<AutomationPlan> out;
    for (const auto& [key, entry] : targets)
    {
        const auto& [target, lane] = entry;
        AutomationPlan plan;
        double fallback = 0;
        if (target.kind == "volume")
            plan.kind = AutomationKind::volume;
        else if (target.kind == "pan")
            plan.kind = AutomationKind::pan;
        else if (target.kind == "mute")
            plan.kind = AutomationKind::mute;
        else if (target.kind == "sendLevel")
        {
            plan.kind = AutomationKind::send;
            for (std::size_t i = 0; i < sends.size(); ++i)
                if (sends[i].sendId == target.sendId)
                    plan.index = static_cast<int> (i);
            if (plan.index < 0)
                continue; // removed, muted or looping: nothing to automate
        }
        else if (target.kind == "pluginParam")
        {
            plan.kind = AutomationKind::param;
            const Processor* processor = nullptr;
            if (instrument != nullptr && track.strip.instrument && track.strip.instrument->id == target.pluginId)
                processor = &instrument->instrument();
            for (std::size_t i = 0; i < inserts.size(); ++i)
                if (inserts[i].pluginId == target.pluginId)
                {
                    plan.index = static_cast<int> (i);
                    processor = inserts[i].slot->processor.get();
                }
            if (processor == nullptr)
                continue; // removed or bypassed
            plan.param = processor->parameterIndex (target.paramId);
            if (plan.param < 0)
                continue;
            fallback = processor->parameter (plan.param);
        }
        else
            continue;
        plan.staticValue = automation::staticValue (track, target, fallback);
        if (lane != nullptr)
            for (const auto& p : automation::sortedPoints (*lane))
                plan.nodes.push_back ({ snap.tickToSample (p.pos), p.value, p.curve });
        plan.glideFrames = std::max<std::int64_t> (1, std::llround (AutomationSlot::glideSeconds * snap.sampleRate));
        plan.slot = mixer.automation (track.id, key);
        out.push_back (std::move (plan));
    }
    return out;
}
} // namespace

std::unique_ptr<EngineSnapshot> compileSnapshot (const model::ProjectModel& model, double deviceRate, int maxBlock, const MediaPool& mediaPool,
                                                 MixerRacks racks, const LocalMixState& local, const std::set<std::string>& touchedTargets)
{
    const auto& instruments = racks.instruments;
    auto snap = std::make_unique<EngineSnapshot>();
    snap->sampleRate = deviceRate;
    snap->maxBlock = std::max (1, maxBlock);
    snap->tempo = TempoMap (model.tempo.points(), deviceRate);
    snap->signatures = model.signatures;
    const auto& cycle = model.info.cycle;
    snap->cycleEnabled = cycle.enabled && cycle.end > cycle.start;
    snap->cycleStart = snap->tickToSample (cycle.start);
    snap->cycleEnd = snap->tickToSample (cycle.end);
    const auto& punch = model.info.punch;
    snap->punchEnabled = punch.enabled && punch.end > punch.start;
    snap->punchStart = snap->tickToSample (punch.start);
    snap->punchEnd = snap->tickToSample (punch.end);
    snap->fadeFrames = std::max (1, static_cast<int> (std::lround (0.010 * deviceRate)));
    snap->masterState = racks.mixer.channel (model.info.masterTrackId);
    if (const auto* master = model.track (model.info.masterTrackId))
    {
        snap->masterGain = dbToGain (master->strip.volumeDb);
        snap->masterAudible = ! master->mute;
        const auto [ml, mr] = balanceGains (master->strip.pan);
        snap->masterPanLeft = ml;
        snap->masterPanRight = mr;
        int unused = 0;
        snap->masterInserts = planInserts (master->strip, racks.effects, unused);
    }

    const double projectRate = model.info.sampleRate > 0 ? model.info.sampleRate : 48000.0;

    // Routing (non-disabled edges only, repair 5): where each channel's output and sends go.
    std::map<Id, Id> outputOf;
    std::map<Id, std::vector<std::pair<Id, const model::Send*>>> sendsOf;
    std::map<Id, std::vector<Id>> feeds; // track -> destinations (outputs and sends)
    for (const auto& edge : model.routingEdges())
    {
        if (model.disabledEdges.contains (edge.key()))
            continue;
        const auto* to = model.track (edge.toTrackId);
        if (to == nullptr || to->kind != model::TrackKind::bus)
            continue;
        feeds[edge.fromTrackId].push_back (edge.toTrackId);
        if (edge.sendId.empty())
            outputOf[edge.fromTrackId] = edge.toTrackId;
        else if (const auto* from = model.track (edge.fromTrackId))
            for (const auto& send : from->strip.sends)
                if (send.id == edge.sendId)
                    sendsOf[edge.fromTrackId].emplace_back (edge.toTrackId, &send);
    }

    // Solo (D-W6-1): soloed channels plus everything upstream and downstream of them.
    std::set<Id> audibleBySolo;
    if (! local.soloed.empty())
    {
        std::map<Id, std::vector<Id>> fedBy;
        for (const auto& [from, tos] : feeds)
            for (const auto& to : tos)
                fedBy[to].push_back (from);
        const auto walk = [&] (const std::map<Id, std::vector<Id>>& graph) {
            std::vector<Id> stack (local.soloed.begin(), local.soloed.end());
            while (! stack.empty())
            {
                const auto id = stack.back();
                stack.pop_back();
                const auto it = graph.find (id);
                if (it == graph.end())
                    continue;
                for (const auto& next : it->second)
                    if (audibleBySolo.insert (next).second)
                        stack.push_back (next);
            }
        };
        audibleBySolo.insert (local.soloed.begin(), local.soloed.end());
        walk (feeds);
        walk (fedBy);
    }

    // Processing order: a channel's depth is the longest chain of buses before it.
    std::map<Id, int> depth;
    const std::function<int (const Id&, int)> depthOf = [&] (const Id& id, int guard) -> int {
        if (const auto it = depth.find (id); it != depth.end())
            return it->second;
        int d = 0;
        if (guard < 64)
            for (const auto& [from, tos] : feeds)
                if (std::find (tos.begin(), tos.end(), id) != tos.end())
                    d = std::max (d, depthOf (from, guard + 1) + 1);
        depth[id] = d;
        return d;
    };

    std::vector<const model::Track*> ordered;
    for (const auto& trackId : model.trackOrder)
        if (const auto* track = model.track (trackId);
            track != nullptr && (track->kind == model::TrackKind::audio || track->kind == model::TrackKind::instrument || track->kind == model::TrackKind::bus))
            ordered.push_back (track);
    std::stable_sort (ordered.begin(), ordered.end(), [&] (const model::Track* a, const model::Track* b) { return depthOf (a->id, 0) < depthOf (b->id, 0); });

    for (const auto* track : ordered)
    {
        ChannelPlan plan;
        plan.trackId = track->id;
        plan.kind = track->kind;
        plan.sourceChannels = track->kind == model::TrackKind::audio ? std::clamp (track->channels, 1, 2) : 2;
        plan.gain = dbToGain (track->strip.volumeDb);
        const auto [l, r] = plan.sourceChannels == 1 ? panGains (track->strip.pan) : balanceGains (track->strip.pan);
        plan.panLeft = l;
        plan.panRight = r;
        plan.soloAudible = local.soloed.empty() || audibleBySolo.contains (track->id);
        plan.audible = ! track->mute && plan.soloAudible;
        plan.instrument = track->kind == model::TrackKind::instrument ? instruments.find (track->id) : nullptr;
        plan.armed = local.armed.contains (track->id) && track->kind != model::TrackKind::bus;
        plan.monitor = monitorFromString (track->monitor);
        if (track->kind == model::TrackKind::audio && track->input.kind == model::InputAssignment::Kind::audio)
            plan.inputFirst = std::max (0, track->input.firstChannel);
        if (track->kind == model::TrackKind::instrument)
        {
            plan.midiChannel = track->input.kind == model::InputAssignment::Kind::midi ? std::clamp (track->input.channel, 0, 16) : 0;
            plan.liveMidi = plan.armed || local.selectedTrack == track->id;
        }
        if (track->kind != model::TrackKind::bus)
            planRegions (plan, model, *track, *snap, mediaPool, projectRate);
        plan.inserts = planInserts (track->strip, racks.effects, plan.latency);
        if (plan.instrument)
            plan.latency += std::max (0, plan.instrument->instrument().latencySamples());
        plan.state = racks.mixer.channel (track->id);
        snap->channels.push_back (std::move (plan));
    }

    // Resolve outputs and sends to channel indexes.
    for (auto& plan : snap->channels)
    {
        if (const auto it = outputOf.find (plan.trackId); it != outputOf.end())
            plan.output = snap->channelIndex (it->second);
        if (const auto it = sendsOf.find (plan.trackId); it != sendsOf.end())
            for (const auto& [busId, send] : it->second)
            {
                const auto target = snap->channelIndex (busId);
                if (target >= 0 && ! send->muted)
                    plan.sends.push_back ({ target, dbToGain (send->levelDb), send->pre, nullptr, 0, send->id });
            }
    }

    // Delay compensation (F-MIX-13). Channels come before the buses they feed, so a bus's input
    // latency (the latest of its feeders) is known when the bus is reached; every edge into a
    // bus or the master is then delayed up to the latest path arriving there.
    std::vector<int> inputLatency (snap->channels.size(), 0);
    for (std::size_t i = 0; i < snap->channels.size(); ++i)
    {
        auto& plan = snap->channels[i];
        plan.latency += inputLatency[i];
        const auto arrive = [&] (int target) {
            if (target >= 0)
                inputLatency[static_cast<std::size_t> (target)] = std::max (inputLatency[static_cast<std::size_t> (target)], plan.latency);
        };
        arrive (plan.output);
        for (const auto& send : plan.sends)
            arrive (send.target);
        if (plan.output < 0)
            snap->masterLatency = std::max (snap->masterLatency, plan.latency);
    }
    for (auto& plan : snap->channels)
    {
        const auto required = [&] (int target) { return target >= 0 ? inputLatency[static_cast<std::size_t> (target)] : snap->masterLatency; };
        plan.outputDelay = std::max (0, required (plan.output) - plan.latency);
        plan.outputDelayLine = plan.state->delayFor ("out", plan.outputDelay);
        const auto* track = model.track (plan.trackId);
        for (auto& send : plan.sends)
        {
            send.delay = std::max (0, required (send.target) - plan.latency);
            send.delayLine = plan.state->delayFor ("send:" + send.sendId, send.delay);
        }
        if (track != nullptr)
            plan.automation = planAutomation (model, *track, *snap, racks.mixer, touchedTargets, plan.sends, plan.inserts, plan.instrument.get());
        plan.automatesInserts = std::any_of (plan.automation.begin(), plan.automation.end(),
                                             [] (const AutomationPlan& a) { return a.kind == AutomationKind::param && a.index >= 0; });
    }
    if (const auto* master = model.track (model.info.masterTrackId))
        snap->masterAutomation = planAutomation (model, *master, *snap, racks.mixer, touchedTargets, {}, snap->masterInserts, nullptr);

    snap->buffers.assign (snap->channels.size() * 2, std::vector<float> (static_cast<std::size_t> (snap->maxBlock)));
    return snap;
}
} // namespace weft::engine
