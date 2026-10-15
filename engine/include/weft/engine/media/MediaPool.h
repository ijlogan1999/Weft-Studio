#pragma once

#include "weft/media/WavFile.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>

namespace weft::engine
{
/**
    Decoded media by blob hash, shared with engine snapshots. Milestone 2 keeps
    whole files in memory (D-ARCH-12); disk streaming replaces this in M3.
    Message thread only.
*/
class MediaPool
{
public:
    using Audio = std::shared_ptr<const media::AudioData>;

    /** Adds decoded audio under a hash (tests, recording). */
    void add (const std::string& hash, media::AudioData audio);

    /** Loads `<mediaDir>/<hex>.wav` for the hash if it isn't loaded yet. Returns null when missing or unreadable. */
    Audio load (const std::filesystem::path& mediaDir, const std::string& hash, std::string* error = nullptr);

    Audio find (const std::string& hash) const;
    void remove (const std::string& hash) { pool.erase (hash); }
    std::size_t size() const noexcept { return pool.size(); }

private:
    std::map<std::string, Audio> pool;
};
} // namespace weft::engine
