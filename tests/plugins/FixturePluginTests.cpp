#include <juce_audio_processors/juce_audio_processors.h>

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_approx.hpp>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int blockSize = 512;

std::unique_ptr<juce::AudioPluginInstance> loadFixture (const char* path)
{
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile (types, path);
    REQUIRE (types.size() == 1);

    juce::String error;
    auto instance = format.createInstanceFromDescription (*types[0], sampleRate, blockSize, error);
    INFO (error.toStdString());
    REQUIRE (instance != nullptr);
    instance->enableAllBuses();
    instance->prepareToPlay (sampleRate, blockSize);
    return instance;
}
} // namespace

TEST_CASE ("TestGain applies its gain parameter", "[E][fixtures]")
{
    auto plugin = loadFixture (WEFT_FIXTURE_TESTGAIN);
    auto* gain = plugin->getParameters()[0];
    gain->setValueNotifyingHost ((6.0f + 60.0f) / 72.0f); // +6 dB on a -60..+12 dB range
    CHECK (plugin->getLatencySamples() == 0);

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    // Two blocks, so a parameter change applied at the next block boundary still shows.
    for (int pass = 0; pass < 2; ++pass)
    {
        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), 0.25f, blockSize);
        plugin->processBlock (buffer, midi);
    }
    CHECK (buffer.getSample (0, blockSize - 1) == Catch::Approx (0.25 * juce::Decibels::decibelsToGain (6.0)).epsilon (1e-4));
}

TEST_CASE ("TestLatency reports and applies 1,000 samples of delay", "[E][fixtures]")
{
    auto plugin = loadFixture (WEFT_FIXTURE_TESTLATENCY);
    CHECK (plugin->getLatencySamples() == 1000);

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    int impulseAt = -1;
    for (int block = 0; block < 4 && impulseAt < 0; ++block)
    {
        buffer.clear();
        if (block == 0)
            buffer.setSample (0, 0, 1.0f);
        plugin->processBlock (buffer, midi);
        for (int i = 0; i < blockSize; ++i)
            if (buffer.getSample (0, i) > 0.5f)
                impulseAt = block * blockSize + i;
    }
    CHECK (impulseAt == 1000);
}

TEST_CASE ("TestSynth plays a sine at the note frequency scaled by velocity", "[E][fixtures]")
{
    auto plugin = loadFixture (WEFT_FIXTURE_TESTSYNTH);

    const int totalSamples = static_cast<int> (sampleRate); // one second
    juce::AudioBuffer<float> buffer (2, blockSize);
    float peak = 0.0f;
    int upwardCrossings = 0;
    float previous = 0.0f;

    for (int start = 0; start < totalSamples; start += blockSize)
    {
        juce::MidiBuffer midi;
        if (start == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, 69, static_cast<juce::uint8> (64)), 0);
        buffer.clear();
        plugin->processBlock (buffer, midi);
        const int n = std::min (blockSize, totalSamples - start);
        for (int i = 0; i < n; ++i)
        {
            const float s = buffer.getSample (0, i);
            peak = std::max (peak, std::abs (s));
            if (previous <= 0.0f && s > 0.0f)
                ++upwardCrossings;
            previous = s;
        }
    }

    CHECK (peak == Catch::Approx (64.0 / 127.0).epsilon (0.005));
    CHECK (upwardCrossings >= 439);
    CHECK (upwardCrossings <= 441);
}

// Hidden: run on its own by the TestCrashScan ctest entry, which expects the process to die.
TEST_CASE ("TestCrashScan crashes the process that loads it", "[.crash]")
{
    loadFixture (WEFT_FIXTURE_TESTCRASHSCAN);
    FAIL ("TestCrashScan loaded without crashing");
}

int main (int argc, char* argv[])
{
    const juce::ScopedJuceInitialiser_GUI juce;
    return Catch::Session().run (argc, argv);
}
