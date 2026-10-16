#pragma once

#include <filesystem>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace weft::media
{
/** The blob hash of a file's bytes: "b3:" + 64 hex digits of its BLAKE3 hash (03 §4.5). */
std::optional<std::string> hashFile (const std::filesystem::path& file, std::string* error = nullptr);
/** The blob hash of bytes in memory. */
std::string hashBytes (std::span<const std::uint8_t> bytes);
} // namespace weft::media
