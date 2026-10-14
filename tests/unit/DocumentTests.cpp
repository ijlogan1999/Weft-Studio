// Tests for the schema-agnostic CRDT document wrapper (path + value API).

#include "weft/doc/Document.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using weft::doc::Document;
using weft::doc::path;
using weft::doc::Status;
using weft::doc::Value;

namespace
{
Document makeDoc (std::uint8_t actor)
{
    const std::array<std::uint8_t, 16> id { actor, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    auto doc = Document::withActor (id);
    REQUIRE (doc.has_value());
    return std::move (*doc);
}
std::optional<std::string> getString (const Document& d, const char* key)
{
    auto v = d.get (path ({ key }));
    if (! v || ! v->isString())
        return std::nullopt;
    return v->asString();
}

bool putString (Document& d, const char* key, const char* value)
{
    return d.put (path ({ key }), Value (value)) == Status::ok && d.commit ("test", 0);
}
} // namespace

TEST_CASE ("Document stores and reads string fields", "[U][doc]")
{
    Document doc;
    CHECK_FALSE (getString (doc, "name").has_value());
    REQUIRE (putString (doc, "name", "First Song"));
    CHECK (getString (doc, "name") == "First Song");
}

TEST_CASE ("Document survives a save and load round trip", "[U][doc]")
{
    Document doc;
    REQUIRE (putString (doc, "name", "Round Trip"));
    const auto bytes = doc.save();
    REQUIRE_FALSE (bytes.empty());

    auto loaded = Document::load (bytes);
    REQUIRE (loaded.has_value());
    CHECK (getString (*loaded, "name") == "Round Trip");
}

TEST_CASE ("Loading corrupt bytes fails cleanly", "[U][doc]")
{
    const std::array<std::uint8_t, 6> junk { 0x85, 0x6f, 0x4a, 0x83, 0x00, 0x01 };
    CHECK_FALSE (Document::load (junk).has_value());
}

TEST_CASE ("Concurrent edits converge after merging both ways", "[U][doc]")
{
    auto a = makeDoc (0xA0);
    auto b = makeDoc (0xB0);

    REQUIRE (putString (a, "base", "shared"));
    REQUIRE (b.merge (a) == Status::ok);

    // Concurrent writes to the same field and to different fields.
    REQUIRE (putString (a, "name", "from A"));
    REQUIRE (putString (b, "name", "from B"));
    REQUIRE (putString (a, "onlyA", "a"));
    REQUIRE (putString (b, "onlyB", "b"));

    REQUIRE (a.merge (b) == Status::ok);
    REQUIRE (b.merge (a) == Status::ok);

    CHECK (getString (a, "name") == getString (b, "name"));
    CHECK (getString (a, "onlyA") == "a");
    CHECK (getString (b, "onlyA") == "a");
    CHECK (getString (a, "onlyB") == "b");
    CHECK (getString (a, "base") == "shared");
}
