// The JUCE device layer. Containers and CI runners usually have no audio
// hardware, so the open/play part is skipped there; real devices are [HW] tests.

#include "weft/engine/devices/JuceAudioDevice.h"
#include "weft/rt/AudioThread.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace weft::engine;

namespace
{
class Counter final : public AudioCallback
{
public:
    void prepare (const DeviceConfig&) override { ++prepared; }
    void process (const float* const*, float* const* outputs, int numFrames) noexcept override
    {
        onAudioThread = onAudioThread && weft::rt::isAudioThread();
        for (int ch = 0; ch < channels; ++ch)
            std::fill_n (outputs[ch], numFrames, 0.0f);
        frames += numFrames;
    }
    void release() override { ++released; }
    int channels = 2, prepared = 0, released = 0;
    std::atomic<std::int64_t> frames { 0 };
    std::atomic<bool> onAudioThread { true };
};
} // namespace

TEST_CASE ("JUCE device layer lists drivers and runs the default device", "[E][devices][juce]")
{
    const auto types = JuceAudioDevice::driverTypes();
#if defined(_WIN32)
    CHECK (JuceAudioDevice::defaultDriverType() == "Windows Audio");
#endif
    JuceAudioDevice device ("No Such Driver");
    CHECK_FALSE (device.open ({}));
    CHECK_FALSE (device.lastError().empty());

    JuceAudioDevice def;
    DeviceConfig cfg;
    cfg.numInputChannels = 0;
    if (types.empty() || ! def.open (cfg))
        SKIP ("No audio device here: " << def.lastError());

    Counter counter;
    counter.channels = def.config().numOutputChannels;
    REQUIRE (def.start (counter));
    for (int i = 0; i < 100 && counter.frames.load() == 0; ++i)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    def.stop();
    CHECK (counter.prepared == 1);
    CHECK (counter.released == 1);
    CHECK (counter.frames.load() > 0);
    CHECK (counter.onAudioThread.load());
}
