#pragma once

#include "weft/engine/devices/AudioDevice.h"

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace weft::engine
{
/**
    An audio device with no hardware. The caller drives the callback
    synchronously with render(), which makes engine tests deterministic and lets
    the headless CLI run anywhere.

    Inputs come from an optional generator; outputs are captured up to a fixed
    capacity. With deadline simulation on, each process() call is timed against
    the block's real-time period and late calls count as deadline misses
    (used by AT-RT-2 and AT-TRN-17).
*/
class NullDevice final : public AudioDevice
{
public:
    /** Fills dest with numFrames input samples for channel, starting at absolute frame startFrame. */
    using InputGenerator = std::function<void (int channel, std::int64_t startFrame, float* dest, int numFrames)>;

    struct Stats
    {
        std::int64_t blocksProcessed = 0;
        std::int64_t framesProcessed = 0;
        std::int64_t deadlineMisses = 0;
    };

    std::string name() const override { return "Null Device"; }
    bool open (const DeviceConfig& config) override;
    void close() override;
    bool start (AudioCallback& callback) override;
    void stop() override;
    bool isRunning() const override { return callback != nullptr; }
    DeviceConfig config() const override { return cfg; }

    void setInputGenerator (InputGenerator generator) { inputGenerator = std::move (generator); }

    /** Keeps the first capacityFrames output frames of each channel. Call before render(). */
    void setCaptureCapacity (std::int64_t capacityFrames);
    std::span<const float> capturedOutput (int channel) const;

    void setDeadlineSimulation (bool enabled) noexcept { simulateDeadlines = enabled; }

    /** Renders numFrames in blocks of blockSize frames (0 = maxBlockSize); the last block may be shorter. */
    bool render (std::int64_t numFrames, int blockSize = 0);

    /** Renders one block per entry, with the given sizes (for variable-size callback tests). */
    bool renderBlocks (std::span<const int> blockSizes);

    const Stats& stats() const noexcept { return counters; }

private:
    bool renderBlock (int numFrames);

    DeviceConfig cfg;
    bool isOpen = false;
    AudioCallback* callback = nullptr;
    InputGenerator inputGenerator;
    bool simulateDeadlines = false;

    std::vector<std::vector<float>> inputBuffers, outputBuffers;
    std::vector<const float*> inputPointers;
    std::vector<float*> outputPointers;
    std::vector<std::vector<float>> capture;
    std::int64_t captureCapacity = 0;
    Stats counters;
};
} // namespace weft::engine
