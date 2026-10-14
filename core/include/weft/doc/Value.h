#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace weft::doc
{
/**
    A JSON-like value: the C++ view of anything stored in the CRDT document.
    Maps keep keys sorted, so equality and JSON output are canonical.
*/
class Value
{
public:
    using Map = std::map<std::string, Value, std::less<>>;
    using List = std::vector<Value>;
    using Bytes = std::vector<std::uint8_t>;
    using Storage = std::variant<std::monostate, bool, std::int64_t, double, std::string, Bytes, Map, List>;

    Value() = default;
    Value (std::nullptr_t) {}
    Value (bool b) : v (b) {}
    Value (int i) : v (static_cast<std::int64_t> (i)) {}
    Value (std::int64_t i) : v (i) {}
    Value (double d) : v (d) {}
    Value (const char* s) : v (std::string (s)) {}
    Value (std::string s) : v (std::move (s)) {}
    Value (std::string_view s) : v (std::string (s)) {}
    Value (Bytes b) : v (std::move (b)) {}
    Value (Map m) : v (std::move (m)) {}
    Value (List l) : v (std::move (l)) {}

    static Value map() { return Value (Map {}); }
    static Value list() { return Value (List {}); }

    bool isNull() const noexcept { return std::holds_alternative<std::monostate> (v); }
    bool isBool() const noexcept { return std::holds_alternative<bool> (v); }
    bool isInt() const noexcept { return std::holds_alternative<std::int64_t> (v); }
    bool isDouble() const noexcept { return std::holds_alternative<double> (v); }
    bool isNumber() const noexcept { return isInt() || isDouble(); }
    bool isString() const noexcept { return std::holds_alternative<std::string> (v); }
    bool isBytes() const noexcept { return std::holds_alternative<Bytes> (v); }
    bool isMap() const noexcept { return std::holds_alternative<Map> (v); }
    bool isList() const noexcept { return std::holds_alternative<List> (v); }
    bool isContainer() const noexcept { return isMap() || isList(); }

    // Lenient accessors: return the fallback when the type doesn't match. Numbers convert.
    bool asBool (bool fallback = false) const noexcept;
    std::int64_t asInt (std::int64_t fallback = 0) const noexcept;
    double asDouble (double fallback = 0.0) const noexcept;
    std::string asString (std::string_view fallback = {}) const;

    const Map& asMap() const;   // empty map if not a map
    const List& asList() const; // empty list if not a list
    const Bytes& asBytes() const;
    Map& mapRef();              // converts to an empty map first if needed
    List& listRef();            // converts to an empty list first if needed

    /** Child of a map, or nullptr. */
    const Value* find (std::string_view key) const;
    /** Child of a map, or a null value. */
    const Value& operator[] (std::string_view key) const;
    /** Sets a map entry (converting this value to a map if needed) and returns *this for chaining. */
    Value& set (std::string_view key, Value child);

    const Storage& storage() const noexcept { return v; }

    friend bool operator== (const Value& a, const Value& b) { return a.v == b.v; }

private:
    Storage v;
};

/** Canonical JSON (sorted keys, no whitespace unless pretty). Bytes are written as {"$bytes":"hex"}. */
std::string toJson (const Value& value, bool pretty = false);

/** Binary codec shared with crates/weft-crdt/src/codec.rs. */
std::vector<std::uint8_t> encode (const Value& value);
std::optional<Value> decode (std::span<const std::uint8_t> bytes);
} // namespace weft::doc
