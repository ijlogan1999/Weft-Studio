// Engine and transport acceptance tests with the null device (spec 06 §2, §12, §13, §16).

#include "EngineFixtures.h"

#include "weft/rt/AudioThread.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>

using namespace weft;
using namespace weft::test;

TEST_CASE ("AT-TRN-1: playback reaches a region at bar 2 at sample 96,000", "[E][transport][AT-TRN-1]")
{
    Rig rig;
    rig.addAudioRegion (rig.audioTrack(), barTicks, barSamples);
    rig.engine.play (0);
    const auto out = rig.run (2 * barSamples);
    CHECK (firstNonSilent (out) == barSamples);
    for (std::int64_t i = 0; i < barSamples; ++i)
        REQUIRE (out[static_cast<std::size_t> (barSamples + i)] == rampValue (i));
    CHECK (rig.engine.state() == TransportState::playing);
}

TEST_CASE ("AT-TRN-2: stop returns to the play start when the setting is on", "[E][transport][AT-TRN-2]")
{
    Rig rig;
    auto settings = rig.engine.settings();

    SECTION ("on")
    {
        settings.returnToStartOnStop = true;
        rig.engine.setSettings (settings);
        rig.engine.play (barTicks);
        rig.run (48000);
        CHECK (rig.engine.positionSamples() == barSamples + 48000);
        rig.engine.stop();
        rig.run (256);
        CHECK (rig.engine.state() == TransportState::stopped);
        CHECK (rig.engine.positionSamples() == barSamples);
    }
    SECTION ("off")
    {
        settings.returnToStartOnStop = false;
        rig.engine.setSettings (settings);
        rig.engine.play (barTicks);
        rig.run (48000);
        rig.engine.stop();
        rig.run (256);
        CHECK (rig.engine.positionSamples() == barSamples + 48000);
        // Play continues from where it stopped.
        rig.engine.play();
        rig.run (1000);
        CHECK (rig.engine.positionSamples() == barSamples + 49000);
    }
}

TEST_CASE ("AT-TRN-4: record with no armed tracks plays and says so", "[E][transport][AT-TRN-4]")
{
    Rig rig;
    const auto regionsBefore = rig.session.model().regions.size();
    const auto result = rig.engine.record (0);
    CHECK (result.started);
    CHECK_FALSE (result.recording);
    CHECK (result.message == "No tracks armed");
    rig.run (48000);
    CHECK (rig.engine.state() == TransportState::playing);
    CHECK_FALSE (rig.finishRecording());
    CHECK (rig.session.model().regions.size() == regionsBefore);
}

namespace
{
/** Timeline position for each output frame when cycling [cs, ce) from 0. */
std::int64_t cycledPosition (std::int64_t frame, std::int64_t cs, std::int64_t ce)
{
    return frame < ce ? frame : cs + (frame - ce) % (ce - cs);
}
} // namespace

TEST_CASE ("AT-TRN-7: cycle playback wraps from 191,999 to 96,000 with no gap", "[E][transport][AT-TRN-7]")
{
    const auto block = GENERATE (256, 441, 1024);
    Rig rig (block);
    rig.addAudioRegion (rig.audioTrack(), 0, 4 * barSamples);
    REQUIRE (rig.session.setCycle (true, barTicks, 2 * barTicks));
    rig.sync();
    rig.engine.play (0);
    const auto frames = barSamples + barSamples * 7 / 2; // up to the cycle, then 3.5 cycle lengths
    const auto out = rig.run (frames);
    for (std::int64_t f = 0; f < frames; ++f)
        REQUIRE (out[static_cast<std::size_t> (f)] == rampValue (cycledPosition (f, barSamples, 2 * barSamples)));
    CHECK (rig.engine.positionSamples() == barSamples + barSamples / 2);
}

TEST_CASE ("AT-TRN-8: a cycle shorter than one buffer wraps inside the buffer", "[E][transport][AT-TRN-8]")
{
    Rig rig (256);
    rig.addAudioRegion (rig.audioTrack(), 0, 2 * barSamples);
    REQUIRE (rig.session.setCycle (true, barTicks, barTicks + 4)); // 4 ticks = 100 samples
    rig.sync();
    rig.engine.play (barTicks);
    const auto out = rig.run (256 * 8);
    for (std::size_t f = 0; f < out.size(); ++f)
        REQUIRE (out[f] == rampValue (barSamples + static_cast<std::int64_t> (f % 100)));
}

namespace
{
void clickSettings (Rig& rig, ClickDuring during, double levelDb = 0.0, int countIn = 0)
{
    TransportSettings s;
    s.metronomeOn = true;
    s.metronomeDuring = during;
    s.metronomeLevelDb = levelDb;
    s.countInBars = countIn;
    rig.engine.setSettings (s);
}
} // namespace

TEST_CASE ("AT-TRN-9: metronome clicks on every beat with an accent on each bar", "[E][transport][metronome][AT-TRN-9]")
{
    Rig rig;
    clickSettings (rig, ClickDuring::both);

    SECTION ("4/4")
    {
        rig.engine.play (0);
        const auto out = rig.run (2 * barSamples + 100);
        const auto clicks = onsets (out);
        CHECK (clicks == std::vector<std::int64_t> { 0, 24000, 48000, 72000, 96000, 120000, 144000, 168000, 192000 });
        const auto accent = zeroCrossings (out, 0, 1000), normal = zeroCrossings (out, 24000, 1000);
        CHECK (accent > normal); // bar starts are higher in pitch
        CHECK (zeroCrossings (out, 96000, 1000) == accent);
        CHECK (peak (out, 0, 100) == Catch::Approx (peak (out, 24000, 100)));
    }
    SECTION ("6/8")
    {
        REQUIRE (rig.session.setSignatureAt (0, 6, 8));
        rig.sync();
        rig.engine.play (0);
        const auto out = rig.run (2 * 72000 + 2000); // an eighth note is 12,000 samples; a bar is 72,000
        const auto clicks = onsets (out);
        REQUIRE (clicks.size() == 13);
        for (std::size_t i = 0; i < clicks.size(); ++i)
        {
            CHECK (clicks[i] == static_cast<std::int64_t> (i) * 12000);
            const bool accented = zeroCrossings (out, clicks[i], 1000) > zeroCrossings (out, 12000, 1000);
            CHECK (accented == (i % 6 == 0));
        }
    }
}

TEST_CASE ("AT-TRN-10: metronome during record/play and level", "[E][transport][metronome][AT-TRN-10]")
{
    Rig rig;
    rig.engine.setArmedTracks ({ rig.audioTrack() });

    SECTION ("record only")
    {
        clickSettings (rig, ClickDuring::record);
        rig.engine.play (0);
        CHECK (firstNonSilent (rig.run (48000)) == -1);
        rig.engine.stop();
        REQUIRE (rig.engine.record (0).recording);
        CHECK (firstNonSilent (rig.run (48000)) == 0);
    }
    SECTION ("play only")
    {
        clickSettings (rig, ClickDuring::play);
        rig.engine.play (0);
        CHECK (firstNonSilent (rig.run (48000)) == 0);
        rig.engine.stop();
        REQUIRE (rig.engine.record (0).recording);
        CHECK (firstNonSilent (rig.run (48000)) == -1);
    }
    SECTION ("level")
    {
        clickSettings (rig, ClickDuring::both, 0.0);
        rig.engine.play (0);
        const auto loud = peak (rig.run (24000), 0, 24000);
        rig.engine.stop();
        clickSettings (rig, ClickDuring::both, -12.0);
        rig.engine.play (0);
        const auto quiet = peak (rig.run (24000), 0, 24000);
        CHECK (loud == Catch::Approx (Metronome::peakAtZeroDb));
        CHECK (20.0 * std::log10 (quiet / loud) == Catch::Approx (-12.0).margin (0.01));
    }
}

namespace
{
float inputValue (int channel, std::int64_t frame) { return static_cast<float> ((frame % 5000) + 1) / 8192.0f * (channel == 0 ? 1.0f : -1.0f); }
} // namespace

TEST_CASE ("AT-TRN-11: one-bar count-in before recording at bar 3", "[E][transport][AT-TRN-11]")
{
    Rig rig;
    rig.device.setInputGenerator ([] (int ch, std::int64_t start, float* dest, int n) {
        for (int i = 0; i < n; ++i)
            dest[i] = inputValue (ch, start + i);
    });
    REQUIRE (rig.session.setTrackMonitor (rig.audioTrack(), "off")); // hear only the clicks
    rig.sync();
    rig.engine.setArmedTracks ({ rig.audioTrack() });
    clickSettings (rig, ClickDuring::record, 0.0, 1);

    REQUIRE (rig.engine.record (2 * barTicks).recording);
    const auto out = rig.run (2 * barSamples);
    // Clicks for the bar before bar 3 (device frames 0..96,000 cover timeline bar 2), then the record pass.
    const auto clicks = onsets (out);
    REQUIRE (clicks.size() >= 5);
    CHECK (std::vector<std::int64_t> (clicks.begin(), clicks.begin() + 5) == std::vector<std::int64_t> { 0, 24000, 48000, 72000, 96000 });
    CHECK (rig.engine.state() == TransportState::recording);
    CHECK (rig.engine.positionSamples() == 3 * barSamples);

    const auto outcome = rig.finishRecording();
    REQUIRE (outcome);
    CHECK (outcome->committed);
    CHECK (rig.engine.positionSamples() == 2 * barSamples); // return to the record start
    const auto regions = rig.regionsOn (rig.audioTrack());
    REQUIRE (regions.size() == 1);
    CHECK (regions[0]->start == 2 * barTicks);
    CHECK (regions[0]->audio.sourceOffset == 0);
    const auto file = rig.regionFile (regions[0]->id);
    REQUIRE (file.numChannels() == 2);
    REQUIRE (file.numFrames() >= barSamples);
    // The file holds the input from bar 3 on: nothing from the count-in, and no clicks.
    for (int ch = 0; ch < 2; ++ch)
        for (std::int64_t i = 0; i < barSamples; ++i)
            REQUIRE (file.channels[static_cast<std::size_t> (ch)][static_cast<std::size_t> (i)] == inputValue (ch, barSamples + i));
}

TEST_CASE ("AT-TRN-12: a two-bar count-in from bar 1 pre-rolls into negative time", "[E][transport][AT-TRN-12]")
{
    Rig rig;
    rig.engine.setArmedTracks ({ rig.audioTrack() });
    clickSettings (rig, ClickDuring::record, 0.0, 2);
    rig.addAudioRegion (rig.audioTrack(), 0, barSamples);

    REQUIRE (rig.engine.record (0).recording);
    CHECK (rig.run (1)[0] == Catch::Approx (Metronome::peakAtZeroDb)); // accented click at -2 bars
    CHECK (rig.engine.state() == TransportState::countIn);
    CHECK (rig.engine.positionSamples() == -2 * barSamples + 1);

    const auto out = rig.run (3 * barSamples, 0, 1);
    const auto clicks = onsets (out);
    REQUIRE (clicks.size() >= 8);
    CHECK (clicks[0] == 0); // the rest of the first click
    for (std::size_t i = 1; i < 8; ++i)
        CHECK (clicks[i] == static_cast<std::int64_t> (i) * 24000 - 1);
    // Regions don't play during the count-in; recording and playback start at tick 0.
    const auto outcome = rig.finishRecording();
    REQUIRE (outcome);
    CHECK (outcome->committed);
    const auto regions = rig.regionsOn (rig.audioTrack());
    REQUIRE (regions.size() == 2); // the existing region and the recording on top
    const auto* recorded = regions[0]->z > regions[1]->z ? regions[0] : regions[1];
    CHECK (recorded->start == 0);
    CHECK (rig.regionFile (recorded->id).numFrames() == barSamples + 1);
}

TEST_CASE ("AT-TRN-17: CPU meter and dropout counter", "[E][transport][AT-TRN-17]")
{
    Rig rig (1024);
    auto burn = std::make_shared<BurnInstrument>();
    const auto track = rig.session.createTrack (TrackKind::instrument, "Burn").value();
    rig.rack.set (track, burn);
    rig.sync();

    rig.device.setDeadlineSimulation (true);
    REQUIRE (rig.device.render (1024 * 40));
    CHECK (rig.engine.cpuLoad() >= 0.75);
    CHECK (rig.engine.cpuLoad() <= 0.90);
    const auto dropouts = rig.engine.dropouts();

    burn->fraction = 1.5; // force a missed deadline
    REQUIRE (rig.device.render (1024));
    CHECK (rig.engine.dropouts() == dropouts + 1);
    CHECK (rig.device.stats().deadlineMisses >= 1);
}

TEST_CASE ("AT-TMP-2: clicks, MIDI and audio follow a tempo change", "[E][tempo][AT-TMP-2]")
{
    Rig rig;
    REQUIRE (rig.session.addTempoPoint (3840, 60.0));
    clickSettings (rig, ClickDuring::both);

    auto probe = std::make_shared<ProbeInstrument>();
    const auto keys = rig.session.createTrack (TrackKind::instrument, "Keys").value();
    rig.rack.set (keys, probe);
    const auto region = rig.session.createMidiRegion (keys, 0, 2 * barTicks).value();
    model::Note note;
    note.pos = 4800;
    note.pitch = 64;
    note.length = 480;
    REQUIRE (rig.session.addNote (region, note));
    rig.addAudioRegion (rig.audioTrack(), 4800, 48000);

    // Clicks alone first (the region and the instrument would mask the onsets).
    {
        Rig clickRig;
        REQUIRE (clickRig.session.addTempoPoint (3840, 60.0));
        clickSettings (clickRig, ClickDuring::both);
        clickRig.sync();
        clickRig.engine.play (0);
        const auto out = clickRig.run (192000 + 100);
        CHECK (onsets (out) == std::vector<std::int64_t> { 0, 24000, 48000, 72000, 96000, 144000, 192000 });
    }

    TransportSettings quiet;
    quiet.metronomeOn = false;
    rig.engine.setSettings (quiet);
    rig.sync();
    rig.engine.play (0);
    const auto out = rig.run (200000);
    REQUIRE (! probe->seen.empty());
    CHECK (probe->seen.front().frame == 144000);
    CHECK (probe->seen.front().event.isNoteOn());
    CHECK (probe->seen.front().event.data1 == 64);
    // 480 ticks at 60 BPM is 24,000 samples.
    REQUIRE (probe->seen.size() >= 2);
    CHECK (probe->seen[1].frame == 168000);
    CHECK (probe->seen[1].event.isNoteOff());
    // The audio region starts at 144,000 and plays at its own speed (D-TMP-1).
    const float noteLevel = 0.1f * 100.0f / 127.0f;
    CHECK (out[143999] == 0.0f);
    for (std::int64_t i = 0; i < 48000; ++i)
    {
        const float expected = rampValue (i) + (i < 24000 ? noteLevel : 0.0f);
        REQUIRE (out[static_cast<std::size_t> (144000 + i)] == Catch::Approx (expected).margin (1e-6));
    }
}

TEST_CASE ("AT-UI-5: changing buffer size and sample rate keeps output correct", "[E][devices][AT-UI-5]")
{
    Rig rig (64);
    rig.addAudioRegion (rig.audioTrack(), barTicks, barSamples);

    auto check48k = [&] {
        rig.engine.play (0);
        const auto out = rig.run (2 * barSamples);
        CHECK (firstNonSilent (out) == barSamples);
        CHECK (out[static_cast<std::size_t> (barSamples + 777)] == rampValue (777));
        rig.engine.stop();
    };
    check48k();
    rig.reopen (1024);
    check48k();
    rig.reopen (64);
    check48k();

    // The device can't run at the project rate: resampled playback (D-W0-1). Bar 2 is at 88,200.
    rig.reopen (512, 44100.0);
    rig.engine.play (0);
    const auto out = rig.run (2 * 88200);
    CHECK (firstNonSilent (out) == 88200);
    // 44.1 kHz frame k of the region is 48 kHz media frame k x 48/44.1, between two ramp steps.
    const auto k = 4410; // = media frame 4800 exactly
    CHECK (out[static_cast<std::size_t> (88200 + k)] == Catch::Approx (rampValue (4800)).margin (1e-6));
    CHECK (out[static_cast<std::size_t> (88200 + 1000)] == Catch::Approx ((1000.0 * 48000.0 / 44100.0 + 1.0) / 16384.0).margin (1e-5));

    rig.engine.stop();
    rig.run (256);
    rig.engine.setArmedTracks ({ rig.audioTrack() });
    const auto refused = rig.engine.record (0);
    CHECK_FALSE (refused.started);
    CHECK_FALSE (refused.message.empty());
    rig.engine.setArmedTracks ({});

    rig.reopen (256);
    check48k();
}

TEST_CASE ("AT-RT-3: buffer sizes from 32 to 2,048 and odd sizes give identical output", "[E][transport][AT-RT-3]")
{
    auto render = [] (int block) {
        Rig rig (std::max (block, 2048));
        rig.addAudioRegion (rig.audioTrack(), barTicks / 2, barSamples);
        auto probe = std::make_shared<ProbeInstrument>();
        const auto keys = rig.session.createTrack (TrackKind::instrument, "Keys").value();
        rig.rack.set (keys, probe);
        const auto region = rig.session.createMidiRegion (keys, 0, 2 * barTicks).value();
        for (int i = 0; i < 12; ++i)
        {
            model::Note n;
            n.pos = i * 597;
            n.length = 401;
            n.pitch = 60 + (i % 3);
            REQUIRE (rig.session.addNote (region, n));
        }
        REQUIRE (rig.session.setCycle (true, 1000, 5000));
        clickSettings (rig, ClickDuring::both);
        rig.sync();
        rig.engine.play (0);
        return rig.run (barSamples * 3, block);
    };

    const auto reference = render (2048);
    REQUIRE (firstNonSilent (reference) == 0);
    for (const int block : { 32, 64, 128, 256, 441, 512, 1000, 1024, 2047 })
    {
        INFO ("block size " << block);
        CHECK (render (block) == reference);
    }
}

TEST_CASE ("AT-RT-1: no allocation or blocking calls on the audio thread", "[E][rt][AT-RT-1]")
{
    Rig rig (256);
    const auto track = rig.audioTrack();
    const auto region = rig.addAudioRegion (track, 0, 2 * barSamples);
    auto probe = std::make_shared<ProbeInstrument>();
    const auto keys = rig.session.createTrack (TrackKind::instrument, "Keys").value();
    rig.rack.set (keys, probe);
    const auto midi = rig.session.createMidiRegion (keys, 0, 4 * barTicks).value();
    for (int i = 0; i < 64; ++i)
    {
        model::Note n;
        n.pos = i * 240;
        n.pitch = 40 + i % 40;
        REQUIRE (rig.session.addNote (midi, n));
    }
    rig.sync();
    rig.engine.setArmedTracks ({ track });
    clickSettings (rig, ClickDuring::both, -6.0, 1);
    rig.device.setCaptureCapacity (0);

    // A second collaborator whose edits arrive as remote changes.
    auto remoteDoc = doc::Document::load (rig.session.document().save());
    REQUIRE (remoteDoc);
    Session remote (std::move (*remoteDoc), "userB", [] { return std::int64_t { 1'700'000'000'000 }; }, 2);

    weft::rt::resetViolations();
    rig.engine.play (0);
    REQUIRE (rig.device.render (48000));
    for (int i = 0; i < 20; ++i)
    {
        // Edit regions during playback.
        REQUIRE (rig.session.moveRegion (region, (i % 4) * 960));
        REQUIRE (rig.session.setRegionGain (region, -1.0 * i));
        rig.sync();
        REQUIRE (rig.device.render (2000, 256));
        // Remote change.
        REQUIRE (remote.setTrackVolume (track, -0.5 * i));
        REQUIRE (rig.session.applyChanges (remote.document().allChanges()) == doc::Status::ok);
        rig.sync();
        REQUIRE (rig.device.render (3000, 128));
    }
    // Instruments added and removed during playback.
    rig.rack.set (keys, nullptr);
    rig.sync();
    REQUIRE (rig.device.render (5000));
    rig.rack.set (keys, probe);
    rig.sync();
    REQUIRE (rig.device.render (5000));
    // Cycle, record with count-in, locate, stop.
    REQUIRE (rig.session.setCycle (true, 0, 3840));
    rig.sync();
    REQUIRE (rig.device.render (200000));
    rig.engine.stop();
    rig.engine.setArmedTracks ({ track, keys });
    REQUIRE (rig.engine.record (3840).recording);
    for (int i = 0; i < 50; ++i)
    {
        // Live MIDI while recording audio and MIDI through cycle wraps.
        const auto at = rig.engine.deviceFrames() + 100;
        rig.engine.sendMidi (0x90, static_cast<std::uint8_t> (60 + i % 12), 100, at);
        rig.engine.sendMidi (0xB0, 64, i % 2 == 0 ? 127 : 0, at + 50);
        rig.engine.sendMidi (0x80, static_cast<std::uint8_t> (60 + i % 12), 0, at + 2000);
        REQUIRE (rig.device.render (5000));
    }
    rig.engine.stopRecording();
    REQUIRE (rig.device.render (5000));
    REQUIRE (rig.engine.record (0).recording);
    REQUIRE (rig.device.render (5000));
    rig.engine.locate (7680);
    REQUIRE (rig.device.render (10000));
    rig.engine.stop();
    REQUIRE (rig.device.render (1000));

    const auto v = weft::rt::violations();
    CHECK (v.allocations == 0);
    CHECK (v.blockingCalls == 0);
    rig.engine.collectGarbage();
    CHECK (rig.engine.pendingRetired() <= 1);
    while (rig.engine.recordingPending())
        REQUIRE (rig.engine.finishRecording (rig.session));
}

TEST_CASE ("Engine snapshot plays the topmost of overlapping regions, with gain and fades", "[E][graph]")
{
    Rig rig;
    const auto track = rig.audioTrack();
    const auto lower = rig.addAudioRegion (track, 0, barSamples, "b3:a");
    const auto upper = rig.addAudioRegion (track, barTicks / 2, barSamples / 4, "b3:b");
    rig.engine.play (0);
    auto out = rig.run (barSamples);
    CHECK (out[1000] == rampValue (1000));
    CHECK (out[48000] == rampValue (0)); // upper region covers 48,000-72,000
    CHECK (out[72000] == rampValue (72000));

    // Muting the upper region reveals the lower one.
    REQUIRE (rig.session.setRegionMuted (upper, true));
    REQUIRE (rig.session.setRegionGain (lower, -6.0));
    REQUIRE (rig.session.setRegionFades (lower, model::Fade { 1000, "linear" }, model::Fade { 0, "linear" }));
    rig.sync();
    rig.engine.play (0);
    out = rig.run (barSamples);
    const float g = std::pow (10.0f, -6.0f / 20.0f);
    CHECK (out[0] == 0.0f);
    CHECK (out[500] == Catch::Approx (rampValue (500) * g * 0.5f));
    CHECK (out[48000] == Catch::Approx (rampValue (48000) * g));

    // Track mute silences the track.
    REQUIRE (rig.session.setTrackMute (track, true));
    rig.sync();
    rig.engine.play (0);
    CHECK (firstNonSilent (rig.run (barSamples)) == -1);
}
