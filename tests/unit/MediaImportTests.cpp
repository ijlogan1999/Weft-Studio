// Import building blocks: the resampler and the MIDI file reader (D-DM-4, F-ARR-27).

#include "Fixtures.h"
#include "../fixtures/SmfWriter.h"

#include "weft/media/MidiFile.h"
#include "weft/media/Resample.h"

#include <catch2/catch_approx.hpp>

#include <cmath>
#include <numbers>

using namespace weft;
using namespace weft::test;

TEST_CASE ("Resampling 44.1 kHz to 48 kHz keeps a sine clean", "[U][media][import]")
{
    media::AudioData in;
    in.sampleRate = 44100;
    in.channels.assign (2, std::vector<float> (44100));
    for (int i = 0; i < 44100; ++i)
    {
        in.channels[0][static_cast<std::size_t> (i)] = static_cast<float> (0.5 * std::sin (2.0 * std::numbers::pi * 1000.0 * i / 44100.0));
        in.channels[1][static_cast<std::size_t> (i)] = static_cast<float> (0.25 * std::sin (2.0 * std::numbers::pi * 5000.0 * i / 44100.0));
    }
    const auto out = media::resample (in, 48000);
    REQUIRE (out.sampleRate == 48000);
    REQUIRE (out.numChannels() == 2);
    REQUIRE (out.numFrames() == 48000);
    double worst = 0;
    for (int i = 200; i < 47800; ++i) // away from the edges, where the filter runs off the file
    {
        const double t = i / 48000.0;
        worst = std::max (worst, std::abs (out.channels[0][static_cast<std::size_t> (i)] - 0.5 * std::sin (2.0 * std::numbers::pi * 1000.0 * t)));
        worst = std::max (worst, std::abs (out.channels[1][static_cast<std::size_t> (i)] - 0.25 * std::sin (2.0 * std::numbers::pi * 5000.0 * t)));
    }
    CHECK (20.0 * std::log10 (worst) < -80.0);
    CHECK (media::resample (in, 44100).channels == in.channels);
    CHECK (media::resampledLength (44100, 44100, 96000) == 96000);
}

TEST_CASE ("Downsampling removes content above the new Nyquist frequency", "[U][media][import]")
{
    media::AudioData in;
    in.sampleRate = 96000;
    in.channels.assign (1, std::vector<float> (96000));
    for (int i = 0; i < 96000; ++i)
        in.channels[0][static_cast<std::size_t> (i)] = static_cast<float> (0.5 * std::sin (2.0 * std::numbers::pi * 30000.0 * i / 96000.0));
    const auto out = media::resample (in, 48000);
    float peak = 0;
    for (int i = 500; i < 47500; ++i)
        peak = std::max (peak, std::abs (out.channels[0][static_cast<std::size_t> (i)]));
    CHECK (20.0 * std::log10 (peak / 0.5) < -80.0);
}

TEST_CASE ("Reading a type 1 MIDI file", "[U][media][import]")
{
    const auto conductor = smfTrack ({ smfTempo (0, 100.0), { 0, { 0xff, 0x58, 0x04, 3, 2, 24, 8 } } });
    const auto piano = smfTrack ({
        smfName ("Piano"),
        { 0, { 0x90, 60, 100 } },
        { 240, { 64, 90 } },        // running status: note on 64
        { 480, { 0x80, 60, 30 } },
        { 480, { 0x90, 64, 0 } },   // note on with velocity 0 ends a note
        { 600, { 0xb1, 64, 127 } },
        { 600, { 0xe1, 0x00, 0x60 } },
        { 700, { 0xc1, 5 } },
        { 900, { 0x91, 67, 80 } },  // still held at the end of the track
    });
    const auto bytes = smfFile (1, 480, { conductor, piano });
    std::string error;
    const auto file = media::parseMidiFile (bytes, &error);
    REQUIRE (file.has_value());
    CHECK (file->format == 1);
    CHECK (file->ticksPerQuarter == 480);
    REQUIRE (file->tracks.size() == 2);
    CHECK (file->tracks[0].notes.empty());
    REQUIRE (file->tempos.size() == 1);
    CHECK (file->tempos[0].bpm == Catch::Approx (100.0).epsilon (1e-5));
    REQUIRE (file->signatures.size() == 1);
    CHECK (file->signatures[0].numerator == 3);
    CHECK (file->signatures[0].denominator == 4);

    const auto& t = file->tracks[1];
    CHECK (t.name == "Piano");
    REQUIRE (t.notes.size() == 3);
    CHECK (t.notes[0].pitch == 60);
    CHECK (t.notes[0].length == 480);
    CHECK (t.notes[0].releaseVelocity == 30);
    CHECK (t.notes[1].pitch == 64);
    CHECK (t.notes[1].tick == 240);
    CHECK (t.notes[1].length == 240);
    CHECK (t.notes[2].channel == 2);
    CHECK (t.notes[2].length == 1); // ends with the track (end of track at 900)
    REQUIRE (t.controllers.size() == 3);
    CHECK (t.controllers[0].kind == "cc");
    CHECK (t.controllers[0].channel == 2);
    CHECK (t.controllers[1].kind == "pitchBend");
    CHECK (t.controllers[1].value == (0x60 << 7) - 8192);
    CHECK (t.controllers[2].kind == "programChange");
    CHECK (t.controllers[2].value == 5);
}

TEST_CASE ("Damaged and unsupported MIDI files are rejected with a reason", "[U][media][import]")
{
    std::string error;
    const std::vector<std::uint8_t> junk { 'R', 'I', 'F', 'F', 0, 0 };
    CHECK (! media::parseMidiFile (junk, &error));
    CHECK (error == "Not a Standard MIDI File.");
    auto smpte = smfFile (0, 480, { smfTrack ({}) });
    smpte[12] = 0xe7; // -25 fps
    CHECK (! media::parseMidiFile (smpte, &error));
    CHECK (error == "MIDI files with SMPTE timing aren't supported.");
}
