#include "weft/engine/devices/NullDevice.h"

#include "weft/rt/AudioThread.h"

#include <algorithm>
#include <chrono>

namespace weft::engine
{
bool NullDevice::open (const DeviceConfig& config)
{
    if (callback != nullptr || config.sampleRate <= 0.0 || config.maxBlockSize <= 0
        || config.numInputChannels < 0 || config.numOutputChannels < 0)
        return false;

    if (loopback > 0 && loopback < config.maxBlockSize)
        return false;
    cfg = config;
    if (loopback > 0)
    {
        cfg.outputLatency = loopback / 2;
        cfg.inputLatency = loopback - cfg.outputLatency;
        loopHistory.assign (static_cast<std::size_t> (cfg.numOutputChannels), std::vector<float> (static_cast<std::size_t> (loopback + cfg.maxBlockSize)));
    }
    else
        loopHistory.clear();
    const auto block = static_cast<std::size_t> (cfg.maxBlockSize);
    inputBuffers.assign (static_cast<std::size_t> (cfg.numInputChannels), std::vector<float> (block));
    outputBuffers.assign (static_cast<std::size_t> (cfg.numOutputChannels), std::vector<float> (block));
    inputPointers.clear();
    outputPointers.clear();
    for (auto& b : inputBuffers)
        inputPointers.push_back (b.data());
    for (auto& b : outputBuffers)
        outputPointers.push_back (b.data());
    capture.assign (outputBuffers.size(), {});
    captureCapacity = 0;
    counters = {};
    isOpen = true;
    return true;
}

void NullDevice::close()
{
    stop();
    isOpen = false;
}

bool NullDevice::start (AudioCallback& cb)
{
    if (! isOpen || callback != nullptr)
        return false;
    cb.prepare (cfg);
    callback = &cb;
    return true;
}

void NullDevice::stop()
{
    if (callback != nullptr)
    {
        callback->release();
        callback = nullptr;
    }
}

void NullDevice::setCaptureCapacity (std::int64_t capacityFrames)
{
    captureCapacity = std::max<std::int64_t> (0, capacityFrames);
    for (auto& c : capture)
    {
        c.clear();
        c.reserve (static_cast<std::size_t> (captureCapacity));
    }
}

std::span<const float> NullDevice::capturedOutput (int channel) const
{
    if (channel < 0 || channel >= static_cast<int> (capture.size()))
        return {};
    return capture[static_cast<std::size_t> (channel)];
}

bool NullDevice::render (std::int64_t numFrames, int blockSize)
{
    if (blockSize <= 0)
        blockSize = cfg.maxBlockSize;
    if (callback == nullptr || blockSize > cfg.maxBlockSize)
        return false;

    while (numFrames > 0)
    {
        const auto n = static_cast<int> (std::min<std::int64_t> (numFrames, blockSize));
        renderBlock (n);
        numFrames -= n;
    }
    return true;
}

bool NullDevice::renderBlocks (std::span<const int> blockSizes)
{
    if (callback == nullptr)
        return false;
    for (const int n : blockSizes)
        if (n <= 0 || n > cfg.maxBlockSize)
            return false;
    for (const int n : blockSizes)
        renderBlock (n);
    return true;
}

bool NullDevice::renderBlock (int numFrames)
{
    const auto frames = static_cast<std::size_t> (numFrames);

    for (std::size_t ch = 0; ch < inputBuffers.size(); ++ch)
    {
        auto* dest = inputBuffers[ch].data();
        if (! loopHistory.empty())
        {
            const auto& hist = loopHistory[std::min (ch, loopHistory.size() - 1)];
            const auto size = static_cast<std::int64_t> (hist.size());
            for (int i = 0; i < numFrames; ++i)
            {
                const auto src = counters.framesProcessed + i - loopback;
                dest[i] = src < 0 ? 0.0f : hist[static_cast<std::size_t> (src % size)];
            }
        }
        else if (inputGenerator)
            inputGenerator (static_cast<int> (ch), counters.framesProcessed, dest, numFrames);
        else
            std::fill_n (dest, frames, 0.0f);
    }
    for (auto& b : outputBuffers)
        std::fill_n (b.data(), frames, 0.0f);

    const auto started = std::chrono::steady_clock::now();
    {
        const rt::ScopedAudioThread audioThread;
        callback->process (inputPointers.data(), outputPointers.data(), numFrames);
    }
    if (simulateDeadlines)
    {
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
        if (elapsed.count() > numFrames / cfg.sampleRate)
            ++counters.deadlineMisses;
    }

    for (std::size_t ch = 0; ch < loopHistory.size(); ++ch)
    {
        auto& hist = loopHistory[ch];
        const auto size = static_cast<std::int64_t> (hist.size());
        for (int i = 0; i < numFrames; ++i)
            hist[static_cast<std::size_t> ((counters.framesProcessed + i) % size)] = outputBuffers[ch][static_cast<std::size_t> (i)];
    }

    for (std::size_t ch = 0; ch < capture.size(); ++ch)
    {
        auto& c = capture[ch];
        const auto room = static_cast<std::size_t> (captureCapacity) - c.size();
        const auto* src = outputBuffers[ch].data();
        c.insert (c.end(), src, src + std::min (room, frames));
    }

    ++counters.blocksProcessed;
    counters.framesProcessed += numFrames;
    return true;
}
} // namespace weft::engine
