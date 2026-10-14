#include "weft/doc/Path.h"

namespace weft::doc
{
Path path (std::initializer_list<std::string> keys)
{
    Path p;
    p.reserve (keys.size());
    for (const auto& k : keys)
        p.push_back (PathSegment::k (k));
    return p;
}

Path operator/ (Path p, PathSegment s)
{
    p.push_back (std::move (s));
    return p;
}

Path operator/ (Path p, std::string key)
{
    p.push_back (PathSegment::k (std::move (key)));
    return p;
}

std::vector<std::uint8_t> encodePath (const Path& p)
{
    std::vector<std::uint8_t> out;
    const auto u32 = [&out] (std::uint64_t x) {
        for (int i = 0; i < 4; ++i)
            out.push_back (static_cast<std::uint8_t> (x >> (8 * i)));
    };
    for (const auto& s : p)
    {
        out.push_back (static_cast<std::uint8_t> (s.kind));
        if (s.kind == PathSegment::Kind::index)
            u32 (s.index);
        else
        {
            u32 (s.key.size());
            out.insert (out.end(), s.key.begin(), s.key.end());
        }
    }
    return out;
}

std::string toString (const Path& p)
{
    std::string out;
    for (const auto& s : p)
    {
        out += '/';
        switch (s.kind)
        {
            case PathSegment::Kind::key: out += s.key; break;
            case PathSegment::Kind::index: out += '[' + std::to_string (s.index) + ']'; break;
            case PathSegment::Kind::byId: out += "[id=" + s.key + ']'; break;
        }
    }
    return out.empty() ? "/" : out;
}
} // namespace weft::doc
