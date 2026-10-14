#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace weft::doc
{
/** One step in a document path: a map key, a list index, or the list element with a given id. */
struct PathSegment
{
    enum class Kind : std::uint8_t { key = 0, index = 1, byId = 2 };

    Kind kind = Kind::key;
    std::string key;     // key or id
    std::size_t index = 0;

    static PathSegment k (std::string name) { return { Kind::key, std::move (name), 0 }; }
    static PathSegment at (std::size_t i) { return { Kind::index, {}, i }; }
    static PathSegment id (std::string elementId) { return { Kind::byId, std::move (elementId), 0 }; }

    friend bool operator== (const PathSegment&, const PathSegment&) = default;
};

using Path = std::vector<PathSegment>;

/** Builds a path of map keys: path ({ "tracks", id, "name" }). */
Path path (std::initializer_list<std::string> keys);

/** Returns p with one more segment. */
Path operator/ (Path p, PathSegment s);
Path operator/ (Path p, std::string key);

std::vector<std::uint8_t> encodePath (const Path& p);
std::string toString (const Path& p);
} // namespace weft::doc
