// Piano roll commands (spec 01 F-PRL-2..15, spec 02 W-5): notes across the regions
// a piano roll shows, controller points, and quantize.

#include "weft/commands/Session.h"
#include "weft/time/Quantize.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>

namespace weft::commands
{
using model::Note;
using model::Region;
namespace keys = model::keys;

namespace
{
Path regionPath (const Id& id) { return doc::path ({ keys::regions, id }); }
Path notePath (const Id& seq, const Id& note) { return doc::path ({ keys::midiSequences, seq, "notes", note }); }
Path ccPath (const Id& seq, const Id& cc) { return doc::path ({ keys::midiSequences, seq, "controllers", cc }); }
EditResult wrote (bool success) { return success ? EditResult {} : EditResult::fail ("The document rejected the edit."); }

const model::MidiSequence* sequenceOf (const model::ProjectModel& m, const Region* r)
{
    return r != nullptr && r->isMidi ? m.sequence (r->midi.sequenceId) : nullptr;
}

/** The region length (ticks) that holds a note ending at timeline tick `end`: rounded up to a bar. */
Ticks lengthToHold (const model::ProjectModel& m, const Region& r, Ticks end)
{
    if (end <= r.start + r.length)
        return r.length;
    const auto at = m.signatures.barAt (end);
    const auto barEnd = at.offset == 0 ? m.signatures.barStart (at.bar) : m.signatures.barStart (at.bar + 1);
    return barEnd - r.start;
}

std::map<Id, std::vector<Id>> byRegion (const std::vector<NoteRef>& notes)
{
    std::map<Id, std::vector<Id>> out;
    for (const auto& n : notes)
        out[n.regionId].push_back (n.noteId);
    return out;
}
} // namespace

// Helpers -------------------------------------------------------------------------

NoteClipboard NoteClipboard::of (const model::ProjectModel& m, const std::vector<NoteRef>& refs)
{
    NoteClipboard clip;
    for (const auto& ref : refs)
        if (const auto* n = findNote (m, ref))
        {
            auto copy = *n;
            copy.pos = noteTimelineTick (*m.region (ref.regionId), n->pos);
            clip.notes.push_back (copy);
        }
    return clip;
}

const model::Note* findNote (const model::ProjectModel& m, const NoteRef& ref)
{
    const auto* seq = sequenceOf (m, m.region (ref.regionId));
    if (seq == nullptr)
        return nullptr;
    const auto it = seq->notes.find (ref.noteId);
    return it == seq->notes.end() ? nullptr : &it->second;
}

const model::Region* midiRegionAt (const model::ProjectModel& m, const Id& trackId, Ticks at)
{
    const Region* best = nullptr;
    for (const auto* r : m.regionsOnTrack (trackId))
        if (r->isMidi && r->start <= at && at < r->start + r->length && (best == nullptr || r->z > best->z))
            best = r;
    return best;
}

std::string noteName (int pitch)
{
    static const std::array<const char*, 12> names { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const auto k = ((pitch % 12) + 12) % 12;
    const auto octave = (pitch - k) / 12 - 2; // 60 -> C3 (D-W4-1)
    return std::string (names[static_cast<std::size_t> (k)]) + std::to_string (octave);
}

NoteSpan resizeNote (NoteSpan note, bool rightEdge, Ticks pointer, Ticks grid) noexcept
{
    const auto snap = [grid] (Ticks t) { return grid > 0 ? quantizeTick (t, grid, 1.0, 0.0) : t; };
    const auto shortest = std::max<Ticks> (1, grid);
    if (rightEdge)
        return { note.start, std::max (shortest, snap (pointer) - note.start) };
    const auto start = std::min (snap (pointer), note.end() - shortest);
    return { start, note.end() - start };
}

// Session commands ------------------------------------------------------------------

std::optional<NoteRef> Session::drawNote (const Id& regionId, Ticks at, int pitch, Ticks length, int velocity, int channel)
{
    const auto* r = projection.region (regionId);
    if (sequenceOf (projection, r) == nullptr)
    {
        error = "Notes go in MIDI regions.";
        return std::nullopt;
    }
    if (at < r->start)
    {
        error = {};
        return std::nullopt; // clicking before the region start does nothing (AT-PRL-2)
    }
    Note n;
    n.id = newId();
    n.pos = noteSequenceTick (*r, at);
    n.length = std::max<Ticks> (1, length);
    n.pitch = std::clamp (pitch, 0, 127);
    n.velocity = std::clamp (velocity, 1, 127);
    n.channel = std::clamp (channel, 1, 16);
    const auto newLength = lengthToHold (projection, *r, at + n.length);
    const auto id = runCreate ("Add Note", n.id, [&] (TxnWriter& w) {
        bool good = w.put (notePath (r->midi.sequenceId, n.id), n.toValue());
        if (newLength != r->length)
            good = good && w.put (regionPath (regionId) / "length", Value (newLength));
        stamp (w, regionId);
        return wrote (good);
    });
    if (! id)
        return std::nullopt;
    return NoteRef { regionId, *id };
}

EditResult Session::moveNotes (const std::vector<NoteRef>& notes, Ticks deltaTicks, int deltaPitch, std::string_view name)
{
    struct Item
    {
        const Region* region;
        const Note* note;
    };
    std::vector<Item> items;
    for (const auto& ref : notes)
        if (const auto* n = findNote (projection, ref))
            items.push_back ({ projection.region (ref.regionId), n });
    if (items.empty())
        return reject ("Select notes first.");
    // As a group: pitch moves only if every note stays in range; time stops at the earliest region start.
    for (const auto& it : items)
        if (it.note->pitch + deltaPitch < 0 || it.note->pitch + deltaPitch > 127)
            deltaPitch = 0;
    for (const auto& it : items)
        deltaTicks = std::max (deltaTicks, it.region->start - noteTimelineTick (*it.region, it.note->pos));
    if (deltaTicks == 0 && deltaPitch == 0)
        return EditResult::fail ({});
    std::map<Id, Ticks> grow; // region -> new length
    for (const auto& it : items)
    {
        const auto end = noteTimelineTick (*it.region, it.note->pos) + deltaTicks + it.note->length;
        auto& len = grow.try_emplace (it.region->id, it.region->length).first->second;
        len = std::max (len, lengthToHold (projection, *it.region, end));
    }
    return run (name, [&] (TxnWriter& w) {
        bool good = true;
        std::set<Id> touched;
        for (const auto& it : items)
        {
            const auto base = notePath (it.region->midi.sequenceId, it.note->id);
            if (deltaTicks != 0)
                good = good && w.put (base / "pos", Value (it.note->pos + deltaTicks));
            if (deltaPitch != 0)
                good = good && w.put (base / "pitch", Value (it.note->pitch + deltaPitch));
            touched.insert (it.region->id);
        }
        for (const auto& [regionId, len] : grow)
            if (len != projection.region (regionId)->length)
                good = good && w.put (regionPath (regionId) / "length", Value (len));
        for (const auto& id : touched)
            stamp (w, id);
        return wrote (good);
    });
}

EditResult Session::setNoteSpan (const NoteRef& ref, Ticks start, Ticks length)
{
    const auto* n = findNote (projection, ref);
    if (n == nullptr)
        return reject ("That note no longer exists.");
    const auto* r = projection.region (ref.regionId);
    start = std::max (start, r->start);
    length = std::max<Ticks> (1, length);
    const auto pos = noteSequenceTick (*r, start);
    if (pos == n->pos && length == n->length)
        return EditResult::fail ({});
    const auto newLength = lengthToHold (projection, *r, start + length);
    return run ("Resize Note", [&] (TxnWriter& w) {
        const auto base = notePath (r->midi.sequenceId, n->id);
        bool good = true;
        if (pos != n->pos)
            good = good && w.put (base / "pos", Value (pos));
        if (length != n->length)
            good = good && w.put (base / "length", Value (length));
        if (newLength != r->length)
            good = good && w.put (regionPath (r->id) / "length", Value (newLength));
        stamp (w, r->id);
        return wrote (good);
    });
}

EditResult Session::deleteNotes (const std::vector<NoteRef>& notes)
{
    const auto groups = byRegion (notes);
    return run (notes.size() == 1 ? "Delete Note" : "Delete Notes", [&] (TxnWriter& w) {
        bool good = true;
        for (const auto& [regionId, noteIds] : groups)
        {
            const auto* r = projection.region (regionId);
            const auto* seq = sequenceOf (projection, r);
            if (seq == nullptr)
                continue;
            for (const auto& id : noteIds)
                if (seq->notes.contains (id))
                    good = good && w.remove (notePath (r->midi.sequenceId, id));
            stamp (w, regionId);
        }
        return wrote (good);
    });
}

std::vector<NoteRef> Session::pasteNotes (const NoteClipboard& clip, const Id& trackId, Ticks at)
{
    if (clip.empty())
        return {};
    const auto* r = midiRegionAt (projection, trackId, at);
    if (r == nullptr)
    {
        error = "Put the playhead over a MIDI region to paste notes.";
        return {};
    }
    Ticks earliest = clip.notes.front().pos;
    Ticks latestEnd = 0;
    for (const auto& n : clip.notes)
        earliest = std::min (earliest, n.pos);
    const auto regionId = r->id;
    std::vector<Note> placed;
    for (auto n : clip.notes)
    {
        n.id = newId();
        const auto timeline = at + (n.pos - earliest);
        n.pos = noteSequenceTick (*r, timeline);
        latestEnd = std::max (latestEnd, timeline + n.length);
        placed.push_back (n);
    }
    const auto newLength = lengthToHold (projection, *r, latestEnd);
    const auto result = run ("Paste", [&] (TxnWriter& w) {
        bool good = true;
        for (const auto& n : placed)
            good = good && w.put (notePath (r->midi.sequenceId, n.id), n.toValue());
        if (newLength != r->length)
            good = good && w.put (regionPath (r->id) / "length", Value (newLength));
        stamp (w, r->id);
        return wrote (good);
    });
    if (! result)
        return {};
    // r is stale now: the edit rebuilt the projection.
    std::vector<NoteRef> out;
    for (const auto& n : placed)
        out.push_back ({ regionId, n.id });
    return out;
}

namespace
{
template <typename Fn>
std::vector<std::pair<Path, int>> velocityWrites (const model::ProjectModel& m, const std::vector<NoteRef>& notes, Fn&& value)
{
    std::vector<std::pair<Path, int>> out;
    for (const auto& ref : notes)
        if (const auto* n = findNote (m, ref))
            if (const auto v = std::clamp (value (*n), 1, 127); v != n->velocity)
                out.emplace_back (notePath (m.region (ref.regionId)->midi.sequenceId, ref.noteId) / "velocity", v);
    return out;
}
} // namespace

EditResult Session::setNotesVelocity (const std::vector<NoteRef>& notes, int velocity)
{
    const auto writes = velocityWrites (projection, notes, [velocity] (const Note&) { return velocity; });
    return run ("Velocity", [&] (TxnWriter& w) {
        bool good = true;
        for (const auto& [path, v] : writes)
            good = good && w.put (path, Value (v));
        return wrote (good);
    });
}

EditResult Session::offsetNotesVelocity (const std::vector<NoteRef>& notes, int delta)
{
    // Each note clamps to 1..127 on its own (AT-PRL-7).
    const auto writes = velocityWrites (projection, notes, [delta] (const Note& n) { return n.velocity + delta; });
    return run ("Velocity", [&] (TxnWriter& w) {
        bool good = true;
        for (const auto& [path, v] : writes)
            good = good && w.put (path, Value (v));
        return wrote (good);
    });
}

EditResult Session::toggleNotesMuted (const std::vector<NoteRef>& notes)
{
    std::vector<std::pair<NoteRef, const Note*>> found;
    for (const auto& ref : notes)
        if (const auto* n = findNote (projection, ref))
            found.emplace_back (ref, n);
    if (found.empty())
        return reject ("Select notes first.");
    const bool allMuted = std::all_of (found.begin(), found.end(), [] (const auto& f) { return f.second->muted; });
    return run (allMuted ? "Unmute Notes" : "Mute Notes", [&] (TxnWriter& w) {
        bool good = true;
        for (const auto& [ref, n] : found)
            if (n->muted == allMuted)
                good = good && w.put (notePath (projection.region (ref.regionId)->midi.sequenceId, ref.noteId) / "muted", Value (! allMuted));
        return wrote (good);
    });
}

EditResult Session::quantizeNotes (const std::vector<Id>& regionIds, const std::vector<NoteRef>& notes, Ticks grid, double strength, double swingPercent)
{
    if (grid <= 0 || strength < 0 || strength > 1 || swingPercent < 0 || swingPercent > 100)
        return reject ("Quantize settings are out of range.");
    const auto selected = byRegion (notes);
    std::vector<std::pair<NoteRef, Ticks>> moves;
    for (const auto& regionId : regionIds)
    {
        const auto* r = projection.region (regionId);
        const auto* seq = sequenceOf (projection, r);
        if (seq == nullptr)
            continue;
        const auto picked = selected.find (regionId);
        if (! notes.empty() && picked == selected.end())
            continue;
        for (const auto& [id, n] : seq->notes)
        {
            if (notes.empty() ? (n.pos < r->midi.sequenceOffset || n.pos >= r->midi.sequenceOffset + r->length)
                              : std::find (picked->second.begin(), picked->second.end(), id) == picked->second.end())
                continue;
            const auto to = std::max (quantizeTick (noteTimelineTick (*r, n.pos), grid, strength, swingPercent), r->start);
            if (const auto pos = noteSequenceTick (*r, to); pos != n.pos)
                moves.emplace_back (NoteRef { regionId, id }, pos);
        }
    }
    return run ("Quantize", [&] (TxnWriter& w) {
        bool good = true;
        for (const auto& [ref, pos] : moves)
            good = good && w.put (notePath (projection.region (ref.regionId)->midi.sequenceId, ref.noteId) / "pos", Value (pos));
        return wrote (good);
    });
}

EditResult Session::applyRegionQuantize (const Id& regionId)
{
    const auto* r = projection.region (regionId);
    const auto* seq = sequenceOf (projection, r);
    if (seq == nullptr)
        return reject ("Quantize applies to MIDI regions.");
    if (! r->midi.quantize)
        return reject ("This region has no quantize setting to apply.");
    const auto q = *r->midi.quantize;
    std::vector<std::pair<Id, Ticks>> moves;
    for (const auto& [id, n] : seq->notes)
    {
        if (n.pos < r->midi.sequenceOffset || n.pos >= r->midi.sequenceOffset + r->length)
            continue;
        // The same positions playback uses (SnapshotCompiler).
        const auto to = quantizeTick (noteTimelineTick (*r, n.pos), q.grid, q.strength, q.swing * 100.0);
        if (const auto pos = noteSequenceTick (*r, std::max (to, r->start)); pos != n.pos)
            moves.emplace_back (id, pos);
    }
    return run ("Apply Quantize", [&] (TxnWriter& w) {
        bool good = w.put (regionPath (regionId) / "midi" / "quantize", Value());
        for (const auto& [id, pos] : moves)
            good = good && w.put (notePath (r->midi.sequenceId, id) / "pos", Value (pos));
        stamp (w, regionId);
        return wrote (good);
    });
}

// Controllers ---------------------------------------------------------------------------

std::optional<Id> Session::addController (const Id& regionId, model::CcEvent e)
{
    const auto* r = projection.region (regionId);
    if (sequenceOf (projection, r) == nullptr)
    {
        error = "Controller points go in MIDI regions.";
        return std::nullopt;
    }
    const bool bend = e.kind == "pitchBend";
    if (e.pos < r->start || e.number < 0 || e.number > 127 || e.channel < 1 || e.channel > 16
        || (bend ? (e.value < -8192 || e.value > 8191) : (e.value < 0 || e.value > 127)))
    {
        error = "Controller values are out of range.";
        return std::nullopt;
    }
    if (e.id.empty())
        e.id = newId();
    e.pos = noteSequenceTick (*r, e.pos);
    return runCreate ("Add Controller Point", e.id, [&] (TxnWriter& w) {
        bool good = w.put (ccPath (r->midi.sequenceId, e.id), e.toValue());
        stamp (w, regionId);
        return wrote (good);
    });
}

EditResult Session::moveController (const Id& regionId, const Id& eventId, Ticks at, int value)
{
    const auto* r = projection.region (regionId);
    const auto* seq = sequenceOf (projection, r);
    if (seq == nullptr || ! seq->controllers.contains (eventId))
        return reject ("That controller point no longer exists.");
    const auto& e = seq->controllers.at (eventId);
    value = e.kind == "pitchBend" ? std::clamp (value, -8192, 8191) : std::clamp (value, 0, 127);
    const auto pos = noteSequenceTick (*r, std::max (at, r->start));
    return run ("Move Controller Point", [&] (TxnWriter& w) {
        bool good = true;
        if (pos != e.pos)
            good = good && w.put (ccPath (r->midi.sequenceId, eventId) / "pos", Value (pos));
        if (value != e.value)
            good = good && w.put (ccPath (r->midi.sequenceId, eventId) / "value", Value (value));
        stamp (w, regionId);
        return wrote (good);
    });
}

EditResult Session::deleteControllers (const Id& regionId, const std::vector<Id>& eventIds)
{
    const auto* r = projection.region (regionId);
    const auto* seq = sequenceOf (projection, r);
    if (seq == nullptr)
        return reject ("That region no longer exists.");
    return run ("Delete Controller Points", [&] (TxnWriter& w) {
        bool good = true;
        for (const auto& id : eventIds)
            if (seq->controllers.contains (id))
                good = good && w.remove (ccPath (r->midi.sequenceId, id));
        stamp (w, regionId);
        return wrote (good);
    });
}
} // namespace weft::commands
