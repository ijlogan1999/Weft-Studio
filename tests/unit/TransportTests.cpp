// Transport commands and position displays that don't need the engine (spec 06 §2).

#include "Fixtures.h"

#include "weft/media/WavFile.h"
#include "weft/time/Position.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace weft;
using namespace weft::test;

namespace
{
constexpr Ticks bar = 3840, beat = 960;
Ticks at (int barNumber, int beatNumber = 1) { return (barNumber - 1) * bar + (beatNumber - 1) * beat; }
} // namespace

TEST_CASE ("AT-TRN-5: Home goes to tick 0, End to the end marker or the last region's end", "[U][transport][AT-TRN-5]")
{
    auto s = newSession();
    const auto track = firstTrackOfKind (s, TrackKind::instrument).empty() ? s.createTrack (TrackKind::instrument, "Keys").value()
                                                                           : firstTrackOfKind (s, TrackKind::instrument);
    CHECK (commands::projectEndTick (s.model()) == 0);
    REQUIRE (s.createMidiRegion (track, at (3), 2 * bar));
    REQUIRE (s.createMidiRegion (track, at (2), bar));
    CHECK (commands::projectEndTick (s.model()) == at (5));
    REQUIRE (s.setEndMarker (at (9)));
    CHECK (commands::projectEndTick (s.model()) == at (9));
    // Home is a locate to tick 0; there is nothing before it to compute.
}

TEST_CASE ("AT-TRN-6: period and comma step by one bar", "[U][transport][AT-TRN-6]")
{
    const SignatureMap sig;
    CHECK (nextBar (at (3, 2), sig) == at (4, 1));
    CHECK (previousBar (at (3, 2), sig) == at (3, 1));
    CHECK (previousBar (at (3, 1), sig) == at (2, 1));
    CHECK (previousBar (at (1, 1), sig) == at (1, 1));
}

TEST_CASE ("AT-TRN-13: the tempo display edits the tempo in effect, clamped", "[U][transport][AT-TRN-13]")
{
    auto s = newSession();
    REQUIRE (s.setTempoAt (0, 90.0));
    CHECK (s.model().tempo.points().front().bpm == 90.0);
    REQUIRE (s.setTempoAt (0, 5.0));
    CHECK (s.model().tempo.points().front().bpm == 20.0);
    REQUIRE (s.setTempoAt (0, 5000.0));
    CHECK (s.model().tempo.points().front().bpm == 999.0);
    REQUIRE (s.setTempoAt (0, 120.004));
    CHECK (s.model().tempo.points().front().bpm == 120.0); // shown and stored to 2 decimals

    const auto bar9 = s.addTempoPoint (at (9), 100.0);
    REQUIRE (bar9);
    REQUIRE (s.setTempoAt (at (10), 140.0)); // playhead at bar 10
    const auto& points = s.model().tempo.points();
    REQUIRE (points.size() == 2);
    CHECK (points[0].bpm == 120.0);
    CHECK (points[1].id == *bar9);
    CHECK (points[1].bpm == 140.0);
}

TEST_CASE ("AT-TRN-14: the signature display edits the signature in effect", "[U][transport][AT-TRN-14]")
{
    auto s = newSession();
    REQUIRE (s.setSignatureAt (0, 3, 4));
    REQUIRE (s.model().signatures.points().size() == 1);
    CHECK (s.model().signatures.points().front().numerator == 3);
    CHECK (s.model().signatures.barStart (1) == 2880);
    CHECK_FALSE (s.setSignatureAt (0, 3, 5)); // invalid denominator is rejected
}

TEST_CASE ("AT-TRN-15: position and time displays", "[U][transport][AT-TRN-15]")
{
    const SignatureMap sig;
    const TempoMap tempo ({}, 48000.0);
    CHECK (formatBarsBeats (4080, sig) == "2.1.2.0");
    CHECK (formatTime (tempo.tickToSample (4080) / 48000.0) == "0:00:02.125");
    CHECK (formatBarsBeats (0, sig) == "1.1.1.0");
    CHECK (formatBarsBeats (at (2) + 3 * beat + 3 * 240 + 239, sig) == "2.4.4.239");
    CHECK (formatTime (3723.5) == "1:02:03.500");
    CHECK (formatTime (-4.0) == "-0:00:04.000");
    // Before bar 1 (count-in), bars count down from 0.
    CHECK (formatBarsBeats (-bar, sig) == "0.1.1.0");
    // In 6/8 a beat is an eighth note and a division stays a sixteenth.
    const SignatureMap sixEight ({ SignaturePoint { "s", 0, 6, 8 } });
    CHECK (formatBarsBeats (480 * 5 + 240, sixEight) == "1.6.2.0");
}

TEST_CASE ("AT-TRN-16 (parsing): typed positions", "[U][transport][AT-TRN-16]")
{
    const SignatureMap sig;
    CHECK (parseBarsBeats ("5 3", sig) == at (5, 3));
    CHECK (parseBarsBeats ("5.3", sig) == at (5, 3));
    CHECK (parseBarsBeats ("2 1 2 0", sig) == 4080);
    CHECK (parseBarsBeats ("7", sig) == at (7));
    CHECK_FALSE (parseBarsBeats ("", sig));
    CHECK_FALSE (parseBarsBeats ("0 1", sig));
    CHECK_FALSE (parseBarsBeats ("x", sig));

    CHECK (parseTime ("0:30") == 30.0);
    CHECK (parseTime ("30") == 30.0);
    CHECK (parseTime ("1:02:03.5") == Catch::Approx (3723.5));
    CHECK_FALSE (parseTime ("1:75"));
    CHECK_FALSE (parseTime ("abc"));
    CHECK_FALSE (parseTime ("1::2"));
}

TEST_CASE ("AT-TRN-19: U sets the cycle to the selected regions, rounded outward to bars", "[U][transport][AT-TRN-19]")
{
    auto s = newSession();
    const auto track = s.createTrack (TrackKind::instrument, "Keys").value();
    // Selection from bar 3 beat 2 through bar 6 beat 1 (the last region ends after that beat).
    const auto a = s.createMidiRegion (track, at (3, 2), bar).value();
    const auto b = s.createMidiRegion (track, at (5, 2), at (6, 2) - at (5, 2)).value();
    REQUIRE (s.setCycleToRegions ({ a, b }));
    CHECK (s.model().info.cycle.enabled);
    CHECK (s.model().info.cycle.start == at (3));
    CHECK (s.model().info.cycle.end == at (7));

    // A region ending exactly on a bar line doesn't add a bar.
    const auto c = s.createMidiRegion (track, at (9), bar).value();
    REQUIRE (s.setCycleToRegions ({ c }));
    CHECK (s.model().info.cycle.start == at (9));
    CHECK (s.model().info.cycle.end == at (10));

    // Nothing selected: nothing changes, and there's no error message.
    const auto undoName = s.undoName();
    CHECK_FALSE (s.setCycleToRegions ({}));
    CHECK (s.undoName() == undoName);
    CHECK (s.model().info.cycle.start == at (9));
}

TEST_CASE ("WAV files round-trip in 16-bit, 24-bit and float formats", "[U][media]")
{
    media::AudioData audio;
    audio.sampleRate = 44100;
    audio.channels.assign (2, std::vector<float> (1000));
    for (int i = 0; i < 1000; ++i)
    {
        audio.channels[0][static_cast<std::size_t> (i)] = static_cast<float> (i % 200 - 100) / 128.0f;
        audio.channels[1][static_cast<std::size_t> (i)] = -audio.channels[0][static_cast<std::size_t> (i)] * 0.5f;
    }
    const auto dir = std::filesystem::temp_directory_path() / "weft-wav-test";
    std::filesystem::create_directories (dir);

    for (const auto [format, tolerance] : { std::pair { media::SampleFormat::int16, 1.0 / 32768 }, std::pair { media::SampleFormat::int24, 1.0 / 8388608 },
                                             std::pair { media::SampleFormat::float32, 0.0 } })
    {
        const auto file = dir / "t.wav";
        std::string error;
        REQUIRE (media::writeWav (file, audio, format, &error));
        const auto back = media::readWav (file, &error);
        REQUIRE (back);
        CHECK (back->sampleRate == 44100);
        REQUIRE (back->numChannels() == 2);
        REQUIRE (back->numFrames() == 1000);
        for (std::size_t ch = 0; ch < 2; ++ch)
            for (std::size_t i = 0; i < 1000; ++i)
                REQUIRE (std::abs (back->channels[ch][i] - audio.channels[ch][i]) <= tolerance);
    }
    CHECK_FALSE (media::readWav (dir / "missing.wav"));
    CHECK (media::mediaFileName ("b3:abc") == "abc.wav");
    std::filesystem::remove_all (dir);
}
