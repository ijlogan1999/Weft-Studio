// Export Regions as Files (L-EXP-2, L-EXP-3; spec 06 AT-EXP-12, AT-EXP-13).

#include "EngineFixtures.h"

#include "weft/engine/arrange/OfflineRender.h"
#include "weft/media/MidiFile.h"
#include "weft/media/WavFile.h"

#include <catch2/catch_approx.hpp>

using namespace weft;
using namespace weft::test;

TEST_CASE ("AT-EXP-12: an audio region exports as a WAV as it plays: gain and fades, no strip", "[E][export][AT-EXP-12]")
{
    Rig rig;
    TempFolder out;
    const auto track = rig.audioTrack();
    const auto r = rig.addAudioRegion (track, barTicks, 48000);
    REQUIRE (rig.session.setRegionGain (r, -6.0206));
    REQUIRE (rig.session.setRegionFades (r, model::Fade { 4800, "linear" }, std::nullopt));
    REQUIRE (rig.session.setRegionName (r, "Gtr: take/1"));
    REQUIRE (rig.session.setTrackVolume (track, -20.0)); // the strip isn't part of the export

    const auto result = engine::exportRegions (rig.session.model(), { r }, rig.media, out.path, 32);
    REQUIRE (result.files.size() == 1);
    CHECK (result.files[0].filename() == "Gtr_ take_1.wav");
    CHECK (result.message == "Exported 1 region to " + out.path.filename().string() + ".");
    const auto audio = media::readWav (result.files[0]);
    REQUIRE (audio);
    CHECK (audio->numFrames() == 48000);
    CHECK (audio->channels[0][10000] == Catch::Approx (rampValue (10000) * 0.5).epsilon (1e-3));
    CHECK (audio->channels[0][2400] == Catch::Approx (rampValue (2400) * 0.5 * 0.5).epsilon (2e-2)); // halfway up the fade

    // A second export keeps both files.
    const auto again = engine::exportRegions (rig.session.model(), { r }, rig.media, out.path, 24);
    REQUIRE (again.files.size() == 1);
    CHECK (again.files[0].filename() == "Gtr_ take_1 (2).wav");
}

TEST_CASE ("AT-EXP-13: a MIDI region exports as a MIDI file as it plays, from the region start", "[E][export][AT-EXP-13]")
{
    Rig rig;
    TempFolder out;
    const auto keys = rig.session.createTrack (TrackKind::instrument, "Keys").value();
    REQUIRE (rig.session.setTempoAt (0, 100.0));
    const auto region = rig.session.createMidiRegion (keys, 2 * barTicks, barTicks).value();
    REQUIRE (rig.session.setRegionName (region, "Lead"));
    model::Note a, b, muted, tail;
    a.pos = 0;
    a.length = 480;
    a.pitch = 60;
    a.velocity = 90;
    b.pos = 960;
    b.length = 10 * barTicks; // cut at the region end
    b.pitch = 64;
    muted.pos = 480;
    muted.muted = true;
    REQUIRE (rig.session.addNote (region, a));
    REQUIRE (rig.session.addNote (region, b));
    REQUIRE (rig.session.addNote (region, muted));
    REQUIRE (rig.session.setRegionTranspose (region, 12));
    REQUIRE (rig.session.setRegionVelocityOffset (region, 10));

    const auto result = engine::exportRegions (rig.session.model(), { region }, rig.media, out.path, 24);
    REQUIRE (result.files.size() == 1);
    CHECK (result.files[0].filename() == "Lead.mid");
    const auto file = media::readMidiFile (result.files[0]);
    REQUIRE (file);
    CHECK (file->ticksPerQuarter == 960);
    REQUIRE (file->tempos.size() == 1);
    CHECK (file->tempos[0].bpm == Catch::Approx (100.0).epsilon (1e-4));
    const auto named = std::find_if (file->tracks.begin(), file->tracks.end(), [] (const media::MidiFileTrack& t) { return t.name == "Lead"; });
    REQUIRE (named != file->tracks.end());
    REQUIRE (named->notes.size() == 2);
    CHECK (named->notes[0].tick == 0);
    CHECK (named->notes[0].pitch == 72);
    CHECK (named->notes[0].velocity == 100);
    CHECK (named->notes[0].length == 480);
    CHECK (named->notes[1].tick == 960);
    CHECK (named->notes[1].length == barTicks - 960);
}

TEST_CASE ("MIDI files written by Weft read back the same (L-EXP-3)", "[U][export][midi]")
{
    media::MidiFileData data;
    data.ticksPerQuarter = 960;
    data.tempos = { { 0, 90.0 }, { 3840, 140.0 } };
    data.signatures = { { 0, 6, 8 } };
    media::MidiFileTrack t;
    t.name = "Bass";
    t.notes = { { 0, 960, 2, 40, 100, 64 }, { 960, 1, 2, 41, 1, 0 }, { 960, 480, 1, 127, 127, 127 } };
    t.controllers = { { 10, "cc", 7, 100, 2 }, { 20, "pitchBend", 0, -8192, 1 }, { 30, "pitchBend", 0, 8191, 1 }, { 40, "programChange", 0, 5, 3 } };
    data.tracks.push_back (t);
    const auto bytes = media::encodeMidiFile (data);
    const auto back = media::parseMidiFile (bytes);
    REQUIRE (back);
    CHECK (back->format == 1);
    REQUIRE (back->tempos.size() == 2);
    CHECK (back->tempos[1].tick == 3840);
    CHECK (back->tempos[1].bpm == Catch::Approx (140.0).epsilon (1e-4));
    REQUIRE (back->signatures.size() == 1);
    CHECK (back->signatures[0].numerator == 6);
    CHECK (back->signatures[0].denominator == 8);
    REQUIRE (back->tracks.size() == 2); // conductor + Bass
    const auto& bass = back->tracks[1];
    CHECK (bass.name == "Bass");
    REQUIRE (bass.notes.size() == 3);
    for (std::size_t i = 0; i < 3; ++i)
    {
        const auto& n = *std::find_if (bass.notes.begin(), bass.notes.end(), [&] (const media::MidiFileNote& x) { return x.pitch == t.notes[i].pitch; });
        CHECK (n.tick == t.notes[i].tick);
        CHECK (n.length == t.notes[i].length);
        CHECK (n.channel == t.notes[i].channel);
        CHECK (n.velocity == t.notes[i].velocity);
    }
    REQUIRE (bass.controllers.size() == 4);
    CHECK (bass.controllers[1].value == -8192);
    CHECK (bass.controllers[2].value == 8191);
    CHECK (bass.controllers[3].kind == "programChange");
    CHECK (bass.controllers[3].channel == 3);
}
