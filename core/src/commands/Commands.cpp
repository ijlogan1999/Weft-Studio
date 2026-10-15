// Session commands (spec 02 W-3, W-4, W-5, W-11; data rules from spec 03).
// Each command validates against the projection before writing anything, so a
// rejected command leaves the document untouched.

#include "weft/commands/Session.h"
#include "weft/time/Quantize.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace weft::commands
{
using doc::PathSegment;
using model::Region;
using model::TrackKind;
namespace keys = model::keys;

namespace
{
Path trackPath (const Id& id) { return doc::path ({ keys::tracks, id }); }
Path regionPath (const Id& id) { return doc::path ({ keys::regions, id }); }
Path sequencePath (const Id& id) { return doc::path ({ keys::midiSequences, id }); }
Path notePath (const Id& seq, const Id& note) { return doc::path ({ keys::midiSequences, seq, "notes", note }); }
Path insertsPath (const Id& track) { return doc::path ({ keys::tracks, track, "strip", "inserts" }); }
Path sendsPath (const Id& track) { return doc::path ({ keys::tracks, track, "strip", "sends" }); }
Path tempoPointPath (const Id& id) { return doc::path ({ keys::tempoMap, "points", id }); }
Path signaturePointPath (const Id& id) { return doc::path ({ keys::signatureMap, "points", id }); }
Path markerPath (const Id& id) { return doc::path ({ keys::markers, id }); }

EditResult ok() { return {}; }
EditResult wrote (bool success) { return success ? ok() : EditResult::fail ("The document rejected the edit."); }

bool canHoldRegions (const model::Track& t, bool midi)
{
    return midi ? t.kind == TrackKind::instrument : t.kind == TrackKind::audio;
}

std::string defaultTrackName (TrackKind kind, std::size_t number)
{
    switch (kind)
    {
        case TrackKind::audio: return "Audio " + std::to_string (number);
        case TrackKind::instrument: return "Inst " + std::to_string (number);
        case TrackKind::bus: return "Bus " + std::to_string (number);
        case TrackKind::master: return "Master";
    }
    return "Track";
}

/** Fades shrink proportionally so they fit the region (W-3h step 4). */
std::pair<model::Fade, model::Fade> fitFades (model::Fade in, model::Fade out, Samples length)
{
    const auto total = in.length + out.length;
    if (total > length && total > 0)
    {
        const double scale = static_cast<double> (length) / static_cast<double> (total);
        in.length = static_cast<Samples> (std::floor (static_cast<double> (in.length) * scale));
        out.length = static_cast<Samples> (std::floor (static_cast<double> (out.length) * scale));
    }
    return { in, out };
}

Value fadeValue (const model::Fade& f) { return Value::map().set ("length", f.length).set ("curve", f.curve); }
} // namespace

// Project ---------------------------------------------------------------------

EditResult Session::renameProject (const std::string& name)
{
    if (name.empty())
        return reject ("A project needs a name.");
    return run ("Rename Project", [&] (TxnWriter& w) { return wrote (w.put (doc::path ({ "name" }), Value (name))); });
}

EditResult Session::setCycle (bool enabled, Ticks start, Ticks end)
{
    if (start < 0 || end <= start)
        return reject ("The cycle must end after it starts.");
    return run ("Set Cycle", [&] (TxnWriter& w) { return wrote (w.put (doc::path ({ "cycle" }), model::TimeRange { enabled, start, end }.toValue())); });
}

EditResult Session::setPunch (bool enabled, Ticks start, Ticks end)
{
    if (start < 0 || end <= start)
        return reject ("The punch range must end after it starts.");
    return run ("Set Punch", [&] (TxnWriter& w) { return wrote (w.put (doc::path ({ "punch" }), model::TimeRange { enabled, start, end }.toValue())); });
}

EditResult Session::setEndMarker (std::optional<Ticks> end)
{
    if (end && *end <= 0)
        return reject ("The project end must be after the start.");
    return run ("Set Project End", [&] (TxnWriter& w) { return wrote (w.put (doc::path ({ "endMarker" }), end ? Value (*end) : Value())); });
}

// Tracks ------------------------------------------------------------------------

std::optional<Id> Session::createTrack (TrackKind kind, const std::string& name, int channels, std::optional<std::size_t> orderIndex)
{
    if (kind == TrackKind::master)
    {
        error = "A project has exactly one master track.";
        return std::nullopt;
    }
    model::Track t;
    t.id = newId();
    t.kind = kind;
    std::size_t sameKind = 1;
    for (const auto& [id, other] : projection.tracks)
        if (other->kind == kind)
            ++sameKind;
    t.name = name.empty() ? defaultTrackName (kind, sameKind) : name;
    t.color.palette = static_cast<int> (projection.tracks.size() % 24);
    t.channels = kind == TrackKind::audio ? std::clamp (channels, 1, 2) : 2;
    if (kind == TrackKind::audio)
        t.input.kind = model::InputAssignment::Kind::audio;
    else if (kind == TrackKind::instrument)
        t.input.kind = model::InputAssignment::Kind::midi;
    const auto index = orderIndex.value_or (projection.storedTrackOrder.size());
    return runCreate ("New Track", t.id, [&] (TxnWriter& w) {
        return wrote (w.put (trackPath (t.id), t.toValue()) && w.listInsert (doc::path ({ keys::trackOrder }), index, Value (t.id), t.id));
    });
}

EditResult Session::deleteTrack (const Id& trackId)
{
    const auto* t = projection.track (trackId);
    if (t == nullptr || ! projection.tracks.contains (trackId))
        return reject ("That track no longer exists.");
    if (t->kind == TrackKind::master)
        return reject ("The master track can't be deleted.");

    return run ("Delete Track", [&] (TxnWriter& w) {
        bool good = true;
        std::set<Id> removedRegions;
        for (const auto& [id, r] : projection.regions)
            if (r->trackId == trackId)
            {
                good = good && w.remove (regionPath (id));
                removedRegions.insert (id);
            }
        for (const auto& [id, r] : projection.regions)
            if (removedRegions.contains (id) && r->isMidi)
            {
                const bool shared = std::any_of (projection.regions.begin(), projection.regions.end(), [&] (const auto& kv) {
                    return ! removedRegions.contains (kv.first) && kv.second->isMidi && kv.second->midi.sequenceId == r->midi.sequenceId;
                });
                if (! shared)
                    good = good && w.remove (sequencePath (r->midi.sequenceId));
            }
        for (const auto& [id, f] : projection.takeFolders)
            if (f->trackId == trackId)
                good = good && w.remove (doc::path ({ keys::takeFolders, id }));
        for (const auto& [id, lane] : projection.storedLanes)
            if (lane->trackId == trackId)
                good = good && w.remove (doc::path ({ keys::automationLanes, id }));
        // Anything routed to a deleted bus falls back to master / loses the send.
        for (const auto& [id, other] : projection.tracks)
        {
            if (id == trackId)
                continue;
            if (other->output.busId == trackId)
                good = good && w.put (trackPath (id) / "output", model::OutputAssignment {}.toValue());
            for (const auto& s : other->strip.sends)
                if (s.targetBusId == trackId)
                    good = good && w.listRemove (sendsPath (id), s.id);
        }
        const auto occurrences = std::count (projection.storedTrackOrder.begin(), projection.storedTrackOrder.end(), trackId);
        for (std::ptrdiff_t i = 0; i < occurrences; ++i)
            good = good && w.listRemove (doc::path ({ keys::trackOrder }), trackId);
        good = good && w.remove (trackPath (trackId));
        return wrote (good);
    });
}

EditResult Session::renameTrack (const Id& trackId, const std::string& name)
{
    if (! projection.tracks.contains (trackId))
        return reject ("That track no longer exists.");
    if (name.empty())
        return reject ("A track needs a name.");
    return run ("Rename Track", [&] (TxnWriter& w) { return wrote (w.put (trackPath (trackId) / "name", Value (name))); });
}

EditResult Session::moveTrack (const Id& trackId, std::size_t newIndex)
{
    if (! projection.tracks.contains (trackId) || projection.tracks.at (trackId)->kind == TrackKind::master)
        return reject ("That track can't be moved.");
    // A move is remove + insert (D-DM-18); the projection dedupes concurrent moves.
    return run ("Move Track", [&] (TxnWriter& w) {
        const auto list = doc::path ({ keys::trackOrder });
        return wrote (w.listRemove (list, trackId) && w.listInsert (list, newIndex, Value (trackId), trackId));
    });
}

EditResult Session::setTrackVolume (const Id& trackId, double volumeDb)
{
    if (! projection.tracks.contains (trackId))
        return reject ("That track no longer exists.");
    return run ("Volume", [&] (TxnWriter& w) {
        return wrote (w.put (trackPath (trackId) / "strip" / "volumeDb", Value (std::clamp (volumeDb, minusInfinityDb, 6.0))));
    });
}

EditResult Session::setTrackPan (const Id& trackId, double pan)
{
    if (! projection.tracks.contains (trackId))
        return reject ("That track no longer exists.");
    return run ("Pan", [&] (TxnWriter& w) { return wrote (w.put (trackPath (trackId) / "strip" / "pan", Value (std::clamp (pan, -1.0, 1.0)))); });
}

EditResult Session::setTrackMute (const Id& trackId, bool mute)
{
    if (! projection.tracks.contains (trackId))
        return reject ("That track no longer exists.");
    return run (mute ? "Mute Track" : "Unmute Track", [&] (TxnWriter& w) { return wrote (w.put (trackPath (trackId) / "mute", Value (mute))); });
}

EditResult Session::setTrackMonitor (const Id& trackId, const std::string& mode)
{
    const auto* t = projection.track (trackId);
    if (t == nullptr || ! projection.tracks.contains (trackId))
        return reject ("That track no longer exists.");
    if (mode != "auto" && mode != "on" && mode != "off")
        return reject ("Monitoring is auto, on or off.");
    return run ("Input Monitoring", [&] (TxnWriter& w) { return wrote (w.put (trackPath (trackId) / "monitor", Value (mode))); });
}

EditResult Session::setTrackInput (const Id& trackId, const model::InputAssignment& input)
{
    const auto* t = projection.track (trackId);
    if (t == nullptr || ! projection.tracks.contains (trackId))
        return reject ("That track no longer exists.");
    const bool fits = (t->kind == TrackKind::audio && input.kind != model::InputAssignment::Kind::midi)
                   || (t->kind == TrackKind::instrument && input.kind != model::InputAssignment::Kind::audio)
                   || input.kind == model::InputAssignment::Kind::none;
    if (! fits || input.firstChannel < 0 || input.channel < 0 || input.channel > 16)
        return reject ("That input doesn't fit this track.");
    return run ("Set Input", [&] (TxnWriter& w) { return wrote (w.put (trackPath (trackId) / "input", input.toValue())); });
}

EditResult Session::setTrackOutput (const Id& trackId, const Id& busId)
{
    const auto* t = projection.track (trackId);
    if (t == nullptr || ! projection.tracks.contains (trackId))
        return reject ("That track no longer exists.");
    if (t->kind == TrackKind::master)
        return reject ("The master track always outputs to the audio device.");
    if (! busId.empty())
    {
        const auto* bus = projection.track (busId);
        if (bus == nullptr || bus->kind != TrackKind::bus)
            return reject ("Outputs can only go to the master or a bus.");
        if (wouldCycle (trackId, busId, model::RoutingEdge { trackId, {}, {} }.key()))
            return reject ("That output would create a feedback loop.");
    }
    model::OutputAssignment out;
    out.busId = busId;
    return run ("Set Output", [&] (TxnWriter& w) { return wrote (w.put (trackPath (trackId) / "output", out.toValue())); });
}

EditResult Session::materializeRecoveredTracks()
{
    if (projection.syntheticTracks.empty())
        return ok();
    // A repair, not a user edit: committed but not put on the undo stack.
    Txn txn { "Recover Orphaned Regions", {} };
    TxnWriter w (doc, txn);
    bool good = true;
    for (const auto& [id, t] : projection.syntheticTracks)
    {
        model::Track real = *t;
        real.locked.reset();
        good = good && w.put (trackPath (id), real.toValue()) && w.listInsert (doc::path ({ keys::trackOrder }), projection.storedTrackOrder.size(), Value (id), id);
    }
    for (const auto& [regionId, trackId] : projection.recoveredTrackFor)
        good = good && w.put (regionPath (regionId) / "trackId", Value (trackId));
    if (! good)
    {
        doc.rollback();
        reproject();
        return reject ("Couldn't create the Recovered track.");
    }
    finish (std::move (txn), false, nullptr);
    return ok();
}

// Regions -----------------------------------------------------------------------

std::optional<Id> Session::createAudioRegion (const Id& trackId, Ticks start, const std::string& mediaHash, Samples sourceOffset, Samples length)
{
    const auto* t = projection.track (trackId);
    const auto* media = projection.mediaRef (mediaHash);
    if (t == nullptr || ! canHoldRegions (*t, false))
        error = "Audio regions go on audio tracks.";
    else if (media == nullptr)
        error = "That audio file isn't in the project.";
    else if (start < 0 || sourceOffset < 0 || length < model::minAudioRegionLength || sourceOffset + length > media->lengthSamples)
        error = "The region doesn't fit the audio file.";
    else
    {
        Region r;
        r.id = newId();
        r.trackId = trackId;
        r.start = start;
        r.length = length;
        const auto& n = media->originalName;
        r.name = n.substr (0, n.find_last_of ('.'));
        for (const auto* other : projection.regionsOnTrack (trackId))
            r.z = std::max (r.z, other->z + 1);
        r.audio.media = mediaHash;
        r.audio.sourceOffset = sourceOffset;
        r.createdBy = r.modifiedBy = userId;
        r.modifiedAt = clock();
        return runCreate ("New Region", r.id, [&] (TxnWriter& w) { return wrote (w.put (regionPath (r.id), r.toValue())); });
    }
    return std::nullopt;
}

std::optional<Id> Session::createMidiRegion (const Id& trackId, Ticks start, Ticks length)
{
    const auto* t = projection.track (trackId);
    if (t == nullptr || ! canHoldRegions (*t, true))
    {
        error = "MIDI regions go on instrument tracks.";
        return std::nullopt;
    }
    if (start < 0 || length < model::minMidiRegionLength)
    {
        error = "A region needs a position and a length.";
        return std::nullopt;
    }
    Region r;
    r.id = newId();
    r.isMidi = true;
    r.trackId = trackId;
    r.start = start;
    r.length = length;
    r.name = t->name;
    for (const auto* other : projection.regionsOnTrack (trackId))
        r.z = std::max (r.z, other->z + 1);
    r.midi.sequenceId = newId();
    r.createdBy = r.modifiedBy = userId;
    r.modifiedAt = clock();
    model::MidiSequence seq;
    seq.id = r.midi.sequenceId;
    return runCreate ("New Region", r.id, [&] (TxnWriter& w) {
        return wrote (w.put (sequencePath (seq.id), seq.toValue()) && w.put (regionPath (r.id), r.toValue()));
    });
}

EditResult Session::moveRegion (const Id& regionId, Ticks newStart, std::optional<Id> newTrackId)
{
    const auto* r = projection.region (regionId);
    if (r == nullptr)
        return reject ("That region no longer exists.");
    if (newStart < 0)
        return reject ("Regions can't start before the project start.");
    const auto target = newTrackId.value_or (r->trackId);
    if (newTrackId)
    {
        const auto* t = projection.track (target);
        if (t == nullptr || ! projection.tracks.contains (target) || ! canHoldRegions (*t, r->isMidi))
            return reject (r->isMidi ? "MIDI regions go on instrument tracks." : "Audio regions go on audio tracks.");
    }
    return run ("Move Region", [&] (TxnWriter& w) {
        bool good = w.put (regionPath (regionId) / "start", Value (newStart));
        if (target != r->trackId)
            good = good && w.put (regionPath (regionId) / "trackId", Value (target));
        stamp (w, regionId);
        return wrote (good);
    });
}

EditResult Session::trimRegionStart (const Id& regionId, Ticks newStart)
{
    const auto* r = projection.region (regionId);
    if (r == nullptr)
        return reject ("That region no longer exists.");
    if (newStart < 0)
        return reject ("Regions can't start before the project start.");
    const auto base = regionPath (regionId);
    if (r->isMidi)
    {
        const auto end = r->start + r->length;
        if (newStart > end - model::minMidiRegionLength)
            return reject ("The region would be too short.");
        const auto delta = newStart - r->start;
        return run ("Trim Region", [&] (TxnWriter& w) {
            bool good = w.put (base / "start", Value (newStart)) && w.put (base / "length", Value (end - newStart))
                     && w.put (base / "midi" / "sequenceOffset", Value (r->midi.sequenceOffset + delta));
            stamp (w, regionId);
            return wrote (good);
        });
    }
    auto delta = static_cast<Samples> (
        std::llround (projection.tempo.tickToSample (static_cast<double> (newStart)) - projection.tempo.tickToSample (static_cast<double> (r->start))));
    // Dragging past the start of the file stops there (W-3d step 3).
    if (r->audio.sourceOffset + delta < 0)
    {
        delta = -r->audio.sourceOffset;
        newStart = static_cast<Ticks> (std::llround (
            projection.tempo.sampleToTick (projection.tempo.tickToSample (static_cast<double> (r->start)) + static_cast<double> (delta))));
        if (newStart < 0)
            return reject ("Regions can't start before the project start.");
    }
    const auto newOffset = r->audio.sourceOffset + delta;
    const auto newLength = r->length - delta;
    if (newLength < model::minAudioRegionLength)
        return reject ("The region would be too short.");
    const auto [fadeIn, fadeOut] = fitFades (r->audio.fadeIn, r->audio.fadeOut, newLength);
    return run ("Trim Region", [&] (TxnWriter& w) {
        bool good = w.put (base / "start", Value (newStart)) && w.put (base / "length", Value (newLength))
                 && w.put (base / "audio" / "sourceOffset", Value (newOffset));
        if (fadeIn.length != r->audio.fadeIn.length || fadeOut.length != r->audio.fadeOut.length)
            good = good && w.put (base / "audio" / "fadeIn", fadeValue (fadeIn)) && w.put (base / "audio" / "fadeOut", fadeValue (fadeOut));
        stamp (w, regionId);
        return wrote (good);
    });
}

EditResult Session::trimRegionLength (const Id& regionId, std::int64_t newLength)
{
    const auto* r = projection.region (regionId);
    if (r == nullptr)
        return reject ("That region no longer exists.");
    const auto base = regionPath (regionId);
    if (r->isMidi)
    {
        if (newLength < model::minMidiRegionLength)
            return reject ("The region would be too short.");
        return run ("Trim Region", [&] (TxnWriter& w) {
            bool good = w.put (base / "length", Value (newLength));
            stamp (w, regionId);
            return wrote (good);
        });
    }
    if (newLength < model::minAudioRegionLength)
        return reject ("The region would be too short.");
    // Extending past the end of the file stops at its end (W-3d step 3, AT-ARR-8).
    if (const auto* m = projection.mediaRef (r->audio.media); m != nullptr && r->audio.sourceOffset + newLength > m->lengthSamples)
        newLength = std::max<std::int64_t> (m->lengthSamples - r->audio.sourceOffset, model::minAudioRegionLength);
    const auto [fadeIn, fadeOut] = fitFades (r->audio.fadeIn, r->audio.fadeOut, newLength);
    return run ("Trim Region", [&] (TxnWriter& w) {
        bool good = w.put (base / "length", Value (newLength));
        if (fadeIn.length != r->audio.fadeIn.length || fadeOut.length != r->audio.fadeOut.length)
            good = good && w.put (base / "audio" / "fadeIn", fadeValue (fadeIn)) && w.put (base / "audio" / "fadeOut", fadeValue (fadeOut));
        stamp (w, regionId);
        return wrote (good);
    });
}

std::optional<Id> Session::splitRegion (const Id& regionId, Ticks at)
{
    const auto* r = projection.region (regionId);
    if (r == nullptr)
    {
        error = "That region no longer exists.";
        return std::nullopt;
    }
    if (static_cast<double> (at) <= static_cast<double> (r->start) || static_cast<double> (at) >= projection.regionEndTick (*r))
    {
        error = "Split inside the region.";
        return std::nullopt;
    }
    Region right = *r;
    right.id = newId();
    right.start = at;
    right.createdBy = right.modifiedBy = userId;
    right.modifiedAt = clock();
    const auto base = regionPath (regionId);

    if (r->isMidi)
    {
        // D-DM-3a / D-ARR-4: notes starting in the right-hand window move to a new sequence
        // (rebased so the new region's sequenceOffset is 0); notes crossing the split stay whole
        // in the left region and are cut at its end during playback.
        const auto leftLength = at - r->start;
        const auto windowStart = r->midi.sequenceOffset + leftLength;
        const auto windowEnd = r->midi.sequenceOffset + r->length;
        right.length = r->length - leftLength;
        right.midi.sequenceId = newId();
        right.midi.sequenceOffset = 0;
        model::MidiSequence seq;
        seq.id = right.midi.sequenceId;
        std::vector<Id> moved;
        if (const auto* src = projection.sequence (r->midi.sequenceId))
        {
            for (const auto& [noteId, n] : src->notes)
                if (n.pos >= windowStart && n.pos < windowEnd)
                {
                    auto copy = n;
                    copy.pos = n.pos - windowStart;
                    seq.notes.emplace (noteId, copy);
                    moved.push_back (noteId);
                }
            for (const auto& [ccId, cc] : src->controllers)
                if (cc.pos >= windowStart && cc.pos < windowEnd)
                {
                    auto copy = cc;
                    copy.pos = cc.pos - windowStart;
                    seq.controllers.emplace (ccId, copy);
                }
        }
        return runCreate ("Split Region", right.id, [&] (TxnWriter& w) {
            bool good = w.put (sequencePath (seq.id), seq.toValue()) && w.put (regionPath (right.id), right.toValue())
                     && w.put (base / "length", Value (leftLength));
            for (const auto& noteId : moved)
                good = good && w.remove (notePath (r->midi.sequenceId, noteId));
            for (const auto& [ccId, cc] : seq.controllers)
                good = good && w.remove (doc::path ({ keys::midiSequences, r->midi.sequenceId, "controllers", ccId }));
            stamp (w, regionId);
            return wrote (good);
        });
    }

    const auto leftLength = static_cast<Samples> (
        std::llround (projection.tempo.tickToSample (static_cast<double> (at)) - projection.tempo.tickToSample (static_cast<double> (r->start))));
    if (leftLength < model::minAudioRegionLength || r->length - leftLength < model::minAudioRegionLength)
    {
        error = "Split too close to the region's edge.";
        return std::nullopt;
    }
    right.length = r->length - leftLength;
    right.audio.sourceOffset = r->audio.sourceOffset + leftLength;
    right.audio.fadeIn.length = 0;
    right.audio.fadeOut.length = std::min (r->audio.fadeOut.length, right.length);
    const auto leftFadeIn = std::min (r->audio.fadeIn.length, leftLength);
    return runCreate ("Split Region", right.id, [&] (TxnWriter& w) {
        bool good = w.put (regionPath (right.id), right.toValue()) && w.put (base / "length", Value (leftLength))
                 && w.put (base / "audio" / "fadeOut", fadeValue ({ 0, r->audio.fadeOut.curve }));
        if (leftFadeIn != r->audio.fadeIn.length)
            good = good && w.put (base / "audio" / "fadeIn", fadeValue ({ leftFadeIn, r->audio.fadeIn.curve }));
        stamp (w, regionId);
        return wrote (good);
    });
}

EditResult Session::joinRegions (const std::vector<Id>& regionIds)
{
    std::vector<const Region*> rs;
    for (const auto& id : regionIds)
    {
        const auto* r = projection.region (id);
        if (r == nullptr)
            return reject ("A selected region no longer exists.");
        if (std::find (rs.begin(), rs.end(), r) == rs.end())
            rs.push_back (r);
    }
    if (rs.size() < 2)
        return reject ("Select two or more regions to join.");
    const auto track = projection.effectiveTrackId (*rs[0]);
    for (const auto* r : rs)
        if (projection.effectiveTrackId (*r) != track || r->isMidi != rs[0]->isMidi)
            return reject ("Only regions of one kind on one track can be joined.");
    std::sort (rs.begin(), rs.end(), [] (const Region* a, const Region* b) { return std::tie (a->start, a->id) < std::tie (b->start, b->id); });
    const auto* first = rs.front();
    const auto base = regionPath (first->id);

    if (first->isMidi)
    {
        Ticks end = first->start + first->length;
        for (const auto* r : rs)
            end = std::max (end, r->start + r->length);
        struct Copy
        {
            Id id;
            model::Note note;
        };
        std::vector<Copy> copies;
        std::set<Id> doomedSequences;
        for (std::size_t i = 1; i < rs.size(); ++i)
        {
            const auto* r = rs[i];
            if (const auto* seq = projection.sequence (r->midi.sequenceId))
                for (const auto* n : seq->sortedNotes())
                    if (n->pos >= r->midi.sequenceOffset && n->pos < r->midi.sequenceOffset + r->length)
                    {
                        auto note = *n;
                        note.pos = r->start + (n->pos - r->midi.sequenceOffset) - first->start + first->midi.sequenceOffset;
                        if (note.pos < 0)
                            continue;
                        note.id = newId();
                        copies.push_back ({ note.id, note });
                    }
            doomedSequences.insert (r->midi.sequenceId);
        }
        for (const auto& [id, other] : projection.regions)
            if (other->isMidi && std::find (rs.begin() + 1, rs.end(), other.get()) == rs.end())
                doomedSequences.erase (other->midi.sequenceId);
        doomedSequences.erase (first->midi.sequenceId);
        return run ("Join Regions", [&] (TxnWriter& w) {
            bool good = w.put (base / "length", Value (end - first->start));
            for (const auto& c : copies)
                good = good && w.put (notePath (first->midi.sequenceId, c.id), c.note.toValue());
            for (std::size_t i = 1; i < rs.size(); ++i)
                good = good && w.remove (regionPath (rs[i]->id));
            for (const auto& s : doomedSequences)
                good = good && w.remove (sequencePath (s));
            stamp (w, first->id);
            return wrote (good);
        });
    }

    Samples total = first->length;
    for (std::size_t i = 1; i < rs.size(); ++i)
    {
        const auto* a = rs[i - 1];
        const auto* b = rs[i];
        const auto aEnd = std::llround (projection.tempo.tickToSample (static_cast<double> (a->start))) + a->length;
        const auto bStart = std::llround (projection.tempo.tickToSample (static_cast<double> (b->start)));
        if (b->audio.media != first->audio.media || b->audio.sourceOffset != a->audio.sourceOffset + a->length || std::llabs (bStart - aEnd) > 1)
            return reject ("Only touching regions from the same audio file can be joined.");
        total += b->length;
    }
    return run ("Join Regions", [&] (TxnWriter& w) {
        bool good = w.put (base / "length", Value (total)) && w.put (base / "audio" / "fadeOut", fadeValue (rs.back()->audio.fadeOut));
        for (std::size_t i = 1; i < rs.size(); ++i)
            good = good && w.remove (regionPath (rs[i]->id));
        stamp (w, first->id);
        return wrote (good);
    });
}

EditResult Session::deleteRegions (const std::vector<Id>& regionIds)
{
    std::set<Id> doomed;
    for (const auto& id : regionIds)
        if (projection.region (id) != nullptr)
            doomed.insert (id);
    if (doomed.empty())
        return reject ("Nothing to delete.");
    std::set<Id> sequences;
    for (const auto& id : doomed)
        if (const auto* r = projection.region (id); r->isMidi)
            sequences.insert (r->midi.sequenceId);
    for (const auto& [id, r] : projection.regions)
        if (! doomed.contains (id) && r->isMidi)
            sequences.erase (r->midi.sequenceId);
    return run (doomed.size() == 1 ? "Delete Region" : "Delete Regions", [&] (TxnWriter& w) {
        bool good = true;
        for (const auto& id : doomed)
            good = good && w.remove (regionPath (id));
        for (const auto& s : sequences)
            good = good && w.remove (sequencePath (s));
        return wrote (good);
    });
}

EditResult Session::setRegionFades (const Id& regionId, std::optional<model::Fade> fadeIn, std::optional<model::Fade> fadeOut)
{
    const auto* r = projection.region (regionId);
    if (r == nullptr)
        return reject ("That region no longer exists.");
    if (r->isMidi)
        return reject ("Fades apply to audio regions.");
    auto in = fadeIn.value_or (r->audio.fadeIn);
    auto out = fadeOut.value_or (r->audio.fadeOut);
    in.length = std::max<Samples> (0, in.length);
    out.length = std::max<Samples> (0, out.length);
    // The edited fade gives way first; the other keeps its length if it can.
    if (in.length + out.length > r->length)
    {
        if (fadeIn && ! fadeOut)
            in.length = r->length - out.length;
        else if (fadeOut && ! fadeIn)
            out.length = r->length - in.length;
        std::tie (in, out) = fitFades (in, out, r->length);
    }
    const auto base = regionPath (regionId) / "audio";
    return run ("Fade", [&] (TxnWriter& w) {
        bool good = true;
        if (fadeIn)
            good = good && w.put (base / "fadeIn", fadeValue (in));
        if (fadeOut)
            good = good && w.put (base / "fadeOut", fadeValue (out));
        stamp (w, regionId);
        return wrote (good);
    });
}

EditResult Session::setRegionGain (const Id& regionId, double gainDb)
{
    const auto* r = projection.region (regionId);
    if (r == nullptr)
        return reject ("That region no longer exists.");
    if (r->isMidi)
        return reject ("Region gain applies to audio regions.");
    return run ("Region Gain", [&] (TxnWriter& w) {
        bool good = w.put (regionPath (regionId) / "audio" / "gainDb", Value (std::clamp (gainDb, minusInfinityDb, 24.0)));
        stamp (w, regionId);
        return wrote (good);
    });
}

EditResult Session::setRegionMuted (const Id& regionId, bool muted)
{
    if (projection.region (regionId) == nullptr)
        return reject ("That region no longer exists.");
    return run (muted ? "Mute Region" : "Unmute Region", [&] (TxnWriter& w) {
        bool good = w.put (regionPath (regionId) / "muted", Value (muted));
        stamp (w, regionId);
        return wrote (good);
    });
}

EditResult Session::setRegionName (const Id& regionId, const std::string& name)
{
    if (projection.region (regionId) == nullptr)
        return reject ("That region no longer exists.");
    return run ("Rename Region", [&] (TxnWriter& w) {
        bool good = w.put (regionPath (regionId) / "name", Value (name));
        stamp (w, regionId);
        return wrote (good);
    });
}

EditResult Session::setRegionQuantize (const Id& regionId, std::optional<model::RegionQuantize> q)
{
    const auto* r = projection.region (regionId);
    if (r == nullptr)
        return reject ("That region no longer exists.");
    if (! r->isMidi)
        return reject ("Quantize applies to MIDI regions.");
    if (q && (q->grid <= 0 || q->strength < 0 || q->strength > 1 || q->swing < 0 || q->swing > 1))
        return reject ("Quantize settings are out of range.");
    Value v;
    if (q)
        v = Value::map().set ("grid", q->grid).set ("strength", q->strength).set ("swing", q->swing);
    return run ("Region Quantize", [&] (TxnWriter& w) {
        bool good = w.put (regionPath (regionId) / "midi" / "quantize", v);
        stamp (w, regionId);
        return wrote (good);
    });
}

EditResult Session::addMedia (const model::MediaRef& media)
{
    if (media.hash.empty())
        return reject ("Media needs a content hash.");
    return run ("Add Audio File", [&] (TxnWriter& w) { return wrote (w.put (doc::path ({ keys::media, media.hash }), media.toValue())); });
}

// Notes --------------------------------------------------------------------------

namespace
{
std::optional<std::string> invalidNote (const model::Note& n)
{
    if (n.pos < 0 || n.length < 1)
        return "Notes need a position and a length.";
    if (n.pitch < 0 || n.pitch > 127 || n.velocity < 1 || n.velocity > 127 || n.releaseVelocity < 0 || n.releaseVelocity > 127 || n.channel < 1
        || n.channel > 16)
        return "Note values are out of range.";
    return std::nullopt;
}
} // namespace

std::optional<Id> Session::addNote (const Id& regionId, model::Note note)
{
    const auto* r = projection.region (regionId);
    if (r == nullptr || ! r->isMidi || projection.sequence (r->midi.sequenceId) == nullptr)
    {
        error = "Notes go in MIDI regions.";
        return std::nullopt;
    }
    if (auto bad = invalidNote (note))
    {
        error = *bad;
        return std::nullopt;
    }
    if (note.id.empty())
        note.id = newId();
    return runCreate ("Add Note", note.id, [&] (TxnWriter& w) {
        bool good = w.put (notePath (r->midi.sequenceId, note.id), note.toValue());
        stamp (w, regionId);
        return wrote (good);
    });
}

EditResult Session::updateNote (const Id& regionId, const model::Note& note)
{
    const auto* r = projection.region (regionId);
    const auto* seq = r != nullptr && r->isMidi ? projection.sequence (r->midi.sequenceId) : nullptr;
    if (seq == nullptr || ! seq->notes.contains (note.id))
        return reject ("That note no longer exists.");
    if (auto bad = invalidNote (note))
        return reject (*bad);
    const auto& old = seq->notes.at (note.id);
    // Field-level writes, so concurrent edits of different fields both win (D-DM-7).
    return run ("Edit Note", [&] (TxnWriter& w) {
        const auto base = notePath (r->midi.sequenceId, note.id);
        const auto now = note.toValue();
        const auto before = old.toValue();
        bool good = true;
        for (const auto& [k, v] : now.asMap())
            if (before[k] != v)
                good = good && w.put (base / k, v);
        stamp (w, regionId);
        return wrote (good);
    });
}

EditResult Session::deleteNotes (const Id& regionId, const std::vector<Id>& noteIds)
{
    const auto* r = projection.region (regionId);
    const auto* seq = r != nullptr && r->isMidi ? projection.sequence (r->midi.sequenceId) : nullptr;
    if (seq == nullptr)
        return reject ("That region no longer exists.");
    return run (noteIds.size() == 1 ? "Delete Note" : "Delete Notes", [&] (TxnWriter& w) {
        bool good = true;
        for (const auto& id : noteIds)
            if (seq->notes.contains (id))
                good = good && w.remove (notePath (r->midi.sequenceId, id));
        stamp (w, regionId);
        return wrote (good);
    });
}

EditResult Session::quantizeNotes (const Id& regionId, const std::vector<Id>& noteIds, Ticks grid, double strength, double swingPercent)
{
    const auto* r = projection.region (regionId);
    const auto* seq = r != nullptr && r->isMidi ? projection.sequence (r->midi.sequenceId) : nullptr;
    if (seq == nullptr)
        return reject ("Quantize applies to MIDI regions.");
    if (grid <= 0 || strength < 0 || strength > 1 || swingPercent < 0 || swingPercent > 100)
        return reject ("Quantize settings are out of range.");
    std::vector<std::pair<Id, Ticks>> moves;
    for (const auto& [id, n] : seq->notes)
    {
        const bool selected = noteIds.empty() ? (n.pos >= r->midi.sequenceOffset && n.pos < r->midi.sequenceOffset + r->length)
                                              : std::find (noteIds.begin(), noteIds.end(), id) != noteIds.end();
        if (! selected)
            continue;
        // Quantize on the timeline grid, then map back to sequence ticks.
        const auto timeline = r->start + (n.pos - r->midi.sequenceOffset);
        const auto newPos = quantizeTick (timeline, grid, strength, swingPercent) - r->start + r->midi.sequenceOffset;
        if (newPos != n.pos && newPos >= 0)
            moves.emplace_back (id, newPos);
    }
    return run ("Quantize", [&] (TxnWriter& w) {
        bool good = true;
        for (const auto& [id, pos] : moves)
            good = good && w.put (notePath (r->midi.sequenceId, id) / "pos", Value (pos));
        if (! moves.empty())
            stamp (w, regionId);
        return wrote (good);
    });
}

// Plugins and sends ---------------------------------------------------------------

std::optional<Id> Session::addInsert (const Id& trackId, model::PluginInstance plugin, std::optional<std::size_t> index)
{
    const auto* t = projection.track (trackId);
    if (t == nullptr || ! projection.tracks.contains (trackId))
        error = "That track no longer exists.";
    else if (t->strip.inserts.size() >= static_cast<std::size_t> (model::maxInserts))
        error = "A channel strip holds up to " + std::to_string (model::maxInserts) + " plug-ins.";
    else if (plugin.pluginUid.empty())
        error = "Choose a plug-in.";
    else
    {
        if (plugin.id.empty())
            plugin.id = newId();
        const auto at = index.value_or (t->strip.inserts.size());
        return runCreate ("Add Plug-in", plugin.id, [&] (TxnWriter& w) { return wrote (w.listInsert (insertsPath (trackId), at, plugin.toValue(), plugin.id)); });
    }
    return std::nullopt;
}

EditResult Session::removeInsert (const Id& trackId, const Id& pluginId)
{
    const auto* t = projection.track (trackId);
    if (t == nullptr || std::none_of (t->strip.inserts.begin(), t->strip.inserts.end(), [&] (const auto& p) { return p.id == pluginId; }))
        return reject ("That plug-in no longer exists.");
    return run ("Remove Plug-in", [&] (TxnWriter& w) { return wrote (w.listRemove (insertsPath (trackId), pluginId)); });
}

EditResult Session::setInsertBypass (const Id& trackId, const Id& pluginId, bool bypass)
{
    const auto* t = projection.track (trackId);
    if (t == nullptr || std::none_of (t->strip.inserts.begin(), t->strip.inserts.end(), [&] (const auto& p) { return p.id == pluginId; }))
        return reject ("That plug-in no longer exists.");
    return run (bypass ? "Bypass Plug-in" : "Enable Plug-in", [&] (TxnWriter& w) {
        return wrote (w.put (insertsPath (trackId) / PathSegment::id (pluginId) / "bypass", Value (bypass)));
    });
}

EditResult Session::setPluginParam (const Id& trackId, const Id& pluginId, const std::string& paramId, double normalized)
{
    const auto* t = projection.track (trackId);
    if (t == nullptr)
        return reject ("That track no longer exists.");
    Path base;
    if (t->strip.instrument && t->strip.instrument->id == pluginId)
        base = trackPath (trackId) / "strip" / "instrument";
    else if (std::any_of (t->strip.inserts.begin(), t->strip.inserts.end(), [&] (const auto& p) { return p.id == pluginId; }))
        base = insertsPath (trackId) / PathSegment::id (pluginId);
    else
        return reject ("That plug-in no longer exists.");
    return run ("Plug-in Parameter", [&] (TxnWriter& w) { return wrote (w.put (base / "params" / paramId, Value (std::clamp (normalized, 0.0, 1.0)))); });
}

std::optional<Id> Session::addSend (const Id& trackId, const Id& busId, double levelDb, bool pre)
{
    const auto* t = projection.track (trackId);
    const auto* bus = projection.track (busId);
    if (t == nullptr || ! projection.tracks.contains (trackId) || t->kind == TrackKind::master)
        error = "That track can't have sends.";
    else if (bus == nullptr || bus->kind != TrackKind::bus)
        error = "Sends go to a bus.";
    else if (t->strip.sends.size() >= static_cast<std::size_t> (model::maxSends))
        error = "A channel strip holds up to " + std::to_string (model::maxSends) + " sends.";
    else if (wouldCycle (trackId, busId, {}))
        error = "That send would create a feedback loop.";
    else
    {
        model::Send s;
        s.id = newId();
        s.targetBusId = busId;
        s.levelDb = std::clamp (levelDb, minusInfinityDb, 6.0);
        s.pre = pre;
        return runCreate ("Add Send", s.id, [&] (TxnWriter& w) { return wrote (w.listInsert (sendsPath (trackId), t->strip.sends.size(), s.toValue(), s.id)); });
    }
    return std::nullopt;
}

EditResult Session::setSendLevel (const Id& trackId, const Id& sendId, double levelDb)
{
    const auto* t = projection.track (trackId);
    if (t == nullptr || std::none_of (t->strip.sends.begin(), t->strip.sends.end(), [&] (const auto& s) { return s.id == sendId; }))
        return reject ("That send no longer exists.");
    return run ("Send Level", [&] (TxnWriter& w) {
        return wrote (w.put (sendsPath (trackId) / PathSegment::id (sendId) / "levelDb", Value (std::clamp (levelDb, minusInfinityDb, 6.0))));
    });
}

EditResult Session::setSendTarget (const Id& trackId, const Id& sendId, const Id& busId)
{
    const auto* t = projection.track (trackId);
    if (t == nullptr || std::none_of (t->strip.sends.begin(), t->strip.sends.end(), [&] (const auto& s) { return s.id == sendId; }))
        return reject ("That send no longer exists.");
    const auto* bus = projection.track (busId);
    if (bus == nullptr || bus->kind != TrackKind::bus)
        return reject ("Sends go to a bus.");
    if (wouldCycle (trackId, busId, model::RoutingEdge { trackId, {}, sendId }.key()))
        return reject ("That send would create a feedback loop.");
    return run ("Send Destination", [&] (TxnWriter& w) {
        return wrote (w.put (sendsPath (trackId) / PathSegment::id (sendId) / "targetBusId", Value (busId)));
    });
}

EditResult Session::removeSend (const Id& trackId, const Id& sendId)
{
    const auto* t = projection.track (trackId);
    if (t == nullptr || std::none_of (t->strip.sends.begin(), t->strip.sends.end(), [&] (const auto& s) { return s.id == sendId; }))
        return reject ("That send no longer exists.");
    return run ("Remove Send", [&] (TxnWriter& w) { return wrote (w.listRemove (sendsPath (trackId), sendId)); });
}

// Automation ------------------------------------------------------------------------

std::optional<Id> Session::addAutomationPoint (const Id& trackId, const model::AutomationTarget& target, Ticks pos, double value, double curve)
{
    if (projection.track (trackId) == nullptr)
    {
        error = "That track no longer exists.";
        return std::nullopt;
    }
    if (pos < 0)
    {
        error = "Automation can't start before the project start.";
        return std::nullopt;
    }
    const auto pointId = newId();
    const auto pointValue = Value::map().set ("pos", pos).set ("value", std::clamp (value, 0.0, 1.0)).set ("curve", std::clamp (curve, -1.0, 1.0));
    const auto* lane = projection.lane (trackId, target);
    if (lane != nullptr)
        return runCreate ("Add Automation Point", pointId, [&] (TxnWriter& w) {
            return wrote (w.put (doc::path ({ keys::automationLanes, lane->id, "points", pointId }), pointValue));
        });
    model::AutomationLane newLane;
    newLane.id = newId();
    newLane.trackId = trackId;
    newLane.target = target;
    auto laneValue = newLane.toValue();
    laneValue.mapRef()["points"] = Value::map().set (pointId, pointValue);
    return runCreate ("Add Automation Point", pointId, [&] (TxnWriter& w) { return wrote (w.put (doc::path ({ keys::automationLanes, newLane.id }), laneValue)); });
}

namespace
{
/** The stored lane that holds a point of a canonical lane (points may live in a merged duplicate). */
const model::AutomationLane* storedLaneFor (const model::ProjectModel& m, const Id& laneId, const Id& pointId)
{
    auto it = m.automationLanes.find (laneId);
    if (it == m.automationLanes.end())
        return nullptr;
    std::vector<Id> candidates { laneId };
    candidates.insert (candidates.end(), it->second->mergedLaneIds.begin(), it->second->mergedLaneIds.end());
    for (const auto& id : candidates)
        if (auto s = m.storedLanes.find (id); s != m.storedLanes.end() && s->second->points.contains (pointId))
            return s->second.get();
    return nullptr;
}
} // namespace

EditResult Session::moveAutomationPoint (const Id& laneId, const Id& pointId, Ticks pos, double value)
{
    const auto* lane = storedLaneFor (projection, laneId, pointId);
    if (lane == nullptr)
        return reject ("That automation point no longer exists.");
    if (pos < 0)
        return reject ("Automation can't start before the project start.");
    const auto base = doc::path ({ keys::automationLanes, lane->id, "points", pointId });
    return run ("Move Automation Point", [&] (TxnWriter& w) {
        return wrote (w.put (base / "pos", Value (pos)) && w.put (base / "value", Value (std::clamp (value, 0.0, 1.0))));
    });
}

EditResult Session::deleteAutomationPoint (const Id& laneId, const Id& pointId)
{
    const auto* lane = storedLaneFor (projection, laneId, pointId);
    if (lane == nullptr)
        return reject ("That automation point no longer exists.");
    return run ("Delete Automation Point",
                [&] (TxnWriter& w) { return wrote (w.remove (doc::path ({ keys::automationLanes, lane->id, "points", pointId }))); });
}

// Tempo, signature, markers -----------------------------------------------------------

std::optional<Id> Session::addTempoPoint (Ticks pos, double bpm)
{
    if (pos <= 0)
    {
        error = "Change the first tempo instead of adding one at the start.";
        return std::nullopt;
    }
    if (bpm < TempoMap::minBpm || bpm > TempoMap::maxBpm)
    {
        error = "Tempo must be between 20 and 999 BPM.";
        return std::nullopt;
    }
    for (const auto& p : projection.storedTempoPoints)
        if (p.pos == pos)
        {
            // One tempo per position: adding where one exists changes it.
            if (setTempoPoint (p.id, pos, bpm))
                return p.id;
            return std::nullopt;
        }
    const auto id = newId();
    return runCreate ("Add Tempo Change", id, [&] (TxnWriter& w) {
        return wrote (w.put (tempoPointPath (id), Value::map().set ("pos", pos).set ("bpm", bpm).set ("curve", "step")));
    });
}

EditResult Session::setTempoPoint (const Id& pointId, Ticks pos, double bpm)
{
    const auto it = std::find_if (projection.storedTempoPoints.begin(), projection.storedTempoPoints.end(), [&] (const auto& p) { return p.id == pointId; });
    if (it == projection.storedTempoPoints.end())
        return reject ("That tempo change no longer exists.");
    if (bpm < TempoMap::minBpm || bpm > TempoMap::maxBpm)
        return reject ("Tempo must be between 20 and 999 BPM.");
    if (pointId == "initial" ? pos != 0 : pos <= 0)
        return reject ("The first tempo stays at the project start.");
    for (const auto& p : projection.storedTempoPoints)
        if (p.id != pointId && p.pos == pos)
            return reject ("There is already a tempo change there.");
    return run ("Tempo Change", [&] (TxnWriter& w) {
        bool good = true;
        if (pos != it->pos)
            good = w.put (tempoPointPath (pointId) / "pos", Value (pos));
        return wrote (good && w.put (tempoPointPath (pointId) / "bpm", Value (bpm)));
    });
}

EditResult Session::deleteTempoPoint (const Id& pointId)
{
    if (pointId == "initial")
        return reject ("The first tempo can't be deleted.");
    if (std::none_of (projection.storedTempoPoints.begin(), projection.storedTempoPoints.end(), [&] (const auto& p) { return p.id == pointId; }))
        return reject ("That tempo change no longer exists.");
    return run ("Delete Tempo Change", [&] (TxnWriter& w) { return wrote (w.remove (tempoPointPath (pointId))); });
}

std::optional<Id> Session::addSignaturePoint (int bar, int numerator, int denominator)
{
    if (bar <= 0)
    {
        error = "Change the first time signature instead of adding one at the start.";
        return std::nullopt;
    }
    if (! SignatureMap::isValid (numerator, denominator))
    {
        error = "That time signature isn't supported.";
        return std::nullopt;
    }
    for (const auto& p : projection.storedSignaturePoints)
        if (p.bar == bar)
        {
            if (setSignaturePoint (p.id, bar, numerator, denominator))
                return p.id;
            return std::nullopt;
        }
    // D-DM-10: regions keep their tick positions; nothing else is written.
    const auto id = newId();
    return runCreate ("Add Signature Change", id, [&] (TxnWriter& w) {
        return wrote (w.put (signaturePointPath (id), Value::map().set ("bar", bar).set ("numerator", numerator).set ("denominator", denominator)));
    });
}

EditResult Session::setSignaturePoint (const Id& pointId, int bar, int numerator, int denominator)
{
    const auto it = std::find_if (projection.storedSignaturePoints.begin(), projection.storedSignaturePoints.end(),
                                  [&] (const auto& p) { return p.id == pointId; });
    if (it == projection.storedSignaturePoints.end())
        return reject ("That signature change no longer exists.");
    if (! SignatureMap::isValid (numerator, denominator))
        return reject ("That time signature isn't supported.");
    if (pointId == "initial" ? bar != 0 : bar <= 0)
        return reject ("The first time signature stays at the project start.");
    for (const auto& p : projection.storedSignaturePoints)
        if (p.id != pointId && p.bar == bar)
            return reject ("There is already a signature change there.");
    return run ("Signature Change", [&] (TxnWriter& w) {
        const auto base = signaturePointPath (pointId);
        bool good = true;
        if (bar != it->bar)
            good = w.put (base / "bar", Value (bar));
        return wrote (good && w.put (base / "numerator", Value (numerator)) && w.put (base / "denominator", Value (denominator)));
    });
}

EditResult Session::deleteSignaturePoint (const Id& pointId)
{
    if (pointId == "initial")
        return reject ("The first time signature can't be deleted.");
    if (std::none_of (projection.storedSignaturePoints.begin(), projection.storedSignaturePoints.end(), [&] (const auto& p) { return p.id == pointId; }))
        return reject ("That signature change no longer exists.");
    return run ("Delete Signature Change", [&] (TxnWriter& w) { return wrote (w.remove (signaturePointPath (pointId))); });
}

std::optional<Id> Session::addMarker (Ticks pos, const std::string& name)
{
    if (pos < 0)
    {
        error = "Markers can't be placed before the project start.";
        return std::nullopt;
    }
    model::Marker m;
    m.id = newId();
    m.pos = pos;
    m.name = name.empty() ? "Marker " + std::to_string (projection.markers.size() + 1) : name;
    m.createdBy = userId;
    return runCreate ("Add Marker", m.id, [&] (TxnWriter& w) { return wrote (w.put (markerPath (m.id), m.toValue())); });
}

EditResult Session::moveMarker (const Id& markerId, Ticks pos)
{
    if (! projection.markers.contains (markerId))
        return reject ("That marker no longer exists.");
    if (pos < 0)
        return reject ("Markers can't be placed before the project start.");
    return run ("Move Marker", [&] (TxnWriter& w) { return wrote (w.put (markerPath (markerId) / "pos", Value (pos))); });
}

EditResult Session::renameMarker (const Id& markerId, const std::string& name)
{
    if (! projection.markers.contains (markerId))
        return reject ("That marker no longer exists.");
    return run ("Rename Marker", [&] (TxnWriter& w) { return wrote (w.put (markerPath (markerId) / "name", Value (name))); });
}

EditResult Session::deleteMarker (const Id& markerId)
{
    if (! projection.markers.contains (markerId))
        return reject ("That marker no longer exists.");
    return run ("Delete Marker", [&] (TxnWriter& w) { return wrote (w.remove (markerPath (markerId))); });
}

// Transport displays (F-TRN-9, 10, 20) ------------------------------------------------

EditResult Session::setTempoAt (Ticks at, double bpm)
{
    // The tempo display edits the tempo in effect at the playhead, clamped to range (AT-TRN-13).
    bpm = std::round (std::clamp (bpm, TempoMap::minBpm, TempoMap::maxBpm) * 100.0) / 100.0;
    const auto& points = projection.tempo.points();
    const TempoPoint* inEffect = &points.front();
    for (const auto& p : points)
        if (p.pos <= at)
            inEffect = &p;
    return setTempoPoint (inEffect->id, inEffect->pos, bpm);
}

EditResult Session::setSignatureAt (int bar, int numerator, int denominator)
{
    const auto& p = projection.signatures.signatureAtBar (std::max (0, bar));
    return setSignaturePoint (p.id, p.bar, numerator, denominator);
}

EditResult Session::setCycleToRegions (const std::vector<Id>& regionIds)
{
    double first = 0, last = 0;
    bool any = false;
    for (const auto& id : regionIds)
        if (const auto* r = projection.region (id))
        {
            const auto start = static_cast<double> (r->start), end = projection.regionEndTick (*r);
            first = any ? std::min (first, start) : start;
            last = any ? std::max (last, end) : end;
            any = true;
        }
    if (! any)
        return EditResult::fail ({}); // nothing selected: U does nothing (AT-TRN-19)

    // Round outward to whole bars.
    const auto& sig = projection.signatures;
    const auto start = sig.barStart (sig.barAt (static_cast<Ticks> (std::floor (first))).bar);
    const auto endBar = sig.barAt (static_cast<Ticks> (std::ceil (last)));
    const auto end = sig.barStart (endBar.offset == 0 ? endBar.bar : endBar.bar + 1);
    return setCycle (true, std::max<Ticks> (0, start), end);
}

Ticks projectEndTick (const ProjectModel& model)
{
    if (model.info.endMarker)
        return *model.info.endMarker;
    double end = 0;
    for (const auto& [id, r] : model.regions)
        end = std::max (end, model.regionEndTick (*r));
    return static_cast<Ticks> (std::ceil (end));
}
} // namespace weft::commands
