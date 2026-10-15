#pragma once

// Plug-in state chunks over 64 KB live outside the document as blobs (03 §5.2), in the
// project's Blobs/ folder named by content hash (D-DM-20).

#include "weft/doc/Value.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace weft::media
{
inline constexpr std::size_t maxInlineStateBytes = 64 * 1024;

/** `<blobDir>/<hex>.bin` for "b3:<hex>". */
std::filesystem::path blobFile (const std::filesystem::path& blobDir, const std::string& hash);
/** Writes bytes as a blob (unless it's already there) and returns its hash. */
std::optional<std::string> storeBlob (std::span<const std::uint8_t> bytes, const std::filesystem::path& blobDir, std::string* error = nullptr);
std::optional<std::vector<std::uint8_t>> loadBlob (const std::string& hash, const std::filesystem::path& blobDir);

/** The document value for a plug-in state: the bytes up to 64 KB, else a blob's hash. */
std::optional<doc::Value> pluginStateValue (std::span<const std::uint8_t> bytes, const std::filesystem::path& blobDir, std::string* error = nullptr);
/** The bytes a plug-in state value stands for; nullopt when it's empty or its blob is missing. */
std::optional<std::vector<std::uint8_t>> pluginStateBytes (const doc::Value& state, const std::filesystem::path& blobDir);
} // namespace weft::media
