#include "weft/commands/Session.h"

#include <algorithm>
#include <chrono>
#include <set>

namespace weft::commands
{
using doc::PathSegment;
using doc::Status;

std::int64_t systemClockMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds> (system_clock::now().time_since_epoch()).count();
}

std::string commitMessage (const UserId& user, std::string_view name) { return "weft1|" + user + "|" + std::string (name); }

namespace
{
std::uint64_t seedOrRandom (std::uint64_t seed)
{
    if (seed != 0)
        return seed;
    std::random_device rd;
    return (static_cast<std::uint64_t> (rd()) << 32) ^ rd();
}

/** Status-bar noun for the object a path belongs to. */
std::string describe (const Path& p)
{
    if (p.empty() || p[0].kind != PathSegment::Kind::key)
        return "an object";
    const auto& c = p[0].key;
    if (c == model::keys::regions)
        return "a region";
    if (c == model::keys::tracks)
        return p.size() > 3 && p[3].key == "sends" ? "a send" : (p.size() > 3 && p[3].key == "inserts" ? "a plug-in" : "a track");
    if (c == model::keys::midiSequences)
        return "a note";
    if (c == model::keys::automationLanes)
        return "an automation lane";
    if (c == model::keys::markers)
        return "a marker";
    if (c == model::keys::tempoMap)
        return "a tempo change";
    if (c == model::keys::signatureMap)
        return "a signature change";
    return "an object";
}

Path depth2 (const Path& p) { return p.size() > 2 ? Path (p.begin(), p.begin() + 2) : p; }
} // namespace

Session::Session (doc::Document document, UserId user, Clock c, std::uint64_t idSeed)
    : doc (std::move (document)), userId (std::move (user)), clock (std::move (c)), ids (seedOrRandom (idSeed))
{
    projection = model::Projector::project (doc);
}

std::optional<Session> Session::createNew (const NewProjectOptions& options, UserId user, std::string* errorOut, Clock c, std::uint64_t idSeed)
{
    auto fail = [&] (std::string why) -> std::optional<Session> {
        if (errorOut != nullptr)
            *errorOut = std::move (why);
        return std::nullopt;
    };
    if (! model::isValidSampleRate (options.sampleRate))
        return fail ("Sample rate " + std::to_string (options.sampleRate) + " Hz is not supported (use 44100, 48000, 88200 or 96000).");
    if (! model::isValidBitDepth (options.recordBitDepth))
        return fail ("Bit depth " + std::to_string (options.recordBitDepth) + " is not supported (use 16, 24 or 32).");

    Session s (doc::Document(), std::move (user), std::move (c), idSeed);
    const auto now = s.clock();
    const auto projectId = s.newId();

    model::Track master;
    master.id = s.newId();
    master.kind = model::TrackKind::master;
    master.name = "Master";
    master.input = {};

    model::Track audio;
    audio.id = s.newId();
    audio.kind = model::TrackKind::audio;
    audio.name = "Audio 1";
    audio.channels = 1;
    audio.input.kind = model::InputAssignment::Kind::audio;

    Value root = Value::map()
                     .set ("schemaVersion", model::schemaVersion)
                     .set ("id", projectId)
                     .set ("name", options.name)
                     .set ("sampleRate", options.sampleRate)
                     .set ("recordBitDepth", options.recordBitDepth)
                     .set ("createdAt", now)
                     .set ("createdBy", s.userId)
                     .set (model::keys::tracks, Value::map().set (master.id, master.toValue()).set (audio.id, audio.toValue()))
                     .set (model::keys::trackOrder, Value (Value::List { Value (audio.id) }))
                     .set ("masterTrackId", master.id)
                     .set (model::keys::regions, Value::map())
                     .set (model::keys::takeFolders, Value::map())
                     .set (model::keys::midiSequences, Value::map())
                     .set (model::keys::media, Value::map())
                     .set (model::keys::tempoMap, Value::map().set ("points", Value::map().set ("initial", Value::map().set ("pos", 0).set ("bpm", 120.0).set ("curve", "step"))))
                     .set (model::keys::signatureMap,
                           Value::map().set ("points", Value::map().set ("initial", Value::map().set ("bar", 0).set ("numerator", 4).set ("denominator", 4))))
                     .set (model::keys::markers, Value::map())
                     .set ("cycle", model::TimeRange {}.toValue())
                     .set ("punch", model::TimeRange {}.toValue())
                     .set ("endMarker", Value())
                     .set ("metronomeDefaults", model::MetronomeSettings {}.toValue())
                     .set (model::keys::automationLanes, Value::map())
                     .set (model::keys::collab, Value::map()
                                                    .set ("projectId", projectId)
                                                    .set ("relayUrl", Value())
                                                    .set ("comments", Value::map())
                                                    .set ("chat", Value::list())
                                                    .set ("versions", Value::map()));
    for (const auto& [key, value] : root.asMap())
        if (s.doc.put (doc::path ({ key }), value) != Status::ok)
            return fail ("Couldn't create the project document.");
    s.doc.commit (commitMessage (s.userId, "New Project"), now);
    s.reproject();
    return s;
}

doc::Status Session::applyChanges (std::span<const std::uint8_t> changes)
{
    const auto before = doc.heads();
    const auto status = doc.apply (changes);
    if (status == Status::ok)
        model::Projector::refresh (projection, doc, doc.changedPaths (before, 2));
    return status;
}

void Session::reproject() { projection = model::Projector::project (doc); }

void Session::refreshFor (const Txn& txn)
{
    std::vector<Path> paths;
    std::set<std::string> seen;
    for (const auto& r : txn.records)
    {
        auto p = depth2 (r.path);
        if (seen.insert (doc::toString (p)).second)
            paths.push_back (std::move (p));
    }
    if (! paths.empty())
        model::Projector::refresh (projection, doc, paths);
}

void Session::finish (Txn txn, bool isUndoRedo, std::vector<Txn>* pushTo)
{
    if (txn.empty())
    {
        doc.rollback();
        return;
    }
    const auto now = clock();
    const auto label = isUndoRedo ? (pushTo == &redoStack ? "Undo " : "Redo ") + txn.name : txn.name;
    if (! doc.commit (commitMessage (userId, label), now))
        return;
    for (auto& r : txn.records)
        if (r.kind == TxnRecord::Kind::set && r.after && r.after->isContainer())
        {
            r.afterOpId = doc.opId (r.path).value_or ("");
            if (! r.restoresOpId.empty() && ! r.afterOpId.empty())
                opIdAliases[r.restoresOpId] = r.afterOpId;
        }
    if (onLocalChange)
        if (auto change = doc.lastLocalChange())
            onLocalChange (*change, now);
    refreshFor (txn);
    if (pushTo != nullptr)
        pushTo->push_back (std::move (txn));
}

EditResult Session::run (std::string_view name, const std::function<EditResult (TxnWriter&)>& fn)
{
    error.clear();
    if (gesture)
    {
        const auto before = gesture->txn.records.size();
        auto r = fn (*gesture->writer);
        if (! r)
            error = r.error;
        if (gesture->txn.records.size() != before || r)
            refreshFor (gesture->txn);
        return r;
    }

    Txn txn { std::string (name), {} };
    TxnWriter writer (doc, txn);
    auto r = fn (writer);
    if (! r)
    {
        doc.rollback();
        refreshFor (txn);
        error = r.error;
        return r;
    }
    if (! txn.empty())
        redoStack.clear();
    finish (std::move (txn), false, &undoStack);
    return r;
}

EditResult Session::edit (std::string_view name, const std::function<EditResult (TxnWriter&)>& fn) { return run (name, fn); }

std::optional<Id> Session::runCreate (std::string_view name, Id id, const std::function<EditResult (TxnWriter&)>& fn)
{
    if (run (name, fn))
        return id;
    return std::nullopt;
}

void Session::beginGesture (std::string name)
{
    if (gesture)
        endGesture();
    gesture.emplace();
    gesture->txn.name = std::move (name);
    gesture->writer = std::make_unique<TxnWriter> (doc, gesture->txn);
}

void Session::endGesture()
{
    if (! gesture)
        return;
    Txn txn = std::move (gesture->txn);
    gesture.reset();
    if (! txn.empty())
        redoStack.clear();
    finish (std::move (txn), false, &undoStack);
}

void Session::cancelGesture()
{
    if (! gesture)
        return;
    Txn txn = std::move (gesture->txn);
    gesture.reset();
    doc.rollback();
    refreshFor (txn);
}

UndoResult Session::undo() { return invert (undoStack, redoStack, "Undo"); }

UndoResult Session::redo() { return invert (redoStack, undoStack, "Redo"); }

UndoResult Session::invert (std::vector<Txn>& from, std::vector<Txn>& to, std::string_view label)
{
    UndoResult result;
    if (from.empty() || gesture)
        return result;
    Txn src = std::move (from.back());
    from.pop_back();

    Txn out { src.name, {} };
    TxnWriter w (doc, out);
    std::vector<std::string> skipped;
    auto parentMissing = [&] (const Path& p) {
        for (std::size_t n = 1; n < p.size(); ++n)
            if (! doc.get (Path (p.begin(), p.begin() + static_cast<std::ptrdiff_t> (n))))
                return true;
        return false;
    };

    for (auto it = src.records.rbegin(); it != src.records.rend(); ++it)
    {
        const auto& r = *it;
        if (parentMissing (r.path) || (r.kind != TxnRecord::Kind::set && ! doc.get (r.path)))
        {
            const auto what = describe (r.path);
            if (std::find (skipped.begin(), skipped.end(), what) == skipped.end())
                skipped.push_back (what);
            continue;
        }
        switch (r.kind)
        {
            case TxnRecord::Kind::set:
            {
                const auto current = doc.get (r.path);
                bool holds = false;
                if (! r.after)
                    holds = ! current.has_value();
                else if (r.after->isContainer())
                    holds = current.has_value() && doc.opId (r.path).value_or ("") == resolveOpId (r.afterOpId);
                else
                    holds = current.has_value() && *current == *r.after;
                if (! holds)
                    continue; // someone else changed it since: leave their value (D-COL-6)
                if (r.before)
                {
                    const auto count = out.records.size();
                    w.put (r.path, *r.before);
                    if (out.records.size() > count && r.before->isContainer())
                        out.records.back().restoresOpId = r.beforeOpId;
                }
                else
                    w.remove (r.path);
                break;
            }
            case TxnRecord::Kind::listInsert:
                w.listRemove (r.path, r.elementId);
                break;
            case TxnRecord::Kind::listRemove:
            {
                const auto list = doc.get (r.path);
                const auto& items = list->asList();
                const bool present = std::any_of (items.begin(), items.end(), [&] (const Value& e) { return listElementId (e) == r.elementId; });
                if (! present && r.after)
                    w.listInsert (r.path, r.index, *r.after, r.elementId);
                break;
            }
        }
    }

    if (! skipped.empty())
    {
        std::string objects;
        for (std::size_t i = 0; i < skipped.size(); ++i)
            objects += (i == 0 ? "" : (i + 1 == skipped.size() ? " and " : ", ")) + skipped[i];
        result.message = "Couldn't fully " + std::string (label == "Undo" ? "undo" : "redo") + " \"" + src.name + "\": " + objects
                       + (skipped.size() == 1 ? " was" : " were") + " deleted by someone else.";
    }
    else if (out.empty())
        result.message = "Nothing to " + std::string (label == "Undo" ? "undo" : "redo") + " for \"" + src.name + "\": it was changed by someone else.";

    result.done = ! out.empty();
    finish (std::move (out), true, &to);
    error = result.message;
    return result;
}

std::string Session::resolveOpId (std::string opId) const
{
    for (std::size_t guard = 0; guard <= opIdAliases.size(); ++guard)
    {
        auto it = opIdAliases.find (opId);
        if (it == opIdAliases.end())
            break;
        opId = it->second;
    }
    return opId;
}

bool Session::wouldCycle (const Id& fromTrack, const Id& toTrack, const std::string& ignoreEdgeKey) const
{
    if (fromTrack == toTrack)
        return true;
    std::map<Id, std::vector<Id>> out;
    for (const auto& e : projection.routingEdges())
        if (e.key() != ignoreEdgeKey && ! projection.disabledEdges.contains (e.key()))
            out[e.fromTrackId].push_back (e.toTrackId);
    out[fromTrack].push_back (toTrack);
    // Does toTrack reach fromTrack?
    std::vector<Id> stack { toTrack };
    std::set<Id> seen;
    while (! stack.empty())
    {
        const auto node = stack.back();
        stack.pop_back();
        if (node == fromTrack)
            return true;
        if (! seen.insert (node).second)
            continue;
        for (const auto& next : out[node])
            stack.push_back (next);
    }
    return false;
}

void Session::stamp (TxnWriter& w, const Id& regionId)
{
    const auto base = doc::path ({ model::keys::regions, regionId });
    w.put (base / "modifiedBy", Value (userId));
    w.put (base / "modifiedAt", Value (clock()));
}
} // namespace weft::commands
