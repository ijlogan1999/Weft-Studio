#pragma once

// Waveform overviews (spec 04 §6.2, D-UI-14): a min/max/RMS pyramid per media file, so a region
// draws crisply at any zoom: as a filled min/max outline when many samples share a pixel, and as
// a smooth line through the finest buckets when zoomed in past them.

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <vector>

namespace weft::ui
{
class WaveSummary
{
public:
    /** Frames per bucket at the finest level; each coarser level is 4 times wider. */
    static constexpr int baseFrames = 32;
    static constexpr int levelFactor = 4;

    struct Bucket
    {
        float lo = 0, hi = 0, rms = 0;
    };

    /** Builds from all channels: lo/hi are the extremes over channels, RMS their mean power. */
    void begin (std::int64_t totalFrames);
    void add (const float* const* channels, int numChannels, std::int64_t firstFrame, std::int64_t numFrames);
    void finish();

    bool empty() const noexcept { return levels.empty() || levels.front().empty(); }
    std::int64_t frames() const noexcept { return length; }
    /** The extremes and RMS of [from, to) frames, read from the coarsest level that fits. */
    Bucket range (double fromFrame, double toFrame) const;
    /** The finest level at a frame, interpolated between bucket centres. */
    Bucket at (double frame) const;

private:
    struct Packed
    {
        std::int16_t lo, hi, rms;
    };
    static std::int16_t pack (float v) noexcept;
    static float unpack (std::int16_t v) noexcept { return static_cast<float> (v) / 32767.0f; }
    Bucket get (std::size_t level, std::int64_t index) const;

    std::vector<std::vector<Packed>> levels;
    std::vector<double> power; // building: summed power per base bucket
    std::vector<int> counted;  // building: samples counted per base bucket
    std::int64_t length = 0;
};

/** Draws `summary` into `area`: `firstFrame` at area's left, `framesPerPixel` across. */
struct WaveformStyle
{
    juce::Colour peak, rms;
    float gain = 1.0f;
    /** Pixels of the area that are inside the source (outside is left empty). */
    float startX = -1e9f, endX = 1e9f;
};
void drawWaveform (juce::Graphics&, const WaveSummary&, juce::Rectangle<float> area, double firstFrame, double framesPerPixel, const WaveformStyle&);
} // namespace weft::ui
