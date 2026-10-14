#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <string_view>

namespace weft
{
/** A 128-bit random id written as 26 Crockford base32 characters (spec 03 §1.2). */
using Id = std::string;
using UserId = std::string;

class IdGenerator
{
public:
    /** Seeded from std::random_device. */
    IdGenerator();
    /** Deterministic sequence, for tests. */
    explicit IdGenerator (std::uint64_t seed);

    Id next();

private:
    std::mt19937_64 rng;
};

/** A deterministic id derived from text, so independent clients compute the same id
    (e.g. the Recovered track for a deleted track, D-DM-17). */
Id deriveId (std::string_view seed);

bool isValidId (std::string_view id) noexcept;
} // namespace weft
