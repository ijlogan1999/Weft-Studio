#pragma once

#include "weft/engine/media/MediaSource.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace weft::engine
{
/**
    The playback disk thread (ARCHITECTURE §3, D-ARCH-17): keeps the blocks of streamed
    media loaded for the next few seconds after the playhead (and the cycle start while
    cycling), and evicts the rest once the audio thread has moved past them.
*/
class MediaStreamer
{
public:
    /** Where a media file plays on the timeline (device samples). */
    struct Window
    {
        std::shared_ptr<MediaSource> source;
        std::int64_t start = 0, end = 0;
        double sourceAtStart = 0, step = 1;
    };
    struct Plan
    {
        std::vector<Window> windows;
        bool cycle = false;
        std::int64_t cycleStart = 0, cycleEnd = 0;
    };
    struct Clocks
    {
        std::function<std::int64_t()> position;    // playhead, device samples
        std::function<std::int64_t()> audioFrames; // frames the audio thread has finished
        std::function<bool()> audioRunning;
    };

    explicit MediaStreamer (Clocks clocks);
    ~MediaStreamer();

    void setPlan (std::shared_ptr<const Plan> plan);
    /** Also read ahead from here (a play or locate target the playhead hasn't reached yet). */
    void hint (std::int64_t position) noexcept;
    void setLookahead (std::int64_t frames) noexcept { lookahead = frames; }

    void startThread();
    void stopThread();
    /** One pass: load what's needed, evict what isn't, free what the audio thread has left. */
    void pump();

    /** Streamed blocks currently loaded. */
    std::size_t loadedBlocks() const;

private:
    Clocks clocks;
    std::atomic<std::int64_t> lookahead { 96000 };
    std::atomic<std::int64_t> hinted { INT64_MIN };

    mutable std::mutex lock; // plan, tracked, retired: message and disk threads only
    std::shared_ptr<const Plan> plan;
    struct Tracked
    {
        std::shared_ptr<MediaSource> source;
        std::set<std::int64_t> blocks;
    };
    std::map<const MediaSource*, Tracked> tracked;
    struct Retired
    {
        std::unique_ptr<MediaSource::Block[]> block;
        std::int64_t afterFrames;
    };
    std::vector<Retired> retired;
    std::mutex pumping;

    std::thread thread;
    std::mutex wakeLock;
    std::condition_variable wake;
    bool stopping = false;
};
} // namespace weft::engine
