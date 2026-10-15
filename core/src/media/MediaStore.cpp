#include "weft/media/MediaStore.h"

#include "weft/media/Hash.h"

#include <atomic>
#include <chrono>

namespace weft::media
{
std::optional<std::string> storeMediaFile (const std::filesystem::path& file, const std::filesystem::path& mediaDir, std::string* error)
{
    auto hash = hashFile (file, error);
    if (! hash)
        return std::nullopt;
    std::error_code ec;
    std::filesystem::create_directories (mediaDir, ec);
    const auto dest = mediaDir / mediaFileName (*hash);
    if (std::filesystem::exists (dest, ec))
    {
        std::filesystem::remove (file, ec);
        return hash;
    }
    std::filesystem::rename (file, dest, ec);
    if (ec)
    {
        ec.clear();
        std::filesystem::copy_file (file, dest, ec);
        if (ec)
        {
            if (error != nullptr)
                *error = "Couldn't move the file into Media: " + ec.message();
            return std::nullopt;
        }
        std::filesystem::remove (file, ec);
    }
    return hash;
}

std::optional<std::string> storeMedia (const AudioData& audio, SampleFormat format, const std::filesystem::path& mediaDir, std::string* error)
{
    static std::atomic<std::uint64_t> counter { 0 };
    std::error_code ec;
    std::filesystem::create_directories (mediaDir, ec);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temp = mediaDir / ("incoming-" + std::to_string (stamp) + "-" + std::to_string (counter++) + ".wav.part");
    if (! writeWav (temp, audio, format, error))
    {
        std::filesystem::remove (temp, ec);
        return std::nullopt;
    }
    return storeMediaFile (temp, mediaDir, error);
}
} // namespace weft::media
