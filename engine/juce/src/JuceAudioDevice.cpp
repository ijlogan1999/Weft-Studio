#include "weft/engine/devices/JuceAudioDevice.h"

#include "weft/rt/AudioThread.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <algorithm>
#include <atomic>

namespace weft::engine
{
namespace
{
juce::OwnedArray<juce::AudioIODeviceType> createTypes()
{
    juce::OwnedArray<juce::AudioIODeviceType> types;
    juce::AudioDeviceManager manager;
    manager.createAudioDeviceTypes (types);
    for (auto* t : types)
        t->scanForDevices();
    return types;
}
} // namespace

struct JuceAudioDevice::Impl final : juce::AudioIODeviceCallback
{
    std::string typeName, deviceName, inputName;
    juce::OwnedArray<juce::AudioIODeviceType> types;
    std::unique_ptr<juce::AudioIODevice> device;
    DeviceConfig cfg;
    std::string error;
    std::atomic<AudioCallback*> callback { nullptr };
    std::atomic<bool> stopping { false }, failed { false };
    std::vector<std::vector<float>> silentInputs, chunkOutputs;
    std::vector<const float*> inputPtrs;
    std::vector<float*> outputPtrs;

    void audioDeviceAboutToStart (juce::AudioIODevice* d) override
    {
        cfg.sampleRate = d->getCurrentSampleRate();
        cfg.maxBlockSize = d->getCurrentBufferSizeSamples();
        cfg.numInputChannels = d->getActiveInputChannels().countNumberOfSetBits();
        cfg.numOutputChannels = d->getActiveOutputChannels().countNumberOfSetBits();
        cfg.inputLatency = d->getInputLatencyInSamples();
        cfg.outputLatency = d->getOutputLatencyInSamples();
        const auto block = static_cast<std::size_t> (cfg.maxBlockSize);
        silentInputs.assign (static_cast<std::size_t> (cfg.numInputChannels), std::vector<float> (block));
        inputPtrs.assign (static_cast<std::size_t> (cfg.numInputChannels), nullptr);
        outputPtrs.assign (static_cast<std::size_t> (cfg.numOutputChannels), nullptr);
        if (auto* cb = callback.load())
            cb->prepare (cfg);
    }

    void audioDeviceStopped() override
    {
        if (! stopping.load())
            failed.store (true); // stopped by the driver, not by us
        if (auto* cb = callback.load())
            cb->release();
    }

    void audioDeviceError (const juce::String&) override { failed.store (true); }

    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        const weft::rt::ScopedAudioThread audioThread;
        auto* cb = callback.load (std::memory_order_acquire);
        if (cb == nullptr || numOutputs != cfg.numOutputChannels || numInputs != cfg.numInputChannels)
        {
            for (int ch = 0; ch < numOutputs; ++ch)
                if (outputs[ch] != nullptr)
                    std::fill_n (outputs[ch], numSamples, 0.0f);
            return;
        }
        // Drivers may deliver more frames than the size they announced: split into chunks.
        for (int done = 0; done < numSamples;)
        {
            const int n = std::min (numSamples - done, cfg.maxBlockSize);
            for (std::size_t ch = 0; ch < inputPtrs.size(); ++ch)
                inputPtrs[ch] = inputs[ch] != nullptr ? inputs[ch] + done : silentInputs[ch].data();
            for (std::size_t ch = 0; ch < outputPtrs.size(); ++ch)
                outputPtrs[ch] = outputs[ch] + done;
            cb->process (inputPtrs.data(), outputPtrs.data(), n);
            done += n;
        }
    }
};

std::vector<std::string> JuceAudioDevice::driverTypes()
{
    std::vector<std::string> names;
    for (auto* t : createTypes())
        names.push_back (t->getTypeName().toStdString());
    return names;
}

std::vector<std::string> JuceAudioDevice::deviceNames (const std::string& driverType, bool inputs)
{
    std::vector<std::string> names;
    for (auto* t : createTypes())
        if (t->getTypeName().toStdString() == driverType)
            for (const auto& n : t->getDeviceNames (inputs))
                names.push_back (n.toStdString());
    return names;
}

std::string JuceAudioDevice::defaultDeviceName (const std::string& driverType, bool input)
{
    for (auto* t : createTypes())
        if (t->getTypeName().toStdString() == driverType)
        {
            const auto names = t->getDeviceNames (input);
            const auto index = t->getDefaultDeviceIndex (input);
            return index >= 0 && index < names.size() ? names[index].toStdString() : std::string();
        }
    return {};
}

JuceAudioDevice::Capabilities JuceAudioDevice::capabilities (const std::string& driverType, const std::string& output, const std::string& input)
{
    Capabilities caps;
    for (auto* t : createTypes())
    {
        if (t->getTypeName().toStdString() != driverType)
            continue;
        const auto in = t->hasSeparateInputsAndOutputs() ? juce::String (input.empty() ? output : input) : juce::String (output);
        std::unique_ptr<juce::AudioIODevice> d (t->createDevice (output, in));
        if (d == nullptr)
            break;
        for (const auto r : d->getAvailableSampleRates())
            caps.sampleRates.push_back (r);
        for (const auto b : d->getAvailableBufferSizes())
            caps.bufferSizes.push_back (b);
        caps.currentRate = d->getCurrentSampleRate();
        caps.defaultBufferSize = d->getDefaultBufferSize();
    }
    return caps;
}

std::string JuceAudioDevice::defaultDriverType()
{
    const auto types = driverTypes();
#if JUCE_WINDOWS
    if (std::find (types.begin(), types.end(), "Windows Audio") != types.end())
        return "Windows Audio";
#endif
    return types.empty() ? std::string() : types.front();
}

JuceAudioDevice::JuceAudioDevice (std::string driverType, std::string device, std::string input) : impl (std::make_unique<Impl>())
{
    impl->typeName = driverType.empty() ? defaultDriverType() : std::move (driverType);
    impl->deviceName = std::move (device);
    impl->inputName = std::move (input);
}

JuceAudioDevice::~JuceAudioDevice() { close(); }

std::string JuceAudioDevice::name() const
{
    return impl->device != nullptr ? impl->typeName + ": " + impl->device->getName().toStdString() : impl->typeName;
}

bool JuceAudioDevice::open (const DeviceConfig& config)
{
    close();
    impl->error.clear();
    impl->failed.store (false);
    impl->types = createTypes();
    juce::AudioIODeviceType* type = nullptr;
    for (auto* t : impl->types)
        if (t->getTypeName().toStdString() == impl->typeName)
            type = t;
    if (type == nullptr)
    {
        impl->error = "The audio driver \"" + impl->typeName + "\" isn't available.";
        return false;
    }
    const auto outputs = type->getDeviceNames (false);
    const auto inputs = type->getDeviceNames (true);
    auto outName = impl->deviceName.empty() ? outputs[type->getDefaultDeviceIndex (false)] : juce::String (impl->deviceName);
    auto inName = ! impl->inputName.empty() ? juce::String (impl->inputName)
                  : impl->deviceName.empty() ? inputs[type->getDefaultDeviceIndex (true)] : juce::String (impl->deviceName);
    if (! type->hasSeparateInputsAndOutputs())
        inName = outName;
    impl->device.reset (type->createDevice (outName, inName));
    if (impl->device == nullptr)
    {
        impl->error = "No audio device could be opened.";
        return false;
    }

    juce::BigInteger ins, outs;
    ins.setRange (0, config.numInputChannels, true);
    outs.setRange (0, config.numOutputChannels, true);
    const auto result = impl->device->open (ins, outs, config.sampleRate, config.maxBlockSize);
    if (result.isNotEmpty())
    {
        impl->error = result.toStdString();
        impl->device.reset();
        return false;
    }
    impl->cfg = config;
    impl->cfg.sampleRate = impl->device->getCurrentSampleRate();
    impl->cfg.maxBlockSize = impl->device->getCurrentBufferSizeSamples();
    impl->cfg.numInputChannels = impl->device->getActiveInputChannels().countNumberOfSetBits();
    impl->cfg.numOutputChannels = impl->device->getActiveOutputChannels().countNumberOfSetBits();
    impl->cfg.inputLatency = impl->device->getInputLatencyInSamples();
    impl->cfg.outputLatency = impl->device->getOutputLatencyInSamples();
    return true;
}

void JuceAudioDevice::close()
{
    stop();
    if (impl->device != nullptr)
        impl->device->close();
    impl->device.reset();
}

bool JuceAudioDevice::start (AudioCallback& cb)
{
    if (impl->device == nullptr || impl->callback.load() != nullptr)
        return false;
    impl->callback.store (&cb, std::memory_order_release);
    impl->device->start (impl.get()); // calls audioDeviceAboutToStart, which prepares the callback
    return impl->device->isPlaying();
}

void JuceAudioDevice::stop()
{
    if (impl->device != nullptr && impl->callback.load() != nullptr)
    {
        impl->stopping.store (true);
        impl->device->stop(); // calls audioDeviceStopped, which releases the callback
        impl->stopping.store (false);
    }
    impl->callback.store (nullptr, std::memory_order_release);
}

bool JuceAudioDevice::failed() const
{
    // A driver whose device went away ends its thread, so the device stops playing (WASAPI);
    // others report an error or stop the callback.
    return impl->failed.load() || (impl->device != nullptr && impl->callback.load() != nullptr && ! impl->device->isPlaying());
}

bool JuceAudioDevice::isRunning() const { return impl->device != nullptr && impl->device->isPlaying(); }
DeviceConfig JuceAudioDevice::config() const { return impl->cfg; }
const std::string& JuceAudioDevice::lastError() const noexcept { return impl->error; }
std::int64_t JuceAudioDevice::xrunCount() const { return impl->device != nullptr ? std::max (0, impl->device->getXRunCount()) : 0; }
int JuceAudioDevice::outputLatency() const { return impl->device != nullptr ? impl->device->getOutputLatencyInSamples() : 0; }
} // namespace weft::engine
