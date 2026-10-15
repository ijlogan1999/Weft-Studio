#include "weft/media/Hash.h"

#include "weft/rt/AudioThread.h"

#include <weft_crdt.h>

#include <array>
#include <fstream>
#include <memory>

namespace weft::media
{
std::optional<std::string> hashFile (const std::filesystem::path& file, std::string* error)
{
    rt::noteBlockingCall();
    std::ifstream in (file, std::ios::binary);
    if (! in)
    {
        if (error != nullptr)
            *error = "Couldn't open " + file.string();
        return std::nullopt;
    }
    const std::unique_ptr<WeftHasher, decltype (&weft_hasher_free)> hasher (weft_hasher_new(), &weft_hasher_free);
    std::array<char, 1 << 16> buffer {};
    while (in)
    {
        in.read (buffer.data(), buffer.size());
        const auto n = static_cast<std::size_t> (in.gcount());
        if (n > 0)
            weft_hasher_update (hasher.get(), reinterpret_cast<const std::uint8_t*> (buffer.data()), n);
    }
    std::array<std::uint8_t, 64> hex {};
    weft_hasher_finish_hex (hasher.get(), hex.data());
    return "b3:" + std::string (hex.begin(), hex.end());
}
} // namespace weft::media
