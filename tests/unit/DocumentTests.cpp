// Smoke tests for the CRDT document wrapper. The spec 03 schema and its
// acceptance tests arrive in Milestone 1; these prove the Rust shim links and
// behaves on every platform.

#include "weft/doc/Document.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using weft::doc::Document;

namespace
{
Document makeDoc (std::uint8_t actor)
{
    const std::array<std::uint8_t, 16> id { actor, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    auto doc = Document::withActor (id);
    REQUIRE (doc.has_value());
    return std::move (*doc);
}
} // namespace

TEST_CASE ("Document stores and reads string fields", "[U][doc]")
{
    Document doc;
    CHECK_FALSE (doc.getString ("name").has_value());
    REQUIRE (doc.putString ("name", "First Song"));
    CHECK (doc.getString ("name") == "First Song");
}

TEST_CASE ("Document survives a save and load round trip", "[U][doc]")
{
    Document doc;
    REQUIRE (doc.putString ("name", "Round Trip"));
    const auto bytes = doc.save();
    REQUIRE_FALSE (bytes.empty());

    auto loaded = Document::load (bytes);
    REQUIRE (loaded.has_value());
    CHECK (loaded->getString ("name") == "Round Trip");
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

    REQUIRE (a.putString ("base", "shared"));
    REQUIRE (b.merge (a));

    // Concurrent writes to the same field and to different fields.
    REQUIRE (a.putString ("name", "from A"));
    REQUIRE (b.putString ("name", "from B"));
    REQUIRE (a.putString ("onlyA", "a"));
    REQUIRE (b.putString ("onlyB", "b"));

    REQUIRE (a.merge (b));
    REQUIRE (b.merge (a));

    CHECK (a.getString ("name") == b.getString ("name"));
    CHECK (a.getString ("onlyA") == "a");
    CHECK (b.getString ("onlyA") == "a");
    CHECK (a.getString ("onlyB") == "b");
    CHECK (a.getString ("base") == "shared");
}
