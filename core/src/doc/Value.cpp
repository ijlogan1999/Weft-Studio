#include "weft/doc/Value.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace weft::doc
{
namespace
{
const Value nullValue;
const Value::Map emptyMap;
const Value::List emptyList;
const Value::Bytes emptyBytes;

enum Tag : std::uint8_t
{
    tagNull = 0,
    tagFalse = 1,
    tagTrue = 2,
    tagInt = 3,
    tagF64 = 4,
    tagStr = 5,
    tagBytes = 6,
    tagMap = 7,
    tagList = 8
};

constexpr int maxDepth = 64;

void putU32 (std::vector<std::uint8_t>& out, std::uint32_t x)
{
    for (int i = 0; i < 4; ++i)
        out.push_back (static_cast<std::uint8_t> (x >> (8 * i)));
}

void putU64 (std::vector<std::uint8_t>& out, std::uint64_t x)
{
    for (int i = 0; i < 8; ++i)
        out.push_back (static_cast<std::uint8_t> (x >> (8 * i)));
}

void putRawString (std::vector<std::uint8_t>& out, std::string_view s)
{
    putU32 (out, static_cast<std::uint32_t> (s.size()));
    out.insert (out.end(), s.begin(), s.end());
}

void encodeInto (std::vector<std::uint8_t>& out, const Value& value)
{
    std::visit (
        [&out] (const auto& x) {
            using T = std::decay_t<decltype (x)>;
            if constexpr (std::is_same_v<T, std::monostate>)
                out.push_back (tagNull);
            else if constexpr (std::is_same_v<T, bool>)
                out.push_back (x ? tagTrue : tagFalse);
            else if constexpr (std::is_same_v<T, std::int64_t>)
            {
                out.push_back (tagInt);
                putU64 (out, static_cast<std::uint64_t> (x));
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                out.push_back (tagF64);
                std::uint64_t bits;
                std::memcpy (&bits, &x, sizeof bits);
                putU64 (out, bits);
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                out.push_back (tagStr);
                putRawString (out, x);
            }
            else if constexpr (std::is_same_v<T, Value::Bytes>)
            {
                out.push_back (tagBytes);
                putU32 (out, static_cast<std::uint32_t> (x.size()));
                out.insert (out.end(), x.begin(), x.end());
            }
            else if constexpr (std::is_same_v<T, Value::Map>)
            {
                out.push_back (tagMap);
                putU32 (out, static_cast<std::uint32_t> (x.size()));
                for (const auto& [k, child] : x)
                {
                    putRawString (out, k);
                    encodeInto (out, child);
                }
            }
            else
            {
                out.push_back (tagList);
                putU32 (out, static_cast<std::uint32_t> (x.size()));
                for (const auto& child : x)
                    encodeInto (out, child);
            }
        },
        value.storage());
}

struct Reader
{
    std::span<const std::uint8_t> data;
    std::size_t pos = 0;

    bool take (std::size_t n, const std::uint8_t*& p)
    {
        if (n > data.size() - pos)
            return false;
        p = data.data() + pos;
        pos += n;
        return true;
    }

    bool u8 (std::uint8_t& x)
    {
        const std::uint8_t* p;
        if (! take (1, p))
            return false;
        x = *p;
        return true;
    }

    bool u32 (std::uint32_t& x)
    {
        const std::uint8_t* p;
        if (! take (4, p))
            return false;
        x = 0;
        for (int i = 0; i < 4; ++i)
            x |= static_cast<std::uint32_t> (p[i]) << (8 * i);
        return true;
    }

    bool u64 (std::uint64_t& x)
    {
        const std::uint8_t* p;
        if (! take (8, p))
            return false;
        x = 0;
        for (int i = 0; i < 8; ++i)
            x |= static_cast<std::uint64_t> (p[i]) << (8 * i);
        return true;
    }

    bool str (std::string& s)
    {
        std::uint32_t n;
        const std::uint8_t* p;
        if (! u32 (n) || ! take (n, p))
            return false;
        s.assign (reinterpret_cast<const char*> (p), n);
        return true;
    }

    bool value (Value& out, int depth)
    {
        if (depth > maxDepth)
            return false;
        std::uint8_t tag;
        if (! u8 (tag))
            return false;
        switch (tag)
        {
            case tagNull: out = Value(); return true;
            case tagFalse: out = Value (false); return true;
            case tagTrue: out = Value (true); return true;
            case tagInt:
            {
                std::uint64_t x;
                if (! u64 (x))
                    return false;
                out = Value (static_cast<std::int64_t> (x));
                return true;
            }
            case tagF64:
            {
                std::uint64_t bits;
                if (! u64 (bits))
                    return false;
                double d;
                std::memcpy (&d, &bits, sizeof d);
                out = Value (d);
                return true;
            }
            case tagStr:
            {
                std::string s;
                if (! str (s))
                    return false;
                out = Value (std::move (s));
                return true;
            }
            case tagBytes:
            {
                std::uint32_t n;
                const std::uint8_t* p;
                if (! u32 (n) || ! take (n, p))
                    return false;
                out = Value (Value::Bytes (p, p + n));
                return true;
            }
            case tagMap:
            {
                std::uint32_t n;
                if (! u32 (n))
                    return false;
                Value::Map m;
                for (std::uint32_t i = 0; i < n; ++i)
                {
                    std::string k;
                    Value child;
                    if (! str (k) || ! value (child, depth + 1))
                        return false;
                    m.insert_or_assign (std::move (k), std::move (child));
                }
                out = Value (std::move (m));
                return true;
            }
            case tagList:
            {
                std::uint32_t n;
                if (! u32 (n))
                    return false;
                Value::List l;
                l.reserve (std::min<std::uint32_t> (n, 4096));
                for (std::uint32_t i = 0; i < n; ++i)
                {
                    Value child;
                    if (! value (child, depth + 1))
                        return false;
                    l.push_back (std::move (child));
                }
                out = Value (std::move (l));
                return true;
            }
            default: return false;
        }
    }
};

void jsonString (std::string& out, std::string_view s)
{
    out += '"';
    for (const char ch : s)
    {
        const auto c = static_cast<unsigned char> (ch);
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    char buf[8];
                    std::snprintf (buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                }
                else
                    out += ch;
        }
    }
    out += '"';
}

void jsonInto (std::string& out, const Value& value, bool pretty, int indent)
{
    const auto newline = [&] (int level) {
        if (pretty)
        {
            out += '\n';
            out.append (static_cast<std::size_t> (level) * 2, ' ');
        }
    };

    std::visit (
        [&] (const auto& x) {
            using T = std::decay_t<decltype (x)>;
            if constexpr (std::is_same_v<T, std::monostate>)
                out += "null";
            else if constexpr (std::is_same_v<T, bool>)
                out += x ? "true" : "false";
            else if constexpr (std::is_same_v<T, std::int64_t>)
                out += std::to_string (x);
            else if constexpr (std::is_same_v<T, double>)
            {
                if (! std::isfinite (x))
                    out += "null";
                else
                {
                    char buf[32];
                    std::snprintf (buf, sizeof buf, "%.17g", x);
                    out += buf;
                    if (std::strpbrk (buf, ".eE") == nullptr)
                        out += ".0"; // keep doubles distinguishable from ints
                }
            }
            else if constexpr (std::is_same_v<T, std::string>)
                jsonString (out, x);
            else if constexpr (std::is_same_v<T, Value::Bytes>)
            {
                static const char* hex = "0123456789abcdef";
                std::string h;
                for (auto b : x)
                {
                    h += hex[b >> 4];
                    h += hex[b & 15];
                }
                out += "{\"$bytes\":";
                jsonString (out, h);
                out += '}';
            }
            else if constexpr (std::is_same_v<T, Value::Map>)
            {
                out += '{';
                bool first = true;
                for (const auto& [k, child] : x)
                {
                    if (! first)
                        out += ',';
                    first = false;
                    newline (indent + 1);
                    jsonString (out, k);
                    out += pretty ? ": " : ":";
                    jsonInto (out, child, pretty, indent + 1);
                }
                if (! x.empty())
                    newline (indent);
                out += '}';
            }
            else
            {
                out += '[';
                bool first = true;
                for (const auto& child : x)
                {
                    if (! first)
                        out += ',';
                    first = false;
                    newline (indent + 1);
                    jsonInto (out, child, pretty, indent + 1);
                }
                if (! x.empty())
                    newline (indent);
                out += ']';
            }
        },
        value.storage());
}
} // namespace

bool Value::asBool (bool fallback) const noexcept
{
    if (const auto* b = std::get_if<bool> (&v))
        return *b;
    return fallback;
}

std::int64_t Value::asInt (std::int64_t fallback) const noexcept
{
    if (const auto* i = std::get_if<std::int64_t> (&v))
        return *i;
    if (const auto* d = std::get_if<double> (&v))
        return std::isfinite (*d) ? static_cast<std::int64_t> (std::llround (*d)) : fallback;
    return fallback;
}

double Value::asDouble (double fallback) const noexcept
{
    if (const auto* d = std::get_if<double> (&v))
        return std::isfinite (*d) ? *d : fallback;
    if (const auto* i = std::get_if<std::int64_t> (&v))
        return static_cast<double> (*i);
    return fallback;
}

std::string Value::asString (std::string_view fallback) const
{
    if (const auto* s = std::get_if<std::string> (&v))
        return *s;
    return std::string (fallback);
}

const Value::Map& Value::asMap() const
{
    const auto* m = std::get_if<Map> (&v);
    return m != nullptr ? *m : emptyMap;
}

const Value::List& Value::asList() const
{
    const auto* l = std::get_if<List> (&v);
    return l != nullptr ? *l : emptyList;
}

const Value::Bytes& Value::asBytes() const
{
    const auto* b = std::get_if<Bytes> (&v);
    return b != nullptr ? *b : emptyBytes;
}

Value::Map& Value::mapRef()
{
    if (! isMap())
        v = Map {};
    return std::get<Map> (v);
}

Value::List& Value::listRef()
{
    if (! isList())
        v = List {};
    return std::get<List> (v);
}

const Value* Value::find (std::string_view key) const
{
    const auto* m = std::get_if<Map> (&v);
    if (m == nullptr)
        return nullptr;
    const auto it = m->find (key);
    return it != m->end() ? &it->second : nullptr;
}

const Value& Value::operator[] (std::string_view key) const
{
    const auto* found = find (key);
    return found != nullptr ? *found : nullValue;
}

Value& Value::set (std::string_view key, Value child)
{
    mapRef().insert_or_assign (std::string (key), std::move (child));
    return *this;
}

std::string toJson (const Value& value, bool pretty)
{
    std::string out;
    jsonInto (out, value, pretty, 0);
    return out;
}

std::vector<std::uint8_t> encode (const Value& value)
{
    std::vector<std::uint8_t> out;
    encodeInto (out, value);
    return out;
}

std::optional<Value> decode (std::span<const std::uint8_t> bytes)
{
    Reader r { bytes };
    Value v;
    if (! r.value (v, 0) || r.pos != bytes.size())
        return std::nullopt;
    return v;
}
} // namespace weft::doc
