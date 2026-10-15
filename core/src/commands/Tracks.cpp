// Track commands beyond the basics in Commands.cpp: the New Track dialog with a count,
// Duplicate, and colors (spec 02 W-5, F-TRK-6, F-TRK-9).

#include "weft/commands/Session.h"

#include <algorithm>

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
EditResult wrote (bool success) { return success ? EditResult {} : EditResult::fail ("The document rejected the edit."); }

std::string kindName (TrackKind kind)
{
    switch (kind)
    {
        case TrackKind::audio: return "Audio";
        case TrackKind::instrument: return "Inst";
        case TrackKind::bus: return "Bus";
        case TrackKind::master: return "Master";
    }
    return "Track";
}
} // namespace

std::vector<Id> Session::createTracks (TrackKind kind, int count, int channels, const Id& belowTrackId)
{
    std::vector<Id> created;
    if (kind == TrackKind::master || count < 1)
    {
        error = kind == TrackKind::master ? "A project has exactly one master track." : "Create at least one track.";
        return created;
    }
    std::size_t sameKind = 0;
    for (const auto& [id, t] : projection.tracks)
        sameKind += t->kind == kind ? 1 : 0;
    auto index = projection.storedTrackOrder.size();
    if (! belowTrackId.empty())
        if (const auto it = std::find (projection.storedTrackOrder.begin(), projection.storedTrackOrder.end(), belowTrackId);
            it != projection.storedTrackOrder.end())
            index = static_cast<std::size_t> (it - projection.storedTrackOrder.begin()) + 1;

    std::vector<model::Track> tracks;
    for (int i = 0; i < count; ++i)
    {
        model::Track t;
        t.id = newId();
        t.kind = kind;
        t.name = kindName (kind) + " " + std::to_string (sameKind + 1 + static_cast<std::size_t> (i));
        t.color.palette = static_cast<int> ((projection.tracks.size() + static_cast<std::size_t> (i)) % 24);
        t.channels = kind == TrackKind::audio ? std::clamp (channels, 1, 2) : 2;
        if (kind == TrackKind::audio)
            t.input.kind = model::InputAssignment::Kind::audio;
        else if (kind == TrackKind::instrument)
            t.input.kind = model::InputAssignment::Kind::midi;
        tracks.push_back (std::move (t));
    }
    const auto result = run (count == 1 ? "New Track" : "New Tracks", [&] (TxnWriter& w) {
        bool good = true;
        for (std::size_t i = 0; i < tracks.size(); ++i)
            good = good && w.put (trackPath (tracks[i].id), tracks[i].toValue())
                   && w.listInsert (doc::path ({ keys::trackOrder }), index + i, Value (tracks[i].id), tracks[i].id);
        return wrote (good);
    });
    if (result)
        for (const auto& t : tracks)
            created.push_back (t.id);
    return created;
}

std::optional<Id> Session::duplicateTrack (const Id& trackId, bool includeRegions)
{
    const auto* original = projection.track (trackId);
    if (original == nullptr || ! projection.tracks.contains (trackId) || original->kind == TrackKind::master)
    {
        error = "That track can't be duplicated.";
        return std::nullopt;
    }
    model::Track copy = *original;
    copy.id = newId();
    copy.name = original->name + " copy";
    copy.locked.reset();
    if (copy.strip.instrument)
        copy.strip.instrument->id = newId();
    for (auto& p : copy.strip.inserts)
        p.id = newId();
    for (auto& s : copy.strip.sends)
        s.id = newId();
    auto index = projection.storedTrackOrder.size();
    if (const auto it = std::find (projection.storedTrackOrder.begin(), projection.storedTrackOrder.end(), trackId); it != projection.storedTrackOrder.end())
        index = static_cast<std::size_t> (it - projection.storedTrackOrder.begin()) + 1;

    return runCreate ("Duplicate Track", copy.id, [&] (TxnWriter& w) {
        bool good = w.put (trackPath (copy.id), copy.toValue()) && w.listInsert (doc::path ({ keys::trackOrder }), index, Value (copy.id), copy.id);
        if (! includeRegions)
            return wrote (good);
        // Regions get new ids; MIDI regions get their own copy of the sequence so edits stay separate.
        std::map<Id, Id> folders, copies;
        for (const auto* r : projection.regionsOnTrack (trackId))
        {
            Region c = *r;
            c.id = newId();
            copies[r->id] = c.id;
            c.trackId = copy.id;
            c.createdBy = c.modifiedBy = userId;
            c.modifiedAt = clock();
            if (c.isMidi)
                if (const auto* seq = projection.sequence (r->midi.sequenceId))
                {
                    auto s = *seq;
                    s.id = newId();
                    c.midi.sequenceId = s.id;
                    good = good && w.put (sequencePath (s.id), s.toValue());
                }
            if (! c.takeFolderId.empty())
            {
                auto& f = folders[c.takeFolderId];
                if (f.empty())
                    f = newId();
                c.takeFolderId = f;
            }
            good = good && w.put (regionPath (c.id), c.toValue());
        }
        for (const auto& [oldId, newFolder] : folders)
        {
            const auto it = projection.takeFolders.find (oldId);
            if (it == projection.takeFolders.end())
                continue;
            model::TakeFolder f = *it->second;
            f.id = newFolder;
            f.trackId = copy.id;
            f.takeIds.clear();
            for (const auto& take : it->second->takeIds)
                if (const auto c = copies.find (take); c != copies.end())
                    f.takeIds.push_back (c->second);
            const auto active = copies.find (it->second->activeTakeId);
            f.activeTakeId = active != copies.end() ? active->second : Id {};
            good = good && w.put (doc::path ({ keys::takeFolders, f.id }), f.toValue());
        }
        return wrote (good);
    });
}

EditResult Session::setTrackColor (const Id& trackId, const model::Color& color)
{
    if (! projection.tracks.contains (trackId))
        return reject ("That track no longer exists.");
    if (color.rgb.empty() && (color.palette < 0 || color.palette > 23))
        return reject ("Pick one of the palette colors.");
    return run ("Track Color", [&] (TxnWriter& w) { return wrote (w.put (trackPath (trackId) / "color", color.toValue())); });
}

EditResult Session::setRegionColor (const Id& regionId, std::optional<model::Color> color)
{
    if (projection.region (regionId) == nullptr)
        return reject ("That region no longer exists.");
    return run ("Region Color", [&] (TxnWriter& w) {
        bool good = w.put (regionPath (regionId) / "color", color ? color->toValue() : Value());
        stamp (w, regionId);
        return wrote (good);
    });
}
} // namespace weft::commands
