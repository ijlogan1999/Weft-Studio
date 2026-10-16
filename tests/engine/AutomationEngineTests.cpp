// Automation on the engine (spec 06 §11): lanes playing volume, pan, mute, sends and plug-in
// parameters (AT-AUT-4..7, 12, AT-PLG-10 with Weft Gain) and live writing in Touch, Latch and
// Write (AT-AUT-8..10).

#include "EngineFixtures.h"

#include "weft/engine/arrange/OfflineRender.h"
#include "weft/engine/dsp/BuiltinEffects.h"
#include "weft/engine/transport/LiveAutomation.h"
#include "weft/model/Automation.h"
#include "weft/rt/AudioThread.h"

#include <cmath>
#include <numbers>

using namespace weft;
using namespace weft::test;

namespace
{
constexpr double rate = 48000.0;
constexpr std::int64_t samplesPerTick = barSamples / barTicks; // 25

/** A stereo track playing a constant 0.5 for `bars` bars. */
Id dcTrack (Rig& rig, int bars = 8, int channels = 2, const std::string& hash = "b3:dc")
{
    if (! rig.media.find (hash))
    {
        media::AudioData a;
        a.sampleRate = rate;
        a.channels.assign (1, std::vector<float> (static_cast<std::size_t> (bars * barSamples), 0.5f));
        rig.media.add (hash, a);
        model::MediaRef m;
        m.hash = hash;
        m.originalName = "dc.wav";
        m.lengthSamples = bars * barSamples;
        REQUIRE (rig.session.addMedia (m));
    }
    const auto t = rig.session.createTrack (TrackKind::audio, "DC", channels).value();
    REQUIRE (rig.session.createAudioRegion (t, 0, hash, 0, bars * barSamples));
    rig.sync();
    return t;
}

model::AutomationTarget target (const std::string& kind)
{
    model::AutomationTarget t;
    t.kind = kind;
    return t;
}

model::AutomationTarget sendTarget (const Id& sendId)
{
    auto t = target ("sendLevel");
    t.sendId = sendId;
    return t;
}

model::AutomationTarget paramTarget (const Id& pluginId, const std::string& paramId)
{
    auto t = target ("pluginParam");
    t.pluginId = pluginId;
    t.paramId = paramId;
    return t;
}

void point (Rig& rig, const Id& track, const model::AutomationTarget& t, Ticks pos, double value, double curve = 0.0)
{
    REQUIRE (rig.session.addAutomationPoint (track, t, pos, value, curve));
}

struct Stereo
{
    std::vector<float> left, right;
};

/** Renders `frames` more (the transport as it is) and returns both channels. */
Stereo render (Rig& rig, std::int64_t frames, int block = 0)
{
    rig.device.setCaptureCapacity (frames);
    REQUIRE (rig.device.render (frames, block));
    const auto l = rig.device.capturedOutput (0), r = rig.device.capturedOutput (1);
    return { { l.begin(), l.end() }, { r.begin(), r.end() } };
}

double gainFor (double normalized) { return dbToGain (automation::toDb (normalized)); }

double rms (const std::vector<float>& v, std::size_t from, std::size_t to)
{
    double sum = 0;
    for (auto i = from; i < to; ++i)
        sum += static_cast<double> (v[i]) * v[i];
    return std::sqrt (sum / static_cast<double> (to - from));
}

/** Plays to `tick` from where the transport is, ticking the live writer every 10 ms. */
std::optional<LiveAutomation::Pass> playTo (Rig& rig, LiveAutomation& live, Ticks tick, int block = 240)
{
    std::optional<LiveAutomation::Pass> pass;
    while (rig.engine.positionTicks() < static_cast<double> (tick) - 0.5)
    {
        const auto left = static_cast<std::int64_t> (std::llround ((static_cast<double> (tick) - rig.engine.positionTicks()) * samplesPerTick));
        REQUIRE (rig.device.render (std::min<std::int64_t> (480, std::max<std::int64_t> (1, left)), block));
        if (auto p = live.tick (rig.session.model()))
            pass = std::move (p);
    }
    return pass;
}

std::optional<LiveAutomation::Pass> stopAndTick (Rig& rig, LiveAutomation& live)
{
    rig.engine.stop();
    REQUIRE (rig.device.render (256));
    return live.tick (rig.session.model());
}

std::vector<model::AutoPoint> lanePoints (Rig& rig, const Id& track, const model::AutomationTarget& t)
{
    const auto* lane = rig.session.model().lane (track, t);
    REQUIRE (lane != nullptr);
    return automation::sortedPoints (*lane);
}

const double zeroDb = automation::fromDb (0.0);
} // namespace

TEST_CASE ("AT-AUT-4 a volume lane: holds before and after, linear midpoint, curved segment monotonic", "[E][automation]")
{
    Rig rig (64);
    const auto track = dcTrack (rig);
    // Bar 2 at 0 dB to bar 3 at -inf (normalized 0); the lane holds before bar 2.
    point (rig, track, target ("volume"), barTicks, zeroDb);
    point (rig, track, target ("volume"), 2 * barTicks, 0.0);
    rig.sync();
    rig.engine.play (0);
    const auto out = render (rig, 4 * barSamples);
    const auto at = [&] (std::int64_t s) { return static_cast<double> (out.left[static_cast<std::size_t> (s)]); };
    CHECK (std::abs (at (barSamples / 2) - 0.5) < 1e-4);                   // before: first value (0 dB)
    CHECK (std::abs (at (barSamples + barSamples / 2) - 0.5 * gainFor (zeroDb / 2)) < 2e-3); // midpoint
    CHECK (std::abs (at (3 * barSamples + 1000)) < 1e-6);                     // after: last value
    double previous = 1.0;
    for (std::int64_t s = barSamples; s < 2 * barSamples; s += 64)
    {
        CHECK (at (s) <= previous + 1e-6);
        previous = at (s);
    }

    // Curve +0.5: the midpoint differs from linear and the segment stays monotonic.
    const auto* lane = rig.session.model().lane (track, target ("volume"));
    REQUIRE (lane != nullptr);
    const auto first = automation::sortedPoints (*lane).front();
    REQUIRE (rig.session.setAutomationCurve (lane->id, first.id, 0.5));
    rig.sync();
    rig.engine.play (0);
    const auto curved = render (rig, 3 * barSamples);
    const auto mid = static_cast<double> (curved.left[static_cast<std::size_t> (barSamples + barSamples / 2)]);
    CHECK (std::abs (mid - at (barSamples + barSamples / 2)) > 0.01);
    CHECK (std::abs (mid - 0.5 * gainFor (zeroDb * (1.0 - std::pow (0.5, std::pow (4.0, 0.5)))) ) < 2e-3);
    previous = 1.0;
    for (std::int64_t s = barSamples; s < 2 * barSamples; s += 64)
    {
        const auto v = static_cast<double> (curved.left[static_cast<std::size_t> (s)]);
        CHECK (v <= previous + 1e-6);
        previous = v;
    }
}

TEST_CASE ("AT-AUT-5 pan moves a mono sine from left to right; mute silences; send level changes the bus input", "[E][automation]")
{
    SECTION ("pan")
    {
        Rig rig (128);
        media::AudioData a;
        a.sampleRate = rate;
        a.channels.assign (1, std::vector<float> (static_cast<std::size_t> (2 * barSamples)));
        for (std::size_t i = 0; i < a.channels[0].size(); ++i)
            a.channels[0][i] = 0.5f * static_cast<float> (std::sin (2.0 * std::numbers::pi * 440.0 * static_cast<double> (i) / rate));
        rig.media.add ("b3:sine", a);
        model::MediaRef m;
        m.hash = "b3:sine";
        m.lengthSamples = 2 * barSamples;
        REQUIRE (rig.session.addMedia (m));
        const auto track = rig.session.createTrack (TrackKind::audio, "Mono", 1).value();
        REQUIRE (rig.session.createAudioRegion (track, 0, "b3:sine", 0, 2 * barSamples));
        point (rig, track, target ("pan"), 0, automation::fromPan (-1.0));
        point (rig, track, target ("pan"), barTicks, automation::fromPan (1.0));
        rig.sync();
        rig.engine.play (0);
        const auto out = render (rig, barSamples + 4800);
        const auto window = [&] (const std::vector<float>& v, std::int64_t centre) {
            return rms (v, static_cast<std::size_t> (centre - 2400), static_cast<std::size_t> (centre + 2400));
        };
        CHECK (window (out.left, 2400) > 0.3);
        CHECK (window (out.right, 2400) < 0.03);
        CHECK (std::abs (window (out.left, barSamples / 2) - window (out.right, barSamples / 2)) < 0.02);
        CHECK (window (out.right, barSamples + 2400) > 0.3);
        CHECK (window (out.left, barSamples + 2400) < 1e-3);
    }
    SECTION ("mute")
    {
        Rig rig (256);
        const auto track = dcTrack (rig);
        const auto mute = target ("mute");
        point (rig, track, mute, 0, 0.0);
        point (rig, track, mute, 1919, 0.0);
        point (rig, track, mute, 1920, 1.0);
        point (rig, track, mute, 2879, 1.0);
        point (rig, track, mute, 2880, 0.0);
        rig.sync();
        rig.engine.play (0);
        const auto out = render (rig, barSamples);
        CHECK (rms (out.left, 1000, 40000) > 0.49);
        CHECK (rms (out.left, 49000, 70000) < 1e-6);
        CHECK (rms (out.left, 74000, 95000) > 0.49);
    }
    SECTION ("send level")
    {
        Rig rig (256);
        const auto track = dcTrack (rig);
        const auto bus = rig.session.createTrack (TrackKind::bus, "Bus").value();
        const auto send = rig.session.addSend (track, bus, 0.0).value();
        REQUIRE (rig.session.setSendPre (track, send, true));
        REQUIRE (rig.session.setTrackVolume (track, minusInfinityDb)); // only the bus reaches the master
        point (rig, track, sendTarget (send), 0, zeroDb);
        point (rig, track, sendTarget (send), 1919, zeroDb);
        point (rig, track, sendTarget (send), 1920, automation::fromDb (-6.0206));
        rig.sync();
        rig.engine.play (0);
        const auto out = render (rig, barSamples);
        CHECK (std::abs (rms (out.left, 1000, 40000) - 0.5) < 1e-3);
        CHECK (std::abs (rms (out.left, 50000, 90000) - 0.25) < 1e-3);
    }
}

TEST_CASE ("AT-AUT-6 AT-PLG-10 a Weft Gain lane changes the output at the exact sample", "[E][automation]")
{
    Rig rig (256);
    const auto track = dcTrack (rig);
    auto gain = model::PluginInstance {};
    gain.format = "builtin";
    gain.pluginUid = WeftGain::pluginUid;
    gain.name = "Weft Gain";
    const auto plugin = rig.session.addInsert (track, gain).value();
    // 0 dB, then +6 dB from tick 3,845 (sample 96,125: inside a 256-frame block).
    const auto normalized = [] (double db) { return (db + 60.0) / 84.0; };
    point (rig, track, paramTarget (plugin, "gain"), 0, normalized (0.0));
    point (rig, track, paramTarget (plugin, "gain"), barTicks + 4, normalized (0.0));
    point (rig, track, paramTarget (plugin, "gain"), barTicks + 5, normalized (6.0));
    point (rig, track, paramTarget (plugin, "gain"), barTicks + 960, normalized (-60.0));
    rig.sync();
    REQUIRE (96125 % 256 != 0);
    rig.engine.play (0);
    const auto out = render (rig, 2 * barSamples);
    const auto plusSix = 0.5 * std::pow (10.0, 6.0 / 20.0);
    CHECK (std::abs (out.left[96124] - 0.5f) < 1e-6f);
    CHECK (std::abs (out.left[96125] - plusSix) < 1e-5);
    CHECK (std::abs (out.left[96126] - plusSix) < 1e-5);
    // Between points the value follows the lane block by block: falling towards -60 dB.
    CHECK (out.left[96125 + 12000] < out.left[96125 + 100]);
    CHECK (out.left[110000] > out.left[119000]);
    CHECK (std::abs (out.left[120100] - 0.5 * std::pow (10.0, -3.0)) < 1e-5);

    // The static value is left alone while automated, and comes back when the mode is Off.
    REQUIRE (rig.session.setTrackAutomationMode (track, "off"));
    rig.sync();
    rig.engine.play (0);
    const auto off = render (rig, barSamples + 26000);
    CHECK (std::abs (off.left[120100] - 0.5f) < 1e-6f);
}

TEST_CASE ("AT-AUT-7 Off ignores the lane; Read follows it and a touch snaps back without writing", "[E][automation]")
{
    Rig rig (240);
    const auto track = dcTrack (rig);
    const auto volume = target ("volume");
    point (rig, track, volume, 0, automation::fromDb (-6.0206));
    REQUIRE (rig.session.setTrackVolume (track, 0.0));

    REQUIRE (rig.session.setTrackAutomationMode (track, "off"));
    rig.sync();
    rig.engine.play (0);
    auto out = render (rig, 9600);
    CHECK (std::abs (out.left[5000] - 0.5f) < 1e-4f);

    REQUIRE (rig.session.setTrackAutomationMode (track, "read"));
    rig.sync();
    LiveAutomation live (rig.engine);
    CHECK (live.isLive (rig.session.model(), track, volume));
    rig.engine.play (0);
    CHECK_FALSE (playTo (rig, live, barTicks / 4));
    out = render (rig, 4800);
    CHECK (std::abs (out.left[4000] - 0.25f) < 1e-4f);
    live.touch (rig.session.model(), track, volume, zeroDb);
    CHECK_FALSE (playTo (rig, live, barTicks / 2));
    out = render (rig, 4800);
    CHECK (std::abs (out.left[4000] - 0.5f) < 1e-4f);
    live.release (rig.session.model(), track, volume);
    out = render (rig, 4800);
    CHECK (std::abs (out.left[4000] - 0.25f) < 1e-4f); // snapped back at once
    CHECK_FALSE (playTo (rig, live, barTicks));
    CHECK_FALSE (stopAndTick (rig, live)); // nothing written
    CHECK (lanePoints (rig, track, volume).size() == 1);
}

TEST_CASE ("AT-AUT-8 Touch writes only while touched and glides back within 200 ms", "[E][automation]")
{
    Rig rig (240);
    const auto track = dcTrack (rig);
    const auto volume = target ("volume");
    const auto low = automation::fromDb (-12.0);
    point (rig, track, volume, 0, low);
    point (rig, track, volume, 6 * barTicks, low);
    REQUIRE (rig.session.setTrackAutomationMode (track, "touch"));
    rig.sync();
    LiveAutomation live (rig.engine);
    rig.engine.play (0);
    CHECK_FALSE (playTo (rig, live, barTicks));
    live.touch (rig.session.model(), track, volume, zeroDb);
    CHECK_FALSE (playTo (rig, live, barTicks + 960));
    live.touch (rig.session.model(), track, volume, 0.7);
    CHECK_FALSE (playTo (rig, live, 2 * barTicks));
    live.release (rig.session.model(), track, volume);

    // The glide: part way back after 100 ms, on the lane after 200 ms.
    const auto out = render (rig, 14400, 240);
    const auto lane = 0.5 * gainFor (low);
    CHECK (out.left[4790] > lane + 0.01);
    CHECK (out.left[4790] < 0.5 * gainFor (0.7));
    CHECK (std::abs (out.left[9840] - lane) < 1e-4);
    CHECK (std::abs (out.left[14000] - lane) < 1e-4);

    CHECK_FALSE (playTo (rig, live, 4 * barTicks));
    const auto pass = stopAndTick (rig, live);
    REQUIRE (pass);
    REQUIRE (pass->writes.size() == 1);
    const auto& w = pass->writes[0];
    CHECK (w.from >= barTicks - 1);
    CHECK (w.from <= barTicks + 20);
    CHECK (w.to >= 2 * barTicks - 1);
    CHECK (w.to <= 2 * barTicks + 20);
    for (const auto& [pos, value] : w.points)
    {
        CHECK (pos >= w.from);
        CHECK (pos <= w.to);
    }
    CHECK (pass->toLatch.empty());
    REQUIRE (rig.session.writeAutomation (pass->writes, pass->toLatch));
    const auto points = lanePoints (rig, track, volume);
    CHECK (points.front().pos == 0);
    CHECK (points.back().pos == 6 * barTicks);
    for (const auto& p : points)
        if (p.pos > 0 && p.pos < 6 * barTicks)
        {
            CHECK (p.pos >= w.from);
            CHECK (p.pos <= w.to);
        }
}

TEST_CASE ("AT-AUT-9 Latch writes from the touch until stop, holding the release value", "[E][automation]")
{
    Rig rig (240);
    const auto track = dcTrack (rig);
    const auto volume = target ("volume");
    point (rig, track, volume, 0, automation::fromDb (-12.0));
    REQUIRE (rig.session.setTrackAutomationMode (track, "latch"));
    rig.sync();
    LiveAutomation live (rig.engine);
    rig.engine.play (0);
    CHECK_FALSE (playTo (rig, live, barTicks));
    live.touch (rig.session.model(), track, volume, zeroDb);
    CHECK_FALSE (playTo (rig, live, barTicks + 1920));
    live.touch (rig.session.model(), track, volume, 0.6);
    CHECK_FALSE (playTo (rig, live, 2 * barTicks));
    live.release (rig.session.model(), track, volume);
    const auto held = render (rig, 9600);
    CHECK (std::abs (held.left[9000] - 0.5 * gainFor (0.6)) < 1e-4); // holds after release
    CHECK_FALSE (playTo (rig, live, 4 * barTicks));
    const auto pass = stopAndTick (rig, live);
    REQUIRE (pass);
    REQUIRE (rig.session.writeAutomation (pass->writes, pass->toLatch));
    const auto points = lanePoints (rig, track, volume);
    REQUIRE (points.size() >= 3);
    CHECK (points[0].pos == 0);
    CHECK (points[1].pos >= barTicks - 1);
    CHECK (points[1].pos <= barTicks + 20);
    CHECK (points.back().pos >= 4 * barTicks - 20);
    CHECK (std::abs (points.back().value - 0.6) < 1e-9);
    for (const auto& p : points)
        if (p.pos > 2 * barTicks + 20)
            CHECK (std::abs (p.value - 0.6) < 1e-9);
    CHECK (rig.session.model().track (track)->automationMode == "latch");
}

TEST_CASE ("AT-AUT-10 Write replaces the played range with the fader, then switches to Latch, one undo step", "[E][automation]")
{
    Rig rig (240);
    const auto track = dcTrack (rig);
    const auto volume = target ("volume");
    for (int bar = 0; bar <= 5; ++bar)
        point (rig, track, volume, bar * barTicks, bar % 2 == 0 ? 0.2 : 0.9);
    REQUIRE (rig.session.setTrackAutomationMode (track, "write"));
    rig.sync();
    LiveAutomation live (rig.engine);
    CHECK (live.isLive (rig.session.model(), track, volume));
    live.touch (rig.session.model(), track, volume, 0.5);
    rig.engine.play (0);
    CHECK_FALSE (playTo (rig, live, barTicks + 960));
    live.touch (rig.session.model(), track, volume, 0.7);
    CHECK_FALSE (playTo (rig, live, 2 * barTicks));
    live.release (rig.session.model(), track, volume);
    CHECK_FALSE (playTo (rig, live, 3 * barTicks));
    CHECK (live.writingTracks() == std::vector<Id> { track });
    const auto pass = stopAndTick (rig, live);
    REQUIRE (pass);
    CHECK (pass->toLatch == std::vector<Id> { track });
    const auto undoDepth = rig.session.canUndo();
    REQUIRE (rig.session.writeAutomation (pass->writes, pass->toLatch));
    CHECK (undoDepth);
    const auto points = lanePoints (rig, track, volume);
    // Bars 1-4 (ticks 0 to 3 bars) now hold the fader's values; bar 5 and 6 are untouched.
    for (const auto& p : points)
    {
        if (p.pos <= 3 * barTicks - 20)
        {
            CHECK ((std::abs (p.value - 0.5) < 1e-9 || std::abs (p.value - 0.7) < 1e-9));
        }
    }
    CHECK (points.back().pos == 5 * barTicks);
    CHECK (std::abs (points.back().value - 0.9) < 1e-9);
    CHECK (rig.session.model().track (track)->automationMode == "latch");
    CHECK (rig.session.undoName() == "Write Automation");
    REQUIRE (rig.session.undo().done);
    CHECK (rig.session.model().track (track)->automationMode == "write");
    CHECK (lanePoints (rig, track, volume).size() == 6);
}

TEST_CASE ("AT-AUT-12 master volume automation affects the bounce; bus pan automation the bus output", "[E][automation]")
{
    Rig rig (256);
    const auto track = dcTrack (rig, 2);
    const auto master = rig.session.model().info.masterTrackId;
    point (rig, master, target ("volume"), 0, zeroDb);
    point (rig, master, target ("volume"), 1919, zeroDb);
    point (rig, master, target ("volume"), 1920, automation::fromDb (-6.0206));
    OfflineRenderOptions options;
    options.frames = barSamples;
    const auto bounce = renderOffline (rig.session.model(), rig.media, options);
    REQUIRE (bounce.channels.size() == 2);
    CHECK (std::abs (bounce.channels[0][10000] - 0.5f) < 1e-4f);
    CHECK (std::abs (bounce.channels[0][60000] - 0.25f) < 1e-4f);

    const auto bus = rig.session.createTrack (TrackKind::bus, "Bus").value();
    REQUIRE (rig.session.setTrackOutput (track, bus));
    point (rig, bus, target ("pan"), 0, automation::fromPan (-1.0));
    point (rig, bus, target ("pan"), 1919, automation::fromPan (-1.0));
    point (rig, bus, target ("pan"), 1920, automation::fromPan (1.0));
    rig.sync();
    rig.engine.play (0);
    const auto out = render (rig, barSamples);
    CHECK (std::abs (out.left[10000] - 0.5f) < 1e-4f);
    CHECK (std::abs (out.right[10000]) < 1e-6f);
    CHECK (std::abs (out.left[60000]) < 1e-6f);
    CHECK (std::abs (out.right[60000] - 0.25f) < 1e-4f); // the master lane halves it here
}

TEST_CASE ("AT-RT-1 automated playback and touches allocate nothing on the audio thread", "[E][rt][automation]")
{
    Rig rig (256);
    const auto track = dcTrack (rig);
    auto gain = model::PluginInstance {};
    gain.format = "builtin";
    gain.pluginUid = WeftGain::pluginUid;
    gain.name = "Weft Gain";
    const auto plugin = rig.session.addInsert (track, gain).value();
    for (int i = 0; i < 32; ++i)
    {
        point (rig, track, target ("volume"), i * 480, (i % 4) / 4.0);
        point (rig, track, target ("pan"), i * 480 + 7, (i % 3) / 3.0);
        point (rig, track, paramTarget (plugin, "gain"), i * 480 + 11, (i % 5) / 5.0);
    }
    point (rig, rig.session.model().info.masterTrackId, target ("volume"), 0, 0.7);
    rig.sync();
    LiveAutomation live (rig.engine);
    live.touch (rig.session.model(), track, target ("mute"), 0.0); // a plan made by a touch
    REQUIRE (rig.device.render (1024));
    weft::rt::resetViolations();
    rig.engine.play (0);
    REQUIRE (rig.device.render (48000, 256));
    live.touch (rig.session.model(), track, target ("volume"), 0.3);
    REQUIRE (rig.device.render (20000, 128));
    live.release (rig.session.model(), track, target ("volume"));
    REQUIRE (rig.device.render (20000, 77));
    const auto v = weft::rt::violations();
    CHECK (v.allocations == 0);
    CHECK (v.blockingCalls == 0);
}
