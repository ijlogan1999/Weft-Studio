#pragma once

// Standard MIDI File reading (type 0 and 1) for import (F-ARR-27) and writing for export (L-EXP-3). Positions stay in
// the file's own ticks; the importer converts them to 960 PPQN.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace weft::media
{
struct MidiFileNote
{
    std::int64_t tick = 0, length = 0;
    int channel = 1; // 1-16
    int pitch = 60, velocity = 100, releaseVelocity = 64;
};

struct MidiFileController
{
    std::int64_t tick = 0;
    std::string kind = "cc"; // cc | pitchBend | channelPressure | polyPressure | programChange
    int number = 0, value = 0, channel = 1;
};

struct MidiFileTrack
{
    std::string name;
    std::vector<MidiFileNote> notes;
    std::vector<MidiFileController> controllers;
};

struct MidiFileTempo
{
    std::int64_t tick = 0;
    double bpm = 120.0;
};

struct MidiFileSignature
{
    std::int64_t tick = 0;
    int numerator = 4, denominator = 4;
};

struct MidiFileKey
{
    std::int64_t tick = 0;
    int fifths = 0; // sharps (positive) or flats (negative)
    bool minor = false;
};

struct MidiFileData
{
    int format = 1;
    int ticksPerQuarter = 480;
    std::vector<MidiFileTrack> tracks;     // in file order, including ones without notes
    std::vector<MidiFileTempo> tempos;     // sorted; empty means 120 BPM
    std::vector<MidiFileSignature> signatures;
    std::vector<MidiFileKey> keys;
};

/** Parses SMF bytes. Notes still held at the end of a track end there. SMPTE time division is rejected. */
std::optional<MidiFileData> parseMidiFile (std::span<const std::uint8_t> bytes, std::string* error = nullptr);
std::optional<MidiFileData> readMidiFile (const std::filesystem::path& file, std::string* error = nullptr);

/**
    SMF bytes for `data` (L-EXP-3): type 1 with a conductor track (tempos, signatures and keys)
    followed by data.tracks, or type 0 with everything in one track when format is 0.
    Note-offs at a tick go before note-ons there.
*/
std::vector<std::uint8_t> encodeMidiFile (const MidiFileData& data);
bool writeMidiFile (const std::filesystem::path& file, const MidiFileData& data, std::string* error = nullptr);
} // namespace weft::media
