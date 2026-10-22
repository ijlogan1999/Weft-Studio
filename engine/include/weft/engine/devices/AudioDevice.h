#pragma once

#include <string>

namespace weft::engine
{
struct DeviceConfig
{
    double sampleRate = 48000.0;
    int maxBlockSize = 256;
    int numInputChannels = 2;
    int numOutputChannels = 2;
    /** Driver-reported latencies in samples; recordings shift earlier by their sum (F-REC-9). */
    int inputLatency = 0;
    int outputLatency = 0;
};

/** Implemented by the engine. process() runs on the audio thread and must be
    real-time safe: no locks, allocation, I/O or exceptions. */
class AudioCallback
{
public:
    virtual ~AudioCallback() = default;

    /** Called on a non-audio thread before the first process() call. Allocate here. */
    virtual void prepare (const DeviceConfig& config) = 0;

    /** numFrames is between 1 and config.maxBlockSize and can vary between calls. */
    virtual void process (const float* const* inputs, float* const* outputs, int numFrames) noexcept = 0;

    /** Called on a non-audio thread after the last process() call. */
    virtual void release() {}
};

/** Device abstraction. Implementations: NullDevice (tests, headless CLI) and,
    from Milestone 2, a JUCE-backed device for WASAPI and ASIO. */
class AudioDevice
{
public:
    virtual ~AudioDevice() = default;

    virtual std::string name() const = 0;
    virtual bool open (const DeviceConfig& config) = 0;
    virtual void close() = 0;
    virtual bool start (AudioCallback& callback) = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    virtual DeviceConfig config() const = 0;
    /** The device stopped by itself after start() (unplugged, or the driver failed). It stays
        failed until the next open(). Polled on the message thread. */
    virtual bool failed() const { return false; }
};
} // namespace weft::engine
