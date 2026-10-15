#pragma once

// Standard MIDI File reading (type 0 and 1) for import (F-ARR-27). Positions stay in
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

struct MidiFileData
{
    int format = 1;
    int ticksPerQuarter = 480;
    std::vector<MidiFileTrack> tracks;     // in file order, including ones without notes
    std::vector<MidiFileTempo> tempos;     // sorted; empty means 120 BPM
    std::vector<MidiFileSignature> signatures;
};

/** Parses SMF bytes. Notes still held at the end of a track end there. SMPTE time division is rejected. */
std::optional<MidiFileData> parseMidiFile (std::span<const std::uint8_t> bytes, std::string* error = nullptr);
std::optional<MidiFileData> readMidiFile (const std::filesystem::path& file, std::string* error = nullptr);
} // namespace weft::media
