// The MP3 part of AT-ARR-25: the platform decoders read compressed files for import.

#include "EngineFixtures.h"

#include "weft/engine/arrange/Import.h"
#include "weft/engine/media/JuceDecoders.h"
#include "weft/media/Hash.h"

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
