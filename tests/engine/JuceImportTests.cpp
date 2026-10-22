// The MP3 part of AT-ARR-25: compressed files decode for import, and MP3s land on time.

#include "EngineFixtures.h"

#include "weft/engine/arrange/Import.h"
#include "weft/engine/media/JuceDecoders.h"
#include "weft/media/Hash.h"

#include <weft_codec.h>

#include <array>

using namespace weft;
using namespace weft::test;

TEST_CASE ("AT-ARR-25: a 44.1 kHz stereo MP3 dropped at bar 3 becomes a 48 kHz WAV region", "[E][import][AT-ARR-25]")
{
    Rig rig;
    const auto mediaDir = rig.folder.path / "Media";
    const auto track = rig.session.createTrack (TrackKind::audio, "Gtr", 2).value();
    const auto decoders = engine::importDecoders();
    const std::filesystem::path mp3 = WEFT_FIXTURES_DIR "/media/sine-1k-44k1-stereo.mp3";
    REQUIRE (decoders.canDecode (mp3));

    const auto result = engine::importFiles (rig.session, rig.media, mediaDir, decoders, { mp3 }, { 2 * barTicks, track });
    INFO (result.message);
    REQUIRE (result.regions.size() == 1);
    const auto& r = *rig.session.model().region (result.regions[0]);
    CHECK (r.start == 2 * barTicks);
    CHECK (r.trackId == track);
    const auto* m = rig.session.model().mediaRef (r.audio.media);
    REQUIRE (m != nullptr);
    CHECK (m->sampleRate == 48000);
    CHECK (m->channels == 2);
    CHECK (m->format == "wav");
    // One second of audio, plus the decoder's priming and padding frames.
    CHECK (m->lengthSamples >= 48000);
    CHECK (m->lengthSamples < 52000);
    const auto stored = rig.mediaFile (r.audio.media);
    CHECK (media::hashFile (stored) == r.audio.media);
    const auto info = media::readWavInfo (stored);
    REQUIRE (info.has_value());
    CHECK (info->sampleRate == 48000);

    // The middle of the file is the tone, not silence.
    const auto audio = rig.regionFile (r.id);
    float peak = 0;
    for (std::size_t i = 10000; i < 30000; ++i)
        peak = std::max (peak, std::abs (audio.channels[0][i]));
    CHECK (peak > 0.05f);
}

TEST_CASE ("An imported MP3 starts on time: the encoder delay and padding are trimmed", "[E][import]")
{
    // A click at 0.5 s in 2 s of 48 kHz stereo, encoded by our own MP3 writer.
    Rig rig;
    const auto mp3 = rig.folder.path / "click.mp3";
    std::vector<float> left (96000, 0.0f);
    for (int i = 0; i < 48; ++i)
        left[static_cast<std::size_t> (24000 + i)] = 0.8f;
    const auto path = mp3.u8string();
    WeftMp3Settings s {};
    s.sample_rate = 48000;
    s.channels = 2;
    s.bitrate_kbps = 320;
    std::array<char, 256> err {};
    auto* enc = weft_mp3_open (reinterpret_cast<const char*> (path.c_str()), &s, err.data(), err.size());
    REQUIRE (enc != nullptr);
    REQUIRE (weft_mp3_write (enc, left.data(), left.data(), left.size(), err.data(), err.size()) == 0);
    REQUIRE (weft_mp3_finish (enc, err.data(), err.size()) == 0);

    const auto track = rig.session.createTrack (TrackKind::audio, "Click", 2).value();
    const auto result = engine::importFiles (rig.session, rig.media, rig.folder.path / "Media", engine::importDecoders(), { mp3 }, { 0, track });
    INFO (result.message);
    REQUIRE (result.regions.size() == 1);
    const auto& r = *rig.session.model().region (result.regions[0]);
    CHECK (r.length == 96000);
    const auto audio = rig.regionFile (r.id);
    std::size_t onset = 0;
    while (onset < audio.channels[0].size() && std::abs (audio.channels[0][onset]) < 0.3f)
        ++onset;
    CHECK (std::abs (static_cast<std::int64_t> (onset) - 24000) < 10);
}
