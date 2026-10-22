#pragma once

#include "weft/engine/devices/AudioDevice.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace weft::engine
{
/**
    Hardware audio through JUCE's device layer: WASAPI shared (the default,
    D-W0-2), WASAPI exclusive, and ASIO when built with WEFT_ASIO_SDK_DIR
    (D-ARCH-14). JUCE types stay inside the implementation.

    open/close/start/stop are called on the message thread. The device's
    callback thread is marked as the audio thread (AT-RT-1 checks).
*/
class JuceAudioDevice final : public AudioDevice
{
public:
    /** Driver types available on this machine, e.g. "Windows Audio", "ASIO". */
    static std::vector<std::string> driverTypes();
    /** Output (or input) device names for a driver type. */
    static std::vector<std::string> deviceNames (const std::string& driverType, bool inputs = false);
    /** The default driver type: WASAPI shared on Windows, the platform default elsewhere. */
    static std::string defaultDriverType();
    /** The device a driver type picks when none is named. */
    static std::string defaultDeviceName (const std::string& driverType, bool input = false);

    /** What a device can run at (Audio Setup, W-0 step 3-4). */
    struct Capabilities
    {
        std::vector<double> sampleRates;
        std::vector<int> bufferSizes;
        double currentRate = 0;
        int defaultBufferSize = 0;
    };
    static Capabilities capabilities (const std::string& driverType, const std::string& output, const std::string& input = {});

    /** Empty strings pick the default driver type and its default devices; an empty input uses the output's name. */
    explicit JuceAudioDevice (std::string driverType = {}, std::string deviceName = {}, std::string inputName = {});
    ~JuceAudioDevice() override;

    std::string name() const override;
    bool open (const DeviceConfig& config) override;
    void close() override;
    bool start (AudioCallback& callback) override;
    void stop() override;
    bool isRunning() const override;
    DeviceConfig config() const override;
    bool failed() const override;

    /** Why the last open() failed. */
    const std::string& lastError() const noexcept;
    /** Buffer under/overruns the driver reported since open (feeds the dropout counter). */
    std::int64_t xrunCount() const;
    /** Output latency in samples as reported by the driver. */
    int outputLatency() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace weft::engine
