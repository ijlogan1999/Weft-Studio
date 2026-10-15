// Region edits as they play (spec 06 AT-ARR-8, 9, 11, 13, 14, 15, 16, 17).

#include "EngineFixtures.h"

#include "weft/engine/arrange/OfflineRender.h"

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

    REQUIRE (rig.session.setRegionGain (r, 40.0));
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
