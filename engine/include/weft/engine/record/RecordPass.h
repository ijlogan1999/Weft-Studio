#pragma once

// One recording pass (W-1, W-2): set up on the message thread, filled by the audio
// thread through lock-free FIFOs, drained to WAV files by the writer.

#include "weft/core/Id.h"

#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace weft::engine
{
/** Single-producer single-consumer planar float FIFO. write() is audio-thread safe. */
class AudioFifo
{
public:
    AudioFifo (int channels, std::int64_t capacityFrames);
    int channels() const noexcept { return static_cast<int> (data.size()); }
    /** Copies up to `frames` frames from src[c] + offset; returns how many fit. */
    std::int64_t write (const float* const* src, int offset, std::int64_t frames) noexcept;
    /** Copies up to maxFrames into dest[c]; returns how many. */
    std::int64_t read (float* const* dest, std::int64_t maxFrames) noexcept;
    std::int64_t available() const noexcept;

private:
    std::vector<std::vector<float>> data;
    std::int64_t capacity;
    std::atomic<std::int64_t> writePos { 0 }, readPos { 0 };
};

/** A MIDI message captured during recording, at its timeline sample. Status 0 marks a cycle wrap. */
struct CapturedMidi
{
    std::int64_t sample = 0;
    std::uint8_t status = 0, data1 = 0, data2 = 0;
};

struct RecordPass
{
    struct AudioLane
    {
        Id trackId;
        std::string trackName;
        int inputFirst = 0;
        int channels = 1;
        std::unique_ptr<AudioFifo> fifo;
        std::atomic<std::int64_t> dropped { 0 };
    };
    struct MidiLane
    {
        Id trackId;
        std::string trackName;
        int midiChannel = 0; // filter, 0 = all
        std::vector<CapturedMidi> events; // reserved up front; the audio thread never grows it
        std::atomic<std::size_t> count { 0 };
        std::atomic<std::int64_t> dropped { 0 };
    };

    std::vector<std::unique_ptr<AudioLane>> audio;
    std::vector<std::unique_ptr<MidiLane>> midi;

    // Set up by the message thread.
    std::int64_t recordStart = 0;     // device samples
    std::int64_t latency = 0;         // input + output + user offset (F-REC-9)
    bool punch = false;
    std::int64_t captureStart = 0, captureEnd = 0; // punch window with handles
    bool cycle = false;
    std::int64_t cycleStart = 0, cycleEnd = 0;

    // Written by the audio thread.
    std::atomic<std::int64_t> firstSample { LLONG_MIN }; // timeline sample of the first captured frame
    std::atomic<std::int64_t> frames { 0 };              // frames captured per audio lane
    static constexpr int maxWraps = 4096;
    std::array<std::int64_t, maxWraps> wraps {};         // `frames` at each cycle wrap
    std::atomic<int> numWraps { 0 };
    std::atomic<std::int64_t> stopSample { 0 };
    std::atomic<bool> finished { false };
    bool punchDone = false; // audio thread only: punch records the first pass through the range
};
} // namespace weft::engine
