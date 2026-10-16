#pragma once

// The projection: a typed, repaired, read-only view of the project document.
// Built by Projector from a Document and refreshed incrementally after edits.
// All repairs (spec 03 §10.4) are deterministic functions of the document, so
// every client shows the same result; the document itself is not changed.

#include "weft/doc/Document.h"
#include "weft/model/Model.h"
#include "weft/time/SignatureMap.h"
#include "weft/time/TempoMap.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace weft::model
{
template <typename T>
using Ref = std::shared_ptr<const T>;

/** A routing edge: a track's output, or one of its sends. */
struct RoutingEdge
{
    Id fromTrackId;
    Id toTrackId;
    Id sendId; // empty for the output edge

    /** "out:<track>" or "send:<track>:<send>"; used for disabledEdges and tie-breaks. */
    std::string key() const { return sendId.empty() ? "out:" + fromTrackId : "send:" + fromTrackId + ":" + sendId; }
};

struct ProjectModel
{
    ProjectInfo info;

    // Stored entities (as read, with lenient field repair).
    std::map<Id, Ref<Track>> tracks;
    std::map<Id, Ref<Region>> regions;
    std::map<Id, Ref<TakeFolder>> takeFolders;
    std::map<Id, Ref<MidiSequence>> midiSequences;
    std::map<std::string, Ref<MediaRef>> media;
    std::map<Id, Ref<Marker>> markers;
    std::map<Id, Ref<AutomationLane>> storedLanes;
    std::vector<Id> storedTrackOrder; // raw list, may contain duplicates
    std::vector<TempoPoint> storedTempoPoints;
    std::vector<SignaturePoint> storedSignaturePoints;
    std::map<Id, Ref<Comment>> comments;     // collab.comments
    std::vector<ChatMessage> chat;           // collab.chat, in list order (D-DM-14)
    std::map<Id, Ref<VersionTag>> versions;  // collab.versions

    // Derived (repaired) views.
    std::vector<Id> trackOrder;                           // deduped, complete, excludes master (repairs 1, 2)
    std::map<Id, Id> recoveredTrackFor;                   // orphan region -> Recovered track id (repair 3)
    std::map<Id, Ref<Track>> syntheticTracks;             // Recovered lanes not yet in the document
    TempoMap tempo;                                       // repair 4
    SignatureMap signatures;                              // repair 4
    std::set<std::string> disabledEdges;                  // RoutingEdge keys (repair 5)
    std::map<Id, Ref<AutomationLane>> automationLanes;    // canonical lanes, unioned (repair 6)

    // Lookups (stored or synthetic) ---------------------------------------
    const Track* track (const Id& id) const;
    const Region* region (const Id& id) const;
    const MidiSequence* sequence (const Id& id) const;
    const MediaRef* mediaRef (const std::string& hash) const;
    const Comment* comment (const Id& id) const;
    const VersionTag* version (const Id& id) const;

    /** Track a region is shown on: its trackId, or the Recovered track for an orphan. */
    Id effectiveTrackId (const Region& r) const;
    std::vector<const Region*> regionsOnTrack (const Id& trackId) const;
    /** The canonical lane for (track, target), if any. */
    const AutomationLane* lane (const Id& trackId, const AutomationTarget& target) const;
    std::vector<RoutingEdge> routingEdges() const;

    /** End of a region on the timeline in ticks (audio lengths convert through the tempo map). */
    double regionEndTick (const Region& r) const;
    /** The color a region shows: its own, or its track's (F-TRK-9). */
    Color regionColor (const Region& r) const;
};

/** Document root keys for entity collections (depth-2 paths are entities). */
namespace keys
{
inline constexpr const char* tracks = "tracks";
inline constexpr const char* trackOrder = "trackOrder";
inline constexpr const char* regions = "regions";
inline constexpr const char* takeFolders = "takeFolders";
inline constexpr const char* midiSequences = "midiSequences";
inline constexpr const char* media = "media";
inline constexpr const char* tempoMap = "tempoMap";
inline constexpr const char* signatureMap = "signatureMap";
inline constexpr const char* markers = "markers";
inline constexpr const char* automationLanes = "automationLanes";
inline constexpr const char* collab = "collab";
} // namespace keys

class Projector
{
public:
    /** Reads the whole document. */
    static ProjectModel project (const doc::Document& document);

    /** Re-reads the entities under the given paths (truncated to depth 2), then re-derives
        the repaired views. Paths outside entity collections re-read the project fields. */
    static void refresh (ProjectModel& model, const doc::Document& document, const std::vector<doc::Path>& changed);

    /** Recomputes the derived views from the stored entities. */
    static void derive (ProjectModel& model, const doc::Document& document);
};

/** Id of the Recovered track for regions orphaned by deleting trackId (D-DM-17). */
Id recoveredTrackId (const Id& deletedTrackId);
} // namespace weft::model
