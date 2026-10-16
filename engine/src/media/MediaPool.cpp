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

MediaPool::Audio MediaPool::loadPartial (const std::filesystem::path& partFile, const std::string& hash, std::int64_t totalFrames)
{
    if (auto existing = find (hash))
    {
        existing->extend();
        return existing;
    }
    auto source = MediaSource::openPartial (partFile, totalFrames);
    if (source)
        pool[hash] = source;
    return source;
}

MediaPool::Audio MediaPool::completeDownload (const std::filesystem::path& mediaDir, const std::string& hash, std::string* error)
{
    if (auto existing = find (hash); existing && ! existing->partial())
        return existing;
    pool.erase (hash);
    return load (mediaDir, hash, error);
}

bool MediaPool::available (const std::string& hash) const
{
    const auto source = find (hash);
    return source != nullptr && ! source->partial();
}

MediaPool::Audio MediaPool::find (const std::string& hash) const
{
    const auto it = pool.find (hash);
    return it == pool.end() ? nullptr : it->second;
}
} // namespace weft::engine
