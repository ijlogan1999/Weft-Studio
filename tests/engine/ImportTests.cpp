// Drag-and-drop import (spec 06 AT-ARR-25 with WAV, AT-ARR-26). The MP3 part of
// AT-ARR-25 is in JuceImportTests.cpp, which links the platform decoders.

#include "EngineFixtures.h"
#include "../fixtures/SmfWriter.h"

#include "weft/engine/arrange/Import.h"
#include "weft/media/Hash.h"

#include <catch2/catch_approx.hpp>

#include <fstream>
#include <numbers>

using namespace weft;
using namespace weft::test;

namespace
{
std::filesystem::path writeSine (const std::filesystem::path& dir, const std::string& name, double rate, int channels, double seconds)
{
    media::AudioData a;
    a.sampleRate = rate;
    const auto frames = static_cast<std::size_t> (rate * seconds);
    a.channels.assign (static_cast<std::size_t> (channels), std::vector<float> (frames));
    for (std::size_t i = 0; i < frames; ++i)
        for (auto& c : a.channels)
            c[i] = static_cast<float> (0.5 * std::sin (2.0 * std::numbers::pi * 440.0 * static_cast<double> (i) / rate));
    const auto file = dir / name;
    REQUIRE (media::writeWav (file, a, media::SampleFormat::int16));
    return file;
}

std::filesystem::path writeBytes (const std::filesystem::path& file, const std::vector<std::uint8_t>& bytes)
{
    std::ofstream out (file, std::ios::binary);
    out.write (reinterpret_cast<const char*> (bytes.data()), static_cast<std::streamsize> (bytes.size()));
    return file;
}
} // namespace

TEST_CASE ("AT-ARR-25: dropped audio is converted, hashed and placed; unsupported files are named", "[E][import][AT-ARR-25]")
{
    Rig rig;
    TempFolder drops;
    const auto mediaDir = rig.folder.path / "Media";
    const media::DecoderSet decoders;
    const auto track = rig.session.createTrack (TrackKind::audio, "Gtr", 2).value();

    SECTION ("a 44.1 kHz stereo file at bar 3 becomes a 48 kHz WAV in Media/")
    {
        const auto file = writeSine (drops.path, "riff.wav", 44100, 2, 1.0);
        const auto result = engine::importFiles (rig.session, rig.media, mediaDir, decoders, { file }, { 2 * barTicks, track });
        REQUIRE (result.regions.size() == 1);
        CHECK (result.tracks.empty());
        const auto& r = *rig.session.model().region (result.regions[0]);
        CHECK (r.trackId == track);
        CHECK (r.start == 2 * barTicks);
        const auto* m = rig.session.model().mediaRef (r.audio.media);
        REQUIRE (m != nullptr);
        CHECK (m->sampleRate == 48000);
        CHECK (m->channels == 2);
        CHECK (m->lengthSamples == 48000);
        CHECK (m->originalName == "riff.wav");
        const auto stored = rig.mediaFile (r.audio.media);
        REQUIRE (std::filesystem::exists (stored));
        CHECK (media::hashFile (stored) == r.audio.media);
        CHECK (media::readWavInfo (stored)->sampleRate == 48000);
        CHECK (rig.session.undoName() == "Import");

        // And it plays.
        rig.sync();
        rig.engine.play (2 * barTicks);
        const auto out = rig.run (4800);
        CHECK (firstNonSilent (out) >= 0);
    }
    SECTION ("three files below the last track make three tracks, one undo step")
    {
        const auto before = rig.session.model().trackOrder.size();
        const auto result = engine::importFiles (rig.session, rig.media, mediaDir, decoders,
                                                 { writeSine (drops.path, "a.wav", 48000, 1, 0.5), writeSine (drops.path, "b.wav", 48000, 2, 0.5),
                                                   writeSine (drops.path, "c.wav", 96000, 1, 0.5) },
                                                 { barTicks, {} });
        CHECK (result.tracks.size() == 3);
        CHECK (result.regions.size() == 3);
        CHECK (rig.session.model().trackOrder.size() == before + 3);
        CHECK (rig.session.model().track (result.tracks[0])->name == "a");
        CHECK (rig.session.model().track (result.tracks[0])->channels == 1);
        CHECK (rig.session.model().track (result.tracks[1])->channels == 2);
        REQUIRE (rig.session.undo().done);
        CHECK (rig.session.model().trackOrder.size() == before);
    }
    SECTION ("several files on one audio track go one after another")
    {
        const auto result = engine::importFiles (rig.session, rig.media, mediaDir, decoders,
                                                 { writeSine (drops.path, "a.wav", 48000, 2, 1.0), writeSine (drops.path, "b.wav", 48000, 2, 1.0) },
                                                 { 0, track });
        REQUIRE (result.regions.size() == 2);
        CHECK (rig.session.model().region (result.regions[1])->start == barTicks / 2);
    }
    SECTION ("unsupported files are rejected with their names; the rest still import")
    {
        std::ofstream (drops.path / "notes.txt") << "hello";
        std::ofstream (drops.path / "broken.wav") << "not a wav";
        const auto heads = rig.session.document().heads();
        const auto none = engine::importFiles (rig.session, rig.media, mediaDir, decoders, { drops.path / "notes.txt", drops.path / "broken.wav" }, { 0, {} });
        CHECK (none.regions.empty());
        CHECK (none.rejected == std::vector<std::string> { "notes.txt", "broken.wav" });
        CHECK (none.message == "Couldn't import notes.txt, broken.wav: unsupported or damaged files.");
        CHECK (rig.session.document().heads() == heads);

        const auto some = engine::importFiles (rig.session, rig.media, mediaDir, decoders,
                                               { drops.path / "notes.txt", writeSine (drops.path, "ok.wav", 48000, 1, 0.2) }, { 0, {} });
        CHECK (some.regions.size() == 1);
        CHECK (some.message == "Couldn't import notes.txt: unsupported or damaged file.");
    }
    SECTION ("a file that isn't there is reported as missing, not as damaged")
    {
        const auto result = engine::importFiles (rig.session, rig.media, mediaDir, decoders,
                                                 { drops.path / "gone.wav", writeSine (drops.path, "ok.wav", 48000, 1, 0.2) }, { 0, {} });
        CHECK (result.regions.size() == 1);
        CHECK (result.rejected == std::vector<std::string> { "gone.wav" });
        CHECK (result.message == "Couldn't find gone.wav.");
    }
    SECTION ("a multichannel file keeps two channels and says so (D-ARR-7)")
    {
        const auto result = engine::importFiles (rig.session, rig.media, mediaDir, decoders, { writeSine (drops.path, "surround.wav", 48000, 6, 0.2) }, { 0, {} });
        REQUIRE (result.regions.size() == 1);
        CHECK (rig.session.model().mediaRef (rig.session.model().region (result.regions[0])->audio.media)->channels == 2);
        CHECK (result.message == "Kept the first two channels of surround.wav.");
    }
}

TEST_CASE ("AT-ARR-26: a type 1 MIDI file makes instrument tracks; an empty project takes its tempo (D-AT-1)", "[E][import][AT-ARR-26]")
{
    TempFolder drops;
    // 3 tracks of notes plus a conductor track at 100 BPM, 480 PPQN.
    std::vector<std::vector<std::uint8_t>> tracks { smfTrack ({ smfTempo (0, 100.0) }) };
    for (int t = 0; t < 3; ++t)
        tracks.push_back (smfTrack ({ smfName ("Part " + std::to_string (t + 1)),
                                      { 0, { 0x90, static_cast<std::uint8_t> (60 + t), 100 } },
                                      { 240, { 0x80, static_cast<std::uint8_t> (60 + t), 64 } },
                                      { 1440, { 0x90, 72, 90 } },
                                      { 1920, { 0x80, 72, 64 } } }));
    const auto file = writeBytes (drops.path / "song.mid", smfFile (1, 480, tracks));

    auto checkTracks = [] (Rig& rig, const engine::ImportOutcome& result, Ticks at) {
        REQUIRE (result.tracks.size() == 3);
        for (std::size_t i = 0; i < 3; ++i)
        {
            const auto* t = rig.session.model().track (result.tracks[i]);
            CHECK (t->kind == TrackKind::instrument);
            CHECK (t->name == "Part " + std::to_string (i + 1));
            CHECK (! t->strip.instrument.has_value()); // plays Weft Keys
            const auto regions = rig.regionsOn (t->id);
            REQUIRE (regions.size() == 1);
            CHECK (regions[0]->start == at);
            const auto notes = rig.session.model().sequence (regions[0]->midi.sequenceId)->sortedNotes();
            REQUIRE (notes.size() == 2);
            CHECK (notes[0]->pos == 0);
            CHECK (notes[0]->length == 480);
            CHECK (notes[0]->pitch == 60 + static_cast<int> (i));
            CHECK (notes[1]->pos == 2880);
            CHECK (notes[1]->length == 960);
        }
    };

    SECTION ("an empty project adopts the file's tempo")
    {
        Rig rig;
        const auto result = engine::importFiles (rig.session, rig.media, rig.folder.path / "Media", {}, { file }, { 0, {} });
        checkTracks (rig, result, 0);
        CHECK (rig.session.model().tempo.bpmAt (0) == Catch::Approx (100.0).epsilon (1e-5));
    }
    SECTION ("a project with regions keeps its tempo")
    {
        Rig rig;
        rig.addAudioRegion (rig.audioTrack(), 0, 4800);
        const auto result = engine::importFiles (rig.session, rig.media, rig.folder.path / "Media", {}, { file }, { 4 * barTicks, {} });
        checkTracks (rig, result, 4 * barTicks);
        CHECK (rig.session.model().tempo.bpmAt (0) == 120.0);
        REQUIRE (rig.session.undo().done);
        CHECK (rig.session.model().trackOrder.size() == 2);
    }
}
