#pragma once

#include "weft/core/Id.h"
#include "weft/engine/rt/SpscQueue.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace weft::engine
{
/** A MIDI message at a frame offset within the block passed to Processor::process. */
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

/** A parameter a processor exposes (F-PLG-4 generic editor, F-PLG-14 automation). */
struct ParameterInfo
{
    std::string id;   // stable across sessions: the model's params key
    std::string name;
    std::string unit; // "dB", "Hz", "ms", "%", ...
    double defaultValue = 0; // normalized
    int steps = 0;           // > 1: a choice of this many values (presets, filter types)
};

/**
    Anything that sits in a channel strip's instrument slot or an insert slot (03 §5):
    the built-in instrument and effects, hosted VST3 plug-ins (M6) and the placeholder
    for a plug-in that isn't installed (F-PLG-15). process() runs on the audio thread
    and follows its rules; everything else runs on the message thread.
*/
class Processor
{
public:
    virtual ~Processor() = default;
    /** Message thread, before process() and again when the device changes. */
    virtual void prepare (double sampleRate, int maxBlockSize) = 0;
    /**
        Instruments write (not add) numFrames into each channel. Effects process the
        channels in place. Events are sorted by offset; effects normally get none.
    */
    virtual void process (std::span<const MidiEvent> events, float* const* io, int numChannels, int numFrames) noexcept = 0;

    /** Samples of delay the processor adds (F-MIX-13). The engine re-reads it on refresh. */
    virtual int latencySamples() const noexcept { return 0; }
    virtual std::vector<ParameterInfo> parameters() const { return {}; }
    /** Sets a parameter by index, normalized 0..1. Safe while process() runs. */
    virtual void setParameter (int, double) noexcept {}
    virtual double parameter (int) const noexcept { return 0; }
    /** Text for a normalized value of a parameter, e.g. "-6.0 dB". */
    virtual std::string parameterText (int index, double normalized) const;
    /** The opaque state chunk (F-PLG-5). */
    virtual std::vector<std::uint8_t> state() const { return {}; }
    virtual bool setState (std::span<const std::uint8_t>) { return true; }
    /** A placeholder keeps the model's state untouched and passes audio (F-PLG-15). */
    virtual bool isPlaceholder() const noexcept { return false; }

    /** The parameter with this id, or -1. */
    int parameterIndex (const std::string& id) const;
};

/** Instruments are processors that ignore their input and respond to MIDI. */
using Instrument = Processor;

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
    const std::shared_ptr<Instrument>& shared() const noexcept { return inst; }

    // Message thread: which plug-in instance this slot was made for (empty: set by hand), and
    // the model state and parameters last applied to it.
    std::string key;
    struct Applied
    {
        std::optional<std::vector<std::uint8_t>> state;
        std::map<std::string, double> params;
    } applied;

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

    /** Audition (F-PRL-11): a note the editor plays on its own, outside the timeline. */
    struct Audition
    {
        std::uint8_t channel = 1, pitch = 60, velocity = 100;
        std::int32_t frames = 0; // how long it sounds
    };
    rt::SpscQueue<Audition, 64> auditions; // message thread pushes, audio thread pops
    struct Auditioning
    {
        std::int64_t framesLeft = 0;
        std::uint8_t channel = 1, pitch = 0;
    };
    static constexpr std::size_t maxAuditions = 16;
    std::array<Auditioning, maxAuditions> auditioning {};
    std::size_t numAuditioning = 0;

private:
    std::shared_ptr<Instrument> inst;
};

/** Instruments by track id. Message thread only. */
class InstrumentRack
{
public:
    /** Sets or replaces a track's instrument (prepared now if the device is known). Null removes it. */
    std::shared_ptr<InstrumentSlot> set (const Id& trackId, std::shared_ptr<Instrument> instrument, std::string key = {});
    std::shared_ptr<InstrumentSlot> find (const Id& trackId) const;
    void prepare (double sampleRate, int maxBlockSize);
    /** Removes slots made for a plug-in (non-empty key) whose track is not in `keep`. */
    void prune (const std::set<Id>& keep);

private:
    std::map<Id, std::shared_ptr<InstrumentSlot>> slots;
    std::optional<std::pair<double, int>> config;
};

/** An insert's processor and what was last applied to it (message thread). */
struct EffectSlot
{
    std::shared_ptr<Processor> processor;
    std::string key; // format + uid it was made for
    InstrumentSlot::Applied applied;
};

/** Insert processors by plug-in instance id. Message thread only; snapshots share the slots. */
class EffectRack
{
public:
    std::shared_ptr<EffectSlot> find (const Id& pluginId) const;
    std::shared_ptr<EffectSlot> set (const Id& pluginId, std::shared_ptr<Processor> processor, std::string key);
    void prepare (double sampleRate, int maxBlockSize);
    void prune (const std::set<Id>& keep);
    const std::map<Id, std::shared_ptr<EffectSlot>>& all() const noexcept { return slots; }

private:
    std::map<Id, std::shared_ptr<EffectSlot>> slots;
    std::optional<std::pair<double, int>> config;
};
} // namespace weft::engine
