// weft-cli: headless driver for engine and sync tests. Milestone 0 offers
// `version` and `null-render`; later milestones add project and sync commands.

#include "weft/core/Version.h"
#include "weft/engine/devices/NullDevice.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <string_view>

namespace
{
class TestTone final : public weft::engine::AudioCallback
{
public:
    void prepare (const weft::engine::DeviceConfig& config) override { sampleRate = config.sampleRate; }

    void process (const float* const*, float* const* outputs, int numFrames) noexcept override
    {
        for (int i = 0; i < numFrames; ++i)
        {
            const auto sample = static_cast<float> (0.25 * std::sin (phase));
            phase += 2.0 * std::numbers::pi * 440.0 / sampleRate;
            outputs[0][i] = sample;
            peak = std::max (peak, std::abs (sample));
        }
    }

    double sampleRate = 48000.0;
    double phase = 0.0;
    float peak = 0.0f;
};

int usage()
{
    std::fputs ("usage: weft-cli version\n"
                "       weft-cli null-render [seconds] [sample-rate] [block-size]\n",
                stderr);
    return 2;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 2)
        return usage();

    const std::string_view command = argv[1];
    if (command == "version")
    {
        std::printf ("%.*s %.*s\n", static_cast<int> (weft::productName.size()), weft::productName.data(),
                     static_cast<int> (weft::versionString().size()), weft::versionString().data());
        return 0;
    }

    if (command == "null-render")
    {
        const double seconds = argc > 2 ? std::atof (argv[2]) : 1.0;
        weft::engine::DeviceConfig config;
        config.sampleRate = argc > 3 ? std::atof (argv[3]) : 48000.0;
        config.maxBlockSize = argc > 4 ? std::atoi (argv[4]) : 256;
        config.numInputChannels = 0;
        config.numOutputChannels = 1;

        weft::engine::NullDevice device;
        TestTone tone;
        if (! device.open (config) || ! device.start (tone))
            return usage();
        device.setDeadlineSimulation (true);
        device.render (static_cast<std::int64_t> (seconds * config.sampleRate));
        device.stop();

        const auto& s = device.stats();
        std::printf ("blocks=%lld frames=%lld deadline_misses=%lld peak=%.4f\n", static_cast<long long> (s.blocksProcessed),
                     static_cast<long long> (s.framesProcessed), static_cast<long long> (s.deadlineMisses), static_cast<double> (tone.peak));
        return 0;
    }

    return usage();
}
