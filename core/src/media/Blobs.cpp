#include "weft/media/Blobs.h"

#include "weft/media/Hash.h"

#include <fstream>

namespace weft::media
{
std::filesystem::path blobFile (const std::filesystem::path& blobDir, const std::string& hash)
{
    const auto colon = hash.find (':');
    return blobDir / ((colon == std::string::npos ? hash : hash.substr (colon + 1)) + ".bin");
}

std::optional<std::string> storeBlob (std::span<const std::uint8_t> bytes, const std::filesystem::path& blobDir, std::string* error)
{
    const auto hash = hashBytes (bytes);
    const auto file = blobFile (blobDir, hash);
    std::error_code ec;
    if (std::filesystem::exists (file, ec))
        return hash;
    std::filesystem::create_directories (blobDir, ec);
    const auto temp = file.string() + ".tmp";
    {
        std::ofstream out (temp, std::ios::binary);
        out.write (reinterpret_cast<const char*> (bytes.data()), static_cast<std::streamsize> (bytes.size()));
        if (! out)
        {
            if (error != nullptr)
                *error = "Couldn't write " + temp;
            return std::nullopt;
        }
    }
    std::filesystem::rename (temp, file, ec);
    if (ec)
    {
        if (error != nullptr)
            *error = ec.message();
        return std::nullopt;
    }
    return hash;
}

std::optional<std::vector<std::uint8_t>> loadBlob (const std::string& hash, const std::filesystem::path& blobDir)
{
    std::ifstream in (blobFile (blobDir, hash), std::ios::binary);
    if (! in)
        return std::nullopt;
    std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    if (hashBytes (bytes) != hash)
        return std::nullopt; // damaged
    return bytes;
}

std::optional<doc::Value> pluginStateValue (std::span<const std::uint8_t> bytes, const std::filesystem::path& blobDir, std::string* error)
{
    if (bytes.size() <= maxInlineStateBytes)
        return doc::Value (doc::Value::Bytes (bytes.begin(), bytes.end()));
    if (const auto hash = storeBlob (bytes, blobDir, error))
        return doc::Value (*hash);
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> pluginStateBytes (const doc::Value& state, const std::filesystem::path& blobDir)
{
    if (state.isBytes())
        return state.asBytes().empty() ? std::nullopt : std::optional (state.asBytes());
    if (state.isString() && ! state.asString().empty())
        return loadBlob (state.asString(), blobDir);
    return std::nullopt;
}
} // namespace weft::media
