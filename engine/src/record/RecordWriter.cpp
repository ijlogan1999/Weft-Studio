#include "weft/engine/record/RecordWriter.h"

#include <chrono>

namespace weft::engine
{
namespace
{
constexpr std::int64_t chunkFrames = 16384;
std::atomic<std::uint64_t> fileCounter { 0 };
} // namespace

RecordWriter::RecordWriter (RecordPass& p, std::filesystem::path recoveryDir) : pass (p), dir (std::move (recoveryDir)) {}

RecordWriter::~RecordWriter() { finish(); }

bool RecordWriter::open (double sampleRate, media::SampleFormat format, const media::BextInfo& bext, std::string* error)
{
    std::error_code ec;
    std::filesystem::create_directories (dir, ec);
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::system_clock::now().time_since_epoch()).count();
    for (const auto& lane : pass.audio)
    {
        auto file = dir / ("rec-" + std::to_string (stamp) + "-" + std::to_string (fileCounter++) + "-" + lane->trackId + ".wav");
        auto writer = std::make_unique<media::WavWriter>();
        auto laneBext = bext;
        laneBext.description = lane->trackName;
        if (! writer->open (file, sampleRate, lane->channels, format, &laneBext, error))
            return false;
        files.push_back (std::move (file));
        writers.push_back (std::move (writer));
    }
    scratch.assign (2, std::vector<float> (static_cast<std::size_t> (chunkFrames)));
    return true;
}

void RecordWriter::startThread()
{
    if (thread.joinable())
        return;
    thread = std::thread ([this] {
        while (! stopping.load())
        {
            pump();
            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }
    });
}

void RecordWriter::pump()
{
    const std::scoped_lock lock (pumping);
    float* dest[2] = { scratch[0].data(), scratch[1].data() };
    for (std::size_t i = 0; i < writers.size(); ++i)
    {
        auto& fifo = *pass.audio[i]->fifo;
        while (writers[i]->isOpen())
        {
            const auto n = fifo.read (dest, chunkFrames);
            if (n <= 0)
                break;
            if (! writers[i]->write (dest, n))
                writeFailed = true;
        }
    }
}

bool RecordWriter::finish()
{
    stopping = true;
    if (thread.joinable())
        thread.join();
    pump();
    bool ok = ! writeFailed;
    for (auto& w : writers)
        if (w->isOpen())
            ok = w->close() && ok;
    return ok;
}
} // namespace weft::engine
