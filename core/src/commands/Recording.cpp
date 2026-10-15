// Turning a finished recording pass into document edits (spec 02 W-1a step 8,
// W-1c, W-1d, W-2 step 6-8).

#include "weft/commands/Session.h"

#include <algorithm>
#include <cmath>

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
} // namespace

double recordingMinutesAvailable (std::uint64_t freeBytes, int sampleRate, int bitDepth, int totalChannels) noexcept
{
    const double bytesPerMinute = 60.0 * sampleRate * (bitDepth / 8.0) * totalChannels;
    return bytesPerMinute > 0 ? static_cast<double> (freeBytes) / bytesPerMinute : 1e9;
}

bool needsDiskSpaceWarning (std::uint64_t freeBytes, int sampleRate, int bitDepth, int totalChannels) noexcept
{
    return totalChannels > 0 && recordingMinutesAvailable (freeBytes, sampleRate, bitDepth, totalChannels) < 10.0;
}

std::string Session::nextRecordingName (const std::string& trackName) const
{
    const auto prefix = trackName + "#";
    int highest = 0;
    for (const auto& [hash, m] : projection.media)
        if (m->originalName.starts_with (prefix))
        {
            const auto rest = m->originalName.substr (prefix.size());
            if (! rest.empty() && std::all_of (rest.begin(), rest.end(), [] (char c) { return c >= '0' && c <= '9'; }) && rest.size() < 9)
                highest = std::max (highest, std::stoi (rest));
        }
    return prefix + std::to_string (highest + 1);
}

EditResult Session::setActiveTake (const Id& takeFolderId, const Id& regionId)
{
    const auto folder = projection.takeFolders.find (takeFolderId);
    if (folder == projection.takeFolders.end())
        return reject ("That take folder no longer exists.");
    const auto& takes = folder->second->takeIds;
    if (std::find (takes.begin(), takes.end(), regionId) == takes.end())
        return reject ("That take isn't in the folder.");
    return run ("Select Take", [&] (TxnWriter& w) {
        return wrote (w.put (doc::path ({ keys::takeFolders, takeFolderId, "activeTakeId" }), Value (regionId)));
    });
}

EditResult Session::commitRecording (const std::vector<RecordedAudio>& audio, const std::vector<RecordedMidi>& midi)
{
    if (audio.empty() && midi.empty())
        return {};
    return run ("Record", [&] (TxnWriter& w) {
        bool good = true;
        const auto now = clock();
        auto order = projection.storedTrackOrder.size();
        std::map<Id, Id> recovered; // deleted track -> its recovered track (D-W1-6)
        std::map<Id, std::int64_t> nextZ;
        std::map<std::string, int> lastNumber; // names handed out in this commit
        const auto recordingName = [&] (const std::string& trackName) {
            auto it = lastNumber.find (trackName);
            if (it == lastNumber.end())
            {
                const auto next = nextRecordingName (trackName);
                it = lastNumber.emplace (trackName, std::stoi (next.substr (trackName.size() + 1)) - 1).first;
            }
            return trackName + "#" + std::to_string (++it->second);
        };

        const auto targetTrack = [&] (const Id& trackId, const std::string& name, TrackKind kind, int channels) -> Id {
            if (const auto* t = projection.track (trackId); t != nullptr && projection.tracks.contains (trackId))
                return trackId;
            if (auto it = recovered.find (trackId); it != recovered.end())
                return it->second;
            model::Track t;
            t.id = newId();
            t.kind = kind;
            t.name = name + " (recovered)";
            t.color.palette = static_cast<int> ((projection.tracks.size() + recovered.size()) % 24);
            t.channels = kind == TrackKind::audio ? std::clamp (channels, 1, 2) : 2;
            t.input.kind = kind == TrackKind::audio ? model::InputAssignment::Kind::audio : model::InputAssignment::Kind::midi;
            good = good && w.put (trackPath (t.id), t.toValue()) && w.listInsert (doc::path ({ keys::trackOrder }), order++, Value (t.id), t.id);
            recovered[trackId] = t.id;
            return t.id;
        };
        const auto topZ = [&] (const Id& trackId) -> std::int64_t& {
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

        for (const auto& rec : audio)
        {
            if (rec.takes.empty())
                continue;
            const auto trackId = targetTrack (rec.trackId, rec.trackName, TrackKind::audio, rec.trackChannels);
            auto media = rec.media;
            if (media.originalName.empty())
            {
                const auto* t = projection.track (trackId);
                media.originalName = recordingName (t != nullptr ? t->name : rec.trackName + " (recovered)");
            }
            media.addedBy = userId;
            media.addedAt = now;
            good = good && w.put (doc::path ({ keys::media, media.hash }), media.toValue());

            model::TakeFolder folder;
            if (rec.takeFolder)
            {
                folder.id = newId();
                folder.trackId = trackId;
                folder.start = rec.folderStart;
                folder.endTicks = rec.folderEnd;
            }
            auto& z = topZ (trackId);
            for (std::size_t i = 0; i < rec.takes.size(); ++i)
            {
                const auto& take = rec.takes[i];
                Region r;
                r.id = newId();
                r.trackId = trackId;
                r.start = take.start;
                r.length = take.length;
                r.name = rec.takeFolder ? media.originalName + " Take " + std::to_string (i + 1) : media.originalName;
                r.z = z;
                r.audio.media = media.hash;
                r.audio.sourceOffset = take.sourceOffset;
                r.audio.fadeIn.length = take.fadeIn;
                r.audio.fadeOut.length = take.fadeOut;
                r.takeFolderId = folder.id;
                r.createdBy = r.modifiedBy = userId;
                r.modifiedAt = now;
                good = good && w.put (regionPath (r.id), r.toValue());
                folder.takeIds.push_back (r.id);
            }
            ++z;
            if (rec.takeFolder)
            {
                folder.activeTakeId = folder.takeIds.back();
                good = good && w.put (doc::path ({ keys::takeFolders, folder.id }), folder.toValue());
            }
        }

        for (const auto& rec : midi)
        {
            if (rec.notes.empty() && rec.controllers.empty())
                continue;
            const auto trackId = targetTrack (rec.trackId, rec.trackName, TrackKind::instrument, 2);

            // Merge target: the topmost unlooped MIDI region overlapping the span.
            const Region* into = nullptr;
            if (rec.merge && trackId == rec.trackId)
                for (const auto* r : projection.regionsOnTrack (trackId))
                    if (r->isMidi && r->takeFolderId.empty() && ! (r->loop && r->loop->enabled) && projection.sequence (r->midi.sequenceId) != nullptr
                        && r->start < rec.regionEnd && r->start + r->length > rec.regionStart && (into == nullptr || r->z > into->z))
                        into = r;

            Id sequenceId, regionId;
            Ticks base = 0; // project tick of sequence position 0
            if (into != nullptr)
            {
                regionId = into->id;
                sequenceId = into->midi.sequenceId;
                const auto start = std::min (into->start, rec.regionStart);
                const auto end = std::max (into->start + into->length, rec.regionEnd);
                base = into->start - into->midi.sequenceOffset;
                if (start < base)
                {
                    // Sequence positions can't go negative: shift the existing content right.
                    const auto shift = base - start;
                    for (const auto& [id, n] : projection.sequence (sequenceId)->notes)
                        good = good && w.put (sequencePath (sequenceId) / "notes" / id / "pos", Value (n.pos + shift));
                    for (const auto& [id, c] : projection.sequence (sequenceId)->controllers)
                        good = good && w.put (sequencePath (sequenceId) / "controllers" / id / "pos", Value (c.pos + shift));
                    base = start;
                }
                const auto offset = start - base; // the new sequence offset
                good = good && w.put (regionPath (regionId) / "start", Value (start)) && w.put (regionPath (regionId) / "length", Value (end - start))
                       && w.put (regionPath (regionId) / "midi" / "sequenceOffset", Value (offset)) && w.put (regionPath (regionId) / "modifiedBy", Value (userId))
                       && w.put (regionPath (regionId) / "modifiedAt", Value (now));
            }
            else
            {
                Region r;
                r.id = regionId = newId();
                r.isMidi = true;
                r.trackId = trackId;
                r.start = base = rec.regionStart;
                r.length = std::max<Ticks> (model::minMidiRegionLength, rec.regionEnd - rec.regionStart);
                const auto* t = projection.track (trackId);
                r.name = t != nullptr ? t->name : rec.trackName;
                r.z = topZ (trackId)++;
                r.midi.sequenceId = sequenceId = newId();
                r.createdBy = r.modifiedBy = userId;
                r.modifiedAt = now;
                model::MidiSequence seq;
                seq.id = sequenceId;
                good = good && w.put (sequencePath (seq.id), seq.toValue()) && w.put (regionPath (r.id), r.toValue());
            }
            for (auto n : rec.notes)
            {
                n.id = newId();
                n.pos -= base;
                good = good && w.put (sequencePath (sequenceId) / "notes" / n.id, n.toValue());
            }
            for (auto c : rec.controllers)
            {
                c.id = newId();
                c.pos -= base;
                good = good && w.put (sequencePath (sequenceId) / "controllers" / c.id, c.toValue());
            }
        }
        return wrote (good);
    });
}
} // namespace weft::commands
