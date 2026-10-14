#pragma once

#include "weft/core/Id.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>

namespace weft::engine
{
/** A MIDI message at a frame offset within the block passed to Instrument::process. */
struct MidiEvent
{
    int offset = 0;
    std::uint8_t status = 0, data1 = 0, data2 = 0;

    static MidiEvent noteOn (int offset, int channel, int pitch, int velocity) noexcept
    {
        return { offset, static_cast<std::uint8_t> (0x90 | ((channel - 1) & 0x0f)), static_cast<std::uint8_t> (pitch & 0x7f), static_cast<std::uint8_t> (velocity & 0x7f) };
    }
    static MidiEvent noteOff (int offset, int channel, int pitch, int velocity = 64) noexcept
    {
        return { offset, static_cast<std::uint8_t> (0x80 | ((channel - 1) & 0x0f)), static_cast<std::uint8_t> (pitch & 0x7f), static_cast<std::uint8_t> (velocity & 0x7f) };
    }
    bool isNoteOn() const noexcept { return (status & 0xf0) == 0x90 && data2 > 0; }
    bool isNoteOff() const noexcept { return (status & 0xf0) == 0x80 || ((status & 0xf0) == 0x90 && data2 == 0); }
};

/**
    A sound source driven by MIDI: the built-in Weft Keys (M3) and hosted VST3
    instruments (M6) implement this. process() runs on the audio thread.
*/
class Instrument
{
public:
    virtual ~Instrument() = default;
    /** Message thread, before process() and again when the device changes. */
    virtual void prepare (double sampleRate, int maxBlockSize) = 0;
    /** Writes (not adds) numFrames into each output channel. Events are sorted by offset. */
    virtual void process (std::span<const MidiEvent> events, float* const* outputs, int numChannels, int numFrames) noexcept = 0;
};

/**
    An instrument plus the audio-thread state that must outlive engine snapshots:
    the notes it is currently sounding and when they end. Snapshots point at
    slots; the slot table below owns them (ARCHITECTURE §3).
*/
class InstrumentSlot
{
public:
    explicit InstrumentSlot (std::shared_ptr<Instrument> instrument) : inst (std::move (instrument)) {}
    Instrument& instrument() const noexcept { return *inst; }

    // Audio thread only ------------------------------------------------------
    struct ActiveNote
    {
        std::int64_t endSample = 0; // timeline sample at device rate
        std::uint8_t channel = 1, pitch = 0;
    };
    static constexpr std::size_t maxActiveNotes = 256;
    std::array<ActiveNote, maxActiveNotes> active {};
    std::size_t numActive = 0;
    bool flushPending = false; // send note-offs for everything at the next process call

private:
    std::shared_ptr<Instrument> inst;
};

/** Instruments by track id. Message thread only. */
class InstrumentRack
{
public:
    /** Sets or replaces a track's instrument (prepared now if the device is known). Null removes it. */
    void set (const Id& trackId, std::shared_ptr<Instrument> instrument);
    std::shared_ptr<InstrumentSlot> find (const Id& trackId) const;
    void prepare (double sampleRate, int maxBlockSize);

private:
    std::map<Id, std::shared_ptr<InstrumentSlot>> slots;
    std::optional<std::pair<double, int>> config;
};
} // namespace weft::engine
