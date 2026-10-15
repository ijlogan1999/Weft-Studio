#include "weft/engine/graph/EngineSnapshot.h"

#include "weft/time/Quantize.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <numbers>

namespace weft::engine
{
namespace
{
using Interval = std::pair<std::int64_t, std::int64_t>;

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
    }
    std::sort (plan.segments.begin(), plan.segments.end(), [] (const AudioSegment& a, const AudioSegment& b) { return a.start < b.start; });
    std::stable_sort (plan.notes.begin(), plan.notes.end(), [] (const ScheduledNote& a, const ScheduledNote& b) { return a.on < b.on; });
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

std::unique_ptr<EngineSnapshot> compileSnapshot (const model::ProjectModel& model, double deviceRate, int maxBlock, const MediaPool& mediaPool,
                                                 const InstrumentRack& instruments, const LocalMixState& local)
{
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
    if (const auto* master = model.track (model.info.masterTrackId))
        snap->masterGain = master->mute ? 0.0f : dbToGain (master->strip.volumeDb);

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
        plan.audible = ! track->mute && (local.soloed.empty() || audibleBySolo.contains (track->id));
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
                    plan.sends.push_back ({ target, dbToGain (send->levelDb), send->pre });
            }
    }

    snap->buffers.assign (snap->channels.size() * 2, std::vector<float> (static_cast<std::size_t> (snap->maxBlock)));
    snap->meters = std::make_unique<ChannelMeters[]> (snap->channels.size());
    return snap;
}
} // namespace weft::engine
