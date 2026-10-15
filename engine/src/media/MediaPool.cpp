#include "weft/engine/media/MediaPool.h"

namespace weft::engine
{
void MediaPool::add (const std::string& hash, const media::AudioData& audio) { pool[hash] = std::make_shared<MediaSource> (audio); }

MediaPool::Audio MediaPool::load (const std::filesystem::path& mediaDir, const std::string& hash, std::string* error)
{
    if (auto existing = find (hash))
        return existing;
    auto source = MediaSource::open (mediaDir / media::mediaFileName (hash), residentFrames, error);
    if (source)
        pool[hash] = source;
    return source;
}

MediaPool::Audio MediaPool::find (const std::string& hash) const
{
    const auto it = pool.find (hash);
    return it == pool.end() ? nullptr : it->second;
}
} // namespace weft::engine
