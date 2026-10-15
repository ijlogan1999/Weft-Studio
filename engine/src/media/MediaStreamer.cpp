#include "weft/engine/media/MediaStreamer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <climits>

namespace weft::engine
{
MediaStreamer::MediaStreamer (Clocks c) : clocks (std::move (c)) {}

MediaStreamer::~MediaStreamer() { stopThread(); }

void MediaStreamer::setPlan (std::shared_ptr<const Plan> p)
{
    {
        const std::scoped_lock l (lock);
        plan = std::move (p);
    }
    wake.notify_one();
}

void MediaStreamer::hint (std::int64_t position) noexcept
{
    hinted.store (position);
    wake.notify_one();
}

void MediaStreamer::startThread()
{
    if (thread.joinable())
        return;
    {
        const std::scoped_lock l (wakeLock);
        stopping = false;
    }
    thread = std::thread ([this] {
        std::unique_lock l (wakeLock);
        while (! stopping)
        {
            l.unlock();
            pump();
            l.lock();
            wake.wait_for (l, std::chrono::milliseconds (5), [this] { return stopping; });
        }
    });
}

void MediaStreamer::stopThread()
{
    {
        const std::scoped_lock l (wakeLock);
        stopping = true;
    }
    wake.notify_one();
    if (thread.joinable())
        thread.join();
}

std::size_t MediaStreamer::loadedBlocks() const
{
    const std::scoped_lock l (lock);
    std::size_t n = 0;
    for (const auto& [s, t] : tracked)
        n += t.blocks.size();
    return n;
}

void MediaStreamer::pump()
{
    const std::scoped_lock pass (pumping);
    std::shared_ptr<const Plan> current;
    {
        const std::scoped_lock l (lock);
        current = plan;
    }

    // Timeline ranges to have loaded.
    const auto ahead = lookahead.load();
    const auto pos = clocks.position ? clocks.position() : 0;
    std::vector<std::pair<std::int64_t, std::int64_t>> ranges { { pos, pos + ahead } };
    if (const auto h = hinted.exchange (INT64_MIN); h != INT64_MIN)
        ranges.emplace_back (h, h + ahead);
    if (current && current->cycle && current->cycleEnd > current->cycleStart)
        ranges.emplace_back (current->cycleStart, current->cycleStart + ahead);

    // Blocks they need, nearest first.
    std::map<const MediaSource*, std::set<std::int64_t>> needed;
    std::vector<std::tuple<std::int64_t, std::shared_ptr<MediaSource>, std::int64_t>> toLoad; // (distance, source, block)
    if (current)
        for (const auto& w : current->windows)
        {
            if (! w.source || w.source->resident())
                continue;
            auto& want = needed[w.source.get()];
            for (const auto& [a, b] : ranges)
            {
                const auto s = std::max (a, w.start), e = std::min (b, w.end);
                if (s >= e)
                    continue;
                // Two frames either side for interpolated reads.
                const auto first = static_cast<std::int64_t> (std::floor (w.sourceAtStart + static_cast<double> (s - w.start) * w.step)) - 2;
                const auto last = static_cast<std::int64_t> (std::ceil (w.sourceAtStart + static_cast<double> (e - w.start) * w.step)) + 2;
                const auto b0 = std::max<std::int64_t> (0, first / MediaSource::blockFrames);
                const auto b1 = std::min<std::int64_t> (w.source->numBlocks() - 1, std::max<std::int64_t> (0, last) / MediaSource::blockFrames);
                for (auto blk = b0; blk <= b1; ++blk)
                    if (want.insert (blk).second)
                        toLoad.emplace_back (std::abs (s - a) + (blk - b0) * MediaSource::blockFrames, w.source, blk);
            }
        }
    std::sort (toLoad.begin(), toLoad.end(), [] (const auto& x, const auto& y) { return std::get<0> (x) < std::get<0> (y); });

    for (const auto& [distance, source, blk] : toLoad)
    {
        if (! source->isLoaded (blk) && ! source->load (blk))
            continue;
        const std::scoped_lock l (lock);
        auto& t = tracked[source.get()];
        t.source = source;
        t.blocks.insert (blk);
    }

    // Evict what's no longer needed; free it once the audio thread has finished a block since.
    const auto frames = clocks.audioFrames ? clocks.audioFrames() : 0;
    const bool running = clocks.audioRunning && clocks.audioRunning();
    const std::scoped_lock l (lock);
    for (auto it = tracked.begin(); it != tracked.end();)
    {
        auto& t = it->second;
        const auto want = needed.find (it->first);
        for (auto b = t.blocks.begin(); b != t.blocks.end();)
        {
            if (want != needed.end() && want->second.contains (*b))
            {
                ++b;
                continue;
            }
            if (auto block = t.source->evict (*b))
                retired.push_back ({ std::move (block), frames });
            b = t.blocks.erase (b);
        }
        it = t.blocks.empty() ? tracked.erase (it) : std::next (it);
    }
    std::erase_if (retired, [&] (const Retired& r) { return ! running || frames > r.afterFrames; });
}
} // namespace weft::engine
