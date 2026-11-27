// Region edits as they play (spec 06 AT-ARR-8, 9, 11, 13, 14, 15, 16, 17).

#include "EngineFixtures.h"

#include "weft/engine/arrange/OfflineRender.h"
#include "weft/engine/dsp/Stretch.h"

#include <catch2/catch_approx.hpp>

#include <numbers>

using namespace weft;
using namespace weft::test;

namespace
{
media::AudioData constantMedia (std::int64_t frames, float value = 1.0f)
{
    media::AudioData a;
    a.sampleRate = 48000;
    a.channels.assign (1, std::vector<float> (static_cast<std::size_t> (frames), value));
    return a;
}

media::AudioData sineMedia (std::int64_t frames, double hz = 440.0, float amplitude = 0.5f)
{
    media::AudioData a;
    a.sampleRate = 48000;
    a.channels.assign (1, std::vector<float> (static_cast<std::size_t> (frames)));
    for (std::int64_t i = 0; i < frames; ++i)
        a.channels[0][static_cast<std::size_t> (i)] = amplitude * static_cast<float> (std::sin (2.0 * std::numbers::pi * hz * static_cast<double> (i) / 48000.0));
    return a;
}

void addMedia (Rig& rig, const std::string& hash, const media::AudioData& audio)
{
    rig.media.add (hash, audio);
    model::MediaRef m;
    m.hash = hash;
    m.originalName = "test.wav";
    m.lengthSamples = static_cast<Samples> (audio.channels[0].size());
    REQUIRE (rig.session.addMedia (m));
}

std::vector<float> play (Rig& rig, std::int64_t frames, Ticks from = 0)
{
    rig.sync();
    rig.engine.play (from);
    return rig.run (frames);
}

Ticks samplesToTicks (std::int64_t s) { return s * barTicks / barSamples; }
} // namespace

TEST_CASE ("AT-ARR-13: fade curves on a constant signal, and fades shrink with the region", "[E][arrange][AT-ARR-13]")
{
    Rig rig;
    const auto track = rig.audioTrack();
    addMedia (rig, "b3:one", constantMedia (96000));
    const auto r = rig.session.createAudioRegion (track, 0, "b3:one", 0, 96000).value();

    SECTION ("linear")
    {
        REQUIRE (rig.session.setRegionFades (r, model::Fade { 4800, "linear" }, std::nullopt));
        const auto out = play (rig, 9600);
        for (int i = 0; i < 4800; ++i)
            REQUIRE (out[static_cast<std::size_t> (i)] == Catch::Approx (i / 4800.0).margin (1e-6));
        CHECK (out[4800] == 1.0f);
    }
    SECTION ("equal power")
    {
        REQUIRE (rig.session.setRegionFades (r, model::Fade { 4800, "equalPower" }, std::nullopt));
        const auto out = play (rig, 4800);
        for (int i = 0; i < 4800; ++i)
            REQUIRE (out[static_cast<std::size_t> (i)] == Catch::Approx (std::sin (std::numbers::pi / 2.0 * i / 4800.0)).margin (1e-6));
    }
    SECTION ("S-curve is monotonic and symmetric about its midpoint")
    {
        REQUIRE (rig.session.setRegionFades (r, model::Fade { 4800, "sCurve" }, std::nullopt));
        const auto out = play (rig, 4801);
        for (int i = 1; i < 4800; ++i)
        {
            REQUIRE (out[static_cast<std::size_t> (i)] >= out[static_cast<std::size_t> (i - 1)]);
            REQUIRE (out[static_cast<std::size_t> (i)] + out[static_cast<std::size_t> (4800 - i)] == Catch::Approx (1.0).margin (1e-6));
        }
    }
    SECTION ("trimming shorter than both fades shrinks them in proportion")
    {
        REQUIRE (rig.session.setRegionFades (r, model::Fade { 4800, "linear" }, model::Fade { 2400, "linear" }));
        REQUIRE (rig.session.trimRegionLength (r, 3600));
        const auto& region = *rig.session.model().region (r);
        CHECK (region.audio.fadeIn.length == 2400);
        CHECK (region.audio.fadeOut.length == 1200);
    }
}

TEST_CASE ("AT-ARR-14: an equal-power crossfade is +3 dB at its midpoint and has no clicks", "[E][arrange][AT-ARR-14]")
{
    Rig rig;
    const auto track = rig.audioTrack();
    // Region A: 0-96,000. Region B starts 9,600 samples before A ends, reading the same
    // material at the same time (correlated), and runs on.
    const std::int64_t overlap = 9600, bStart = 96000 - overlap;

    SECTION ("summed gain of correlated equal signals")
    {
        addMedia (rig, "b3:one", constantMedia (192000));
        const auto a = rig.session.createAudioRegion (track, 0, "b3:one", 0, 96000).value();
        const auto b = rig.session.createAudioRegion (track, samplesToTicks (bStart), "b3:one", bStart, 96000).value();
        REQUIRE (rig.session.crossfadeRegions ({ a, b }));
        CHECK (rig.session.model().region (a)->audio.fadeOut.length == overlap);
        CHECK (rig.session.model().region (b)->audio.fadeIn.length == overlap);
        const auto out = play (rig, 192000);
        const auto mid = out[static_cast<std::size_t> (bStart + overlap / 2)];
        CHECK (20.0 * std::log10 (mid) == Catch::Approx (3.01).margin (0.1));
        CHECK (out[static_cast<std::size_t> (bStart - 10)] == 1.0f);
        CHECK (out[static_cast<std::size_t> (96000 + 10)] == 1.0f);
    }
    SECTION ("no discontinuities on a sine")
    {
        addMedia (rig, "b3:sine", sineMedia (192000));
        const auto a = rig.session.createAudioRegion (track, 0, "b3:sine", 0, 96000).value();
        const auto b = rig.session.createAudioRegion (track, samplesToTicks (bStart), "b3:sine", bStart, 96000).value();
        REQUIRE (rig.session.crossfadeRegions ({ a, b }));
        const auto out = play (rig, 192000);
        // A 440 Hz sine at 0.5 changes by at most 0.029 per sample; the crossfade lifts it by 3 dB at most.
        float maxDelta = 0;
        for (std::size_t i = 1; i < 190000; ++i)
            maxDelta = std::max (maxDelta, std::abs (out[i] - out[i - 1]));
        CHECK (maxDelta < 0.05f);
    }
}

TEST_CASE ("AT-ARR-15: region gain", "[E][arrange][AT-ARR-15]")
{
    Rig rig;
    const auto track = rig.audioTrack();
    const auto r = rig.addAudioRegion (track, 0, 48000);
    REQUIRE (rig.session.setRegionGain (r, -6.02));
    auto out = play (rig, 48000);
    CHECK (20.0 * std::log10 (out[1000] / rampValue (1000)) == Catch::Approx (-6.02).margin (0.01));
    CHECK (out[1000] == Catch::Approx (rampValue (1000) * 0.5).epsilon (0.002));

    REQUIRE (rig.session.setRegionGain (r, minusInfinityDb));
    CHECK (firstNonSilent (play (rig, 48000)) == -1);

    // Above +24 dB is refused, not clamped (R0 parity fix); +24 itself is the top.
    CHECK_FALSE (rig.session.setRegionGain (r, 40.0));
    CHECK (rig.session.model().region (r)->audio.gainDb == minusInfinityDb);
    REQUIRE (rig.session.setRegionGain (r, 24.0));
    CHECK (rig.session.model().region (r)->audio.gainDb == 24.0);
    out = play (rig, 48000);
    CHECK (out[10] == Catch::Approx (rampValue (10) * std::pow (10.0, 24.0 / 20.0)).epsilon (1e-5));
}

TEST_CASE ("AT-ARR-16: a looped MIDI region repeats its notes and follows source edits", "[E][arrange][AT-ARR-16]")
{
    Rig rig;
    const auto keys = rig.session.createTrack (TrackKind::instrument, "Keys").value();
    auto probe = std::make_shared<ProbeInstrument>();
    rig.rack.set (keys, probe);
    const auto m = rig.session.createMidiRegion (keys, 0, barTicks).value();
    model::Note beat1, beat3;
    beat1.pos = 0;
    beat1.length = 240;
    beat3.pos = 1920;
    beat3.length = 240;
    beat3.pitch = 67;
    REQUIRE (rig.session.addNote (m, beat1));
    const auto beat3Id = rig.session.addNote (m, beat3).value();

    auto noteOns = [&] (int pitch = -1) {
        std::vector<std::int64_t> frames;
        for (const auto& e : probe->seen)
            if (e.event.isNoteOn() && (pitch < 0 || e.event.data1 == pitch))
                frames.push_back (e.frame);
        return frames;
    };

    REQUIRE (rig.session.setRegionLoop (m, 4 * barTicks));
    probe->seen.clear();
    play (rig, 5 * barSamples);
    CHECK (noteOns().size() == 8);
    CHECK (noteOns (60) == std::vector<std::int64_t> { 0, barSamples, 2 * barSamples, 3 * barSamples });

    // Ends at bar 3 beat 3: 2.5 iterations, and the beat-3 note that would start there doesn't play.
    REQUIRE (rig.session.setRegionLoop (m, 2 * barTicks + 1920));
    probe->seen.clear();
    play (rig, 5 * barSamples);
    CHECK (noteOns (60).size() == 3);
    CHECK (noteOns (67).size() == 2);

    auto edited = rig.session.model().sequence (rig.session.model().region (m)->midi.sequenceId)->notes.at (beat3Id);
    edited.pitch = 72;
    REQUIRE (rig.session.updateNote (m, edited));
    probe->seen.clear();
    play (rig, 5 * barSamples);
    CHECK (noteOns (67).empty());
    CHECK (noteOns (72).size() == 2);
}

TEST_CASE ("AT-ARR-17: M mutes a region in playback, and again unmutes it", "[E][arrange][AT-ARR-17]")
{
    Rig rig;
    const auto track = rig.audioTrack();
    const auto r = rig.addAudioRegion (track, 0, 48000);
    REQUIRE (rig.session.toggleRegionsMuted ({ r }));
    CHECK (firstNonSilent (play (rig, 48000)) == -1);
    REQUIRE (rig.session.toggleRegionsMuted ({ r }));
    CHECK (play (rig, 48000)[100] == rampValue (100));
}

TEST_CASE ("AT-ARR-8 and AT-ARR-9: trimming and splitting don't change what plays", "[E][arrange][AT-ARR-8][AT-ARR-9]")
{
    Rig rig;
    const auto track = rig.audioTrack();
    const auto r = rig.addAudioRegion (track, 0, 2 * barSamples);
    REQUIRE (rig.session.setRegionFades (r, model::Fade { 480, "linear" }, model::Fade { 960, "sCurve" }));
    REQUIRE (rig.session.setRegionGain (r, -3.0));
    const auto before = play (rig, 3 * barSamples);

    SECTION ("split at bar 2 is sample-identical")
    {
        REQUIRE (rig.session.splitRegionsAt ({ r }, barTicks).size() == 1);
        CHECK (play (rig, 3 * barSamples) == before);
    }
    SECTION ("trimming the left edge by 4,800 samples leaves the rest in place")
    {
        REQUIRE (rig.session.setRegionFades (r, model::Fade { 0, "linear" }, model::Fade { 960, "sCurve" }));
        const auto untrimmed = play (rig, 3 * barSamples);
        REQUIRE (rig.session.trimRegionStart (r, samplesToTicks (4800)));
        const auto trimmed = play (rig, 3 * barSamples);
        for (std::size_t i = 0; i < 4800; ++i)
            REQUIRE (trimmed[i] == 0.0f);
        for (std::size_t i = 4800; i < trimmed.size(); ++i)
            REQUIRE (trimmed[i] == untrimmed[i]);
    }
}

TEST_CASE ("AT-ARR-11: joining MIDI, contiguous audio, and rendered audio", "[E][arrange][AT-ARR-11]")
{
    Rig rig;
    const auto mediaDir = rig.folder.path / "Media";

    SECTION ("MIDI notes keep their absolute positions")
    {
        const auto keys = rig.session.createTrack (TrackKind::instrument, "Keys").value();
        const auto a = rig.session.createMidiRegion (keys, 0, barTicks).value();
        const auto b = rig.session.createMidiRegion (keys, 2 * barTicks, barTicks).value();
        model::Note n;
        n.pos = 100;
        REQUIRE (rig.session.addNote (a, n));
        REQUIRE (rig.session.addNote (b, n));
        REQUIRE (engine::joinRegions (rig.session, { a, b }, rig.media, mediaDir));
        CHECK (rig.session.model().region (b) == nullptr);
        const auto& joined = *rig.session.model().region (a);
        std::vector<Ticks> positions;
        for (const auto* note : rig.session.model().sequence (joined.midi.sequenceId)->sortedNotes())
            positions.push_back (joined.start + note->pos - joined.midi.sequenceOffset);
        CHECK (positions == std::vector<Ticks> { 100, 2 * barTicks + 100 });
    }
    SECTION ("contiguous pieces of one file join without rendering")
    {
        const auto track = rig.audioTrack();
        const auto r = rig.addAudioRegion (track, 0, 2 * barSamples);
        const auto right = rig.session.splitRegionsAt ({ r }, barTicks);
        const auto mediaCount = rig.session.model().media.size();
        REQUIRE (engine::joinRegions (rig.session, { r, right[0] }, rig.media, mediaDir));
        CHECK (rig.session.model().media.size() == mediaCount);
        CHECK (rig.session.model().region (r)->length == 2 * barSamples);
    }
    SECTION ("anything else renders a new file that plays the same")
    {
        const auto track = rig.audioTrack();
        addMedia (rig, "b3:sine", sineMedia (4 * barSamples, 330.0, 0.4f));
        const auto a = rig.session.createAudioRegion (track, 0, "b3:sine", 1000, barSamples).value();
        const auto b = rig.session.createAudioRegion (track, barTicks + barTicks / 4, "b3:sine", 50000, barSamples).value();
        REQUIRE (rig.session.setRegionGain (b, -4.0));
        REQUIRE (rig.session.setRegionFades (a, model::Fade { 2000, "equalPower" }, model::Fade { 3000, "linear" }));
        REQUIRE (rig.session.setRegionFades (b, model::Fade { 500, "sCurve" }, model::Fade { 4000, "equalPower" }));
        const auto before = play (rig, 3 * barSamples);
        const auto mediaCount = rig.session.model().media.size();

        REQUIRE (engine::joinRegions (rig.session, { a, b }, rig.media, mediaDir));
        CHECK (rig.session.model().media.size() == mediaCount + 1);
        const auto regions = rig.regionsOn (track);
        REQUIRE (regions.size() == 1);
        const auto& joined = *regions[0];
        CHECK (joined.start == 0);
        CHECK (joined.audio.sourceOffset == 0);
        CHECK (joined.audio.fadeIn.length == 2000);
        CHECK (joined.audio.fadeOut.length == 4000);
        CHECK (joined.length == barSamples + barSamples / 4 + barSamples);
        CHECK (std::filesystem::exists (rig.mediaFile (joined.audio.media)));
        CHECK (rig.session.undoName() == "Join Regions");

        const auto after = play (rig, 3 * barSamples);
        const float lsb = 1.0f / 8388608.0f;
        float worst = 0;
        for (std::size_t i = 0; i < before.size(); ++i)
            worst = std::max (worst, std::abs (after[i] - before[i]));
        CHECK (worst <= lsb * 1.01f);

        REQUIRE (rig.session.undo().done);
        CHECK (rig.regionsOn (track).size() == 2);
    }
}

TEST_CASE ("AT-LIB-1 (engine part): a file preview plays once on the outputs, at the device rate", "[E][arrange][AT-LIB-1]")
{
    Rig rig;
    auto audio = std::make_shared<media::AudioData> (constantMedia (24000, 0.25f));
    audio->sampleRate = 24000; // resampled to the device's 48 kHz
    rig.engine.preview (audio);
    auto out = rig.run (60000);
    CHECK (rig.engine.previewing() == false);
    CHECK (out[1000] == Catch::Approx (0.25f).margin (1e-3));
    CHECK (out[47000] == Catch::Approx (0.25f).margin (1e-3));
    CHECK (std::abs (out[49000]) < 1e-3f);
    // Stopping mid-way silences it.
    rig.engine.preview (audio);
    rig.run (256);
    CHECK (rig.engine.previewing());
    rig.engine.stopPreview();
    out = rig.run (512);
    CHECK (firstNonSilent (out) == -1);
}

TEST_CASE ("AT-ARR-29: Reverse plays a region's audio backwards; undo and a second reverse restore it", "[E][arrange][AT-ARR-29]")
{
    Rig rig;
    const auto track = rig.audioTrack();
    rig.media.add ("b3:ramp", rampMedia (20000));
    model::MediaRef m;
    m.hash = "b3:ramp";
    m.originalName = "ramp.wav";
    m.lengthSamples = 20000;
    REQUIRE (rig.session.addMedia (m));
    const auto r = rig.session.createAudioRegion (track, 0, "b3:ramp", 100, 4800).value();
    const auto mediaDir = rig.folder.path / "Media";

    REQUIRE (engine::reverseRegions (rig.session, { r }, rig.media, mediaDir));
    CHECK (rig.session.undoName() == "Reverse");
    const auto& reversed = *rig.session.model().region (r);
    CHECK (reversed.audio.sourceOffset == 0);
    CHECK (reversed.audio.media != "b3:ramp");
    CHECK (reversed.start == 0);
    CHECK (reversed.length == 4800);
    auto audio = engine::renderRegion (rig.session.model(), r, rig.media);
    REQUIRE (audio);
    CHECK (audio->channels[0][0] == rampValue (100 + 4799));
    CHECK (audio->channels[0][4799] == rampValue (100));

    // Reversing again plays forwards (from a file of its own).
    REQUIRE (engine::reverseRegions (rig.session, { r }, rig.media, mediaDir));
    audio = engine::renderRegion (rig.session.model(), r, rig.media);
    CHECK (audio->channels[0][0] == rampValue (100));
    rig.session.undo();
    rig.session.undo();
    CHECK (rig.session.model().region (r)->audio.media == "b3:ramp");
    CHECK (rig.session.model().region (r)->audio.sourceOffset == 100);

    // MIDI regions are left alone.
    const auto keys = rig.session.createTrack (TrackKind::instrument, "Keys").value();
    const auto midi = rig.session.createMidiRegion (keys, 0, barTicks).value();
    CHECK_FALSE (engine::reverseRegions (rig.session, { midi }, rig.media, mediaDir));
}

TEST_CASE ("AT-EDA-3: the Audio File Editor's functions rewrite the file and move every region using it", "[E][arrange][AT-EDA-3]")
{
    Rig rig;
    const auto track = rig.audioTrack();
    rig.media.add ("b3:ramp", rampMedia (20000));
    model::MediaRef m;
    m.hash = "b3:ramp";
    m.originalName = "ramp.wav";
    m.lengthSamples = 20000;
    REQUIRE (rig.session.addMedia (m));
    const auto a = rig.session.createAudioRegion (track, 0, "b3:ramp", 0, 4000).value();
    const auto b = rig.session.createAudioRegion (track, barTicks, "b3:ramp", 10000, 4000).value();
    const auto mediaDir = rig.folder.path / "Media";
    const auto fileOf = [&] (const Id& r) { return rig.session.model().region (r)->audio.media; };
    const auto read = [&] {
        auto audio = engine::readMediaFile (rig.session.model(), fileOf (a), rig.media, mediaDir);
        REQUIRE (audio);
        return std::move (*audio);
    };
    const auto near = [] (float x, float y) { return std::abs (x - y) < 1e-4f; };

    // Invert a selection: only that part changes, and both regions now play the new file.
    REQUIRE (engine::editAudioFile (rig.session, "b3:ramp", engine::FileEdit::invert, 100, 200, rig.media, mediaDir));
    CHECK (rig.session.undoName() == "Edit File: Invert");
    CHECK (fileOf (a) != "b3:ramp");
    CHECK (fileOf (b) == fileOf (a));
    CHECK (rig.session.model().mediaRef (fileOf (a))->originalName == "ramp.wav");
    auto audio = read();
    CHECK (audio.numFrames() == 20000);
    CHECK (near (audio.channels[0][150], -rampValue (150)));
    CHECK (near (audio.channels[0][99], rampValue (99)));
    CHECK (near (audio.channels[0][200], rampValue (200)));
    REQUIRE (rig.session.undo().done);
    CHECK (fileOf (a) == "b3:ramp");
    CHECK (fileOf (b) == "b3:ramp");

    // With nothing selected, Normalize brings the loudest sample of the whole file to 0 dBFS.
    REQUIRE (engine::editAudioFile (rig.session, "b3:ramp", engine::FileEdit::normalize, 0, 0, rig.media, mediaDir));
    audio = read();
    float peak = 0.0f;
    for (const auto x : audio.channels[0])
        peak = std::max (peak, std::abs (x));
    CHECK (peak == Catch::Approx (1.0f).margin (1e-4));
    REQUIRE (rig.session.undo().done);

    // Change Gain, Silence, Fade In, Remove DC Offset and Reverse on a selection.
    REQUIRE (engine::editAudioFile (rig.session, "b3:ramp", engine::FileEdit::gain, 0, 1000, rig.media, mediaDir, -6.0206));
    CHECK (near (read().channels[0][500], rampValue (500) * 0.5f));
    REQUIRE (rig.session.undo().done);
    REQUIRE (engine::editAudioFile (rig.session, "b3:ramp", engine::FileEdit::silence, 0, 1000, rig.media, mediaDir));
    CHECK (read().channels[0][500] == 0.0f);
    REQUIRE (rig.session.undo().done);
    REQUIRE (engine::editAudioFile (rig.session, "b3:ramp", engine::FileEdit::fadeIn, 0, 1001, rig.media, mediaDir));
    audio = read();
    CHECK (audio.channels[0][0] == 0.0f);
    CHECK (near (audio.channels[0][500], rampValue (500) * 0.5f));
    REQUIRE (rig.session.undo().done);
    REQUIRE (engine::editAudioFile (rig.session, "b3:ramp", engine::FileEdit::removeDc, 0, 1000, rig.media, mediaDir));
    audio = read();
    double sum = 0.0;
    for (int i = 0; i < 1000; ++i)
        sum += audio.channels[0][static_cast<std::size_t> (i)];
    CHECK (std::abs (sum / 1000.0) < 1e-4);
    REQUIRE (rig.session.undo().done);
    REQUIRE (engine::editAudioFile (rig.session, "b3:ramp", engine::FileEdit::reverse, 0, 1000, rig.media, mediaDir));
    CHECK (near (read().channels[0][0], rampValue (999)));
    REQUIRE (rig.session.undo().done);

    // Delete 1000 frames before region b: b's start in the file moves back; a (before the cut) stays.
    REQUIRE (engine::editAudioFile (rig.session, "b3:ramp", engine::FileEdit::remove, 5000, 6000, rig.media, mediaDir));
    CHECK (read().numFrames() == 19000);
    CHECK (rig.session.model().region (a)->audio.sourceOffset == 0);
    CHECK (rig.session.model().region (a)->length == 4000);
    CHECK (rig.session.model().region (b)->audio.sourceOffset == 9000);
    CHECK (rig.session.model().region (b)->length == 4000);
    REQUIRE (rig.session.undo().done);
    // Deleting part of a region shortens it.
    REQUIRE (engine::editAudioFile (rig.session, "b3:ramp", engine::FileEdit::remove, 1000, 2000, rig.media, mediaDir));
    CHECK (rig.session.model().region (a)->length == 3000);
    REQUIRE (rig.session.undo().done);
    // Deleting all of a region is refused.
    CHECK_FALSE (engine::editAudioFile (rig.session, "b3:ramp", engine::FileEdit::remove, 9000, 15000, rig.media, mediaDir));
    CHECK (fileOf (b) == "b3:ramp");

    // Paste 500 copied frames over frames [0, 100): later regions move 400 frames on.
    const auto clip = engine::copyMediaFrames (rig.session.model(), "b3:ramp", 1000, 1500, rig.media, mediaDir);
    REQUIRE (clip);
    CHECK (clip->numFrames() == 500);
    REQUIRE (engine::editAudioFile (rig.session, "b3:ramp", engine::FileEdit::insert, 0, 100, rig.media, mediaDir, 0.0, &*clip));
    CHECK (read().numFrames() == 20400);
    CHECK (near (read().channels[0][0], rampValue (1000)));
    CHECK (rig.session.model().region (b)->audio.sourceOffset == 10400);
    REQUIRE (rig.session.undo().done);

    // Trim keeps the selection; one that would leave region a with nothing is refused.
    CHECK_FALSE (engine::editAudioFile (rig.session, "b3:ramp", engine::FileEdit::trim, 10000, 14000, rig.media, mediaDir));
    REQUIRE (engine::editAudioFile (rig.session, "b3:ramp", engine::FileEdit::trim, 2000, 14000, rig.media, mediaDir));
    CHECK (rig.session.undoName() == "Trim Audio File");
    CHECK (read().numFrames() == 12000);
    CHECK (rig.session.model().region (a)->audio.sourceOffset == 0);
    CHECK (rig.session.model().region (a)->length == 2000);
    CHECK (rig.session.model().region (b)->audio.sourceOffset == 8000);
    CHECK (rig.session.model().region (b)->length == 4000);
    REQUIRE (rig.session.undo().done);
    CHECK_FALSE (engine::editAudioFile (rig.session, "b3:ramp", engine::FileEdit::trim, 0, 0, rig.media, mediaDir));
}

namespace
{
/** The frequency over [from, to) of the output, from its upward zero crossings. */
double frequencyOf (const std::vector<float>& v, std::size_t from, std::size_t to)
{
    std::vector<double> ups;
    for (std::size_t i = from + 1; i < to && i < v.size(); ++i)
        if (v[i - 1] < 0.0f && v[i] >= 0.0f)
            ups.push_back (static_cast<double> (i - 1) + static_cast<double> (-v[i - 1]) / static_cast<double> (v[i] - v[i - 1]));
    if (ups.size() < 2)
        return 0.0;
    return static_cast<double> (ups.size() - 1) / ((ups.back() - ups.front()) / 48000.0);
}

float peakOf (const std::vector<float>& v, std::size_t from, std::size_t to)
{
    float peak = 0.0f;
    for (std::size_t i = from; i < to && i < v.size(); ++i)
        peak = std::max (peak, std::abs (v[i]));
    return peak;
}
} // namespace

TEST_CASE ("AT-ARR-48: audio region transpose, fine tune, speed, speed fades and time stretch", "[E][arrange][AT-ARR-48]")
{
    Rig rig;
    const auto track = rig.audioTrack();
    rig.media.add ("b3:sine", sineMedia (48000, 440.0));
    model::MediaRef m;
    m.hash = "b3:sine";
    m.originalName = "sine.wav";
    m.lengthSamples = 48000;
    REQUIRE (rig.session.addMedia (m));
    const auto id = rig.session.createAudioRegion (track, 0, "b3:sine", 0, 48000).value();
    rig.sync();
    auto& s = rig.session;
    const auto region = [&] { return *s.model().region (id); };
    const auto play = [&] {
        rig.sync();
        rig.engine.stop();
        rig.engine.play (0);
        return rig.run (200000);
    };

    // As it is: 440 Hz for a second.
    auto out = play();
    CHECK (frequencyOf (out, 2000, 46000) == Catch::Approx (440.0).epsilon (0.002));

    // Speed 200%: twice as fast and an octave up, half as long.
    REQUIRE (s.setAudioRegionSpeed (id, 2.0));
    CHECK (s.undoName() == "Region Speed");
    CHECK (region().length == 24000);
    out = play();
    CHECK (frequencyOf (out, 1000, 23000) == Catch::Approx (880.0).epsilon (0.002));
    CHECK (peakOf (out, 24100, 30000) == 0.0f);
    // Trims and splits follow the speed: half a region's length is all of the second half of the file.
    const auto right = s.splitRegion (id, 480); // 12000 samples in
    REQUIRE (right);
    CHECK (s.model().region (*right)->audio.sourceOffset == 24000);
    CHECK (s.model().region (*right)->audio.speed == Catch::Approx (2.0));
    REQUIRE (s.joinRegions ({ id, *right }));
    CHECK (region().length == 24000);
    s.trimRegionLength (id, 48000); // can't run past the file's end
    CHECK (region().length == 24000);
    REQUIRE (s.undoName() == "Join Regions");
    REQUIRE (s.undo().done); // the join
    REQUIRE (s.undo().done); // the split
    REQUIRE (s.undo().done); // the speed
    CHECK (region().length == 48000);
    CHECK (region().audio.speed == Catch::Approx (1.0));
    CHECK_FALSE (s.setAudioRegionSpeed (id, 5.0));

    // Transpose +12: an octave up, the length unchanged. Fine Tune +50 cents on its own: a quarter tone up.
    REQUIRE (s.setAudioRegionPitch (id, 12, 0));
    CHECK (s.undoName() == "Transpose");
    CHECK (region().length == 48000);
    out = play();
    CHECK (frequencyOf (out, 4000, 44000) == Catch::Approx (880.0).epsilon (0.01));
    CHECK (peakOf (out, 40000, 47000) > 0.25f);
    CHECK (rig.media.find (engine::derivedMediaKey (region().audio)) != nullptr);
    REQUIRE (s.setAudioRegionPitch (id, 0, 50));
    out = play();
    CHECK (frequencyOf (out, 4000, 44000) == Catch::Approx (440.0 * std::pow (2.0, 50.0 / 1200.0)).epsilon (0.01));
    CHECK_FALSE (s.setAudioRegionPitch (id, 25, 0));
    CHECK_FALSE (s.setAudioRegionPitch (id, 0, 51));
    REQUIRE (s.setAudioRegionPitch (id, 0, 0));
    rig.sync();
    // Derived audio no region uses any more is let go.
    CHECK (rig.media.size() == 1);

    // Time stretch to twice the length: two seconds of 440 Hz. It stays within 4x.
    REQUIRE (s.stretchAudioRegion (id, 96000));
    CHECK (s.undoName() == "Time Stretch");
    CHECK (region().length == 96000);
    CHECK (region().audio.stretch == Catch::Approx (2.0));
    out = play();
    CHECK (frequencyOf (out, 4000, 92000) == Catch::Approx (440.0).epsilon (0.01));
    CHECK (peakOf (out, 88000, 95000) > 0.25f);
    CHECK (peakOf (out, 96100, 100000) == 0.0f);
    REQUIRE (s.stretchAudioRegion (id, 1000000));
    CHECK (region().audio.stretch == Catch::Approx (4.0));
    CHECK (region().length == 192000);
    // From the start edge: the end stays put.
    REQUIRE (s.undo().done);
    REQUIRE (s.undo().done);
    REQUIRE (s.moveRegion (id, barTicks));
    const auto endTick = s.model().regionEndTick (region());
    REQUIRE (s.stretchAudioRegion (id, 24000, true));
    CHECK (region().audio.stretch == Catch::Approx (0.5));
    CHECK (s.model().regionEndTick (region()) == Catch::Approx (endTick).margin (1.0));
    CHECK (region().start == barTicks + 960); // 24000 samples later
    REQUIRE (s.undo().done);
    REQUIRE (s.undo().done);
    REQUIRE (region().audio.stretch == Catch::Approx (1.0));
    REQUIRE (region().start == 0);

    // Speed Up over the first half second: no level fade, a slow start, and in time once it's over.
    model::Fade up;
    up.length = 24000;
    up.speed = true;
    REQUIRE (s.setRegionFades (id, up, std::nullopt));
    CHECK (region().audio.fadeIn.speed);
    out = play();
    CHECK (peakOf (out, 2000, 6000) > 0.25f);                            // full level from the start
    CHECK (frequencyOf (out, 0, 12000) < 440.0 * 0.6);                    // but slow
    for (std::size_t i = 30000; i < 31000; i += 97)                       // and back in time after the fade
        CHECK (out[i] == Catch::Approx (0.5 * std::sin (2.0 * std::numbers::pi * 440.0 * static_cast<double> (i) / 48000.0)).margin (0.01));
    // Slow Down over the last half second winds down to a standstill.
    model::Fade down;
    down.length = 24000;
    down.speed = true;
    REQUIRE (s.setRegionFades (id, model::Fade {}, down));
    out = play();
    CHECK (frequencyOf (out, 2000, 22000) == Catch::Approx (440.0).epsilon (0.01));
    CHECK (frequencyOf (out, 40000, 47900) < 440.0 * 0.4);

    // Saved with the project; a region left alone writes none of it.
    REQUIRE (s.setAudioRegionPitch (id, -3, 7));
    REQUIRE (s.setAudioRegionSpeed (id, 1.5));
    const auto saved = model::Region::fromValue (id, region().toValue());
    CHECK (saved.audio.transpose == -3);
    CHECK (saved.audio.fineTune == 7);
    CHECK (saved.audio.speed == Catch::Approx (1.5));
    CHECK (saved.audio.fadeOut.speed);
    model::Region plain;
    plain.isMidi = false;
    const auto v = plain.toValue();
    CHECK (v["audio"]["transpose"].isNull());
    CHECK (v["audio"]["speed"].isNull());
    CHECK (v["audio"]["fadeIn"]["speed"].isNull());
}
