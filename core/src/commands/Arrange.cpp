// Arrange-view commands on several regions: move, copy, paste, repeat, nudge, split
// at the playhead, loop, mute and crossfade (spec 02 W-3, F-ARR-4/5/7/11/13/14/23/29).

#include "weft/commands/Session.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace weft::commands
{
using model::Region;
using model::TrackKind;
namespace keys = model::keys;

namespace
{
Path trackPath (const Id& id) { return doc::path ({ keys::tracks, id }); }
Path regionPath (const Id& id) { return doc::path ({ keys::regions, id }); }
Path sequencePath (const Id& id) { return doc::path ({ keys::midiSequences, id }); }
Path folderPath (const Id& id) { return doc::path ({ keys::takeFolders, id }); }
EditResult wrote (bool success) { return success ? EditResult {} : EditResult::fail ("The document rejected the edit."); }

bool canHold (const model::Track& t, bool midi) { return midi ? t.kind == TrackKind::instrument : t.kind == TrackKind::audio; }

std::string kindName (TrackKind kind) { return kind == TrackKind::instrument ? "Inst" : "Audio"; }

Value fadeValue (const model::Fade& f) { return Value::map().set ("length", f.length).set ("curve", f.curve); }

Ticks regionEnd (const model::ProjectModel& m, const Region& r)
{
    return static_cast<Ticks> (std::llround (m.regionEndTick (r)));
}
} // namespace

// Clipboard and plan --------------------------------------------------------------

Ticks RegionClipboard::earliestStart() const
{
    Ticks t = regions.empty() ? 0 : regions.front().start;
    for (const auto& r : regions)
        t = std::min (t, r.start);
    return t;
}

const model::Region* RegionClipboard::anchor() const
{
    const Region* best = nullptr;
    for (const auto& r : regions)
        if (best == nullptr || std::tie (r.start, r.id) < std::tie (best->start, best->id))
            best = &r;
    return best;
}

RegionClipboard RegionClipboard::of (const model::ProjectModel& m, const std::vector<Id>& regionIds)
{
    std::set<Id> ids;
    for (const auto& id : regionIds)
        if (const auto* r = m.region (id))
        {
            ids.insert (id);
            // D-ARR-6: a take brings its whole folder.
            if (const auto f = m.takeFolders.find (r->takeFolderId); f != m.takeFolders.end())
                for (const auto& take : f->second->takeIds)
                    if (m.region (take) != nullptr)
                        ids.insert (take);
        }
    RegionClipboard clip;
    for (const auto& id : ids)
    {
        auto r = *m.region (id);
        r.trackId = m.effectiveTrackId (r);
        if (r.isMidi)
            if (const auto* seq = m.sequence (r.midi.sequenceId))
                clip.sequences.emplace (seq->id, *seq);
        if (const auto f = m.takeFolders.find (r.takeFolderId); f != m.takeFolders.end())
            clip.takeFolders.emplace (f->first, *f->second);
        else
            r.takeFolderId.clear();
        clip.regions.push_back (std::move (r));
    }
    return clip;
}

int trackRow (const model::ProjectModel& m, const Id& trackId)
{
    const auto it = std::find (m.trackOrder.begin(), m.trackOrder.end(), trackId);
    return it == m.trackOrder.end() ? -1 : static_cast<int> (it - m.trackOrder.begin());
}

MovePlan planMove (const model::ProjectModel& m, const RegionClipboard& clip, Ticks deltaTicks, int deltaTracks, int fallbackRow)
{
    MovePlan plan;
    if (clip.empty())
        return plan;
    plan.deltaTicks = std::max (deltaTicks, -clip.earliestStart());
    const auto rows = static_cast<int> (m.trackOrder.size());
    std::map<std::pair<int, TrackKind>, int> newTrackIndex;

    // Take folders move as one: every take follows the row of the folder's first take.
    std::map<Id, int> folderRow;
    for (const auto& r : clip.regions)
    {
        auto row = trackRow (m, r.trackId);
        if (row < 0)
            row = fallbackRow;
        if (! r.takeFolderId.empty())
            row = folderRow.emplace (r.takeFolderId, row).first->second;

        Placement p;
        p.regionId = r.id;
        p.start = r.start + plan.deltaTicks;
        p.trackId = r.trackId;
        const auto target = row < 0 ? -1 : row + deltaTracks;
        if (row < 0 || target < 0)
        {
            // Stays where it is (or, when its track is gone, can't be placed off the list).
        }
        else if (target < rows)
        {
            const auto* t = m.track (m.trackOrder[static_cast<std::size_t> (target)]);
            if (t != nullptr && canHold (*t, r.isMidi))
                p.trackId = t->id;
        }
        else
        {
            const auto kind = r.isMidi ? TrackKind::instrument : TrackKind::audio;
            const auto key = std::make_pair (target - rows, kind);
            auto it = newTrackIndex.find (key);
            if (it == newTrackIndex.end())
            {
                it = newTrackIndex.emplace (key, static_cast<int> (plan.newTracks.size())).first;
                plan.newTracks.push_back ({ kind, r.isMidi ? 2 : 1, target - rows });
            }
            auto& spec = plan.newTracks[static_cast<std::size_t> (it->second)];
            if (! r.isMidi)
                if (const auto* media = m.mediaRef (r.audio.media))
                    spec.channels = std::max (spec.channels, std::clamp (media->channels, 1, 2));
            p.trackId.clear();
            p.newTrack = it->second;
        }
        // A region whose track is gone and that didn't land anywhere can't be placed.
        if (p.newTrack < 0 && m.track (p.trackId) == nullptr)
            continue;
        plan.placements.push_back (p);
    }
    // New tracks are created top to bottom.
    std::vector<int> order (plan.newTracks.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = static_cast<int> (i);
    std::stable_sort (order.begin(), order.end(), [&] (int a, int b) {
        const auto& x = plan.newTracks[static_cast<std::size_t> (a)];
        const auto& y = plan.newTracks[static_cast<std::size_t> (b)];
        return std::tie (x.below, x.kind) < std::tie (y.below, y.kind);
    });
    std::vector<int> remap (order.size());
    std::vector<NewTrackSpec> sorted;
    for (std::size_t i = 0; i < order.size(); ++i)
    {
        remap[static_cast<std::size_t> (order[i])] = static_cast<int> (i);
        sorted.push_back (plan.newTracks[static_cast<std::size_t> (order[i])]);
    }
    plan.newTracks = std::move (sorted);
    for (auto& p : plan.placements)
        if (p.newTrack >= 0)
            p.newTrack = remap[static_cast<std::size_t> (p.newTrack)];
    return plan;
}

// Writing a plan --------------------------------------------------------------------

bool Session::writePlacement (TxnWriter& w, const RegionClipboard& clip, const MovePlan& plan, bool copy, std::vector<Id>& newTrackIds,
                              std::map<Id, std::int64_t>& nextZ, std::vector<Id>* created)
{
    bool good = true;
    // New tracks go below the last one, in plan order.
    std::map<TrackKind, std::size_t> sameKind;
    for (const auto& [id, t] : projection.tracks)
        ++sameKind[t->kind];
    if (newTrackIds.size() < plan.newTracks.size())
    {
        const auto first = newTrackIds.size();
        for (std::size_t i = first; i < plan.newTracks.size(); ++i)
        {
            const auto& spec = plan.newTracks[i];
            model::Track t;
            t.id = newId();
            t.kind = spec.kind;
            t.name = kindName (spec.kind) + " " + std::to_string (++sameKind[spec.kind]);
            t.color.palette = static_cast<int> ((projection.tracks.size() + i) % 24);
            t.channels = spec.kind == TrackKind::audio ? spec.channels : 2;
            t.input.kind = spec.kind == TrackKind::audio ? model::InputAssignment::Kind::audio : model::InputAssignment::Kind::midi;
            good = good && w.put (trackPath (t.id), t.toValue())
                   && w.listInsert (doc::path ({ keys::trackOrder }), projection.storedTrackOrder.size() + i, Value (t.id), t.id);
            newTrackIds.push_back (t.id);
        }
    }

    std::map<Id, const Region*> byId;
    for (const auto& r : clip.regions)
        byId[r.id] = &r;
    auto topZ = [&] (const Id& trackId) -> std::int64_t& {
        auto it = nextZ.find (trackId);
        if (it == nextZ.end())
        {
            std::int64_t z = 0;
            for (const auto* other : projection.regionsOnTrack (trackId))
                z = std::max (z, other->z + 1);
            it = nextZ.emplace (trackId, z).first;
        }
        return it->second;
    };

    // Placements in stacking order, so moved regions keep their relative z.
    std::vector<const Placement*> ordered;
    for (const auto& p : plan.placements)
        ordered.push_back (&p);
    std::stable_sort (ordered.begin(), ordered.end(), [&] (const Placement* a, const Placement* b) { return byId[a->regionId]->z < byId[b->regionId]->z; });

    std::map<Id, Id> newRegionIds, newFolderIds, newSequenceIds;
    std::map<Id, Id> folderTrack;
    for (const auto* p : ordered)
    {
        const auto& src = *byId[p->regionId];
        const auto trackId = p->newTrack >= 0 ? newTrackIds[static_cast<std::size_t> (p->newTrack)] : p->trackId;
        const auto z = topZ (trackId)++;
        if (! src.takeFolderId.empty())
            folderTrack[src.takeFolderId] = trackId;
        if (! copy)
        {
            const auto base = regionPath (src.id);
            good = good && w.put (base / "start", Value (p->start)) && w.put (base / "z", Value (z));
            if (trackId != src.trackId || projection.region (src.id)->trackId != trackId)
                good = good && w.put (base / "trackId", Value (trackId));
            if (src.loop)
                good = good && w.put (base / "loop" / "endTicks", Value (src.loop->endTicks + plan.deltaTicks));
            stamp (w, src.id);
            continue;
        }
        Region c = src;
        c.id = newId();
        newRegionIds[src.id] = c.id;
        c.trackId = trackId;
        c.start = p->start;
        c.z = z;
        if (c.loop)
            c.loop->endTicks += plan.deltaTicks;
        c.createdBy = c.modifiedBy = userId;
        c.modifiedAt = clock();
        if (c.isMidi)
        {
            auto& seqId = newSequenceIds[src.midi.sequenceId];
            if (seqId.empty())
            {
                seqId = newId();
                model::MidiSequence s;
                if (const auto it = clip.sequences.find (src.midi.sequenceId); it != clip.sequences.end())
                    s = it->second;
                s.id = seqId;
                good = good && w.put (sequencePath (s.id), s.toValue());
            }
            c.midi.sequenceId = seqId;
        }
        if (! c.takeFolderId.empty())
        {
            auto& f = newFolderIds[c.takeFolderId];
            if (f.empty())
                f = newId();
            c.takeFolderId = f;
        }
        good = good && w.put (regionPath (c.id), c.toValue());
        if (created != nullptr)
            created->push_back (c.id);
    }

    // Take folders follow their takes.
    for (const auto& [oldId, folder] : clip.takeFolders)
    {
        const auto t = folderTrack.find (oldId);
        if (t == folderTrack.end())
            continue;
        model::TakeFolder f = folder;
        f.trackId = t->second;
        f.start += plan.deltaTicks;
        f.endTicks += plan.deltaTicks;
        if (copy)
        {
            f.id = newFolderIds[oldId];
            f.takeIds.clear();
            for (const auto& take : folder.takeIds)
                if (const auto c = newRegionIds.find (take); c != newRegionIds.end())
                    f.takeIds.push_back (c->second);
            const auto active = newRegionIds.find (folder.activeTakeId);
            f.activeTakeId = active != newRegionIds.end() ? active->second : Id {};
            good = good && w.put (folderPath (f.id), f.toValue());
        }
        else
        {
            good = good && w.put (folderPath (oldId) / "trackId", Value (f.trackId)) && w.put (folderPath (oldId) / "start", Value (f.start))
                   && w.put (folderPath (oldId) / "endTicks", Value (f.endTicks));
        }
    }
    return good;
}

// Commands --------------------------------------------------------------------------

EditResult Session::moveRegions (const std::vector<Id>& regionIds, Ticks deltaTicks, int deltaTracks)
{
    const auto clip = RegionClipboard::of (projection, regionIds);
    if (clip.empty())
        return reject ("Select a region first.");
    const auto plan = planMove (projection, clip, deltaTicks, deltaTracks);
    const bool moves = plan.deltaTicks != 0 || std::any_of (plan.placements.begin(), plan.placements.end(), [&] (const Placement& p) {
                           return p.newTrack >= 0 || p.trackId != projection.effectiveTrackId (*projection.region (p.regionId));
                       });
    if (! moves)
        return {};
    return run (clip.regions.size() == 1 ? "Move Region" : "Move Regions", [&] (TxnWriter& w) {
        std::vector<Id> tracks;
        std::map<Id, std::int64_t> z;
        return wrote (writePlacement (w, clip, plan, false, tracks, z, nullptr));
    });
}

EditResult Session::nudgeRegions (const std::vector<Id>& regionIds, Ticks deltaTicks)
{
    const auto clip = RegionClipboard::of (projection, regionIds);
    if (clip.empty())
        return reject ("Select a region first.");
    const auto plan = planMove (projection, clip, deltaTicks, 0);
    if (plan.deltaTicks == 0)
        return {};
    return run ("Nudge", [&] (TxnWriter& w) {
        std::vector<Id> tracks;
        std::map<Id, std::int64_t> z;
        return wrote (writePlacement (w, clip, plan, false, tracks, z, nullptr));
    });
}

std::vector<Id> Session::placeCopies (std::string_view name, const RegionClipboard& clip, const std::vector<std::pair<Ticks, int>>& shifts, int fallbackRow)
{
    std::vector<Id> created;
    if (clip.empty() || shifts.empty())
    {
        error = "Select a region first.";
        return created;
    }
    const auto result = run (name, [&] (TxnWriter& w) {
        std::vector<Id> tracks;
        std::map<Id, std::int64_t> z;
        bool good = true;
        for (const auto& [dt, dtr] : shifts)
            good = good && writePlacement (w, clip, planMove (projection, clip, dt, dtr, fallbackRow), true, tracks, z, &created);
        return wrote (good);
    });
    if (! result)
        created.clear();
    return created;
}

std::vector<Id> Session::copyRegions (const std::vector<Id>& regionIds, Ticks deltaTicks, int deltaTracks)
{
    const auto clip = RegionClipboard::of (projection, regionIds);
    return placeCopies (clip.regions.size() == 1 ? "Copy Region" : "Copy Regions", clip, { { deltaTicks, deltaTracks } }, -1);
}

std::vector<Id> Session::pasteRegions (const RegionClipboard& clip, Ticks at, const Id& targetTrackId)
{
    const auto* anchor = clip.anchor();
    if (anchor == nullptr)
    {
        error = "Nothing to paste.";
        return {};
    }
    auto target = trackRow (projection, targetTrackId);
    if (target < 0)
        target = trackRow (projection, anchor->trackId);
    auto anchorRow = trackRow (projection, anchor->trackId);
    if (anchorRow < 0)
        anchorRow = target;
    if (target < 0)
    {
        error = "Select a track to paste onto.";
        return {};
    }
    return placeCopies ("Paste", clip, { { std::max<Ticks> (at, 0) - anchor->start, target - anchorRow } }, anchorRow);
}

std::vector<Id> Session::repeatRegions (const std::vector<Id>& regionIds, int times)
{
    const auto clip = RegionClipboard::of (projection, regionIds);
    if (clip.empty() || times < 1)
    {
        error = clip.empty() ? "Select a region first." : "Repeat at least once.";
        return {};
    }
    Ticks end = 0;
    for (const auto& r : clip.regions)
        end = std::max (end, regionEnd (projection, r));
    const auto block = end - clip.earliestStart();
    std::vector<std::pair<Ticks, int>> shifts;
    for (int k = 1; k <= times; ++k)
        shifts.emplace_back (block * k, 0);
    return placeCopies ("Repeat", clip, shifts, -1);
}

std::vector<Id> Session::splitRegionsAt (const std::vector<Id>& regionIds, Ticks at)
{
    std::vector<Id> crossing;
    for (const auto& id : regionIds)
        if (const auto* r = projection.region (id); r != nullptr && r->start < at && static_cast<double> (at) < projection.regionEndTick (*r))
            crossing.push_back (id);
    std::vector<Id> pieces;
    if (crossing.empty())
    {
        error = "The playhead doesn't cross a selected region.";
        return pieces;
    }
    const bool own = ! inGesture();
    if (own)
        beginGesture (crossing.size() == 1 ? "Split Region" : "Split Regions");
    for (const auto& id : crossing)
        if (const auto right = splitRegion (id, at))
            pieces.push_back (*right);
    if (own)
    {
        if (pieces.empty())
            cancelGesture();
        else
            endGesture();
    }
    return pieces;
}

EditResult Session::setRegionLoop (const Id& regionId, std::optional<Ticks> loopEnd)
{
    const auto* r = projection.region (regionId);
    if (r == nullptr)
        return reject ("That region no longer exists.");
    if (loopEnd && static_cast<double> (*loopEnd) <= projection.regionEndTick (*r))
        loopEnd.reset();
    return run (loopEnd ? "Loop Region" : "Unloop Region", [&] (TxnWriter& w) {
        bool good = w.put (regionPath (regionId) / "loop", loopEnd ? Value::map().set ("enabled", true).set ("endTicks", *loopEnd) : Value());
        stamp (w, regionId);
        return wrote (good);
    });
}

EditResult Session::toggleRegionsMuted (const std::vector<Id>& regionIds)
{
    std::vector<const Region*> rs;
    for (const auto& id : regionIds)
        if (const auto* r = projection.region (id))
            rs.push_back (r);
    if (rs.empty())
        return reject ("Select a region first.");
    const bool mute = ! std::all_of (rs.begin(), rs.end(), [] (const Region* r) { return r->muted; });
    return run (mute ? "Mute Regions" : "Unmute Regions", [&] (TxnWriter& w) {
        bool good = true;
        for (const auto* r : rs)
            if (r->muted != mute)
            {
                good = good && w.put (regionPath (r->id) / "muted", Value (mute));
                stamp (w, r->id);
            }
        return wrote (good);
    });
}

EditResult Session::crossfadeRegions (const std::vector<Id>& regionIds)
{
    std::vector<const Region*> rs;
    for (const auto& id : regionIds)
        if (const auto* r = projection.region (id); r != nullptr && ! r->isMidi)
            rs.push_back (r);
    std::sort (rs.begin(), rs.end(), [] (const Region* a, const Region* b) { return std::tie (a->start, a->id) < std::tie (b->start, b->id); });
    struct Fades
    {
        model::Fade in, out;
    };
    std::map<Id, Fades> fades;
    for (const auto* r : rs)
        fades[r->id] = { r->audio.fadeIn, r->audio.fadeOut };
    for (std::size_t i = 0; i < rs.size(); ++i)
        for (std::size_t j = i + 1; j < rs.size(); ++j)
        {
            const auto* a = rs[i];
            const auto* b = rs[j];
            if (projection.effectiveTrackId (*a) != projection.effectiveTrackId (*b))
                continue;
            const auto aStart = std::llround (projection.tempo.tickToSample (static_cast<double> (a->start)));
            const auto bStart = std::llround (projection.tempo.tickToSample (static_cast<double> (b->start)));
            const auto aEnd = aStart + a->length;
            const auto overlap = std::min (aEnd, bStart + b->length) - bStart;
            // Only a real overlap where b starts inside a and ends after it.
            if (overlap <= 0 || bStart <= aStart || bStart + b->length <= aEnd)
                continue;
            auto& fa = fades[a->id];
            auto& fb = fades[b->id];
            fa.out = { std::min<Samples> (overlap, a->length - fa.in.length), "equalPower" };
            fb.in = { std::min<Samples> (overlap, b->length - fb.out.length), "equalPower" };
        }
    std::vector<Id> changed;
    for (const auto* r : rs)
    {
        const auto& f = fades.at (r->id);
        if (f.in.length != r->audio.fadeIn.length || f.out.length != r->audio.fadeOut.length || f.in.curve != r->audio.fadeIn.curve
            || f.out.curve != r->audio.fadeOut.curve)
            changed.push_back (r->id);
    }
    if (changed.empty())
        return reject ("Select overlapping audio regions on one track.");
    return run ("Crossfade", [&] (TxnWriter& w) {
        bool good = true;
        for (const auto& id : changed)
        {
            const auto& f = fades.at (id);
            good = good && w.put (regionPath (id) / "audio" / "fadeIn", fadeValue (f.in)) && w.put (regionPath (id) / "audio" / "fadeOut", fadeValue (f.out));
            stamp (w, id);
        }
        return wrote (good);
    });
}
} // namespace weft::commands
