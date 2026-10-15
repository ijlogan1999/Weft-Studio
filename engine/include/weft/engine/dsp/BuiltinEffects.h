#pragma once

// The built-in effects (F-PLG-10): original designs from textbook DSP. Each processes
// one or two channels in place and reports zero latency.

#include "weft/engine/dsp/BuiltinProcessor.h"

#include <array>
#include <cmath>
#include <vector>

namespace weft::engine
{
/** A second-order IIR section (RBJ cookbook forms), transposed direct form II. */
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    std::array<double, 2> z1 {}, z2 {};

    enum class Type : std::uint8_t
    {
        highPass,
        lowPass,
        peak,
        lowShelf,
        highShelf
    };
    void design (Type type, double rate, double freq, double q, double gainDb) noexcept;
    float run (int ch, float x) noexcept
    {
        const double y = b0 * x + z1[static_cast<std::size_t> (ch)];
        z1[static_cast<std::size_t> (ch)] = b1 * x - a1 * y + z2[static_cast<std::size_t> (ch)];
        z2[static_cast<std::size_t> (ch)] = b2 * x - a2 * y;
        if (std::abs (z1[static_cast<std::size_t> (ch)]) < 1e-20)
            z1[static_cast<std::size_t> (ch)] = 0;
        if (std::abs (z2[static_cast<std::size_t> (ch)]) < 1e-20)
            z2[static_cast<std::size_t> (ch)] = 0;
        return static_cast<float> (y);
    }
    void reset() noexcept { z1 = {}, z2 = {}; }
};

/** Channel EQ: high-pass, low shelf, two peaks, high shelf, low-pass (12 dB/oct filters). */
class WeftEq final : public BuiltinProcessor
{
public:
    static constexpr const char* pluginUid = "weft.eq";
    WeftEq();
    void prepare (double sampleRate, int maxBlockSize) override;
    void process (std::span<const MidiEvent>, float* const* io, int numChannels, int numFrames) noexcept override;

private:
    void design() noexcept;
    double rate = 48000;
    std::array<Biquad, 6> bands {};
    std::array<bool, 6> active {};
};

/** Feed-forward compressor: peak detector, hard knee, attack/release on the gain reduction. */
class WeftCompressor final : public BuiltinProcessor
{
public:
    static constexpr const char* pluginUid = "weft.compressor";
    WeftCompressor();
    void prepare (double sampleRate, int maxBlockSize) override;
    void process (std::span<const MidiEvent>, float* const* io, int numChannels, int numFrames) noexcept override;
    /** Current gain reduction in dB (for a meter), positive. */
    float reductionDb() const noexcept { return reduction; }

private:
    double rate = 48000;
    float envelope = 0, reduction = 0;
    float attackCoef = 0, releaseCoef = 0, detectorCoef = 0;
};

/** Stereo delay with feedback; time up to 2 s. */
class WeftDelay final : public BuiltinProcessor
{
public:
    static constexpr const char* pluginUid = "weft.delay";
    WeftDelay();
    void prepare (double sampleRate, int maxBlockSize) override;
    void process (std::span<const MidiEvent>, float* const* io, int numChannels, int numFrames) noexcept override;

private:
    double rate = 48000;
    std::array<std::vector<float>, 2> lines;
    std::size_t write = 0;
};

/** Algorithmic reverb: pre-delay, eight damped feedback combs per side, then four all-passes. */
class WeftReverb final : public BuiltinProcessor
{
public:
    static constexpr const char* pluginUid = "weft.reverb";
    WeftReverb();
    void prepare (double sampleRate, int maxBlockSize) override;
    void process (std::span<const MidiEvent>, float* const* io, int numChannels, int numFrames) noexcept override;

private:
    struct Comb
    {
        std::vector<float> buffer;
        std::size_t pos = 0;
        float feedback = 0, damp = 0, store = 0;
    };
    struct AllPass
    {
        std::vector<float> buffer;
        std::size_t pos = 0;
    };
    void design() noexcept;
    double rate = 48000;
    std::array<std::array<Comb, 8>, 2> combs;
    std::array<std::array<AllPass, 4>, 2> allPasses;
    std::vector<float> preDelay;
    std::size_t preWrite = 0;
};

/** Gain utility: level and polarity. */
class WeftGain final : public BuiltinProcessor
{
public:
    static constexpr const char* pluginUid = "weft.gain";
    WeftGain();
    void prepare (double, int) override {}
    void process (std::span<const MidiEvent>, float* const* io, int numChannels, int numFrames) noexcept override;
};

/**
    Stands in for a plug-in that isn't installed (F-PLG-15): passes audio through (an
    instrument placeholder is silent) and never changes the model's state.
*/
class PlaceholderProcessor final : public Processor
{
public:
    explicit PlaceholderProcessor (bool isInstrument) : instrument (isInstrument) {}
    void prepare (double, int) override {}
    void process (std::span<const MidiEvent>, float* const* io, int numChannels, int numFrames) noexcept override;
    bool isPlaceholder() const noexcept override { return true; }

private:
    bool instrument;
};

/** The built-in plug-ins, effects then the instrument (the insert menu lists them first, W-6). */
struct BuiltinInfo
{
    const char* uid;
    const char* name;
    bool instrument;
};
const std::vector<BuiltinInfo>& builtinPlugins();
/** A new built-in processor, or null for an unknown uid. */
std::shared_ptr<Processor> createBuiltin (const std::string& uid);
} // namespace weft::engine
