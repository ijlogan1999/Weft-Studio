#include "weft/engine/graph/Mixer.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace weft::engine
{
DelayLine::DelayLine (int capacity)
{
    const auto size = std::bit_ceil (static_cast<unsigned> (std::max (2, capacity + 1)));
    buffer[0].assign (size, 0.0f);
    buffer[1].assign (size, 0.0f);
    mask = static_cast<int> (size) - 1;
}

float DelayLine::read (std::size_t ch, int delay, int back) const noexcept
{
    const auto at = (static_cast<std::ptrdiff_t> (write) - back - delay) & mask;
    return buffer[ch][static_cast<std::size_t> (at)];
}

void DelayLine::process (float* l, float* r, int frames, int delay, int fadeFrames) noexcept
{
    delay = std::clamp (delay, 0, mask);
    if (delay != current && fadeLength == 0)
    {
        previous = current;
        current = delay;
        fadePos = 0;
        fadeLength = std::max (1, fadeFrames);
    }
    float* io[2] = { l, r };
    for (int i = 0; i < frames; ++i)
    {
        buffer[0][write] = l[i];
        buffer[1][write] = r[i];
        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            float v = read (ch, current, 0);
            if (fadeLength > 0)
            {
                const float x = static_cast<float> (fadePos) / static_cast<float> (fadeLength);
                v = v * x + read (ch, previous, 0) * (1.0f - x);
            }
            io[ch][i] = v;
        }
        if (fadeLength > 0 && ++fadePos >= fadeLength)
            fadeLength = 0;
        write = (write + 1) & static_cast<std::size_t> (mask);
    }
}

std::shared_ptr<DelayLine> ChannelState::delayFor (const std::string& edge, int delay)
{
    auto& line = delays[edge];
    if (! line || line->capacity() < delay)
        line = std::make_shared<DelayLine> (std::max (delay, 8192));
    return line;
}

std::shared_ptr<ChannelState> MixerState::channel (const Id& trackId)
{
    auto& state = channels[trackId];
    if (! state)
        state = std::make_shared<ChannelState>();
    return state;
}

std::shared_ptr<ChannelState> MixerState::find (const Id& trackId) const
{
    const auto it = channels.find (trackId);
    return it == channels.end() ? nullptr : it->second;
}

void MixerState::prune (const std::vector<Id>& keep)
{
    std::erase_if (channels, [&] (const auto& kv) { return std::find (keep.begin(), keep.end(), kv.first) == keep.end(); });
}

float gainToDb (float gain) noexcept { return gain > 1.0e-7f ? 20.0f * std::log10 (gain) : -144.0f; }

void MeterBallistics::update (float peakLinear, bool clip, double now) noexcept
{
    const float db = gainToDb (peakLinear);
    const auto elapsed = static_cast<float> (std::max (0.0, now - last));
    last = now;
    level = std::max (db, level - static_cast<float> (fallDbPerSecond) * elapsed);
    if (db >= hold || now - holdSince >= holdSeconds)
    {
        hold = std::max (db, now - holdSince >= holdSeconds ? level : hold);
        holdSince = now;
    }
    clipped = clipped || clip;
}
} // namespace weft::engine
