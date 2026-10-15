// Piano roll playback: velocity, audition, controllers, muted notes (spec 06 AT-PRL-9, 13, 14, 15; AT-QNT-1).

#include "EngineFixtures.h"

#include "weft/engine/arrange/OfflineRender.h"

#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>

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
