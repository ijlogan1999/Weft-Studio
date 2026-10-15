#include "weft/engine/devices/NullDevice.h"
#include "weft/rt/AudioThread.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <thread>
#include <vector>

using namespace weft::engine;

namespace
{
/** Copies input channel 0 to every output and records what it saw. Preallocates in prepare(). */
class Recorder final : public AudioCallback
{
public:
    void prepare (const DeviceConfig& c) override
    {
        config = c;
        ++prepareCalls;
        blockSizes.reserve (1024);
    }

    void process (const float* const* inputs, float* const* outputs, int numFrames) noexcept override
    {
        sawAudioThreadFlag = sawAudioThreadFlag && weft::rt::isAudioThread();
        blockSizes.push_back (numFrames); // reserved in prepare(), so no allocation in these tests
        for (int ch = 0; ch < config.numOutputChannels; ++ch)
            for (int i = 0; i < numFrames; ++i)
                outputs[ch][i] = config.numInputChannels > 0 ? inputs[0][i] : 0.0f;
        if (sleepPerBlock.count() > 0)
            std::this_thread::sleep_for (sleepPerBlock);
    }

    void release() override { ++releaseCalls; }

    DeviceConfig config;
    int prepareCalls = 0, releaseCalls = 0;
    bool sawAudioThreadFlag = true;
    std::vector<int> blockSizes;
    std::chrono::microseconds sleepPerBlock { 0 };
};

DeviceConfig makeConfig (int maxBlock)
{
    DeviceConfig c;
    c.sampleRate = 48000.0;
    c.maxBlockSize = maxBlock;
    c.numInputChannels = 1;
    c.numOutputChannels = 2;
    return c;
}
} // namespace

TEST_CASE ("NullDevice drives prepare, process and release", "[E][null-device]")
{
    NullDevice device;
    Recorder rec;
    REQUIRE (device.open (makeConfig (256)));
    REQUIRE (device.start (rec));
    CHECK (device.isRunning());
    CHECK (rec.prepareCalls == 1);

    REQUIRE (device.render (1000));
    CHECK (rec.blockSizes == std::vector<int> { 256, 256, 256, 232 });
    CHECK (device.stats().framesProcessed == 1000);
    CHECK (device.stats().blocksProcessed == 4);
    CHECK (rec.sawAudioThreadFlag);
    CHECK_FALSE (weft::rt::isAudioThread());

    device.stop();
    CHECK (rec.releaseCalls == 1);
    CHECK_FALSE (device.isRunning());
}

TEST_CASE ("NullDevice delivers generated input and captures output", "[E][null-device]")
{
    NullDevice device;
    Recorder rec;
    REQUIRE (device.open (makeConfig (441))); // odd block size (AT-RT-3 groundwork)
    device.setInputGenerator ([] (int, std::int64_t start, float* dest, int n) {
        for (int i = 0; i < n; ++i)
            dest[i] = static_cast<float> (start + i);
    });
    device.setCaptureCapacity (1000);
    REQUIRE (device.start (rec));
    REQUIRE (device.render (2000));

    for (int ch = 0; ch < 2; ++ch)
    {
        const auto out = device.capturedOutput (ch);
        REQUIRE (out.size() == 1000);
        for (std::size_t i = 0; i < out.size(); ++i)
            REQUIRE (out[i] == static_cast<float> (i));
    }
}

TEST_CASE ("NullDevice supports variable block sizes up to the maximum", "[E][null-device]")
{
    NullDevice device;
    Recorder rec;
    REQUIRE (device.open (makeConfig (2048)));
    REQUIRE (device.start (rec));

    const std::array<int, 6> sizes { 32, 2048, 441, 1, 1024, 7 };
    REQUIRE (device.renderBlocks (sizes));
    CHECK (rec.blockSizes == std::vector<int> (sizes.begin(), sizes.end()));

    const std::array<int, 1> tooBig { 4096 };
    CHECK_FALSE (device.renderBlocks (tooBig));
}

TEST_CASE ("NullDevice counts deadline misses when simulation is on", "[E][null-device]")
{
    NullDevice device;
    Recorder rec;
    REQUIRE (device.open (makeConfig (48))); // 1 ms blocks
    REQUIRE (device.start (rec));

    device.setDeadlineSimulation (true);
    REQUIRE (device.render (48 * 4));
    const auto fastMisses = device.stats().deadlineMisses;

    rec.sleepPerBlock = std::chrono::milliseconds (5);
    REQUIRE (device.render (48 * 3));
    CHECK (device.stats().deadlineMisses - fastMisses == 3);
}

TEST_CASE ("NullDevice rejects invalid use", "[E][null-device]")
{
    NullDevice device;
    Recorder rec;
    CHECK_FALSE (device.start (rec)); // not open
    auto bad = makeConfig (0);
    CHECK_FALSE (device.open (bad));
    REQUIRE (device.open (makeConfig (64)));
    CHECK_FALSE (device.render (64)); // not started
}
