// Value codec (shared with crates/weft-crdt/src/codec.rs), JSON, paths and ids.

#include "weft/core/Id.h"
#include "weft/doc/Document.h"

#include <catch2/catch_test_macros.hpp>

using namespace weft;
using doc::PathSegment;
using doc::Value;

TEST_CASE ("Values round-trip through the binary codec", "[U][doc]")
{
    const auto v = Value::map()
                       .set ("null", Value())
                       .set ("bools", Value (Value::List { Value (true), Value (false) }))
                       .set ("int", Value (std::int64_t { -1234567890123 }))
                       .set ("double", Value (-3.5))
                       .set ("str", Value ("héllo"))
                       .set ("bytes", Value (Value::Bytes { 0, 1, 255 }))
                       .set ("nested", Value::map().set ("list", Value::list()));
    const auto decoded = doc::decode (doc::encode (v));
    REQUIRE (decoded);
    CHECK (*decoded == v);
    CHECK_FALSE (doc::decode (std::vector<std::uint8_t> { 7, 1, 0, 0 }).has_value()); // truncated map
}

TEST_CASE ("Canonical JSON", "[U][doc]")
{
    const auto v = Value::map().set ("b", Value (1.0)).set ("a", Value (Value::Bytes { 0xAB })).set ("c", Value ("q\"\n"));
    CHECK (doc::toJson (v) == R"({"a":{"$bytes":"ab"},"b":1.0,"c":"q\"\n"})");
}

TEST_CASE ("Documents store nested values and address list elements by id", "[U][doc]")
{
    doc::Document d;
    const auto sends = Value (Value::List { Value::map().set ("id", "S1").set ("level", -6.0), Value::map().set ("id", "S2").set ("level", 0.0) });
    REQUIRE (d.put (doc::path ({ "sends" }), sends) == doc::Status::ok);
    REQUIRE (d.put (doc::path ({ "sends" }) / PathSegment::id ("S2") / "level", Value (-3.0)) == doc::Status::ok);
    REQUIRE (d.commit ("t", 0));
    CHECK (d.get (doc::path ({ "sends" }) / PathSegment::at (1) / "level")->asDouble() == -3.0);
    CHECK_FALSE (d.get (doc::path ({ "sends" }) / PathSegment::id ("S3")).has_value());
    CHECK (d.remove (doc::path ({ "missing" })) == doc::Status::notFound);
    CHECK_FALSE (d.commit ("nothing", 0)); // no pending writes, no change
}

TEST_CASE ("Ids", "[U][doc]")
{
    IdGenerator a (42), b (42);
    const auto id = a.next();
    CHECK (id.size() == 26);
    CHECK (isValidId (id));
    CHECK (id == b.next());
    CHECK (a.next() != id);
    CHECK (deriveId ("x") == deriveId ("x"));
    CHECK (deriveId ("x") != deriveId ("y"));
    CHECK (isValidId (deriveId ("x")));
    CHECK_FALSE (isValidId ("not-an-id"));
}
