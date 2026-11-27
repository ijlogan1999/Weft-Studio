// Varispeed, Speed and Pitch (D-ARCH-55): spec 06 AT-VS-1, 2, 4, 5, 6, 7, 10.

#include "EngineFixtures.h"

#include "weft/engine/transport/Varispeed.h"
#include "weft/rt/AudioThread.h"

#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <numbers>

using namespace weft;
using namespace weft::test;

namespace
{
media::AudioData sine (std::int64_t frames, double hz, float amplitude = 0.5f)
{
    media::AudioData a;
    a.sampleRate = 48000;
    a.channels.assign (1, std::vector<float> (static_cast<std::size_t> (frames)));
    for (std::int64_t i = 0; i < frames; ++i)
        a.channels[0][static_cast<std::size_t> (i)] = amplitude * static_cast<float> (std::sin (2.0 * std::numbers::pi * hz * static_cast<double> (i) / 48000.0));
    return a;
}

/** A sine region of `frames` at bar 1 on a stereo audio track. */
Id sineRegion (Rig& rig, double hz, std::int64_t frames)
{
    const auto track = rig.audioTrack();
    rig.media.add ("b3:vs-sine", sine (frames, hz));
    model::MediaRef m;
    m.hash = "b3:vs-sine";
    m.originalName = "sine.wav";
    m.lengthSamples = frames;
    REQUIRE (rig.session.addMedia (m));
    const auto id = rig.session.createAudioRegion (track, 0, "b3:vs-sine", 0, frames);
    REQUIRE (id);
    rig.sync();
    return *id;
}

void setSpeed (Rig& rig, double rate)
{
    auto s = rig.engine.settings();
    s.varispeed = rate;
    rig.engine.setSettings (s);
}

/** The frequency over [from, to), from its upward zero crossings. */
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

std::int64_t lastAudible (const std::vector<float>& v, float threshold = 1.0e-3f)
{
    for (auto i = static_cast<std::int64_t> (v.size()) - 1; i >= 0; --i)
        if (std::abs (v[static_cast<std::size_t> (i)]) > threshold)
            return i;
    return -1;
}
} // namespace

TEST_CASE ("AT-VS-1: Varispeed plays a sine faster and higher, or slower and lower", "[E][varispeed][AT-VS-1]")
{
    const auto [rate, hz, length] = GENERATE (std::tuple { 1.5, 1500.0, 32000.0 }, std::tuple { 0.5, 500.0, 96000.0 }, std::tuple { 2.0, 2000.0, 24000.0 });
    Rig rig;
    sineRegion (rig, 1000.0, 48000);
    setSpeed (rig, rate);
    rig.run (512); // the rate ramps in while stopped
    rig.engine.play (0);
    const auto out = rig.run (110000);
    CAPTURE (rate);
    CHECK (frequencyOf (out, 2000, static_cast<std::size_t> (length) - 2000) == Catch::Approx (hz).epsilon (0.001));
    // As long as the region takes at that speed, with the resampler's few frames of delay.
    CHECK (static_cast<double> (lastAudible (out)) == Catch::Approx (length).margin (40));
    // The playhead moves through the timeline at the rate.
    CHECK (static_cast<double> (rig.engine.positionSamples()) == Catch::Approx (110000.0 * rate).margin (64));
}

TEST_CASE ("AT-VS-2: with Varispeed off the output is bit-identical to an engine that never used it", "[E][varispeed][AT-VS-2]")
{
    Rig plain, used;
    plain.addAudioRegion (plain.audioTrack(), 0, 30000);
    used.addAudioRegion (used.audioTrack(), 0, 30000);
    setSpeed (used, 1.25);
    used.run (2048);
    setSpeed (used, 1.0);
    used.run (4096); // back at normal speed, stopped and silent: the plain path again
    plain.run (6144);
    plain.engine.play (0);
    used.engine.play (0);
    const auto a = plain.run (40000), b = used.run (40000);
    CHECK (a == b);
}

TEST_CASE ("AT-VS-4: the click keeps its pitch and comes at the tempo times the rate", "[E][varispeed][metronome][AT-VS-4]")
{
    const auto clicks = [] (double rate) {
        Rig rig;
        auto s = rig.engine.settings();
        s.metronomeOn = true;
        s.metronomeDuring = ClickDuring::both;
        s.varispeed = rate;
        rig.engine.setSettings (s);
        rig.run (512);
        rig.engine.play (0);
        return rig.run (100000);
    };
    const auto normal = clicks (1.0), fast = clicks (1.25);
    const auto at = onsets (normal), atFast = onsets (fast);
    REQUIRE (at.size() >= 4);
    REQUIRE (atFast.size() >= 5);
    // Beats every 24,000 samples at 120 bpm; at +25% every 19,200 (150 bpm), within a sample.
    for (std::size_t i = 1; i < atFast.size(); ++i)
        CHECK (static_cast<double> (atFast[i] - atFast[i - 1]) == Catch::Approx (19200.0).margin (1.0));
    // The same click sound: the beat after the accent matches sample for sample.
    float diff = 0.0f;
    for (std::int64_t i = 0; i < 2000; ++i)
        diff = std::max (diff, std::abs (normal[static_cast<std::size_t> (at[1] + i)] - fast[static_cast<std::size_t> (atFast[1] + i)]));
    CHECK (diff < 1.0e-6f);
}

TEST_CASE ("AT-VS-5: Varispeed units convert and clamp", "[U][varispeed][AT-VS-5]")
{
    using namespace engine::varispeed;
    CHECK (toUnit (1.0, Unit::percent, 120) == Catch::Approx (0.0));
    CHECK (toUnit (1.0, Unit::semitones, 120) == Catch::Approx (0.0));
    CHECK (toUnit (1.0, Unit::tuning, 120) == Catch::Approx (440.0));
    CHECK (toUnit (1.0, Unit::tempo, 120) == Catch::Approx (120.0));
    CHECK (toUnit (2.0, Unit::percent, 120) == Catch::Approx (100.0));
    CHECK (toUnit (2.0, Unit::semitones, 120) == Catch::Approx (12.0));
    CHECK (toUnit (2.0, Unit::tuning, 120) == Catch::Approx (880.0));
    CHECK (toUnit (2.0, Unit::tempo, 120) == Catch::Approx (240.0));
    CHECK (toUnit (0.5, Unit::percent, 120) == Catch::Approx (-50.0));
    CHECK (toUnit (0.5, Unit::semitones, 120) == Catch::Approx (-12.0));
    CHECK (toUnit (0.5, Unit::tuning, 120) == Catch::Approx (220.0));
    CHECK (fromUnit (-15.0, Unit::percent, 120) == Catch::Approx (0.85));
    CHECK (fromUnit (138.0, Unit::tempo, 120) == Catch::Approx (1.15));
    CHECK (fromUnit (7.0, Unit::semitones, 120) == Catch::Approx (std::exp2 (7.0 / 12.0)));
    CHECK (fromUnit (466.16, Unit::tuning, 120) == Catch::Approx (466.16 / 440.0));
    // Outside the range: clamped.
    CHECK (fromUnit (150.0, Unit::percent, 120) == Catch::Approx (2.0));
    CHECK (fromUnit (-80.0, Unit::percent, 120) == Catch::Approx (0.5));
    CHECK (fromUnit (24.0, Unit::semitones, 120) == Catch::Approx (2.0));
    CHECK (fromUnit (100.0, Unit::tuning, 120) == Catch::Approx (0.5));
    CHECK (clampRate (std::nan ("")) == Catch::Approx (1.0));
    CHECK (text (1.15, Unit::percent, 120) == "+15.0%");
    CHECK (text (0.85, Unit::percent, 120) == "-15.0%");
    CHECK (text (1.0, Unit::percent, 120) == "+0.0%");
    CHECK (text (1.15, Unit::tempo, 120) == "138.00 bpm");
    CHECK (text (2.0, Unit::semitones, 120) == "+12.00 st");
    CHECK (text (0.5, Unit::tuning, 120) == "220.0 Hz");
}

TEST_CASE ("AT-VS-6: changing the rate while playing makes no click and allocates nothing", "[E][rt][varispeed][AT-VS-6]")
{
    Rig rig (256);
    sineRegion (rig, 440.0, 200000);
    rig.run (1024);
    weft::rt::resetViolations();
    rig.engine.play (0);
    std::vector<float> out;
    const auto take = [&] (std::int64_t frames, int block) {
        rig.device.setCaptureCapacity (frames);
        REQUIRE (rig.device.render (frames, block));
        const auto o = rig.device.capturedOutput (0);
        out.insert (out.end(), o.begin(), o.end());
    };
    take (20000, 256);
    setSpeed (rig, 1.5); // on, during playback
    take (20000, 256);
    setSpeed (rig, 0.7);
    take (20000, 128);
    setSpeed (rig, 2.0);
    take (20000, 77);
    setSpeed (rig, 1.0);
    take (20000, 256);
    const auto v = weft::rt::violations();
    CHECK (v.allocations == 0);
    CHECK (v.blockingCalls == 0);
    // A 0.5 sine at 440 Hz moves at most 0.029 a sample, at twice the speed 0.058: nothing jumps more.
    float step = 0.0f;
    for (std::size_t i = 1; i < out.size(); ++i)
        step = std::max (step, std::abs (out[i] - out[i - 1]));
    CHECK (step < 0.062f);
    CHECK (frequencyOf (out, 62000, 78000) == Catch::Approx (880.0).epsilon (0.002));
}

namespace
{
Id inputTrack (Rig& rig, const std::string& monitor)
{
    const auto id = rig.session.createTrack (model::TrackKind::audio, "In", 1).value();
    model::InputAssignment in;
    in.kind = model::InputAssignment::Kind::audio;
    in.firstChannel = 0;
    REQUIRE (rig.session.setTrackInput (id, in));
    REQUIRE (rig.session.setTrackMonitor (id, monitor));
    rig.sync();
    return id;
}

void sineInput (Rig& rig, double hz)
{
    rig.device.setInputGenerator ([hz] (int, std::int64_t start, float* dest, int n) {
        for (int i = 0; i < n; ++i)
            dest[i] = 0.5f * static_cast<float> (std::sin (2.0 * std::numbers::pi * hz * static_cast<double> (start + i) / 48000.0));
    });
}
} // namespace

TEST_CASE ("AT-VS-7: a take recorded at -50% plays an octave higher and half as long at normal speed", "[E][varispeed][record][AT-VS-7]")
{
    Rig rig;
    auto s = rig.engine.settings();
    s.countInBars = 0;
    s.varispeed = 0.5;
    rig.engine.setSettings (s);
    sineInput (rig, 1000.0);
    const auto track = inputTrack (rig, "off");
    rig.engine.setArmedTracks ({ track });
    rig.run (512);
    REQUIRE (rig.engine.record (0).recording);
    rig.run (96000); // two seconds of input
    const auto outcome = rig.finishRecording();
    REQUIRE (outcome);
    const auto regions = rig.regionsOn (track);
    REQUIRE (regions.size() == 1);
    // A second of the timeline: two seconds at half speed.
    CHECK (static_cast<double> (regions[0]->length) == Catch::Approx (48000.0).margin (300));
    const auto file = rig.regionFile (regions[0]->id);
    CHECK (frequencyOf (file.channels[0], 2000, 40000) == Catch::Approx (2000.0).epsilon (0.002));
}

TEST_CASE ("AT-VS-10: a monitored input comes out at its own pitch at +50%", "[E][varispeed][monitor][AT-VS-10]")
{
    Rig rig;
    sineInput (rig, 1000.0);
    inputTrack (rig, "on");
    setSpeed (rig, 1.5);
    rig.run (1024);
    rig.engine.play (0);
    const auto out = rig.run (48000);
    CHECK (frequencyOf (out, 4000, 44000) == Catch::Approx (1000.0).epsilon (0.002));
}
