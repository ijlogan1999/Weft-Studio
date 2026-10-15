// Track acceptance tests that need the engine (spec 06 AT-TRK-1, 2, 3, 8, 13).

#include "EngineFixtures.h"

#include "weft/commands/LocalView.h"

#include <catch2/catch_approx.hpp>

#include <cmath>
#include <numbers>

using namespace weft;
using namespace weft::test;

namespace
{
/** Media whose every frame is `value` (one channel per entry). */
void addConstantMedia (Rig& rig, const std::string& hash, std::vector<float> values, std::int64_t frames)
{
    media::AudioData a;
    for (const auto v : values)
        a.channels.emplace_back (static_cast<std::size_t> (frames), v);
    rig.media.add (hash, a);
    model::MediaRef m;
    m.hash = hash;
    m.originalName = hash + ".wav";
    m.channels = static_cast<int> (values.size());
    m.lengthSamples = frames;
    REQUIRE (rig.session.addMedia (m));
}

Id trackWith (Rig& rig, const std::string& name, const std::string& hash, int channels = 2)
{
    const auto t = rig.session.createTrack (TrackKind::audio, name, channels).value();
    REQUIRE (rig.session.createAudioRegion (t, 0, hash, 0, barSamples));
    rig.sync();
    return t;
}

float level (Rig& rig, int channel = 0)
{
    rig.engine.play (0);
    return rig.run (2000, 0, channel)[1000];
}
} // namespace

TEST_CASE ("AT-TRK-1: mono tracks pan at -3 dB to both sides; stereo tracks keep their sides", "[E][tracks][AT-TRK-1]")
{
    Rig rig;
    const auto mono = rig.session.createTrack (TrackKind::audio, "Mono", 1).value();
    rig.addAudioRegion (mono, 0, barSamples);
    rig.engine.play (0);
    rig.device.setCaptureCapacity (2000);
    REQUIRE (rig.device.render (2000));
    const auto left = rig.device.capturedOutput (0), right = rig.device.capturedOutput (1);
    const float g = std::cos (std::numbers::pi_v<float> / 4.0f); // -3 dB
    CHECK (left[1000] == Catch::Approx (rampValue (1000) * g));
    CHECK (right[1000] == Catch::Approx (rampValue (1000) * g));
    CHECK (20.0 * std::log10 (g) == Catch::Approx (-3.01).margin (0.01));

    Rig stereoRig;
    addConstantMedia (stereoRig, "b3:lr", { 0.25f, 0.0f }, barSamples);
    trackWith (stereoRig, "Stereo", "b3:lr");
    CHECK (level (stereoRig, 0) == 0.25f);
    CHECK (level (stereoRig, 1) == 0.0f);
}

TEST_CASE ("AT-TRK-2: Weft Keys sounds during a note and is silent 2 s after its release", "[E][tracks][keys][AT-TRK-2]")
{
    Rig rig;
    const auto keys = rig.session.createTrack (TrackKind::instrument, "Keys").value();
    const auto region = rig.session.createMidiRegion (keys, 0, 4 * barTicks).value();
    model::Note n;
    n.pos = 0;
    n.length = 960; // one beat: 24,000 samples
    n.pitch = 60;
    n.velocity = 100;
    REQUIRE (rig.session.addNote (region, n));
    rig.sync();
    REQUIRE (rig.rack.find (keys) != nullptr); // the default instrument is Weft Keys

    rig.engine.play (0);
    const auto out = rig.run (24000 + 96000 + 4800);
    CHECK (peak (out, 0, 24000) > 0.05f);
    const float minus90 = std::pow (10.0f, -90.0f / 20.0f);
    CHECK (peak (out, 24000 + 96000, 4800) < minus90);
}

TEST_CASE ("AT-TRK-3: a bus sums its sends and holds no regions", "[E][tracks][bus][AT-TRK-3]")
{
    Rig rig;
    addConstantMedia (rig, "b3:a", { 0.125f }, barSamples);
    addConstantMedia (rig, "b3:b", { 0.25f }, barSamples);
    const auto bus = rig.session.createTrack (TrackKind::bus, "Bus 1").value();
    const auto a = trackWith (rig, "A", "b3:a");
    const auto b = trackWith (rig, "B", "b3:b");
    for (const auto& t : { a, b })
    {
        REQUIRE (rig.session.addSend (t, bus, 0.0, true)); // pre-fader
        REQUIRE (rig.session.setTrackVolume (t, minusInfinityDb)); // only the bus is heard
    }
    rig.sync();
    CHECK (level (rig) == 0.375f);
    CHECK_FALSE (rig.session.createAudioRegion (bus, 0, "b3:a", 0, barSamples));
    CHECK_FALSE (rig.session.createMidiRegion (bus, 0, barTicks));
}

TEST_CASE ("AT-TRK-8: mute and solo, with solo keeping the soloed track's bus", "[E][tracks][solo][AT-TRK-8]")
{
    Rig rig;
    constexpr float A = 1.0f / 64, B = 1.0f / 16, C = 1.0f / 4;
    addConstantMedia (rig, "b3:a", { A }, barSamples);
    addConstantMedia (rig, "b3:b", { B }, barSamples);
    addConstantMedia (rig, "b3:c", { C }, barSamples);
    const auto bus = rig.session.createTrack (TrackKind::bus, "Bus 1").value();
    const auto a = trackWith (rig, "A", "b3:a");
    const auto b = trackWith (rig, "B", "b3:b");
    const auto c = trackWith (rig, "C", "b3:c");
    REQUIRE (rig.session.addSend (a, bus, 0.0));
    rig.sync();
    CHECK (level (rig) == 2 * A + B + C);

    REQUIRE (rig.session.setTrackMute (a, true));
    rig.sync();
    CHECK (level (rig) == B + C); // muted: the send goes quiet too
    REQUIRE (rig.session.setTrackMute (a, false));
    rig.sync();

    commands::LocalView view;
    view.toggleSolo (a);
    rig.engine.setLocalView (view);
    CHECK (level (rig) == 2 * A); // A and the bus it feeds
    view.toggleSolo (b);
    rig.engine.setLocalView (view);
    CHECK (level (rig) == 2 * A + B);
    view.soloOnly (c); // Ctrl+click
    rig.engine.setLocalView (view);
    CHECK (view.soloed == std::set<Id> { c });
    CHECK (level (rig) == C);
}

TEST_CASE ("AT-TRK-13: input channel, output to a bus, and the MIDI channel filter", "[E][tracks][routing][AT-TRK-13]")
{
    SECTION ("input 2 records input 2")
    {
        Rig rig;
        rig.device.setInputGenerator ([] (int ch, std::int64_t, float* dest, int n) { std::fill_n (dest, n, ch == 0 ? 0.25f : 0.5f); });
        TransportSettings s;
        s.metronomeOn = false;
        s.countInBars = 0;
        rig.engine.setSettings (s);
        const auto t = rig.session.createTrack (TrackKind::audio, "In 2", 1).value();
        model::InputAssignment in;
        in.kind = model::InputAssignment::Kind::audio;
        in.firstChannel = 1;
        REQUIRE (rig.session.setTrackInput (t, in));
        rig.sync();
        rig.engine.setArmedTracks ({ t });
        REQUIRE (rig.engine.record (0).recording);
        rig.run (barSamples / 2);
        REQUIRE (rig.finishRecording());
        REQUIRE (rig.regionsOn (t).size() == 1);
        CHECK (rig.regionFile (rig.regionsOn (t)[0]->id).channels[0][100] == 0.5f);
    }
    SECTION ("output to Bus 1 instead of the master")
    {
        Rig rig;
        addConstantMedia (rig, "b3:a", { 0.25f }, barSamples);
        const auto bus = rig.session.createTrack (TrackKind::bus, "Bus 1").value();
        const auto t = trackWith (rig, "A", "b3:a");
        REQUIRE (rig.session.setTrackOutput (t, bus));
        REQUIRE (rig.session.setTrackVolume (bus, minusInfinityDb));
        rig.sync();
        CHECK (level (rig) == 0.0f); // nothing reaches the master directly
        REQUIRE (rig.session.setTrackVolume (bus, 0.0));
        rig.sync();
        CHECK (level (rig) == 0.25f);
    }
    SECTION ("an instrument track filtering channel 2 ignores channel 1")
    {
        Rig rig;
        const auto keys = rig.session.createTrack (TrackKind::instrument, "Keys").value();
        model::InputAssignment in;
        in.kind = model::InputAssignment::Kind::midi;
        in.channel = 2;
        REQUIRE (rig.session.setTrackInput (keys, in));
        auto probe = std::make_shared<ProbeInstrument>();
        rig.rack.set (keys, probe);
        rig.sync();
        rig.engine.setSelectedTrack (keys);
        rig.engine.sendMidi (0x90, 60, 100); // channel 1
        rig.run (256);
        CHECK (std::none_of (probe->seen.begin(), probe->seen.end(), [] (const auto& e) { return e.event.isNoteOn(); }));
        rig.engine.sendMidi (0x91, 62, 100); // channel 2
        rig.run (256);
        REQUIRE (std::any_of (probe->seen.begin(), probe->seen.end(), [] (const auto& e) { return e.event.isNoteOn(); }));
    }
}
