#pragma once

// Putting files into a project's Media folder under their content hash (D-W10-1).

#include "weft/media/WavFile.h"

#include <filesystem>
#include <optional>
#include <string>

namespace weft::media
{
/**
    Hashes `file` and moves it to `<mediaDir>/<hex>.wav` (or deletes it when that
    file is already there). Returns the blob hash.
*/
std::optional<std::string> storeMediaFile (const std::filesystem::path& file, const std::filesystem::path& mediaDir, std::string* error = nullptr);

/** Writes audio as a WAV in mediaDir and stores it under its hash. */
std::optional<std::string> storeMedia (const AudioData& audio, SampleFormat format, const std::filesystem::path& mediaDir, std::string* error = nullptr);
} // namespace weft::media
