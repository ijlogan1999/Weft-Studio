// Piano roll playback: velocity, audition, controllers, muted notes (spec 06 AT-PRL-9, 13, 14, 15; AT-QNT-1).

#include "EngineFixtures.h"

#include "weft/engine/arrange/OfflineRender.h"
#include "weft/engine/midi/MidiInputs.h"

#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <tuple>

using namespace weft;
using namespace weft::test;

namespace
{
struct KeysRig
{
    Rig rig;
    Id keys, region;
    std::shared_ptr<ProbeInstrument> probe = std::make_shared<ProbeInstrument>();

    KeysRig()
    {
        keys = rig.session.createTrack (TrackKind::instrument, "Keys").value();
        rig.rack.set (keys, probe);
        region = rig.session.createMidiRegion (keys, 0, 2 * barTicks).value();
        rig.sync();
    }

    std::vector<float> play (std::int64_t frames)
    {
        rig.sync();
        probe->seen.clear();
        probe->frames = 0; // the device renders on this thread, so this is between blocks
        rig.engine.play (0);
        return rig.run (frames);
    }

    std::vector<ProbeInstrument::Seen> seen (std::uint8_t statusHigh) const
    {
        std::vector<ProbeInstrument::Seen> out;
        for (const auto& e : probe->seen)
            if ((e.event.status & 0xf0) == statusHigh)
                out.push_back (e);
        return out;
    }
};

} // namespace

TEST_CASE ("AT-PRL-9: output amplitude follows note velocity", "[E][pianoroll][AT-PRL-9]")
{
    // The probe instrument stands in for the TestSynth fixture until plug-in hosting (M6):
    // both sound at a level proportional to velocity/127.
    KeysRig k;
    model::Note soft, loud;
    soft.pos = 0;
    soft.length = 960;
    soft.velocity = 64;
    loud.pos = 1920;
    loud.length = 960;
    loud.velocity = 127;
    loud.pitch = 62;
    REQUIRE (k.rig.session.addNote (k.region, soft));
    REQUIRE (k.rig.session.addNote (k.region, loud));
    const auto out = k.play (barSamples);
    const auto a = peak (out, 1000, 19000);
    const auto b = peak (out, 49000, 19000);
    REQUIRE (b > 0);
    CHECK (a / b == Catch::Approx (64.0 / 127.0).epsilon (0.005));
}

TEST_CASE ("AT-PRL-13: audition plays a note at its velocity for at most 1 s", "[E][pianoroll][AT-PRL-13]")
{
    KeysRig k;
    k.probe->seen.clear();
    REQUIRE (k.rig.engine.audition (k.keys, 1, 64, 90, 5.0)); // longer than allowed: 1 s
    k.rig.run (60000);
    const auto ons = k.seen (0x90);
    const auto offs = k.seen (0x80);
    REQUIRE (ons.size() == 1);
    CHECK (ons[0].event.data1 == 64);
    CHECK (ons[0].event.data2 == 90);
    REQUIRE (offs.size() == 1);
    CHECK (offs[0].frame - ons[0].frame == 48000);

    // Audition off: the editor sends nothing, so the instrument hears nothing.
    k.probe->seen.clear();
    k.rig.run (4800);
    CHECK (k.probe->seen.empty());

    CHECK_FALSE (k.rig.engine.audition ("no-such-track", 1, 60, 100));
}

TEST_CASE ("AT-PRL-14: controller points play as CC and 14-bit pitch bend", "[E][pianoroll][AT-PRL-14]")
{
    KeysRig k;
    model::CcEvent cc;
    cc.kind = "cc";
    cc.number = 1;
    cc.pos = 0;
    cc.value = 0;
    REQUIRE (k.rig.session.addController (k.region, cc));
    cc.id = {};
    cc.pos = barTicks;
    cc.value = 127;
    REQUIRE (k.rig.session.addController (k.region, cc));
    for (const auto [pos, value] : { std::pair<Ticks, int> { 960, -8192 }, { 1920, 0 }, { 2880, 8191 } })
    {
        model::CcEvent bend;
        bend.kind = "pitchBend";
        bend.pos = pos;
        bend.value = value;
        REQUIRE (k.rig.session.addController (k.region, bend));
    }
    k.play (barSamples + 4800);

    const auto ccs = k.seen (0xb0);
    REQUIRE (ccs.size() == 2);
    CHECK (ccs[0].frame == 0);
    CHECK ((ccs[0].event.data1 == 1 && ccs[0].event.data2 == 0));
    CHECK (ccs[1].frame == barSamples);
    CHECK ((ccs[1].event.data1 == 1 && ccs[1].event.data2 == 127));

    const auto bends = k.seen (0xe0);
    REQUIRE (bends.size() == 3);
    const auto value14 = [] (const MidiEvent& e) { return (e.data2 << 7 | e.data1) - 8192; };
    CHECK (value14 (bends[0].event) == -8192);
    CHECK (value14 (bends[1].event) == 0);
    CHECK (value14 (bends[2].event) == 8191);
    CHECK (bends[1].frame == barSamples / 2);
}

TEST_CASE ("AT-PRL-15: muted notes are not sent at playback or bounce", "[E][pianoroll][AT-PRL-15]")
{
    KeysRig k;
    model::Note n;
    n.pos = 0;
    n.length = 960;
    const auto id = k.rig.session.addNote (k.region, n).value();
    REQUIRE (k.rig.session.toggleNotesMuted ({ { k.region, id } }));
    k.play (barSamples);
    CHECK (k.seen (0x90).empty());

    engine::OfflineRenderOptions options;
    options.frames = barSamples / 2;
    auto bounce = engine::renderOffline (k.rig.session.model(), k.rig.media, options);
    CHECK (peak (bounce.channels[0], 0, options.frames) == 0.0f);

    REQUIRE (k.rig.session.toggleNotesMuted ({ { k.region, id } }));
    k.play (barSamples);
    CHECK (k.seen (0x90).size() == 1);
    bounce = engine::renderOffline (k.rig.session.model(), k.rig.media, options);
    CHECK (peak (bounce.channels[0], 0, options.frames) > 0.0f);
}

TEST_CASE ("AT-QNT-1: region quantize plays quantized; Off restores the stored timing", "[E][pianoroll][AT-QNT-1]")
{
    KeysRig k;
    model::Note n;
    n.pos = 250;
    REQUIRE (k.rig.session.addNote (k.region, n));
    REQUIRE (k.rig.session.setRegionQuantize (k.region, model::RegionQuantize { 480, 1.0, 0.0 }));
    k.play (barSamples);
    const auto ticksToSamples = [] (Ticks t) { return t * barSamples / barTicks; };
    REQUIRE (k.seen (0x90).size() == 1);
    CHECK (k.seen (0x90)[0].frame == ticksToSamples (480));

    REQUIRE (k.rig.session.setRegionQuantize (k.region, std::nullopt));
    k.play (barSamples);
    REQUIRE (k.seen (0x90).size() == 1);
    CHECK (k.seen (0x90)[0].frame == ticksToSamples (250));
}

TEST_CASE ("AT-PRL-16: region transpose and velocity offset play without changing the notes; out of range is refused", "[E][pianoroll][AT-PRL-16]")
{
    KeysRig k;
    model::Note n;
    n.pos = 0;
    n.length = 960;
    n.pitch = 60;
    n.velocity = 100;
    REQUIRE (k.rig.session.addNote (k.region, n));
    REQUIRE (k.rig.session.setRegionTranspose (k.region, 12));
    REQUIRE (k.rig.session.setRegionVelocityOffset (k.region, 20));
    k.play (barSamples);
    REQUIRE (k.seen (0x90).size() == 1);
    CHECK (k.seen (0x90)[0].event.data1 == 72);
    CHECK (k.seen (0x90)[0].event.data2 == 120);
    // The stored note is unchanged; the playback result stays within 1..127.
    const auto& r = *k.rig.session.model().region (k.region);
    CHECK (k.rig.session.model().sequence (r.midi.sequenceId)->sortedNotes()[0]->pitch == 60);
    REQUIRE (k.rig.session.setRegionVelocityOffset (k.region, 99));
    REQUIRE (k.rig.session.setRegionTranspose (k.region, -96));
    k.play (barSamples);
    REQUIRE (k.seen (0x90).size() == 1);
    CHECK (k.seen (0x90)[0].event.data1 == 0);
    CHECK (k.seen (0x90)[0].event.data2 == 127);

    CHECK_FALSE (k.rig.session.setRegionTranspose (k.region, 97));
    CHECK_FALSE (k.rig.session.setRegionVelocityOffset (k.region, -100));
    CHECK (k.rig.session.model().region (k.region)->midi.transpose == -96);

    // One undo step each.
    k.rig.session.undo();
    CHECK (k.rig.session.model().region (k.region)->midi.transpose == 12);
    CHECK (k.rig.session.model().region (k.region)->midi.velocityOffset == 99);

    // Audio regions refuse both.
    const auto audio = k.rig.audioTrack();
    const auto ar = k.rig.addAudioRegion (audio, 0, 4800);
    CHECK_FALSE (k.rig.session.setRegionTranspose (ar, 1));
    CHECK_FALSE (k.rig.session.setRegionVelocityOffset (ar, 1));
}

TEST_CASE ("AT-ARR-30: region delay shifts MIDI and audio playback without moving the region", "[E][arrange][AT-ARR-30]")
{
    KeysRig k;
    model::Note n;
    n.pos = 960;
    n.length = 480;
    REQUIRE (k.rig.session.addNote (k.region, n));
    REQUIRE (k.rig.session.setRegionDelay (k.region, 100.0));
    CHECK (k.rig.session.model().region (k.region)->start == 0);
    k.play (barSamples);
    const auto ticksToSamples = [] (Ticks t) { return t * barSamples / barTicks; };
    REQUIRE (k.seen (0x90).size() == 1);
    CHECK (k.seen (0x90)[0].frame == ticksToSamples (960) + 4800);
    REQUIRE (k.seen (0x80).size() == 1);
    CHECK (k.seen (0x80)[0].frame == ticksToSamples (1440) + 4800);

    REQUIRE (k.rig.session.setRegionDelay (k.region, -10.0));
    k.play (barSamples);
    REQUIRE (k.seen (0x90).size() == 1);
    CHECK (k.seen (0x90)[0].frame == ticksToSamples (960) - 480);
    CHECK_FALSE (k.rig.session.setRegionDelay (k.region, 501.0));

    // Audio: the region's first sample arrives 10 ms late.
    const auto audio = k.rig.audioTrack();
    const auto r = k.rig.addAudioRegion (audio, barTicks, 4800);
    REQUIRE (k.rig.session.setRegionDelay (r, 10.0));
    engine::OfflineRenderOptions options;
    options.from = barTicks;
    options.frames = 2000;
    const auto out = engine::renderOffline (k.rig.session.model(), k.rig.media, options);
    CHECK (out.channels[0][479] == 0.0f);
    CHECK (out.channels[0][480] != 0.0f);
}

TEST_CASE ("AT-TRK-23: track parameters change what an instrument track plays, from regions and live input", "[E][tracks][AT-TRK-23]")
{
    KeysRig k;
    auto& s = k.rig.session;
    // Played with transpose 12 + 2 and velocity +10: 74 and 54 play; 44, 84 and velocity 30 fall outside the limits.
    for (const auto& [pos, pitch, velocity] : { std::tuple { 0, 60, 100 }, { 960, 40, 100 }, { 1920, 30, 100 }, { 2400, 70, 100 }, { 2880, 60, 20 } })
    {
        model::Note n;
        n.pos = pos;
        n.length = 480;
        n.pitch = pitch;
        n.velocity = velocity;
        n.channel = 3;
        REQUIRE (s.addNote (k.region, n));
    }
    REQUIRE (s.setRegionTranspose (k.region, 2));

    model::TrackPlayback p;
    p.transpose = 12;
    p.velocityOffset = 10;
    p.keyLow = 48;
    p.keyHigh = 76;
    p.velocityLow = 50;
    p.outChannel = 5;
    p.delay = 100.0; // ms
    REQUIRE (s.setTrackPlayback (k.keys, p));
    CHECK (s.undoName() == "Track Parameters");
    k.play (barSamples);
    const auto ticksToSamples = [] (Ticks t) { return t * barSamples / barTicks; };
    auto ons = k.seen (0x90);
    REQUIRE (ons.size() == 2);
    CHECK (ons[0].event.data1 == 74); // 60 + 12 + 2
    CHECK (ons[0].event.data2 == 110);
    CHECK ((ons[0].event.status & 0x0f) == 4); // channel 5
    CHECK (ons[0].frame == 4800);              // 100 ms at 48 kHz
    CHECK (ons[1].event.data1 == 54);
    CHECK (ons[1].frame == ticksToSamples (960) + 4800);
    // The stored notes are unchanged.
    const auto& seq = *s.model().sequence (s.model().region (k.region)->midi.sequenceId);
    CHECK (seq.sortedNotes()[0]->pitch == 60);
    CHECK (seq.sortedNotes()[0]->channel == 3);

    // No Transpose ignores the region's transpose; the delay in ticks follows the tempo.
    p.noTranspose = true;
    p.delayInTicks = true;
    p.delay = 480;
    REQUIRE (s.setTrackPlayback (k.keys, p));
    k.play (barSamples);
    ons = k.seen (0x90);
    REQUIRE (ons.size() == 2);
    CHECK (ons[0].event.data1 == 72);
    CHECK (ons[0].frame == ticksToSamples (480));

    // Out of range, crossed limits and audio tracks are refused; undo is one step per change.
    auto bad = p;
    bad.keyLow = 80;
    CHECK_FALSE (s.setTrackPlayback (k.keys, bad));
    bad = p;
    bad.delay = 961;
    CHECK_FALSE (s.setTrackPlayback (k.keys, bad));
    bad = p;
    bad.outChannel = 17;
    CHECK_FALSE (s.setTrackPlayback (k.keys, bad));
    s.undo();
    CHECK (s.model().track (k.keys)->playback.transpose == 12);
    CHECK_FALSE (s.model().track (k.keys)->playback.noTranspose);
    CHECK (s.model().track (k.keys)->playback.delay == 100.0);
    CHECK_FALSE (s.setTrackPlayback (k.rig.audioTrack(), model::TrackPlayback {}));

    // The parameters survive a save and load.
    const auto again = model::Track::fromValue (k.keys, s.model().track (k.keys)->toValue());
    CHECK (again.playback == s.model().track (k.keys)->playback);

    // Live input: transpose, velocity, limits and out channel apply; the MIDI input device filters.
    model::InputAssignment in;
    in.kind = model::InputAssignment::Kind::midi;
    in.device = "USB Keys";
    REQUIRE (s.setTrackInput (k.keys, in));
    k.rig.sync();
    k.rig.engine.stop();
    k.rig.engine.setArmedTracks ({ k.keys });
    k.rig.device.render (512);
    k.probe->seen.clear();
    engine::MidiInputRouter router (k.rig.engine);
    CHECK (router.deliver ("USB Keys", 0x90, 50, 100)); // plays as 62 at 110 on channel 5
    CHECK (router.deliver ("Other Port", 0x90, 52, 100)); // another device: not this track
    CHECK (router.deliver ({}, 0x90, 53, 100));           // the computer keyboard reaches it
    CHECK (router.deliver ("USB Keys", 0x90, 70, 100));   // 82 is above the key limit
    CHECK (router.deliver ("USB Keys", 0x90, 51, 30));    // 40 is below the velocity limit
    k.rig.device.render (512);
    ons = k.seen (0x90);
    REQUIRE (ons.size() == 2);
    CHECK (ons[0].event.data1 == 62);
    CHECK (ons[0].event.data2 == 110);
    CHECK ((ons[0].event.status & 0x0f) == 4);
    CHECK (ons[1].event.data1 == 65);
}
