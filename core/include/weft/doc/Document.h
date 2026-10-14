#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct WeftDoc;

namespace weft::doc
{
/**
    Owning C++ handle for a CRDT document (Automerge, via the weft-crdt shim).

    Milestone 0 exposes only what the smoke tests need: root-level string fields,
    save/load and merge. Milestone 1 adds the typed spec 03 schema on top.
*/
class Document
{
public:
    /** Creates an empty document with a random actor id. */
    Document();

    /** Creates an empty document with a fixed actor id (1 to 32 bytes). Used by tests for determinism. */
    static std::optional<Document> withActor (std::span<const std::uint8_t> actorId);

    /** Loads a saved document; returns nullopt if the bytes are not a valid document. */
    static std::optional<Document> load (std::span<const std::uint8_t> bytes);

    Document (Document&& other) noexcept;
    Document& operator= (Document&& other) noexcept;
    Document (const Document&) = delete;
    Document& operator= (const Document&) = delete;
    ~Document();

    std::vector<std::uint8_t> save() const;

    bool putString (std::string_view key, std::string_view value);
    std::optional<std::string> getString (std::string_view key) const;

    /** Merges every change from other into this document. */
    bool merge (Document& other);

private:
    explicit Document (WeftDoc* handle) noexcept : doc (handle) {}
    WeftDoc* doc = nullptr;
};
} // namespace weft::doc
