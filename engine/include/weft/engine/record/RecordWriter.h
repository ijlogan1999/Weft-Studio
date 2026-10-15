#pragma once

#include "weft/engine/record/RecordPass.h"
#include "weft/media/WavFile.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace weft::engine
{
/**
    Drains a pass's audio FIFOs into one WAV (BWF) file per lane in Recovery/
    (W-1a step 8). Runs its own thread, or the caller pumps it. Never touched by
    the audio thread.
*/
class RecordWriter
{
public:
    RecordWriter (RecordPass& pass, std::filesystem::path recoveryDir);
    ~RecordWriter();

    /** Creates the files. */
    bool open (double sampleRate, media::SampleFormat format, const media::BextInfo& bext, std::string* error = nullptr);
    void startThread();
    /** Moves whatever is in the FIFOs to disk. */
    void pump();
    /** Stops the thread, drains the FIFOs and closes the files. */
    bool finish();

    const std::filesystem::path& file (std::size_t lane) const { return files[lane]; }
    std::int64_t framesWritten (std::size_t lane) const { return writers[lane]->framesWritten(); }
    bool failed() const noexcept { return writeFailed.load(); }

private:
    RecordPass& pass;
    std::filesystem::path dir;
    std::vector<std::filesystem::path> files;
    std::vector<std::unique_ptr<media::WavWriter>> writers;
    std::vector<std::vector<float>> scratch;
    std::mutex pumping;
    std::thread thread;
    std::atomic<bool> stopping { false }, writeFailed { false };
};
} // namespace weft::engine
