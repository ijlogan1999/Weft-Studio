#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace weft::media
{
/** The blob hash of a file's bytes: "b3:" + 64 hex digits of its BLAKE3 hash (03 §4.5). */
std::optional<std::string> hashFile (const std::filesystem::path& file, std::string* error = nullptr);
} // namespace weft::media
