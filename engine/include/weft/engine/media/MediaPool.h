#pragma once

#include "weft/engine/media/MediaSource.h"
#include "weft/media/WavFile.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>

namespace weft::engine
{
/**
    Project media by blob hash, shared with engine snapshots. Short files are held
    in memory; longer ones stream from disk (D-ARCH-17). Message thread only.
*/
class MediaPool
{
public:
    using Audio = std::shared_ptr<MediaSource>;

    /** Adds audio from memory under a hash (tests, generated media). */
    void add (const std::string& hash, const media::AudioData& audio);

    /** Opens `<mediaDir>/<hex>.wav` for the hash if it isn't open yet. Returns null when missing or unreadable. */
    Audio load (const std::filesystem::path& mediaDir, const std::string& hash, std::string* error = nullptr);

    Audio find (const std::string& hash) const;
    void remove (const std::string& hash) { pool.erase (hash); }
    void clear() { pool.clear(); }
    std::size_t size() const noexcept { return pool.size(); }

    /** Files up to this many frames are read whole; longer ones stream. Default: 30 s at 48 kHz. */
    void setResidentLimit (std::int64_t frames) noexcept { residentFrames = frames; }

private:
    std::map<std::string, Audio> pool;
    std::int64_t residentFrames = 30 * 48000;
};
} // namespace weft::engine
