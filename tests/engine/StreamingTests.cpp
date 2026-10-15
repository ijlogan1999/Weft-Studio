// Disk streaming (ARCHITECTURE §3, D-ARCH-17): long media plays from blocks the disk
// thread loads ahead of the playhead.

#include "EngineFixtures.h"

#include "weft/media/Hash.h"

#include <chrono>
#include <thread>

using namespace weft;
using namespace weft::test;

namespace
{
constexpr std::int64_t longFrames = 10 * 48000; // 10 s: 15 blocks

/** Writes ramp media into the rig's Media/ folder and registers it; streamed above 1 s. */
Id streamedRegion (Rig& rig)
{
    const auto dir = rig.folder.path / "Media";
    std::filesystem::create_directories (dir);
    const auto tmp = dir / "tmp.wav";
    REQUIRE (media::writeWav (tmp, rampMedia (longFrames), media::SampleFormat::float32));
    const auto hash = media::hashFile (tmp).value();
    std::filesystem::rename (tmp, dir / media::mediaFileName (hash));

    rig.media.setResidentLimit (48000);
    const auto source = rig.media.load (dir, hash);
    REQUIRE (source);
    CHECK_FALSE (source->resident());
    model::MediaRef m;
    m.hash = hash;
    m.originalName = "long.wav";
    m.lengthSamples = longFrames;
    REQUIRE (rig.session.addMedia (m));
    const auto id = rig.session.createAudioRegion (rig.audioTrack(), 0, hash, 0, longFrames);
    REQUIRE (id);
    rig.sync();
    return *id;
}

/** Renders `frames` in 4,096-frame chunks, pumping the disk reader before each. */
std::vector<float> renderPumped (Rig& rig, std::int64_t frames)
{
    std::vector<float> out;
    while (static_cast<std::int64_t> (out.size()) < frames)
    {
        rig.engine.pumpDisk();
        const auto n = std::min<std::int64_t> (4096, frames - static_cast<std::int64_t> (out.size()));
        const auto chunk = rig.run (n);
        out.insert (out.end(), chunk.begin(), chunk.end());
    }
    return out;
}
} // namespace

TEST_CASE ("Streamed media plays sample-exact with a bounded number of blocks loaded", "[E][disk]")
{
    Rig rig;
    rig.engine.setDiskThread (false);
    streamedRegion (rig);
    rig.engine.play (0);
    std::size_t mostLoaded = 0;
    std::vector<float> out;
    while (static_cast<std::int64_t> (out.size()) < longFrames)
    {
        rig.engine.pumpDisk();
        mostLoaded = std::max (mostLoaded, rig.engine.diskStreamer().loadedBlocks());
        const auto chunk = rig.run (4096);
        out.insert (out.end(), chunk.begin(), chunk.end());
    }
    for (std::int64_t i = 0; i < longFrames; ++i)
        REQUIRE (out[static_cast<std::size_t> (i)] == rampValue (i));
    CHECK (rig.engine.diskUnderruns() == 0);
    CHECK (mostLoaded <= 5); // 2 s of read-ahead plus the block being played, not all 15
}

TEST_CASE ("A block that isn't loaded plays silence and counts an underrun", "[E][disk]")
{
    Rig rig;
    rig.engine.setDiskThread (false);
    streamedRegion (rig);
    rig.engine.locate (4 * barTicks); // 4 s in: nothing loaded there
    rig.engine.play();
    const auto out = rig.run (4096);
    CHECK (peak (out, 0, 4096) == 0.0f);
    CHECK (rig.engine.diskUnderruns() > 0);
}

TEST_CASE ("Streaming reads ahead across cycle wraps", "[E][disk]")
{
    Rig rig;
    rig.engine.setDiskThread (false);
    streamedRegion (rig);
    REQUIRE (rig.session.setCycle (true, barTicks, 3 * barTicks)); // 2 s to 6 s, blocks 2-8
    rig.sync();
    rig.engine.play (0);
    const auto out = renderPumped (rig, 3 * barSamples + barSamples);
    CHECK (rig.engine.diskUnderruns() == 0);
    for (std::int64_t i = 0; i < static_cast<std::int64_t> (out.size()); i += 997)
    {
        const auto t = i < 3 * barSamples ? i : barSamples + (i - 3 * barSamples);
        REQUIRE (out[static_cast<std::size_t> (i)] == rampValue (t));
    }
}

TEST_CASE ("The disk thread loads blocks on its own", "[E][disk]")
{
    Rig rig;
    streamedRegion (rig);
    rig.engine.play (0);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds (5);
    while (rig.engine.diskStreamer().loadedBlocks() < 3 && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    const auto out = rig.run (4096);
    CHECK (out[100] == rampValue (100));
    CHECK (rig.engine.diskUnderruns() == 0);
}
