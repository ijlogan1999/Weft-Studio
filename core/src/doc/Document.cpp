#include "weft/doc/Document.h"

#include <weft_crdt.h>

#include <charconv>
#include <stdexcept>
#include <utility>

namespace weft::doc
{
namespace
{
Bytes takeBytes (WeftBytes bytes)
{
    Bytes out (bytes.data, bytes.data + bytes.len);
    weft_bytes_free (bytes);
    return out;
}

Status toStatus (WeftStatus s)
{
    switch (s)
    {
        case WEFT_OK: return Status::ok;
        case WEFT_INVALID_ARGUMENT: return Status::invalidArgument;
        case WEFT_NOT_FOUND: return Status::notFound;
        case WEFT_WRONG_TYPE: return Status::wrongType;
        case WEFT_CORRUPT_DATA: return Status::corruptData;
        case WEFT_INTERNAL: break;
    }
    return Status::internal;
}
} // namespace

Document::Document() : doc (weft_doc_new())
{
    if (doc == nullptr)
        throw std::runtime_error ("weft_doc_new failed");
}

std::optional<Document> Document::withActor (std::span<const std::uint8_t> actorId)
{
    if (auto* handle = weft_doc_new_with_actor (actorId.data(), actorId.size()))
        return Document (handle);
    return std::nullopt;
}

std::optional<Document> Document::load (std::span<const std::uint8_t> bytes)
{
    if (auto* handle = weft_doc_load (bytes.data(), bytes.size()))
        return Document (handle);
    return std::nullopt;
}

Document::Document (Document&& other) noexcept : doc (std::exchange (other.doc, nullptr)) {}

Document& Document::operator= (Document&& other) noexcept
{
    if (this != &other)
    {
        weft_doc_free (doc);
        doc = std::exchange (other.doc, nullptr);
    }
    return *this;
}

Document::~Document() { weft_doc_free (doc); }

bool Document::setActor (std::span<const std::uint8_t> actorId)
{
    return weft_doc_set_actor (doc, actorId.data(), actorId.size()) == WEFT_OK;
}

Bytes Document::save()
{
    WeftBytes bytes {};
    if (weft_doc_save (doc, &bytes) != WEFT_OK)
        return {};
    return takeBytes (bytes);
}

std::optional<Value> Document::get (const Path& p) const
{
    const auto encoded = encodePath (p);
    WeftBytes bytes {};
    if (weft_doc_get (doc, encoded.data(), encoded.size(), &bytes) != WEFT_OK)
        return std::nullopt;
    return decode (takeBytes (bytes));
}

Value Document::root() const { return get ({}).value_or (Value::map()); }

std::optional<std::string> Document::opId (const Path& p) const
{
    const auto encoded = encodePath (p);
    WeftBytes bytes {};
    if (weft_doc_op_id (doc, encoded.data(), encoded.size(), &bytes) != WEFT_OK)
        return std::nullopt;
    const auto raw = takeBytes (bytes);
    return std::string (raw.begin(), raw.end());
}

Status Document::put (const Path& p, const Value& v)
{
    const auto encodedPath = encodePath (p);
    const auto encodedValue = encode (v);
    return toStatus (weft_doc_put (doc, encodedPath.data(), encodedPath.size(), encodedValue.data(), encodedValue.size()));
}

Status Document::insert (const Path& p, const Value& v)
{
    const auto encodedPath = encodePath (p);
    const auto encodedValue = encode (v);
    return toStatus (weft_doc_insert (doc, encodedPath.data(), encodedPath.size(), encodedValue.data(), encodedValue.size()));
}

Status Document::remove (const Path& p)
{
    const auto encoded = encodePath (p);
    return toStatus (weft_doc_delete (doc, encoded.data(), encoded.size()));
}

bool Document::commit (std::string_view message, std::int64_t timeMs)
{
    return weft_doc_commit (doc, reinterpret_cast<const std::uint8_t*> (message.data()), message.size(), timeMs) == 1;
}

void Document::rollback() { weft_doc_rollback (doc); }

Bytes Document::heads() const
{
    WeftBytes bytes {};
    if (weft_doc_heads (doc, &bytes) != WEFT_OK)
        return {};
    return takeBytes (bytes);
}

std::optional<Bytes> Document::lastLocalChange() const
{
    WeftBytes bytes {};
    if (weft_doc_last_local_change (doc, &bytes) != WEFT_OK)
        return std::nullopt;
    return takeBytes (bytes);
}

Bytes Document::allChanges (std::size_t* count) const
{
    WeftBytes bytes {};
    if (weft_doc_all_changes (doc, &bytes, count) != WEFT_OK)
        return {};
    return takeBytes (bytes);
}

Status Document::apply (std::span<const std::uint8_t> changes)
{
    return toStatus (weft_doc_apply (doc, changes.data(), changes.size()));
}

bool Document::hasChange (std::span<const std::uint8_t> hash32) const
{
    return hash32.size() == 32 && weft_doc_has_change (doc, hash32.data()) == 1;
}

std::vector<Path> Document::changedPaths (std::span<const std::uint8_t> since, std::size_t depth) const
{
    WeftBytes bytes {};
    if (weft_doc_changed_paths (doc, since.data(), since.size(), depth, &bytes) != WEFT_OK)
        return {};
    const auto decoded = decode (takeBytes (bytes));
    std::vector<Path> out;
    if (! decoded)
        return out;
    for (const auto& entry : decoded->asList())
    {
        Path p;
        for (const auto& seg : entry.asList())
            p.push_back (seg.isString() ? PathSegment::k (seg.asString()) : PathSegment::at (static_cast<std::size_t> (seg.asInt())));
        out.push_back (std::move (p));
    }
    return out;
}

Status Document::merge (Document& other) { return toStatus (weft_doc_merge (doc, other.doc)); }

std::optional<OpStamp> OpStamp::parse (std::string_view opId)
{
    const auto at = opId.find ('@');
    if (at == std::string_view::npos)
        return std::nullopt;
    OpStamp s;
    const auto* first = opId.data();
    if (std::from_chars (first, first + at, s.counter).ec != std::errc {})
        return std::nullopt;
    s.actor = std::string (opId.substr (at + 1));
    return s;
}

SyncState::SyncState() : state (weft_sync_state_new()) {}
SyncState::SyncState (SyncState&& other) noexcept : state (std::exchange (other.state, nullptr)) {}
SyncState& SyncState::operator= (SyncState&& other) noexcept
{
    if (this != &other)
    {
        weft_sync_state_free (state);
        state = std::exchange (other.state, nullptr);
    }
    return *this;
}
SyncState::~SyncState() { weft_sync_state_free (state); }

std::optional<Bytes> changeHash (std::span<const std::uint8_t> change)
{
    Bytes out (32);
    if (weft_change_hash (change.data(), change.size(), out.data()) != WEFT_OK)
        return std::nullopt;
    return out;
}

std::optional<Bytes> Document::generateSyncMessage (SyncState& state)
{
    WeftBytes out {};
    if (weft_doc_sync_generate (doc, state.handle(), &out) != WEFT_OK)
        return std::nullopt;
    return takeBytes (out);
}

Status Document::receiveSyncMessage (SyncState& state, std::span<const std::uint8_t> message)
{
    return toStatus (weft_doc_sync_receive (doc, state.handle(), message.data(), message.size()));
}

std::vector<ChangeInfo> Document::changesSince (std::span<const std::uint8_t> since) const
{
    WeftBytes out {};
    if (weft_doc_changes_info (doc, since.data(), since.size(), &out) != WEFT_OK)
        return {};
    const auto bytes = takeBytes (out);
    std::vector<ChangeInfo> result;
    if (const auto v = decode (bytes))
        for (const auto& item : v->asList())
            result.push_back ({ item["hash"].asBytes(), item["actor"].asString(), item["message"].asString(), item["time"].asInt() });
    return result;
}
} // namespace weft::doc
