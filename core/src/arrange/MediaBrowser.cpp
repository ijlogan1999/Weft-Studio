#include "weft/arrange/MediaBrowser.h"

#include <algorithm>
#include <map>

namespace weft::arrange
{
std::vector<MediaUsage> mediaUsage (const model::ProjectModel& m)
{
    std::map<std::string, MediaUsage> byHash;
    for (const auto& [hash, ref] : m.media)
        byHash[hash] = { hash, ref->originalName, ref->sampleRate > 0 ? static_cast<double> (ref->lengthSamples) / ref->sampleRate : 0.0, 0 };
    for (const auto& [id, r] : m.regions)
        if (! r->isMidi && ! r->audio.media.empty())
        {
            auto [it, added] = byHash.try_emplace (r->audio.media);
            if (added)
                it->second = { r->audio.media, "(missing)", 0.0, 0 };
            ++it->second.regions;
        }
    std::vector<MediaUsage> out;
    for (auto& [hash, u] : byHash)
        out.push_back (std::move (u));
    std::sort (out.begin(), out.end(), [] (const MediaUsage& a, const MediaUsage& b) { return std::tie (a.name, a.hash) < std::tie (b.name, b.hash); });
    return out;
}
} // namespace weft::arrange
