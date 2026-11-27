// Piano roll playback: velocity, audition, controllers, muted notes (spec 06 AT-PRL-9, 13, 14, 15; AT-QNT-1).

#include "EngineFixtures.h"

#include "weft/engine/arrange/OfflineRender.h"
#include "weft/commands/Notes.h"
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

TEST_CASE ("AT-PRL-22: MIDI region dynamics, gate time, clip length, Q-Range, Q-Flam and delay in ticks", "[E][pianoroll][AT-PRL-22]")
{
    KeysRig k;
    auto& s = k.rig.session;
    const auto add = [&] (Ticks pos, Ticks length, int pitch, int velocity) {
        model::Note n;
        n.pos = pos;
        n.length = length;
        n.pitch = pitch;
        n.velocity = velocity;
        return *s.addNote (k.region, n);
    };
    const auto played = [&] {
        const auto& r = *s.model().region (k.region);
        return commands::playedNotes (r, *s.model().sequence (r.midi.sequenceId), r.start, r.start + r.length);
    };
    add (0, 480, 60, 100);
    add (960, 480, 62, 40);

    // Dynamics scales the distance from 64; Fixed plays them alike, and the velocity offset adds after.
    REQUIRE (s.setRegionDynamics (k.region, 50));
    CHECK (s.undoName() == "Region Dynamics");
    auto p = played();
    REQUIRE (p.size() == 2);
    CHECK (p[0].velocity == 82);
    CHECK (p[1].velocity == 52);
    REQUIRE (s.setRegionDynamics (k.region, 0));
    REQUIRE (s.setRegionVelocityOffset (k.region, 10));
    p = played();
    CHECK (p[0].velocity == 74);
    CHECK (p[1].velocity == 74);
    CHECK_FALSE (s.setRegionDynamics (k.region, 301));
    REQUIRE (s.setRegionDynamics (k.region, 100));
    REQUIRE (s.setRegionVelocityOffset (k.region, 0));

    // Gate time: a percent of the length, Fixed short, Legato to the next note.
    REQUIRE (s.setRegionGate (k.region, 50));
    CHECK (played()[0].off == 240);
    REQUIRE (s.setRegionGate (k.region, model::MidiRegionData::gateFixed));
    CHECK (played()[0].off == model::MidiRegionData::fixedGateTicks);
    REQUIRE (s.setRegionGate (k.region, model::MidiRegionData::gateLegato));
    p = played();
    CHECK (p[0].off == 960);
    CHECK (p[1].off == 1440); // the last keeps its length
    CHECK_FALSE (s.setRegionGate (k.region, 201));
    REQUIRE (s.setRegionGate (k.region, 0));

    // Clip Length: a note past the region's end stops there, or plays on with it off.
    const auto end = s.model().region (k.region)->length;
    add (end - 240, 960, 64, 100);
    CHECK (played().back().off == end);
    REQUIRE (s.setRegionClipLength (k.region, false));
    CHECK (played().back().off == end + 720);
    k.play (barSamples * 3);
    const auto offs = k.seen (0x80);
    REQUIRE_FALSE (offs.empty());
    CHECK (offs.back().frame == (end + 720) * barSamples / barTicks);
    REQUIRE (s.setRegionClipLength (k.region, true));

    // Q-Range and Q-Flam, on a 1/16 grid.
    const auto chordBase = 2 * barTicks;
    REQUIRE (s.trimRegionLength (k.region, 3 * barTicks));
    add (chordBase + 10, 120, 50, 100);  // 10 ticks late
    add (chordBase + 300, 120, 51, 100); // 60 ticks late of 240
    model::RegionQuantize q;
    q.grid = 240;
    q.range = 30;
    REQUIRE (s.setRegionQuantize (k.region, q));
    const auto onOf = [&] (int pitch) {
        for (const auto& n : played())
            if (n.note->pitch == pitch)
                return n.on;
        return Ticks (-1);
    };
    CHECK (onOf (50) == chordBase);
    CHECK (onOf (51) == chordBase + 300);
    q.range = -30;
    REQUIRE (s.setRegionQuantize (k.region, q));
    CHECK (onOf (50) == chordBase + 10);
    CHECK (onOf (51) == chordBase + 240);
    q.range = 0;
    q.flam = 20; // a chord a beat later, one note 5 ticks late
    add (chordBase + 960, 120, 67, 100);
    add (chordBase + 960, 120, 72, 100);
    add (chordBase + 965, 120, 70, 100);
    REQUIRE (s.setRegionQuantize (k.region, q));
    CHECK (onOf (67) == chordBase + 960);
    CHECK (onOf (70) == chordBase + 980);
    CHECK (onOf (72) == chordBase + 1000);
    q.flam = -20;
    REQUIRE (s.setRegionQuantize (k.region, q));
    CHECK (onOf (72) == chordBase + 960);
    CHECK (onOf (67) == chordBase + 1000);
    q.range = 4000;
    CHECK_FALSE (s.setRegionQuantize (k.region, q));
    REQUIRE (s.setRegionQuantize (k.region, std::nullopt));

    // Delay in ticks; setting milliseconds goes back to them.
    REQUIRE (s.setRegionDelayTicks (k.region, 480));
    CHECK (s.model().region (k.region)->delayInTicks);
    k.play (barSamples);
    REQUIRE_FALSE (k.seen (0x90).empty());
    CHECK (k.seen (0x90)[0].frame == 480 * barSamples / barTicks);
    CHECK_FALSE (s.setRegionDelayTicks (k.region, 961));
    REQUIRE (s.setRegionDelay (k.region, 10.0));
    CHECK_FALSE (s.model().region (k.region)->delayInTicks);
    REQUIRE (s.setRegionDelayTicks (k.region, 240));

    // A save and load keeps them.
    REQUIRE (s.setRegionGate (k.region, 50));
    const auto again = model::Region::fromValue (k.region, s.model().region (k.region)->toValue());
    CHECK (again.midi.gate == 50);
    CHECK (again.delayInTicks);
    CHECK (again.delayTicks == 240);
}

TEST_CASE ("AT-PRL-23: Apply All Parameters writes the region parameters into the notes", "[E][pianoroll][AT-PRL-23]")
{
    KeysRig k;
    auto& s = k.rig.session;
    model::Note n;
    n.pos = 250;
    n.length = 480;
    n.pitch = 60;
    n.velocity = 100;
    const auto note = *s.addNote (k.region, n);
    REQUIRE (s.setRegionQuantize (k.region, model::RegionQuantize { 240, 1.0, 0.0 }));
    REQUIRE (s.setRegionTranspose (k.region, 2));
    REQUIRE (s.setRegionVelocityOffset (k.region, 10));
    REQUIRE (s.setRegionGate (k.region, 50));
    REQUIRE (s.setRegionDelayTicks (k.region, 120));
    k.play (barSamples);
    const auto before = k.seen (0x90);
    REQUIRE (before.size() == 1);

    REQUIRE (s.applyRegionParameters ({ k.region }, false));
    CHECK (s.undoName() == "Apply All Except Delay");
    auto r = *s.model().region (k.region);
    CHECK_FALSE (r.midi.quantize);
    CHECK (r.midi.transpose == 0);
    CHECK (r.midi.velocityOffset == 0);
    CHECK (r.midi.gate == 0);
    CHECK (r.delayTicks == 120); // kept
    const auto* written = s.model().sequence (r.midi.sequenceId)->sortedNotes()[0];
    CHECK (written->pos == 240);
    CHECK (written->length == 240);
    CHECK (written->pitch == 62);
    CHECK (written->velocity == 110);
    // It plays the same.
    k.play (barSamples);
    const auto after = k.seen (0x90);
    REQUIRE (after.size() == 1);
    CHECK (after[0].frame == before[0].frame);
    CHECK (after[0].event.data1 == before[0].event.data1);
    CHECK (after[0].event.data2 == before[0].event.data2);

    // With the delay too, then one undo step back.
    REQUIRE (s.applyRegionParameters ({ k.region }, true));
    CHECK (s.undoName() == "Apply All Parameters");
    r = *s.model().region (k.region);
    CHECK (r.delayTicks == 0);
    CHECK (s.model().sequence (r.midi.sequenceId)->sortedNotes()[0]->pos == 360);
    s.undo();
    s.undo();
    r = *s.model().region (k.region);
    CHECK (r.midi.transpose == 2);
    CHECK (s.model().sequence (r.midi.sequenceId)->sortedNotes()[0]->pos == 250);
    CHECK (s.model().sequence (r.midi.sequenceId)->notes.contains (note));
}

TEST_CASE ("AT-UI-20: controllers from enabled MIDI inputs reach the Smart Controls tap and still play", "[E][midi][AT-UI-20]")
{
    Rig rig;
    engine::MidiInputRouter router (rig.engine);
    std::vector<std::array<std::uint8_t, 3>> tapped;
    router.setControllerTap ([&] (std::uint8_t status, std::uint8_t d1, std::uint8_t d2) { tapped.push_back ({ status, d1, d2 }); });
    router.setDisabled ({ "Off Port" });
    CHECK (router.deliver ("Pad Controller", 0xB0, 21, 64));
    CHECK (router.deliver ("Pad Controller", 0x90, 60, 100)); // notes aren't tapped
    CHECK_FALSE (router.deliver ("Off Port", 0xB3, 22, 1));   // a disabled input is dropped before the tap
    CHECK (router.deliver ("Pad Controller", 0xB2, 7, 127));
    REQUIRE (tapped.size() == 2);
    CHECK (tapped[0] == std::array<std::uint8_t, 3> { 0xB0, 21, 64 });
    CHECK (tapped[1] == std::array<std::uint8_t, 3> { 0xB2, 7, 127 });
    router.setControllerTap (nullptr);
    CHECK (router.deliver ("Pad Controller", 0xB0, 21, 0));
    CHECK (tapped.size() == 2);
}
