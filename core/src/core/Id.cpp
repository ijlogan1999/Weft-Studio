#include "weft/core/Id.h"

namespace weft
{
namespace
{
constexpr std::string_view alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

Id encode128 (std::uint64_t hi, std::uint64_t lo)
{
    // 26 characters x 5 bits = 130 bits; the top two bits are zero.
    Id out (26, '0');
    for (int i = 25; i >= 0; --i)
    {
        out[static_cast<std::size_t> (i)] = alphabet[lo & 31u];
        lo = (lo >> 5) | ((hi & 31u) << 59);
        hi >>= 5;
    }
    return out;
}
} // namespace

IdGenerator::IdGenerator() : rng (std::random_device {}() ^ (static_cast<std::uint64_t> (std::random_device {}()) << 32)) {}

IdGenerator::IdGenerator (std::uint64_t seed) : rng (seed) {}

Id IdGenerator::next()
{
    const auto hi = rng();
    const auto lo = rng();
    return encode128 (hi, lo);
}

Id deriveId (std::string_view seed)
{
    // Two independent 64-bit FNV-1a hashes give 128 bits; collisions with random ids are negligible.
    std::uint64_t a = 0xcbf29ce484222325ull, b = 0x84222325cbf29ce4ull;
    for (const char c : seed)
    {
        a = (a ^ static_cast<unsigned char> (c)) * 0x100000001b3ull;
        b = (b ^ static_cast<unsigned char> (c)) * 0x100000001b3ull;
        b ^= b >> 29;
    }
    return encode128 (a, b);
}

bool isValidId (std::string_view id) noexcept
{
    if (id.size() != 26)
        return false;
    for (const char c : id)
        if (alphabet.find (c) == std::string_view::npos)
            return false;
    return true;
}
} // namespace weft
