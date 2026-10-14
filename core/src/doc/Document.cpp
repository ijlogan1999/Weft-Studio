#include "weft/doc/Document.h"

#include <weft_crdt.h>

#include <stdexcept>
#include <utility>

namespace weft::doc
{
namespace
{
std::vector<std::uint8_t> takeBytes (WeftBytes bytes)
{
    std::vector<std::uint8_t> out (bytes.data, bytes.data + bytes.len);
    weft_bytes_free (bytes);
    return out;
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

std::vector<std::uint8_t> Document::save() const
{
    WeftBytes bytes {};
    if (weft_doc_save (doc, &bytes) != WEFT_OK)
        return {};
    return takeBytes (bytes);
}

bool Document::putString (std::string_view key, std::string_view value)
{
    return weft_doc_put_str (doc, key.data(), key.size(), value.data(), value.size()) == WEFT_OK;
}

std::optional<std::string> Document::getString (std::string_view key) const
{
    WeftBytes bytes {};
    if (weft_doc_get_str (doc, key.data(), key.size(), &bytes) != WEFT_OK)
        return std::nullopt;
    auto raw = takeBytes (bytes);
    return std::string (raw.begin(), raw.end());
}

bool Document::merge (Document& other) { return weft_doc_merge (doc, other.doc) == WEFT_OK; }
} // namespace weft::doc
