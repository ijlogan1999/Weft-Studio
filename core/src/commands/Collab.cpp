// Collaboration commands (spec 05 §7, §9): track locks, comments, chat and version
// history. None of these are undo steps (D-COL-35); restoring a version is.

#include "weft/commands/Session.h"
#include "weft/commands/Collab.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <tuple>
#include <utility>

namespace weft::commands
{
using doc::PathSegment;
namespace keys = model::keys;

namespace
{
EditResult wrote (bool success) { return success ? EditResult {} : EditResult::fail ("The document rejected the edit."); }

Path commentPath (const Id& id) { return doc::path ({ keys::collab, "comments", id }); }
Path chatPath() { return doc::path ({ keys::collab, "chat" }); }
Path versionPath (const Id& id) { return doc::path ({ keys::collab, "versions", id }); }
Path lockPath (const Id& track) { return doc::path ({ keys::tracks, track, "locked" }); }

bool isKey (const Path& p, std::size_t i, std::string_view key) { return p.size() > i && p[i].kind == PathSegment::Kind::key && p[i].key == key; }

std::string hex (std::span<const std::uint8_t> bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve (bytes.size() * 2);
    for (auto b : bytes)
    {
        out.push_back (digits[b >> 4]);
        out.push_back (digits[b & 15]);
    }
    return out;
}

std::optional<doc::Bytes> unhex (const std::vector<std::string>& heads)
{
    doc::Bytes out;
    for (const auto& h : heads)
    {
        if (h.size() != 64)
            return std::nullopt;
        for (std::size_t i = 0; i < h.size(); i += 2)
        {
            auto nibble = [] (char c) -> int {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                return -1;
            };
            const int hi = nibble (h[i]), lo = nibble (h[i + 1]);
            if (hi < 0 || lo < 0)
                return std::nullopt;
            out.push_back (static_cast<std::uint8_t> (hi * 16 + lo));
        }
    }
    return out;
}

/** The user in a commit message "weft1|<user>|<command>". */
UserId authorOf (const std::string& message)
{
    if (! message.starts_with ("weft1|"))
        return {};
    const auto end = message.find ('|', 6);
    return message.substr (6, end == std::string::npos ? std::string::npos : end - 6);
}

/**
    Writes the edits that turn `current` into `target` (D-COL-12: restore is a forward change).
    Maps are compared key by key; lists and scalars are replaced when they differ.
*/
void writeDiff (TxnWriter& w, const Path& at, const Value& current, const Value& target)
{
    if (current.isMap() && target.isMap())
    {
        for (const auto& [key, value] : current.asMap())
            if (! target.asMap().contains (key))
                w.remove (at / key);
        for (const auto& [key, value] : target.asMap())
        {
            auto it = current.asMap().find (key);
            if (it == current.asMap().end())
                w.put (at / key, value);
            else
                writeDiff (w, at / key, it->second, value);
        }
        return;
    }
    if (current != target)
        w.put (at, target);
}
} // namespace

// Writes outside the undo stacks -------------------------------------------------------------

EditResult Session::commitUnrecorded (std::string_view name, const std::function<EditResult (TxnWriter&)>& fn)
{
    error.clear();
    if (gesture)
        return reject ("Finish the current edit first.");
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
    if (! finish (std::move (txn), false, nullptr))
        return EditResult::fail (error);
    return r;
}

// Names and presence ---------------------------------------------------------------------------

void Session::setUserName (const UserId& who, std::string name)
{
    if (! who.empty() && ! name.empty())
        userNames[who] = std::move (name);
}

std::string Session::nameOf (const UserId& who) const
{
    if (who == userId)
        return "you";
    auto it = userNames.find (who);
    return it == userNames.end() ? std::string ("someone") : it->second;
}

void Session::notePresence (const UserId& who, std::int64_t atMs)
{
    auto& seen = lastSeen[who];
    seen = std::max (seen, atMs);
}

std::vector<Notice> Session::takeNotices() { return std::exchange (notices, {}); }

// Locks -----------------------------------------------------------------------------------------

std::optional<model::LockInfo> Session::lockOf (const Id& trackId) const
{
    const auto* t = projection.track (trackId);
    if (t == nullptr || ! t->locked)
        return std::nullopt;
    if (t->locked->by == userId)
        return t->locked;
    auto seen = t->locked->at;
    if (auto it = lastSeen.find (t->locked->by); it != lastSeen.end())
        seen = std::max (seen, it->second);
    if (clock() - seen >= lockExpiryMs)
        return std::nullopt; // 24 h after the holder's last presence (05 §7)
    return t->locked;
}

bool Session::lockedByOther (const Id& trackId) const
{
    const auto lock = lockOf (trackId);
    return lock && lock->by != userId;
}

std::string Session::lockText (const Id& trackId) const
{
    const auto lock = lockOf (trackId);
    if (! lock)
        return {};
    return lock->by == userId ? std::string ("Locked by you") : "Locked by " + nameOf (lock->by);
}

EditResult Session::lockTrack (const Id& trackId, const std::string& note)
{
    const auto* t = projection.track (trackId);
    if (t == nullptr || projection.syntheticTracks.contains (trackId))
        return reject ("That track no longer exists.");
    if (lockedByOther (trackId))
        return reject ("\"" + t->name + "\" is already " + lockText (trackId) + ".");
    if (accessLevel != Access::full)
        return reject (accessReason.empty() ? std::string ("You can't change this project.") : accessReason);
    model::LockInfo lock { userId, clock(), note };
    return commitUnrecorded ("Lock Track", [&] (TxnWriter& w) {
        return wrote (w.put (lockPath (trackId), Value::map().set ("by", lock.by).set ("at", lock.at).set ("note", lock.note)));
    });
}

EditResult Session::unlockTrack (const Id& trackId)
{
    const auto* t = projection.track (trackId);
    if (t == nullptr)
        return reject ("That track no longer exists.");
    if (! t->locked)
        return {};
    if (lockedByOther (trackId))
        return reject ("Only " + nameOf (t->locked->by) + " or the project's Owner can unlock \"" + t->name + "\".");
    return commitUnrecorded ("Unlock Track", [&] (TxnWriter& w) { return wrote (w.put (lockPath (trackId), Value())); });
}

EditResult Session::breakLock (const Id& trackId)
{
    const auto* t = projection.track (trackId);
    if (t == nullptr)
        return reject ("That track no longer exists.");
    if (! t->locked)
        return {};
    if (! projectOwner && t->locked->by != userId)
        return reject ("Only the project's Owner can break a lock.");
    return commitUnrecorded ("Break Lock", [&] (TxnWriter& w) { return wrote (w.put (lockPath (trackId), Value())); });
}

std::set<Id> Session::tracksTouched (const std::vector<Path>& paths, const std::map<Id, Id>& extra) const
{
    std::set<Id> out;
    std::map<Id, Id> sequenceOwner;
    bool sequencesIndexed = false;
    for (const auto& p : paths)
    {
        if (p.size() < 2 || p[0].kind != PathSegment::Kind::key || p[1].kind != PathSegment::Kind::key)
            continue;
        const auto& c = p[0].key;
        const auto& id = p[1].key;
        if (c == keys::tracks)
        {
            if (! isKey (p, 2, "locked"))
                out.insert (id);
        }
        else if (c == keys::regions)
        {
            if (const auto* r = projection.region (id))
                out.insert (r->trackId);
            if (auto v = doc.get (doc::path ({ keys::regions, id, "trackId" })); v && v->isString())
                out.insert (v->asString());
            if (auto it = extra.find (id); it != extra.end())
                out.insert (it->second);
        }
        else if (c == keys::midiSequences)
        {
            if (! sequencesIndexed)
            {
                for (const auto& [rid, r] : projection.regions)
                    if (r->isMidi)
                        sequenceOwner[r->midi.sequenceId] = r->trackId;
                sequencesIndexed = true;
            }
            if (auto it = sequenceOwner.find (id); it != sequenceOwner.end())
                out.insert (it->second);
        }
        else if (c == keys::takeFolders)
        {
            if (auto it = projection.takeFolders.find (id); it != projection.takeFolders.end())
                out.insert (it->second->trackId);
        }
        else if (c == keys::automationLanes)
        {
            if (auto it = projection.storedLanes.find (id); it != projection.storedLanes.end())
                out.insert (it->second->trackId);
            if (auto v = doc.get (doc::path ({ keys::automationLanes, id, "trackId" })); v && v->isString())
                out.insert (v->asString());
        }
    }
    return out;
}

std::map<Id, model::LockInfo> Session::myLocks() const
{
    std::map<Id, model::LockInfo> out;
    for (const auto& [id, t] : projection.tracks)
        if (t->locked && t->locked->by == userId)
            out.emplace (id, *t->locked);
    return out;
}

std::map<Id, Id> Session::regionTracksOn (const std::map<Id, model::LockInfo>& locks) const
{
    std::map<Id, Id> out;
    if (locks.empty())
        return out;
    for (const auto& [id, r] : projection.regions)
        if (locks.contains (r->trackId))
            out.emplace (id, r->trackId);
    return out;
}

void Session::afterRemote (const doc::Bytes& headsBefore, const std::map<Id, model::LockInfo>& locksBefore, const std::map<Id, Id>& regionTracksBefore)
{
    if (locksBefore.empty() && myLocks().empty())
        return;
    std::vector<UserId> authors;
    for (const auto& change : doc.changesSince (headsBefore))
        if (auto who = authorOf (change.message); ! who.empty() && who != userId && std::find (authors.begin(), authors.end(), who) == authors.end())
            authors.push_back (who);
    if (authors.empty())
        return;
    auto trackName = [&] (const Id& id) {
        const auto* t = projection.track (id);
        return t == nullptr ? std::string ("a track") : "\"" + t->name + "\"";
    };

    // Our lock was removed or replaced by someone else (the Owner broke it, or it had expired).
    for (const auto& [trackId, lock] : locksBefore)
    {
        const auto* t = projection.track (trackId);
        if (t != nullptr && t->locked && t->locked->by == userId)
            continue;
        UserId by = t != nullptr && t->locked ? t->locked->by : authors.front();
        notices.push_back ({ Notice::Kind::lockBroken, by, trackId, nameOf (by) + " broke your lock on " + trackName (trackId) + "." });
    }

    // Edits that reached us on tracks we hold: made before their author saw the lock (05 §7).
    const auto mine = myLocks();
    if (mine.empty())
        return;
    const auto touched = tracksTouched (doc.changedPaths (headsBefore, 3), regionTracksBefore);
    for (const auto& [trackId, lock] : mine)
        if (touched.contains (trackId))
            for (const auto& who : authors)
                notices.push_back ({ Notice::Kind::offlineEditsMerged, who, trackId,
                                     nameOf (who) + "'s offline edits to " + trackName (trackId) + " were merged." });
}

// Comments ----------------------------------------------------------------------------------------

std::optional<Id> Session::addComment (const CommentAnchor& anchor, const std::string& body)
{
    error.clear();
    if (body.empty())
        return rejectId ("A comment needs some text.");
    if (body.size() > model::maxMessageLength)
        return rejectId ("Comments can be at most 4,000 characters.");
    if (anchor.pos < 0)
        return rejectId ("Comments can't be placed before the project start.");
    model::Comment c;
    c.id = newId();
    c.pos = anchor.pos;
    c.endPos = anchor.endPos;
    c.trackId = anchor.trackId;
    c.regionId = anchor.regionId;
    c.author = userId;
    c.createdAt = clock();
    c.body = body;
    if (commitUnrecorded ("Add Comment", [&] (TxnWriter& w) { return wrote (w.put (commentPath (c.id), c.toValue())); }))
        return c.id;
    return std::nullopt;
}

std::optional<Id> Session::replyToComment (const Id& commentId, const std::string& body)
{
    error.clear();
    const auto* parent = projection.comment (commentId);
    if (parent == nullptr)
        return rejectId ("That comment no longer exists.");
    if (! parent->parentId.empty())
        if (const auto* root = projection.comment (parent->parentId))
            parent = root;
    if (body.empty())
        return rejectId ("A reply needs some text.");
    if (body.size() > model::maxMessageLength)
        return rejectId ("Comments can be at most 4,000 characters.");
    model::Comment c;
    c.id = newId();
    c.pos = parent->pos;
    c.endPos = parent->endPos;
    c.trackId = parent->trackId;
    c.regionId = parent->regionId;
    c.parentId = parent->id;
    c.author = userId;
    c.createdAt = clock();
    c.body = body;
    if (commitUnrecorded ("Reply", [&] (TxnWriter& w) { return wrote (w.put (commentPath (c.id), c.toValue())); }))
        return c.id;
    return std::nullopt;
}

EditResult Session::editComment (const Id& commentId, const std::string& body)
{
    const auto* c = projection.comment (commentId);
    if (c == nullptr || c->deleted)
        return reject ("That comment no longer exists.");
    if (c->author != userId)
        return reject ("Only " + nameOf (c->author) + " can edit this comment.");
    if (body.empty())
        return reject ("A comment needs some text.");
    if (body.size() > model::maxMessageLength)
        return reject ("Comments can be at most 4,000 characters.");
    const auto now = clock();
    return commitUnrecorded ("Edit Comment", [&] (TxnWriter& w) {
        return wrote (w.put (commentPath (commentId) / "body", Value (body)) && w.put (commentPath (commentId) / "editedAt", Value (now)));
    });
}

EditResult Session::deleteComment (const Id& commentId)
{
    const auto* c = projection.comment (commentId);
    if (c == nullptr || c->deleted)
        return reject ("That comment no longer exists.");
    if (c->author != userId)
        return reject ("Only " + nameOf (c->author) + " can delete this comment.");
    return commitUnrecorded ("Delete Comment", [&] (TxnWriter& w) {
        return wrote (w.put (commentPath (commentId) / "deleted", Value (true)) && w.put (commentPath (commentId) / "body", Value (std::string())));
    });
}

EditResult Session::setCommentResolved (const Id& commentId, bool resolved)
{
    const auto* c = projection.comment (commentId);
    if (c == nullptr)
        return reject ("That comment no longer exists.");
    if (! c->parentId.empty())
        if (const auto* root = projection.comment (c->parentId))
            c = root; // resolving a reply resolves its thread
    const auto id = c->id;
    return commitUnrecorded (resolved ? "Resolve Comment" : "Reopen Comment", [&] (TxnWriter& w) {
        return wrote (w.put (commentPath (id) / "resolved", Value (resolved)) && w.put (commentPath (id) / "resolvedBy", resolved ? Value (userId) : Value()));
    });
}

// Chat ---------------------------------------------------------------------------------------------

std::optional<Id> Session::sendChat (const std::string& body)
{
    error.clear();
    if (body.empty())
        return std::nullopt;
    if (body.size() > model::maxMessageLength)
        return rejectId ("Messages can be at most 4,000 characters.");
    model::ChatMessage m { newId(), userId, clock(), body, false };
    const auto size = doc.get (chatPath()).value_or (Value::list()).asList().size();
    if (commitUnrecorded ("Chat", [&] (TxnWriter& w) { return wrote (w.listInsert (chatPath(), size, m.toValue(), m.id)); }))
        return m.id;
    return std::nullopt;
}

EditResult Session::deleteChat (const Id& messageId)
{
    const auto list = doc.get (chatPath()).value_or (Value::list());
    const auto& items = list.asList();
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        if (listElementId (items[i]) != messageId)
            continue;
        const auto m = model::ChatMessage::fromValue (items[i]);
        if (m.deleted)
            return {};
        if (m.author != userId)
            return reject ("Only " + nameOf (m.author) + " can delete this message.");
        const auto at = chatPath() / PathSegment::id (messageId);
        return commitUnrecorded ("Delete Message", [&] (TxnWriter& w) { return wrote (w.put (at / "deleted", Value (true)) && w.put (at / "body", Value (std::string()))); });
    }
    return reject ("That message no longer exists.");
}

// Version history -----------------------------------------------------------------------------------

std::optional<Id> Session::saveVersion (const std::string& name, bool autoVersion)
{
    error.clear();
    if (accessLevel != Access::full)
        return rejectId (accessReason.empty() ? std::string ("You can't change this project.") : accessReason);
    model::VersionTag t;
    t.id = newId();
    t.name = name.empty() ? std::string ("Untitled version") : name;
    const auto heads = doc.heads();
    for (std::size_t i = 0; i + 32 <= heads.size(); i += 32)
        t.heads.push_back (hex (std::span (heads).subspan (i, 32)));
    t.createdBy = userId;
    t.createdAt = clock();
    t.autoVersion = autoVersion;
    if (commitUnrecorded (autoVersion ? "Auto Version" : "Save Version", [&] (TxnWriter& w) { return wrote (w.put (versionPath (t.id), t.toValue())); }))
    {
        editsAtLastVersion = localEdits;
        return t.id;
    }
    return std::nullopt;
}

std::optional<doc::Document> Session::versionDocument (const Id& versionId) const
{
    const auto* v = projection.version (versionId);
    if (v == nullptr)
        return std::nullopt;
    const auto heads = unhex (v->heads);
    if (! heads)
        return std::nullopt;
    return doc.forkAt (*heads);
}

std::optional<ProjectModel> Session::versionModel (const Id& versionId) const
{
    auto old = versionDocument (versionId);
    if (! old)
        return std::nullopt;
    return model::Projector::project (*old);
}

EditResult Session::restoreVersion (const Id& versionId)
{
    const auto* v = projection.version (versionId);
    if (v == nullptr)
        return reject ("That version no longer exists.");
    if (accessLevel != Access::full)
        return reject ("Only Editors and the Owner can restore a version.");
    const auto name = v->name;
    auto old = versionDocument (versionId);
    if (! old)
        return reject ("This version isn't available on this computer yet.");

    // Everything but the collaboration metadata, and the locks in force now (D-COL-36).
    auto target = old->root();
    auto current = doc.root();
    if (target[keys::tracks].isMap())
        for (auto& [id, t] : target.mapRef()[keys::tracks].mapRef())
            if (t.isMap())
                t.set ("locked", current[keys::tracks][id]["locked"]);
    auto r = run ("Restore '" + name + "'", [&] (TxnWriter& w) {
        for (const auto& [key, value] : current.asMap())
            if (key != keys::collab && ! target.asMap().contains (key))
                w.remove (doc::path ({ key }));
        for (const auto& [key, value] : target.asMap())
        {
            if (key == keys::collab)
                continue;
            auto it = current.asMap().find (key);
            if (it == current.asMap().end())
                w.put (doc::path ({ key }), value);
            else
                writeDiff (w, doc::path ({ key }), it->second, value);
        }
        return EditResult {};
    });
    if (! r)
        return r;
    saveVersion ("Restored '" + name + "'", false);
    return r;
}

std::optional<Id> Session::autoVersion()
{
    if (localEdits == editsAtLastVersion)
        return std::nullopt;
    const auto* who = userNames.contains (userId) ? &userNames.at (userId) : nullptr;
    return saveVersion (who != nullptr ? *who + "'s session" : std::string ("Session"), true);
}
} // namespace weft::commands

// Read-side helpers (Collab.h) -------------------------------------------------------------------

namespace weft::commands
{
namespace
{
bool before (const model::Comment* a, const model::Comment* b) { return std::tie (a->pos, a->createdAt, a->id) < std::tie (b->pos, b->createdAt, b->id); }

std::string lower (std::string s)
{
    for (auto& c : s)
        c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
    return s;
}

bool wordChar (char c) { return std::isalnum (static_cast<unsigned char> (c)) != 0 || c == '_'; }
} // namespace

std::vector<const model::Comment*> commentReplies (const model::ProjectModel& model, const Id& threadId)
{
    std::vector<const model::Comment*> out;
    for (const auto& [id, c] : model.comments)
        if (c->parentId == threadId)
            out.push_back (c.get());
    std::sort (out.begin(), out.end(), [] (auto* a, auto* b) { return std::tie (a->createdAt, a->id) < std::tie (b->createdAt, b->id); });
    return out;
}

std::vector<const model::Comment*> commentThreads (const model::ProjectModel& model, CommentFilter filter, const UserId& me)
{
    std::vector<const model::Comment*> out;
    for (const auto& [id, c] : model.comments)
    {
        if (! c->parentId.empty() && model.comment (c->parentId) != nullptr)
            continue; // a reply (a reply whose thread vanished shows as its own thread)
        auto replies = commentReplies (model, id);
        const bool liveReply = std::any_of (replies.begin(), replies.end(), [] (auto* r) { return ! r->deleted; });
        if (c->deleted && ! liveReply)
            continue;
        replies.insert (replies.begin(), c.get());
        bool keep = true;
        switch (filter)
        {
            case CommentFilter::all: break;
            case CommentFilter::open: keep = ! c->resolved; break;
            case CommentFilter::resolved: keep = c->resolved; break;
            case CommentFilter::mine: keep = std::any_of (replies.begin(), replies.end(), [&] (auto* r) { return r->author == me && ! r->deleted; }); break;
            case CommentFilter::mentions:
                keep = std::any_of (replies.begin(), replies.end(), [&] (auto* r) {
                    const auto m = r->mentions();
                    return ! r->deleted && std::find (m.begin(), m.end(), me) != m.end();
                });
                break;
        }
        if (keep)
            out.push_back (c.get());
    }
    std::sort (out.begin(), out.end(), before);
    return out;
}

Id commentTrack (const model::ProjectModel& model, const model::Comment& comment)
{
    if (! comment.regionId.empty())
        if (const auto* r = model.region (comment.regionId))
            return model.effectiveTrackId (*r);
    if (! comment.trackId.empty() && model.tracks.contains (comment.trackId))
        return comment.trackId;
    return {};
}

std::vector<const model::Comment*> newMentions (const model::ProjectModel& model, const UserId& me, std::int64_t sinceMs)
{
    std::vector<const model::Comment*> out;
    for (const auto& [id, c] : model.comments)
    {
        if (c->deleted || c->author == me || std::max (c->createdAt, c->editedAt.value_or (0)) <= sinceMs)
            continue;
        const auto m = c->mentions();
        if (std::find (m.begin(), m.end(), me) != m.end())
            out.push_back (c.get());
    }
    std::sort (out.begin(), out.end(), [] (auto* a, auto* b) { return std::tie (a->createdAt, a->id) < std::tie (b->createdAt, b->id); });
    return out;
}

bool hasUnreadComments (const model::ProjectModel& model, const UserId& me, std::int64_t sinceMs)
{
    return std::any_of (model.comments.begin(), model.comments.end(), [&] (const auto& c) {
        return ! c.second->deleted && c.second->author != me && std::max (c.second->createdAt, c.second->editedAt.value_or (0)) > sinceMs;
    });
}

std::string mentionIds (const std::string& typed, const std::map<UserId, std::string>& names)
{
    // Longest names first, so "@Ana Lee" wins over "@Ana".
    std::vector<std::pair<std::string, UserId>> byName;
    for (const auto& [id, name] : names)
        if (! name.empty())
            byName.emplace_back (lower (name), id);
    std::sort (byName.begin(), byName.end(), [] (const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    const auto low = lower (typed);
    std::string out;
    for (std::size_t i = 0; i < typed.size();)
    {
        if (typed[i] == '@')
        {
            bool matched = false;
            for (const auto& [name, id] : byName)
                if (low.compare (i + 1, name.size(), name) == 0 && (i + 1 + name.size() >= typed.size() || ! wordChar (typed[i + 1 + name.size()])))
                {
                    out += "@" + id;
                    i += 1 + name.size();
                    matched = true;
                    break;
                }
            if (matched)
                continue;
        }
        out.push_back (typed[i++]);
    }
    return out;
}

std::string mentionNames (const std::string& stored, const std::map<UserId, std::string>& names)
{
    std::string out;
    for (std::size_t i = 0; i < stored.size();)
    {
        if (stored[i] == '@')
        {
            std::size_t end = i + 1;
            while (end < stored.size() && (wordChar (stored[end]) || stored[end] == '-'))
                ++end;
            if (auto it = names.find (stored.substr (i + 1, end - i - 1)); it != names.end())
            {
                out += "@" + it->second;
                i = end;
                continue;
            }
        }
        out.push_back (stored[i++]);
    }
    return out;
}

std::vector<TimeLink> timeLinks (const std::string& text, const model::ProjectModel& model)
{
    std::vector<TimeLink> out;
    const auto low = lower (text);
    auto number = [&] (std::size_t& i, long& value) {
        const auto start = i;
        value = 0;
        while (i < low.size() && std::isdigit (static_cast<unsigned char> (low[i])) != 0 && i - start < 6)
            value = value * 10 + (low[i++] - '0');
        return i > start;
    };
    auto skipSpaces = [&] (std::size_t& i) {
        const auto start = i;
        while (i < low.size() && low[i] == ' ')
            ++i;
        return i > start;
    };
    for (std::size_t i = 0; i < low.size();)
    {
        const bool boundary = i == 0 || ! wordChar (low[i - 1]);
        // bar N [beat M]
        if (boundary && low.compare (i, 3, "bar") == 0)
        {
            std::size_t j = i + 3;
            long bar = 0;
            if (skipSpaces (j) && number (j, bar) && bar >= 1 && (j >= low.size() || ! wordChar (low[j])))
            {
                auto pos = model.signatures.barStart (static_cast<int> (bar - 1));
                std::size_t end = j;
                std::size_t k = j;
                long beat = 0;
                if (skipSpaces (k) && low.compare (k, 4, "beat") == 0)
                {
                    k += 4;
                    const auto& sig = model.signatures.signatureAtBar (static_cast<int> (bar - 1));
                    if (skipSpaces (k) && number (k, beat) && beat >= 1 && beat <= sig.numerator && (k >= low.size() || ! wordChar (low[k])))
                    {
                        pos += (beat - 1) * 4 * ticksPerQuarter / sig.denominator;
                        end = k;
                    }
                }
                out.push_back ({ i, end - i, pos });
                i = end;
                continue;
            }
        }
        // m:ss
        if (boundary && std::isdigit (static_cast<unsigned char> (low[i])) != 0)
        {
            std::size_t j = i;
            long minutes = 0, seconds = 0;
            if (number (j, minutes) && j + 2 < low.size() + 0 && low[j] == ':' && j + 3 <= low.size())
            {
                std::size_t k = j + 1;
                if (k + 2 <= low.size() && std::isdigit (static_cast<unsigned char> (low[k])) != 0 && std::isdigit (static_cast<unsigned char> (low[k + 1])) != 0
                    && (k + 2 >= low.size() || (! wordChar (low[k + 2]) && low[k + 2] != ':')))
                {
                    seconds = (low[k] - '0') * 10 + (low[k + 1] - '0');
                    if (seconds < 60)
                    {
                        const double samples = static_cast<double> (minutes * 60 + seconds) * model.tempo.sampleRate();
                        out.push_back ({ i, k + 2 - i, static_cast<Ticks> (std::llround (model.tempo.sampleToTick (samples))) });
                        i = k + 2;
                        continue;
                    }
                }
            }
            while (j < low.size() && wordChar (low[j]))
                ++j;
            i = std::max (j, i + 1);
            continue;
        }
        ++i;
    }
    return out;
}

std::size_t unreadChat (const model::ProjectModel& model, const UserId& me, const Id& lastReadId)
{
    std::size_t start = 0;
    if (! lastReadId.empty())
        for (std::size_t i = 0; i < model.chat.size(); ++i)
            if (model.chat[i].id == lastReadId)
                start = i + 1;
    std::size_t n = 0;
    for (std::size_t i = start; i < model.chat.size(); ++i)
        if (model.chat[i].author != me && ! model.chat[i].deleted)
            ++n;
    return n;
}
} // namespace weft::commands
