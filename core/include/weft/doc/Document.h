#pragma once

#include "weft/doc/Path.h"
#include "weft/doc/Value.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct WeftDoc;
struct WeftSyncState;

namespace weft::doc
{
enum class Status
{
    ok,
    invalidArgument,
    notFound,
    wrongType,
    corruptData,
    internal
};

using Bytes = std::vector<std::uint8_t>;

/** Automerge sync-protocol state for one peer (spec 05 §3.3). Reset it for each new connection. */
class SyncState
{
public:
    SyncState();
    SyncState (SyncState&& other) noexcept;
    SyncState& operator= (SyncState&& other) noexcept;
    SyncState (const SyncState&) = delete;
    SyncState& operator= (const SyncState&) = delete;
    ~SyncState();

    WeftSyncState* handle() const noexcept { return state; }

private:
    WeftSyncState* state = nullptr;
};

/** Who made a change and when, read from the change itself. */
struct ChangeInfo
{
    Bytes hash;
    std::string actor;   // hex
    std::string message; // the commit message ("weft1|<user>|<command>")
    std::int64_t timeMs = 0;
};

/** The 32-byte hash of one encoded change. */
std::optional<Bytes> changeHash (std::span<const std::uint8_t> change);

/**
    Owning C++ handle for the CRDT document (Automerge, via the weft-crdt shim).

    Schema agnostic: reads and writes take a Path and a Value. Writes are pending
    until commit(), which turns them into one CRDT change; reads see pending
    writes. The spec 03 schema, commands and undo live above this class.
*/
class Document
{
public:
    /** Creates an empty document with a random actor id. */
    Document();

    /** Creates an empty document with a fixed actor id (1 to 32 bytes). */
    static std::optional<Document> withActor (std::span<const std::uint8_t> actorId);

    /** Loads a saved document; returns nullopt if the bytes are not a valid document. */
    static std::optional<Document> load (std::span<const std::uint8_t> bytes);

    Document (Document&& other) noexcept;
    Document& operator= (Document&& other) noexcept;
    Document (const Document&) = delete;
    Document& operator= (const Document&) = delete;
    ~Document();

    bool setActor (std::span<const std::uint8_t> actorId);

    /** Full compressed document. Commits any pending writes first. */
    Bytes save();

    // Reads -------------------------------------------------------------------
    std::optional<Value> get (const Path& p) const;
    Value root() const;
    /** Id of the operation that wrote the value at p (identifies objects; orders writes). */
    std::optional<std::string> opId (const Path& p) const;

    // Writes (pending until commit) ------------------------------------------
    Status put (const Path& p, const Value& v);
    Status insert (const Path& listIndexPath, const Value& v);
    Status remove (const Path& p);

    /** Turns pending writes into one change. Returns true if a change was created. */
    bool commit (std::string_view message, std::int64_t timeMs);
    void rollback();

    // Changes and history -------------------------------------------------------
    Bytes heads() const;
    std::optional<Bytes> lastLocalChange() const;
    Bytes allChanges (std::size_t* count = nullptr) const;
    /** Applies change chunks or saved-document bytes. */
    Status apply (std::span<const std::uint8_t> changes);
    bool hasChange (std::span<const std::uint8_t> hash32) const;
    /** Paths touched since `since` heads, truncated to depth segments. */
    std::vector<Path> changedPaths (std::span<const std::uint8_t> since, std::size_t depth) const;
    Status merge (Document& other);

    // Sync protocol ------------------------------------------------------------
    /** The next sync message for the peer, or nullopt when there is nothing to send. */
    std::optional<Bytes> generateSyncMessage (SyncState& state);
    Status receiveSyncMessage (SyncState& state, std::span<const std::uint8_t> message);
    /** The changes since `since` heads (all for empty), oldest first. */
    std::vector<ChangeInfo> changesSince (std::span<const std::uint8_t> since) const;

private:
    explicit Document (WeftDoc* handle) noexcept : doc (handle) {}
    WeftDoc* doc = nullptr;
};

/** Parses an op id "counter@actor" into (counter, actor) for ordering writes. */
struct OpStamp
{
    std::uint64_t counter = 0;
    std::string actor;

    static std::optional<OpStamp> parse (std::string_view opId);
    friend auto operator<=> (const OpStamp&, const OpStamp&) = default;
};
} // namespace weft::doc
