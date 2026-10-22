#include "weft/model/ProjectModel.h"

#include <algorithm>
#include <functional>

namespace weft::model
{
using doc::Document;
using doc::Path;
using doc::PathSegment;

Id recoveredTrackId (const Id& deletedTrackId) { return deriveId ("weft.recovered/" + deletedTrackId); }

// Lookups -------------------------------------------------------------------

const Track* ProjectModel::track (const Id& id) const
{
    if (auto it = tracks.find (id); it != tracks.end())
        return it->second.get();
    if (auto it = syntheticTracks.find (id); it != syntheticTracks.end())
        return it->second.get();
    return nullptr;
}

const Region* ProjectModel::region (const Id& id) const
{
    auto it = regions.find (id);
    return it == regions.end() ? nullptr : it->second.get();
}

const MidiSequence* ProjectModel::sequence (const Id& id) const
{
    auto it = midiSequences.find (id);
    return it == midiSequences.end() ? nullptr : it->second.get();
}

const MediaRef* ProjectModel::mediaRef (const std::string& hash) const
{
    auto it = media.find (hash);
    return it == media.end() ? nullptr : it->second.get();
}

const Comment* ProjectModel::comment (const Id& id) const
{
    auto it = comments.find (id);
    return it == comments.end() ? nullptr : it->second.get();
}

const VersionTag* ProjectModel::version (const Id& id) const
{
    auto it = versions.find (id);
    return it == versions.end() ? nullptr : it->second.get();
}

Id ProjectModel::effectiveTrackId (const Region& r) const
{
    auto it = recoveredTrackFor.find (r.id);
    return it == recoveredTrackFor.end() ? r.trackId : it->second;
}

std::vector<const Region*> ProjectModel::regionsOnTrack (const Id& trackId) const
{
    std::vector<const Region*> out;
    for (const auto& [id, r] : regions)
        if (effectiveTrackId (*r) == trackId)
            out.push_back (r.get());
    std::sort (out.begin(), out.end(), [] (const Region* a, const Region* b) { return std::tie (a->start, a->z, a->id) < std::tie (b->start, b->z, b->id); });
    return out;
}

const AutomationLane* ProjectModel::lane (const Id& trackId, const AutomationTarget& target) const
{
    const auto key = target.key();
    for (const auto& [id, l] : automationLanes)
        if (l->trackId == trackId && l->target.key() == key)
            return l.get();
    return nullptr;
}

std::vector<RoutingEdge> ProjectModel::routingEdges() const
{
    std::vector<RoutingEdge> edges;
    for (const auto& [id, t] : tracks)
    {
        if (! t->output.toMaster())
            edges.push_back ({ id, t->output.busId, {} });
        for (const auto& s : t->strip.sends)
            if (! s.targetBusId.empty())
                edges.push_back ({ id, s.targetBusId, s.id });
    }
    return edges;
}

double ProjectModel::regionEndTick (const Region& r) const
{
    if (r.isMidi)
        return static_cast<double> (r.start + r.length);
    return tempo.sampleToTick (tempo.tickToSample (static_cast<double> (r.start)) + static_cast<double> (r.length));
}

Color ProjectModel::regionColor (const Region& r) const
{
    if (r.color)
        return *r.color;
    const auto* t = track (effectiveTrackId (r));
    return t != nullptr ? t->color : Color {};
}

// Reading -------------------------------------------------------------------

namespace
{
template <typename T>
void readCollection (std::map<Id, Ref<T>>& out, const Value& v)
{
    out.clear();
    for (const auto& [id, item] : v.asMap())
        if (item.isMap())
            out.emplace (id, std::make_shared<const T> (T::fromValue (id, item)));
}

void readTempo (ProjectModel& m, const Value& v)
{
    m.storedTempoPoints.clear();
    for (const auto& [id, p] : v["points"].asMap())
        m.storedTempoPoints.push_back ({ id, std::max<std::int64_t> (0, p["pos"].asInt()), p["bpm"].asDouble (120.0), false });
}

void readSignatures (ProjectModel& m, const Value& v)
{
    m.storedSignaturePoints.clear();
    for (const auto& [id, p] : v["points"].asMap())
        m.storedSignaturePoints.push_back ({ id, static_cast<int> (p["bar"].asInt()), static_cast<int> (p["numerator"].asInt (4)),
                                             static_cast<int> (p["denominator"].asInt (4)), false });
}

void readCollab (ProjectModel& m, const Value& collab)
{
    readCollection (m.comments, collab["comments"]);
    readCollection (m.versions, collab["versions"]);
    m.chat.clear();
    for (const auto& item : collab["chat"].asList())
        if (item.isMap())
            m.chat.push_back (ChatMessage::fromValue (item));
}

void readInfo (ProjectModel& m, const Value& root)
{
    auto& info = m.info;
    info.schemaVersion = static_cast<int> (root["schemaVersion"].asInt (1));
    info.id = root["id"].asString();
    info.name = root["name"].asString();
    info.sampleRate = static_cast<int> (root["sampleRate"].asInt (48000));
    if (! isValidSampleRate (info.sampleRate))
        info.sampleRate = 48000;
    info.recordBitDepth = static_cast<int> (root["recordBitDepth"].asInt (24));
    if (! isValidBitDepth (info.recordBitDepth))
        info.recordBitDepth = 24;
    info.createdAt = root["createdAt"].asInt();
    info.createdBy = root["createdBy"].asString();
    info.masterTrackId = root["masterTrackId"].asString();
    info.cycle = TimeRange::fromValue (root["cycle"], TimeRange {});
    info.punch = TimeRange::fromValue (root["punch"], TimeRange {});
    info.endMarker = root["endMarker"].isNumber() ? std::optional<Ticks> (std::max<std::int64_t> (0, root["endMarker"].asInt())) : std::nullopt;
    info.metronomeDefaults = MetronomeSettings::fromValue (root["metronomeDefaults"]);

    m.storedTrackOrder.clear();
    for (const auto& item : root["trackOrder"].asList())
        if (item.isString())
            m.storedTrackOrder.push_back (item.asString());
}

/** Keeps one point per position: the greater Id wins and is flagged (D-DM-9). */
template <typename P, typename PosFn>
std::vector<P> dedupeByPosition (std::vector<P> points, PosFn pos)
{
    std::sort (points.begin(), points.end(), [&] (const P& a, const P& b) { return pos (a) != pos (b) ? pos (a) < pos (b) : a.id > b.id; });
    std::vector<P> out;
    for (auto& p : points)
    {
        if (! out.empty() && pos (out.back()) == pos (p))
        {
            out.back().conflicted = true;
            continue;
        }
        out.push_back (p);
    }
    return out;
}

template <typename T>
void refreshEntity (std::map<Id, Ref<T>>& out, const Id& id, const std::optional<Value>& v)
{
    if (v && v->isMap())
        out[id] = std::make_shared<const T> (T::fromValue (id, *v));
    else
        out.erase (id);
}

/** Write stamp of an edge: the op that last wrote its target field. */
std::optional<doc::OpStamp> edgeStamp (const Document& d, const RoutingEdge& e)
{
    Path p = e.sendId.empty() ? doc::path ({ keys::tracks, e.fromTrackId, "output" })
                              : doc::path ({ keys::tracks, e.fromTrackId, "strip", "sends" }) / PathSegment::id (e.sendId) / "targetBusId";
    if (auto op = d.opId (p))
        return doc::OpStamp::parse (*op);
    return std::nullopt;
}
} // namespace

ProjectModel Projector::project (const Document& document)
{
    ProjectModel m;
    const auto root = document.root();
    readInfo (m, root);
    readCollection (m.tracks, root[keys::tracks]);
    readCollection (m.regions, root[keys::regions]);
    readCollection (m.takeFolders, root[keys::takeFolders]);
    readCollection (m.midiSequences, root[keys::midiSequences]);
    readCollection (m.markers, root[keys::markers]);
    readCollection (m.storedLanes, root[keys::automationLanes]);
    readCollab (m, root[keys::collab]);
    m.media.clear();
    for (const auto& [hash, item] : root[keys::media].asMap())
        m.media.emplace (hash, std::make_shared<const MediaRef> (MediaRef::fromValue (hash, item)));
    readTempo (m, root[keys::tempoMap]);
    readSignatures (m, root[keys::signatureMap]);
    derive (m, document);
    return m;
}

void Projector::refresh (ProjectModel& m, const Document& document, const std::vector<Path>& changed)
{
    bool infoChanged = false;
    for (const auto& p : changed)
    {
        if (p.empty() || p[0].kind != PathSegment::Kind::key)
        {
            infoChanged = true;
            continue;
        }
        const auto& c = p[0].key;
        if (c == keys::collab)
            readCollab (m, document.get ({ p[0] }).value_or (Value {}));
        else if (c == keys::tempoMap)
            readTempo (m, document.get ({ p[0] }).value_or (Value {}));
        else if (c == keys::signatureMap)
            readSignatures (m, document.get ({ p[0] }).value_or (Value {}));
        else if (p.size() < 2 || p[1].kind != PathSegment::Kind::key)
        {
            // A whole collection was replaced, or a project field changed.
            if (c == keys::tracks || c == keys::regions || c == keys::takeFolders || c == keys::midiSequences || c == keys::markers
                || c == keys::automationLanes || c == keys::media)
            {
                m = project (document);
                return;
            }
            infoChanged = true;
        }
        else if (c == keys::midiSequences && p.size() >= 4 && m.midiSequences.contains (p[1].key))
        {
            // One note or controller event (Session::refreshFor): re-read just that.
            auto seq = std::make_shared<MidiSequence> (*m.midiSequences.at (p[1].key));
            const auto& eventId = p[3].key;
            const auto v = document.get ({ p[0], p[1], p[2], p[3] });
            if (p[2].key == "notes")
            {
                if (v && v->isMap())
                    seq->notes.insert_or_assign (eventId, Note::fromValue (eventId, *v));
                else
                    seq->notes.erase (eventId);
            }
            else if (v && v->isMap())
                seq->controllers.insert_or_assign (eventId, CcEvent::fromValue (eventId, *v));
            else
                seq->controllers.erase (eventId);
            m.midiSequences[p[1].key] = std::move (seq);
        }
        else
        {
            const auto& id = p[1].key;
            const auto v = document.get ({ p[0], p[1] });
            if (c == keys::tracks)
                refreshEntity (m.tracks, id, v);
            else if (c == keys::regions)
                refreshEntity (m.regions, id, v);
            else if (c == keys::takeFolders)
                refreshEntity (m.takeFolders, id, v);
            else if (c == keys::midiSequences)
                refreshEntity (m.midiSequences, id, v);
            else if (c == keys::markers)
                refreshEntity (m.markers, id, v);
            else if (c == keys::automationLanes)
                refreshEntity (m.storedLanes, id, v);
            else if (c == keys::media)
            {
                if (v && v->isMap())
                    m.media[id] = std::make_shared<const MediaRef> (MediaRef::fromValue (id, *v));
                else
                    m.media.erase (id);
            }
            else
                infoChanged = true;
        }
    }
    if (infoChanged)
    {
        // Project fields are small; re-read them all (but not the collections).
        Value root = Value::map();
        for (const char* k : { "schemaVersion", "id", "name", "sampleRate", "recordBitDepth", "createdAt", "createdBy", "masterTrackId", "cycle",
                               "punch", "endMarker", "metronomeDefaults", keys::trackOrder })
            if (auto v = document.get (doc::path ({ k })))
                root.set (k, std::move (*v));
        readInfo (m, root);
    }
    derive (m, document);
}

void Projector::derive (ProjectModel& m, const Document& document)
{
    // Master track: masterTrackId must name a master track; otherwise use the smallest-Id master.
    if (const auto* t = m.track (m.info.masterTrackId); t == nullptr || t->kind != TrackKind::master)
    {
        m.info.masterTrackId.clear();
        for (const auto& [id, t2] : m.tracks)
            if (t2->kind == TrackKind::master)
            {
                m.info.masterTrackId = id;
                break;
            }
    }

    // 3. Orphan regions go to a Recovered track derived from the missing track's id (D-DM-17).
    m.recoveredTrackFor.clear();
    m.syntheticTracks.clear();
    for (const auto& [id, r] : m.regions)
    {
        if (m.tracks.contains (r->trackId))
            continue;
        const auto rid = recoveredTrackId (r->trackId);
        m.recoveredTrackFor[id] = rid;
        if (! m.tracks.contains (rid) && ! m.syntheticTracks.contains (rid))
        {
            auto t = std::make_shared<Track>();
            t->id = rid;
            t->kind = r->isMidi ? TrackKind::instrument : TrackKind::audio;
            t->name = "Recovered";
            t->locked = LockInfo { "", 0, "Recovered regions are read-only until moved" };
            m.syntheticTracks.emplace (rid, std::move (t));
        }
    }

    // 1 + 2. trackOrder: drop duplicates and unknown ids, then append missing tracks sorted by Id.
    m.trackOrder.clear();
    std::set<Id> seen;
    for (const auto& id : m.storedTrackOrder)
    {
        const auto* t = m.track (id);
        if (t != nullptr && t->kind != TrackKind::master && seen.insert (id).second)
            m.trackOrder.push_back (id);
    }
    std::vector<Id> missing;
    for (const auto* trackMap : { &m.tracks, &m.syntheticTracks })
        for (const auto& [id, t] : *trackMap)
            if (t->kind != TrackKind::master && ! seen.contains (id))
                missing.push_back (id);
    std::sort (missing.begin(), missing.end());
    m.trackOrder.insert (m.trackOrder.end(), missing.begin(), missing.end());

    // 4. Tempo and signature points: one per position, greater Id wins (D-DM-9).
    m.tempo = TempoMap (dedupeByPosition (m.storedTempoPoints, [] (const TempoPoint& p) { return p.pos; }), static_cast<double> (m.info.sampleRate));
    m.signatures = SignatureMap (dedupeByPosition (m.storedSignaturePoints, [] (const SignaturePoint& p) { return p.bar; }));

    // 5. Routing cycles: repeatedly disable the most recently written edge of a cycle (D-DM-6).
    m.disabledEdges.clear();
    auto edges = m.routingEdges();
    std::erase_if (edges, [&] (const RoutingEdge& e) { return m.track (e.toTrackId) == nullptr; });
    if (! edges.empty())
    {
        std::map<std::string, std::optional<doc::OpStamp>> stamps;
        for (;;)
        {
            std::map<Id, std::vector<const RoutingEdge*>> out;
            for (const auto& e : edges)
                if (! m.disabledEdges.contains (e.key()))
                    out[e.fromTrackId].push_back (&e);

            // Iterative DFS that returns the edges of the first cycle found.
            std::vector<const RoutingEdge*> cycle;
            std::map<Id, int> state; // 0 new, 1 on stack, 2 done
            std::vector<const RoutingEdge*> stack;
            std::function<bool (const Id&)> visit = [&] (const Id& node) {
                state[node] = 1;
                for (const auto* e : out[node])
                {
                    stack.push_back (e);
                    const int s = state[e->toTrackId];
                    if (s == 1)
                    {
                        auto it = std::find_if (stack.begin(), stack.end(), [&] (const RoutingEdge* x) { return x->fromTrackId == e->toTrackId; });
                        cycle.assign (it, stack.end());
                        return true;
                    }
                    if (s == 0 && visit (e->toTrackId))
                        return true;
                    stack.pop_back();
                }
                state[node] = 2;
                return false;
            };
            for (const auto& [node, list] : out)
                if (state[node] == 0 && visit (node))
                    break;
            if (cycle.empty())
                break;

            const RoutingEdge* newest = nullptr;
            std::optional<doc::OpStamp> newestStamp;
            for (const auto* e : cycle)
            {
                auto [it, inserted] = stamps.try_emplace (e->key());
                if (inserted)
                    it->second = edgeStamp (document, *e);
                const auto& s = it->second;
                if (newest == nullptr || s > newestStamp || (s == newestStamp && e->key() > newest->key()))
                {
                    newest = e;
                    newestStamp = s;
                }
            }
            m.disabledEdges.insert (newest->key());
        }
    }

    // 6. Duplicate automation lanes for one (track, target): the smallest Id is canonical and
    //    points are unioned (D-DM-11). storedLanes iterates in Id order.
    m.automationLanes.clear();
    std::map<std::pair<Id, std::string>, std::shared_ptr<AutomationLane>> canonical;
    for (const auto& [id, lane] : m.storedLanes)
    {
        auto& slot = canonical[{ lane->trackId, lane->target.key() }];
        if (! slot)
        {
            slot = std::make_shared<AutomationLane> (*lane);
            continue;
        }
        slot->mergedLaneIds.push_back (id);
        for (const auto& [pointId, p] : lane->points)
            slot->points.emplace (pointId, p);
    }
    for (auto& [key, lane] : canonical)
        m.automationLanes.emplace (lane->id, std::move (lane));
}
} // namespace weft::model
