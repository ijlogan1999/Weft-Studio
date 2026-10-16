#include "weft/collab/SyncClient.h"

#include "weft/core/Version.h"
#include "weft/model/Model.h"

#include <weft_sync.h>

#include <algorithm>
#include <fstream>

namespace weft::collab
{
namespace
{
constexpr std::uint8_t frameSync = 4;
constexpr std::uint8_t framePresence = 7;
/** 05 §5 update rates. */
constexpr std::int64_t presenceMinGapMs = 50;   // 20 Hz
constexpr std::int64_t presenceDebounceMs = 100;
constexpr std::int64_t playheadEveryMs = 2000;
constexpr std::int64_t idleAfterMs = 2 * 60 * 1000;
constexpr std::int64_t presenceGoneMs = 45 * 1000;
/** Local changes go out at most every 50 ms (05 §3.3). */
constexpr auto batchInterval = std::chrono::milliseconds (50);
/** A region counts as "mine" for conflict messages while I edited it this recently. */
constexpr std::int64_t recentEditMs = 10 * 60 * 1000;

std::string hex (const doc::Bytes& b)
{
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (const auto c : b)
    {
        s += digits[c >> 4];
        s += digits[c & 15];
    }
    return s;
}

std::optional<doc::Bytes> unhex (const std::string& s)
{
    if (s.size() % 2 != 0)
        return std::nullopt;
    doc::Bytes out;
    for (std::size_t i = 0; i < s.size(); i += 2)
    {
        auto nibble = [] (char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            return -1;
        };
        const auto hi = nibble (s[i]), lo = nibble (s[i + 1]);
        if (hi < 0 || lo < 0)
            return std::nullopt;
        out.push_back (static_cast<std::uint8_t> (hi * 16 + lo));
    }
    return out;
}

/** The user in a commit message "weft1|<user>|<command>". */
std::string authorOf (const std::string& message)
{
    if (! message.starts_with ("weft1|"))
        return {};
    const auto end = message.find ('|', 6);
    return message.substr (6, end == std::string::npos ? std::string::npos : end - 6);
}

commands::Session::Access accessFor (const std::string& role)
{
    if (role == "owner" || role == "editor")
        return commands::Session::Access::full;
    if (role == "viewer")
        return commands::Session::Access::commentsOnly;
    return commands::Session::Access::none;
}
} // namespace

SyncClient::SyncClient (commands::Session& s, SyncOptions o) : session (s), options (std::move (o)), wantOnline (options.startOnline)
{
    if (! options.clock)
        options.clock = commands::systemClockMs;
    lastInput = options.clock();
    loadPending();
    listener = session.addLocalChangeListener ([this] (const doc::Bytes& change) {
        if (auto h = doc::changeHash (change))
        {
            pending.insert (*h);
            savePending();
        }
        outbox.push_back (change);
        updateState();
    });

    const auto config = doc::Value::map()
                            .set ("relayUrl", options.account.relayUrl)
                            .set ("projectId", options.projectId)
                            .set ("clientId", options.clientId)
                            .set ("clientVersion", std::string (versionString()))
                            .set ("schemaVersion", model::schemaVersion)
                            .set ("accessToken", options.account.accessToken)
                            .set ("accessExpiresAt", options.account.accessExpiresAt)
                            .set ("refreshToken", options.account.refreshToken)
                            .set ("oneWayDelayMs", options.oneWayDelayMs)
                            .set ("online", options.startOnline);
    const auto bytes = doc::encode (config);
    conn = weft_conn_start (bytes.data(), bytes.size());
    syncState = options.startOnline ? SyncState::connecting : SyncState::offline;
}

SyncClient::~SyncClient()
{
    session.removeLocalChangeListener (listener);
    weft_conn_free (conn);
}

void SyncClient::setOnline (bool online)
{
    if (wantOnline == online)
        return;
    wantOnline = online;
    welcomed = false;
    outbox.clear(); // the sync exchange on reconnect sends whatever the relay lacks
    if (conn != nullptr)
        weft_conn_set_online (conn, online ? 1 : 0);
    setState (online ? SyncState::reconnecting : SyncState::offline);
}

std::string SyncClient::displayName (const std::string& userId) const
{
    for (const auto& m : memberList)
        if (m.userId == userId)
            return m.displayName;
    return userId == options.account.userId && ! options.account.displayName.empty() ? options.account.displayName : "someone";
}

std::string SyncClient::statusText() const
{
    const auto n = pending.size();
    const auto pendingText = std::to_string (n) + (n == 1 ? " change pending" : " changes pending");
    switch (syncState)
    {
        case SyncState::synced: return "Synced";
        case SyncState::syncing: return "Syncing\xe2\x80\xa6";
        case SyncState::connecting: return "Connecting\xe2\x80\xa6";
        case SyncState::reconnecting: return n > 0 ? "Reconnecting\xe2\x80\xa6 \xe2\x80\x93 " + pendingText : "Reconnecting\xe2\x80\xa6";
        case SyncState::offline: return "Offline \xe2\x80\x93 " + pendingText;
        case SyncState::updateRequired: return "Update required";
        case SyncState::signedOut: return "Signed out \xe2\x80\x93 sign in to sync";
        case SyncState::removed: return "Removed from this project";
        case SyncState::notFound: return "Not on the relay";
    }
    return {};
}

void SyncClient::setState (SyncState s)
{
    if (s == syncState)
        return;
    syncState = s;
    if (s == SyncState::synced)
        lastSynced = commands::systemClockMs();
    if (onStatusChange)
        onStatusChange();
}

void SyncClient::updateState()
{
    if (! welcomed)
        return;
    // Synced: the relay acknowledged all our changes, has everything we had when we
    // joined, and we have every change it last told us it has.
    bool haveRelayHeads = true;
    for (std::size_t i = 0; i + 32 <= serverHeads.size(); i += 32)
        haveRelayHeads = haveRelayHeads && session.document().hasChange (std::span (serverHeads).subspan (i, 32));
    if (pending.empty() && haveRelayHeads && caughtUp)
        setState (SyncState::synced);
    else
        setState (SyncState::syncing);
}

void SyncClient::setRole (const std::string& role)
{
    myRole = role;
    session.setAccess (accessFor (role), role == "viewer" ? "You're a Viewer in this project: you can comment and chat." : "");
}

void SyncClient::sendSync()
{
    if (conn == nullptr || ! welcomed)
        return;
    if (auto m = session.document().generateSyncMessage (sync))
        weft_conn_send (conn, frameSync, m->data(), m->size());
}

void SyncClient::flushOutbox (bool force)
{
    if (outbox.empty() || conn == nullptr)
        return;
    if (! welcomed)
    {
        // Not connected: the sync exchange after reconnecting sends these.
        outbox.clear();
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (! force && now - lastFlush < batchInterval)
        return;
    lastFlush = now;
    outbox.clear();
    // Local changes go out through the sync exchange rather than as bare CHANGE frames:
    // it sends whatever the relay lacks, dependencies first, so a change never reaches
    // the relay before one it depends on (D-COL-21).
    sendSync();
    const auto heads = session.document().heads();
    weft_conn_set_heads (conn, heads.data(), heads.size());
}

void SyncClient::poll()
{
    if (conn == nullptr)
        return;
    // Nothing touches the document while a gesture is open: Automerge closes an open
    // transaction when asked for its heads or changes, which would split the gesture
    // into changes nobody sends (D-ARCH-6). Events wait until it ends.
    if (session.inGesture())
    {
        WeftBytes held {};
        while (weft_conn_poll (conn, &held) == 1)
        {
            if (auto event = doc::decode ({ held.data, held.len }))
                deferred.push_back (std::move (*event));
            weft_bytes_free (held);
            held = {};
        }
        return;
    }
    if (! deferred.empty())
    {
        auto held = std::move (deferred);
        deferred.clear();
        for (const auto& e : held)
            handle (e);
    }
    WeftBytes out {};
    while (weft_conn_poll (conn, &out) == 1)
    {
        const auto event = doc::decode ({ out.data, out.len });
        weft_bytes_free (out);
        out = {};
        if (event)
            handle (*event);
    }
    flushOutbox (false);
    flushPresence (false);
    // Offline for 45 s: collaborators' presence is cleared (05 §5).
    if (! welcomed && disconnectedAt != 0 && ! others.empty() && options.clock() - disconnectedAt >= presenceGoneMs)
    {
        others.clear();
        if (onPresenceChange)
            onPresenceChange();
    }
}

void SyncClient::handle (const doc::Value& e)
{
    const auto kind = e["kind"].asString();
    if (kind == "connecting")
    {
        welcomed = false;
        setState (everWelcomed ? SyncState::reconnecting : SyncState::connecting);
    }
    else if (kind == "welcome")
    {
        welcomed = everWelcomed = true;
        sync = doc::SyncState(); // a fresh exchange per connection
        serverHeads = e["heads"].asBytes();
        caughtUp = serverHeads == session.document().heads();
        memberList.clear();
        for (const auto& m : e["members"].asList())
            memberList.push_back ({ m["userId"].asString(), m["displayName"].asString(), m["role"].asString(), static_cast<int> (m["color"].asInt()) });
        setRole (e["role"].asString());
        outbox.clear();
        sendSync();
        updateState();
        disconnectedAt = 0;
        others.clear();
        for (const auto& p : e["presence"].asList())
            addCollaborator (p["clientId"].asString(), p["userId"].asString(), &p["state"].asBytes());
        // Everyone else learns where we are at once.
        presenceEverSent = false;
        flushPresence (true);
        if (onPresenceChange)
            onPresenceChange();
        if (onJoined)
            onJoined();
    }
    else if (kind == "presence")
    {
        addCollaborator (e["clientId"].asString(), e["userId"].asString(), &e["state"].asBytes());
        if (onPresenceChange)
            onPresenceChange();
    }
    else if (kind == "blob")
    {
        if (onBlobAvailable)
            onBlobAvailable (e["hash"].asString(), e["size"].asInt());
    }
    else if (kind == "sync" || kind == "change")
        applyRemote (e);
    else if (kind == "ack")
    {
        const auto& hashes = e["hashes"].asBytes();
        for (std::size_t i = 0; i + 32 <= hashes.size(); i += 32)
            pending.erase (doc::Bytes (hashes.begin() + static_cast<std::ptrdiff_t> (i), hashes.begin() + static_cast<std::ptrdiff_t> (i + 32)));
        serverHeads = e["heads"].asBytes();
        // The relay's heads equal ours: it has every change we have.
        if (serverHeads == session.document().heads())
        {
            pending.clear();
            caughtUp = true;
        }
        savePending();
        updateState();
    }
    else if (kind == "reject")
    {
        welcomed = false;
        const auto code = e["code"].asString();
        if (code == "auth")
            setState (SyncState::signedOut);
        else if (code == "update_required")
            setState (SyncState::updateRequired);
        else if (code == "forbidden")
        {
            setRole ({});
            setState (SyncState::removed);
        }
        else if (code == "not_found")
            setState (SyncState::notFound);
        if (onMessage && ! e["message"].asString().empty())
            onMessage (e["message"].asString());
    }
    else if (kind == "notice")
    {
        const auto notice = e["notice"].asString();
        const auto user = e["userId"].asString();
        const auto& member = e["member"];
        if (notice == "removed")
        {
            welcomed = false;
            setRole ({});
            session.setAccess (commands::Session::Access::none, "You were removed from this project, so it's read-only.");
            setState (SyncState::removed);
            if (onMessage)
                onMessage ("You were removed from this project.");
        }
        else if (notice == "role_changed")
        {
            for (auto& m : memberList)
                if (m.userId == user)
                    m.role = e["role"].asString();
            if (user == options.account.userId)
            {
                setRole (e["role"].asString());
                if (onMessage)
                    onMessage ("You're now " + std::string (e["role"].asString() == "viewer" ? "a Viewer" : "an Editor") + " in this project.");
            }
        }
        else if (notice == "member_added" && member.isMap())
        {
            memberList.push_back ({ member["userId"].asString(), member["displayName"].asString(), member["role"].asString(),
                                    static_cast<int> (member["color"].asInt()) });
            if (onMessage)
                onMessage (member["displayName"].asString() + " joined the project.");
        }
        else if (notice == "member_removed")
        {
            std::erase_if (memberList, [&] (const Collaborator& m) { return m.userId == user; });
            std::erase_if (others, [&] (const auto& kv) { return kv.second.userId == user; });
            if (onPresenceChange)
                onPresenceChange();
        }
        else if (notice == "client_joined")
        {
            const auto client = e["clientId"].asString();
            if (client != options.clientId)
                addCollaborator (client, user, nullptr);
            if (onPresenceChange)
                onPresenceChange();
        }
        else if (notice == "client_left")
        {
            others.erase (e["clientId"].asString());
            if (onPresenceChange)
                onPresenceChange();
        }
        else if (notice == "forbidden")
        {
            if (onMessage)
                onMessage ("The relay refused a change: " + e["message"].asString (""));
        }
        else if (notice == "project_deleted")
        {
            welcomed = false;
            session.setAccess (commands::Session::Access::none, "The project was deleted from the relay.");
            setState (SyncState::notFound);
            if (onMessage)
                onMessage ("The Owner deleted this project from the relay.");
        }
        if (onStatusChange)
            onStatusChange();
    }
    else if (kind == "disconnected")
    {
        const bool wasWelcomed = welcomed;
        welcomed = false;
        if (disconnectedAt == 0)
            disconnectedAt = options.clock();
        if (! wantOnline)
            setState (SyncState::offline);
        else if (syncState == SyncState::removed || syncState == SyncState::signedOut || syncState == SyncState::updateRequired
                 || syncState == SyncState::notFound)
        {
        }
        else
            setState (wasWelcomed || everWelcomed ? SyncState::reconnecting : SyncState::connecting);
    }
    else if (kind == "tokens")
    {
        options.account.accessToken = e["accessToken"].asString();
        options.account.accessExpiresAt = e["accessExpiresAt"].asInt();
        options.account.refreshToken = e["refreshToken"].asString (options.account.refreshToken);
        if (onAccountChange)
            onAccountChange (options.account);
    }
}

std::map<Id, SyncClient::RegionMark> SyncClient::regionsTouchedByMe() const
{
    std::map<Id, RegionMark> out;
    const auto now = commands::systemClockMs();
    const auto& m = session.model();
    for (const auto& [id, r] : m.regions)
        if (r->modifiedBy == session.user() && now - r->modifiedAt < recentEditMs)
            out[id] = { r->start, m.effectiveTrackId (*r), r->name };
    return out;
}

void SyncClient::noticeConflicts (const std::map<Id, RegionMark>& mine, const doc::Bytes& headsBefore)
{
    if (! onMessage || mine.empty())
        return;
    const auto& m = session.model();
    std::string remoteAuthor;
    for (const auto& c : session.document().changesSince (headsBefore))
        if (const auto a = authorOf (c.message); ! a.empty() && a != session.user())
            remoteAuthor = a;
    for (const auto& [id, before] : mine)
    {
        const auto label = before.name.empty() ? std::string ("A region") : "Region \"" + before.name + "\"";
        const auto* r = m.region (id);
        if (r == nullptr)
        {
            onMessage (label + " was deleted by " + displayName (remoteAuthor));
            continue;
        }
        // Fields merge one by one, so modifiedBy can still name me when my move lost.
        if (r->start != before.start || m.effectiveTrackId (*r) != before.trackId)
            onMessage (label + " moved by " + displayName (r->modifiedBy != session.user() ? r->modifiedBy : remoteAuthor));
    }
}

void SyncClient::applyRemote (const doc::Value& e)
{
    const auto headsBefore = session.document().heads();
    const auto mine = regionsTouchedByMe();
    const auto& data = e["data"].asBytes();
    doc::Status status;
    if (e["kind"].asString() == "sync")
        status = session.receiveSyncMessage (sync, data);
    else
        status = session.applyChanges (data);
    if (status != doc::Status::ok)
    {
        // A corrupt change (05 §13): drop the connection and resync from scratch.
        if (onMessage)
            onMessage ("A change from the relay couldn't be read; resyncing.");
        weft_conn_set_online (conn, 0);
        weft_conn_set_online (conn, wantOnline ? 1 : 0);
        return;
    }
    const bool changed = session.document().heads() != headsBefore;
    if (changed)
    {
        // Regions orphaned by a collaborator deleting their track land on one Recovered
        // track; every client writes the same id, so there's one track (D-ARCH-11).
        if (! session.model().syntheticTracks.empty() && session.access() == commands::Session::Access::full)
            session.materializeRecoveredTracks();
        noticeConflicts (mine, headsBefore);
        // After time offline the sync exchange brings in what collaborators did meanwhile (05 §13).
        if (e["kind"].asString() == "sync" && onMessage)
        {
            int merged = 0;
            for (const auto& c : session.document().changesSince (headsBefore))
                if (authorOf (c.message) != session.user())
                    ++merged;
            if (merged > 1)
                onMessage ("Merged " + std::to_string (merged) + " changes from collaborators");
        }
        if (onRemoteChange)
            onRemoteChange();
    }
    // A CHANGE that didn't move our heads was a duplicate, or is waiting for changes we
    // missed (while offline, say): resume the sync exchange so the relay sends them.
    if (e["kind"].asString() == "change" && ! changed)
    {
        // Our side of the exchange may consider itself done; a fresh state always asks.
        sync = doc::SyncState();
        sendSync();
    }
    else if (e["kind"].asString() == "sync")
        sendSync();
    updateState();
}

void SyncClient::savePending()
{
    if (options.pendingFile.empty())
        return;
    std::error_code ec;
    std::filesystem::create_directories (options.pendingFile.parent_path(), ec);
    std::ofstream f (options.pendingFile, std::ios::trunc);
    for (const auto& h : pending)
        f << hex (h) << '\n';
}

void SyncClient::loadPending()
{
    if (options.pendingFile.empty())
        return;
    std::ifstream f (options.pendingFile);
    std::string line;
    while (std::getline (f, line))
        if (auto h = unhex (line); h && h->size() == 32)
            pending.insert (*h);
}
int SyncClient::colorOf (const std::string& userId) const
{
    for (const auto& m : memberList)
        if (m.userId == userId)
            return m.color;
    return -1;
}

void SyncClient::addCollaborator (const std::string& clientId, const std::string& userId, const doc::Bytes* state)
{
    if (clientId.empty() || clientId == options.clientId)
        return;
    auto& r = others[clientId];
    const auto now = options.clock();
    r.clientId = clientId;
    r.userId = userId;
    r.displayName = displayName (userId);
    r.color = std::max (0, colorOf (userId));
    if (state != nullptr && ! state->empty())
        if (const auto v = doc::decode (*state))
        {
            auto next = Presence::fromValue (*v);
            if (! r.hasState || next.pointer != r.state.pointer)
                r.pointerMovedAt = now;
            r.state = std::move (next);
            r.hasState = true;
        }
    r.receivedAt = now;
}

void SyncClient::noteInput()
{
    lastInput = options.clock();
}

void SyncClient::setPresence (const Presence& local)
{
    localPresence = local;
    flushPresence (false);
}

void SyncClient::flushPresence (bool force)
{
    if (conn == nullptr || ! welcomed)
        return;
    const auto now = options.clock();
    auto next = localPresence;
    if (next.status == Presence::Status::active && now - lastInput >= idleAfterMs)
        next.status = Presence::Status::idle;
    if (! next.playhead.playing)
        next.playhead.at = 0;
    // A running playhead is extrapolated by viewers: its movement alone isn't a change.
    auto unchanged = next;
    unchanged.pointer = sentPresence.pointer;
    if (next.playhead.playing && sentPresence.playhead.playing)
        unchanged.playhead = sentPresence.playhead;
    const bool otherChanged = ! (unchanged == sentPresence);
    const bool pointerMoved = next.pointer != sentPresence.pointer;
    const bool startedOrStopped = next.playhead.playing != sentPresence.playhead.playing;
    const bool playheadDue = next.playhead.playing && now - lastPlayheadSend >= playheadEveryMs;
    if (! otherChanged)
        dirtySince = 0;
    else if (dirtySince == 0)
        dirtySince = now;
    const bool gapOk = now - lastPresenceSend >= presenceMinGapMs;
    const bool send = force || ! presenceEverSent || startedOrStopped || playheadDue || (pointerMoved && gapOk)
                   || (otherChanged && gapOk && now - dirtySince >= presenceDebounceMs);
    if (! send)
        return;
    if (next.playhead.playing)
        lastPlayheadSend = now;
    sentPresence = next;
    presenceEverSent = true;
    lastPresenceSend = now;
    dirtySince = 0;
    sendPresence();
}

void SyncClient::sendPresence()
{
    const auto bytes = doc::encode (sentPresence.toValue());
    weft_conn_send (conn, framePresence, bytes.data(), bytes.size());
    ++sentCount;
}
} // namespace weft::collab
