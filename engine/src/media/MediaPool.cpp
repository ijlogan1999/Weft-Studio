#include "weft/engine/media/MediaPool.h"

namespace weft::engine
{
void MediaPool::add (const std::string& hash, media::AudioData audio)
{
    pool[hash] = std::make_shared<const media::AudioData> (std::move (audio));
}

MediaPool::Audio MediaPool::load (const std::filesystem::path& mediaDir, const std::string& hash, std::string* error)
{
    if (auto existing = find (hash))
        return existing;
    auto audio = media::readWav (mediaDir / media::mediaFileName (hash), error);
    if (! audio)
        return nullptr;
    auto shared = std::make_shared<const media::AudioData> (std::move (*audio));
    pool[hash] = shared;
    return shared;
}

MediaPool::Audio MediaPool::find (const std::string& hash) const
{
    const auto it = pool.find (hash);
    return it == pool.end() ? nullptr : it->second;
}
} // namespace weft::engine
