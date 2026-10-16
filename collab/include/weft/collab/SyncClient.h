#pragma once

// Keeps a Session in step with the relay (spec 05 §3.3): joins the project's room,
// runs the sync exchange, sends local changes as they're made (batched at most every
// 50 ms), applies remote ones (after any gesture in progress, D-ARCH-6), and tracks
// which local changes the relay hasn't acknowledged yet, so offline edits are
// counted and re-sent (§3.3 Durability, §13).
//
// Everything here runs on the caller's thread: call poll() regularly (the app does
// it from a 30 Hz timer). The connection itself lives on a background thread.

#include "weft/collab/Presence.h"
#include "weft/collab/Relay.h"
#include "weft/commands/Session.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct WeftConn;

namespace weft::collab
{
enum class SyncState
{
    connecting,     // first connection attempt
    syncing,        // joined; exchanging changes
    synced,         // the relay has everything we have, and we have everything it has
    reconnecting,   // lost the connection; retrying with backoff
    offline,        // the user went offline, or never connected
    updateRequired, // the project needs a newer Weft
    signedOut,      // the relay refused our sign-in
    removed,        // not (or no longer) a member
    notFound        // the relay doesn't know the project
};

struct Collaborator
{
    std::string userId, displayName, role;
    int color = 0;
};

struct SyncOptions
{
    Account account;
    std::string projectId;
    /** Random per client; one user on two machines is two clients (05 §13). */
    std::string clientId;
    /** Where un-acknowledged change hashes are kept between runs (Cache/sync-pending.txt). */
    std::filesystem::path pendingFile;
    bool startOnline = true;
    /** Test hook: delay added to each direction of the connection. */
    int oneWayDelayMs = 0;
    /** Unix ms for presence (timestamps, rate limits, idle). Tests move it by hand. */
    std::function<std::int64_t()> clock;
};

class SyncClient
{
public:
    SyncClient (commands::Session& session, SyncOptions options);
    ~SyncClient();
    SyncClient (const SyncClient&) = delete;
    SyncClient& operator= (const SyncClient&) = delete;

    /** Handles what arrived and sends what's queued. Call often from the session's thread. */
    void poll();

    /** Go Offline / back online (Share menu). Offline edits are kept and sent on reconnect. */
    void setOnline (bool online);
    bool online() const noexcept { return wantOnline; }

    SyncState state() const noexcept { return syncState; }
    /** Local changes the relay hasn't acknowledged. */
    std::size_t pendingCount() const noexcept { return pending.size(); }
    /** The status bar text (04 §10): "Synced", "Syncing…", "Offline – 3 changes pending", ... */
    std::string statusText() const;
    /** This user's role in the project ("owner", "editor", "viewer"), once joined. */
    const std::string& role() const noexcept { return myRole; }
    const std::vector<Collaborator>& members() const noexcept { return memberList; }
    /** A user's display name, or "someone". */
    std::string displayName (const std::string& userId) const;
    /** The account, with any tokens refreshed along the way. */
    const Account& account() const noexcept { return options.account; }
    const std::string& projectId() const noexcept { return options.projectId; }
    /** Unix ms of the last moment everything was in sync (0: never this run). */
    std::int64_t lastSyncedAt() const noexcept { return lastSynced; }

    /** Changes from collaborators were applied (the UI and engine should refresh). */
    std::function<void()> onRemoteChange;
    /** A status bar message, e.g. "Region "Verse" moved by Ana" (05 §4). */
    std::function<void (const std::string&)> onMessage;
    std::function<void()> onStatusChange;
    /** New tokens to save (the access token was refreshed transparently). */
    std::function<void (const Account&)> onAccountChange;
    /** BLOB_AVAILABLE: a blob finished uploading (05 §8.2). */
    std::function<void (const std::string& hash, std::int64_t size)> onBlobAvailable;
    /** Joined: the relay sent the role and the members (media sync starts here). */
    std::function<void()> onJoined;

    // Presence (05 §5) -----------------------------------------------------------------
    /** This client's presence, as often as convenient: what's sent, and when, follows the
        update rates of 05 §5 (pointer at most 20 Hz, playhead on start and every 2 s while
        playing, the rest debounced 100 ms). The status turns idle after 2 minutes without noteInput(). */
    void setPresence (const Presence& local);
    /** Mouse or keyboard input: the user isn't idle. */
    void noteInput();
    /** Who else is connected, by client id. */
    const std::map<std::string, RemotePresence>& collaborators() const noexcept { return others; }
    std::function<void()> onPresenceChange;
    /** Presence messages sent so far (tests check the rate). */
    std::size_t presenceSent() const noexcept { return sentCount; }
    std::int64_t now() const { return options.clock(); }
    const std::string& clientId() const noexcept { return options.clientId; }
    /** The collaborator color index of a member (05 §5), or -1. */
    int colorOf (const std::string& userId) const;

private:
    void handle (const doc::Value& event);
    void applyRemote (const doc::Value& event);
    void sendSync();
    void flushOutbox (bool force);
    void updateState();
    void setState (SyncState s);
    void setRole (const std::string& role);
    void savePending();
    void loadPending();
    void sendPresence();
    void flushPresence (bool force);
    void addCollaborator (const std::string& clientId, const std::string& userId, const doc::Bytes* state);
    struct RegionMark
    {
        Ticks start = 0;
        Id trackId;
        std::string name;
    };
    std::map<Id, RegionMark> regionsTouchedByMe() const;
    void noticeConflicts (const std::map<Id, RegionMark>& mine, const doc::Bytes& headsBefore);

    commands::Session& session;
    SyncOptions options;
    WeftConn* conn = nullptr;
    int listener = 0;
    doc::SyncState sync;
    bool welcomed = false;
    bool everWelcomed = false;
    bool wantOnline = true;
    SyncState syncState = SyncState::connecting;
    std::string myRole;
    std::vector<Collaborator> memberList;
    std::set<doc::Bytes> pending;
    std::vector<doc::Bytes> outbox;
    std::vector<doc::Value> deferred; // remote events held while a gesture is open
    doc::Bytes serverHeads;
    /** The relay has every change we had when we joined (changes made before syncing, or
        before the project was shared, aren't in `pending`); set once its heads equal ours. */
    bool caughtUp = false;
    std::chrono::steady_clock::time_point lastFlush {};
    std::int64_t lastSynced = 0;

    Presence localPresence, sentPresence;
    bool presenceEverSent = false;
    std::int64_t lastPresenceSend = 0, lastPlayheadSend = 0, dirtySince = 0, lastInput = 0;
    std::size_t sentCount = 0;
    std::map<std::string, RemotePresence> others;
    std::int64_t disconnectedAt = 0; // our clock; collaborators are cleared 45 s after
};
} // namespace weft::collab
