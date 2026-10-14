#include "weft/engine/graph/EngineSnapshot.h"

#include "weft/time/Quantize.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace weft::engine
{
namespace
{
using Interval = std::pair<std::int64_t, std::int64_t>;

/** Parts of [a, b) not in covered (sorted, disjoint); then adds [a, b) to covered. */
std::vector<Interval> takeVisible (std::vector<Interval>& covered, std::int64_t a, std::int64_t b)
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

    covered.emplace_back (a, b);
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

std::unique_ptr<EngineSnapshot> compileSnapshot (const model::ProjectModel& model, double deviceRate, const MediaPool& mediaPool,
                                                 const InstrumentRack& instruments)
{
    auto snap = std::make_unique<EngineSnapshot>();
    snap->sampleRate = deviceRate;
    snap->tempo = TempoMap (model.tempo.points(), deviceRate);
    snap->signatures = model.signatures;
    const auto& cycle = model.info.cycle;
    snap->cycleEnabled = cycle.enabled && cycle.end > cycle.start;
    snap->cycleStart = snap->tickToSample (cycle.start);
    snap->cycleEnd = snap->tickToSample (cycle.end);
    if (const auto* master = model.track (model.info.masterTrackId))
        snap->masterGain = master->mute ? 0.0f : dbToGain (master->strip.volumeDb);

    const double projectRate = model.info.sampleRate > 0 ? model.info.sampleRate : 48000.0;
    const double projectToDevice = deviceRate / projectRate;

    for (const auto& trackId : model.trackOrder)
    {
        const auto* track = model.track (trackId);
        if (track == nullptr || (track->kind != model::TrackKind::audio && track->kind != model::TrackKind::instrument))
            continue;

        TrackPlan plan;
        plan.trackId = trackId;
        plan.muted = track->mute;
        plan.gain = dbToGain (track->strip.volumeDb);
        plan.instrument = instruments.find (trackId);

        // Expand regions into timeline instances. Muted regions neither play nor hide others.
        std::vector<Instance> instances;
        for (const auto* r : model.regionsOnTrack (trackId))
        {
            if (r->muted || r->length <= 0 || r->isMidi != (track->kind == model::TrackKind::instrument))
                continue;
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

        std::vector<Interval> covered;
        for (const auto& inst : instances)
        {
            const auto& r = *inst.region;
            const auto visible = takeVisible (covered, inst.start, inst.end);
            if (visible.empty())
                continue;

            if (! r.isMidi)
            {
                auto audio = mediaPool.find (r.audio.media);
                if (! audio || audio->numFrames() == 0)
                    continue;
                if (std::find (snap->media.begin(), snap->media.end(), audio) == snap->media.end())
                    snap->media.push_back (audio);

                const auto length = inst.end - inst.start;
                auto fadeIn = std::clamp<std::int64_t> (std::llround (static_cast<double> (r.audio.fadeIn.length) * projectToDevice), 0, length);
                auto fadeOut = std::clamp<std::int64_t> (std::llround (static_cast<double> (r.audio.fadeOut.length) * projectToDevice), 0, length - fadeIn);
                for (const auto& [a, b] : visible)
                {
                    AudioSegment seg;
                    seg.start = a;
                    seg.end = b;
                    seg.regionStart = inst.start;
                    seg.regionEnd = inst.end;
                    seg.media = audio.get();
                    seg.sourceAtRegionStart = static_cast<double> (r.audio.sourceOffset) * audio->sampleRate / projectRate;
                    seg.sourceStep = audio->sampleRate / deviceRate;
                    seg.gain = dbToGain (r.audio.gainDb);
                    seg.fadeIn = fadeIn;
                    seg.fadeOut = fadeOut;
                    seg.fadeInCurve = curveFromString (r.audio.fadeIn.curve);
                    seg.fadeOutCurve = curveFromString (r.audio.fadeOut.curve);
                    plan.segments.push_back (seg);
                }
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
        snap->tracks.push_back (std::move (plan));
    }
    return snap;
}
} // namespace weft::engine
