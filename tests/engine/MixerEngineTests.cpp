// Mixer and built-in plug-in acceptance tests that run on the engine (spec 06 §9, §10):
// AT-MIX-2..7, 11..13 and AT-PLG-6..9, 11. The VST3 fixture versions are in tests/plugins.

#include "EngineFixtures.h"

#include "weft/engine/arrange/OfflineRender.h"
#include "weft/engine/dsp/BuiltinEffects.h"
#include "weft/engine/dsp/WeftKeys.h"
#include "weft/media/Blobs.h"
#include "weft/project/ProjectFolder.h"

#include <catch2/catch_approx.hpp>

#include <chrono>
#include <cmath>
#include <numbers>

using namespace weft;
using namespace weft::test;
using doc::Value;

namespace
{
constexpr double rate = 48000.0;

void addMedia (Rig& rig, const std::string& hash, std::vector<std::vector<float>> channels)
{
    media::AudioData a;
    a.sampleRate = 48000;
    a.channels = std::move (channels);
    const auto frames = static_cast<std::int64_t> (a.channels[0].size());
    rig.media.add (hash, a);
    model::MediaRef m;
    m.hash = hash;
    m.originalName = hash + ".wav";
    m.channels = static_cast<int> (a.channels.size());
    m.lengthSamples = frames;
    REQUIRE (rig.session.addMedia (m));
}

std::vector<float> constant (float v, std::int64_t frames = barSamples) { return std::vector<float> (static_cast<std::size_t> (frames), v); }

std::vector<float> sine (double freq, float amplitude, std::int64_t frames = barSamples)
{
    std::vector<float> s (static_cast<std::size_t> (frames));
    for (std::size_t i = 0; i < s.size(); ++i)
        s[i] = amplitude * static_cast<float> (std::sin (2.0 * std::numbers::pi * freq * static_cast<double> (i) / rate));
    return s;
}

/** An audio track playing `hash` from bar 1. */
Id trackPlaying (Rig& rig, const std::string& name, const std::string& hash, int channels = 2, Ticks at = 0)
{
    const auto t = rig.session.createTrack (TrackKind::audio, name, channels).value();
    const auto* m = rig.session.model().media.find (hash) != rig.session.model().media.end() ? rig.session.model().media.at (hash).get() : nullptr;
    REQUIRE (m != nullptr);
    REQUIRE (rig.session.createAudioRegion (t, at, hash, 0, m->lengthSamples));
    rig.sync();
    return t;
}

model::PluginInstance builtin (const std::string& uid)
{
    model::PluginInstance p;
    p.format = "builtin";
    p.pluginUid = uid;
    p.name = uid;
    return p;
}

model::PluginInstance hosted (const std::string& uid)
{
    model::PluginInstance p;
    p.format = "vst3";
    p.pluginUid = uid;
    p.name = "Hosted " + uid;
    return p;
}

/** Renders both channels from bar 1. */
std::pair<std::vector<float>, std::vector<float>> playBoth (Rig& rig, std::int64_t frames, int block = 0)
{
    rig.engine.play (0);
    rig.device.setCaptureCapacity (frames);
    REQUIRE (rig.device.render (frames, block));
    const auto l = rig.device.capturedOutput (0), r = rig.device.capturedOutput (1);
    return { { l.begin(), l.end() }, { r.begin(), r.end() } };
}

float maxDelta (const std::vector<float>& v, std::size_t from = 1)
{
    float d = 0;
    for (std::size_t i = std::max<std::size_t> (1, from); i < v.size(); ++i)
        d = std::max (d, std::abs (v[i] - v[i - 1]));
    return d;
}

double db (double gain) { return 20.0 * std::log10 (gain); }

/** Delays its input and reports the delay (stands in for TestLatency without the VST3 host). */
class LatencyProcessor final : public Processor
{
public:
    explicit LatencyProcessor (int samples) : delay (samples) {}
    void prepare (double, int) override
    {
        line.assign (static_cast<std::size_t> (delay) + 1, {});
        pos = 0;
    }
    void process (std::span<const MidiEvent>, float* const* io, int numChannels, int numFrames) noexcept override
    {
        for (int i = 0; i < numFrames; ++i)
        {
            auto& slot = line[pos];
            const auto read = line[(pos + 1) % line.size()];
            for (int ch = 0; ch < std::min (numChannels, 2); ++ch)
            {
                slot[static_cast<std::size_t> (ch)] = io[ch][i];
                io[ch][i] = read[static_cast<std::size_t> (ch)];
            }
            pos = (pos + 1) % line.size();
        }
    }
    int latencySamples() const noexcept override { return delay; }

private:
    int delay;
    std::vector<std::array<float, 2>> line;
    std::size_t pos = 0;
};

/** A processor whose state is a large opaque chunk (AT-PLG-6 blob path). */
class BigStateProcessor final : public Processor
{
public:
    void prepare (double, int) override {}
    void process (std::span<const MidiEvent>, float* const*, int, int) noexcept override {}
    std::vector<std::uint8_t> state() const override { return bytes; }
    bool setState (std::span<const std::uint8_t> b) override
    {
        bytes.assign (b.begin(), b.end());
        ++loads;
        return true;
    }
    std::vector<std::uint8_t> bytes;
    int loads = 0;
};

/** Factory for the test processors above, by uid. */
Engine::PluginFactory testPlugins (std::shared_ptr<Processor> probe = nullptr)
{
    return [probe] (const model::PluginInstance& p, bool) -> std::shared_ptr<Processor> {
        if (p.pluginUid == "test.latency")
            return std::make_shared<LatencyProcessor> (1000);
        if (p.pluginUid == "test.big")
            return std::make_shared<BigStateProcessor>();
        if (p.pluginUid == "test.probe")
            return probe;
        return nullptr;
    };
}

/** Runs a processor over a buffer pair in blocks. */
void processAll (Processor& p, std::vector<float>& l, std::vector<float>& r, int block = 256)
{
    for (std::size_t at = 0; at < l.size(); at += static_cast<std::size_t> (block))
    {
        const auto n = static_cast<int> (std::min<std::size_t> (static_cast<std::size_t> (block), l.size() - at));
        float* io[2] = { l.data() + at, r.data() + at };
        p.process ({}, io, 2, n);
    }
}

float peakOf (const std::vector<float>& v, std::size_t from = 0, std::size_t to = SIZE_MAX)
{
    float p = 0;
    for (std::size_t i = from; i < std::min (to, v.size()); ++i)
        p = std::max (p, std::abs (v[i]));
    return p;
}

/** Steady-state gain of a processor on a sine, in dB. */
double sineGainDb (Processor& p, double freq, float amplitude = 0.25f)
{
    p.prepare (rate, 256);
    auto l = sine (freq, amplitude, 96000), r = l;
    processAll (p, l, r);
    return db (peakOf (l, 72000) / amplitude);
}
} // namespace

// AT-MIX-2 ---------------------------------------------------------------------------------

TEST_CASE ("AT-MIX-2: the fader scales by its dB value, clamps at +6 dB and ramps changes", "[E][mixer][AT-MIX-2]")
{
    Rig rig;
    addMedia (rig, "b3:half", { constant (0.5f), constant (0.5f) });
    const auto t = trackPlaying (rig, "A", "b3:half");

    const auto levelAt = [&] (double volumeDb) {
        REQUIRE (rig.session.setTrackVolume (t, volumeDb));
        rig.sync();
        return playBoth (rig, 2000).first[1500];
    };
    CHECK (levelAt (0.0) == 0.5f);
    CHECK (levelAt (-6.0206) == Catch::Approx (0.25).epsilon (1e-4));
    CHECK (levelAt (minusInfinityDb) == 0.0f);
    CHECK (levelAt (12.0) == Catch::Approx (0.5 * std::pow (10.0, 6.0 / 20.0)).epsilon (1e-4)); // clamped to +6 dB
    CHECK (rig.session.model().track (t)->strip.volumeDb == 6.0);

    // A change while playing reaches its value within one buffer, without a step.
    REQUIRE (rig.session.setTrackVolume (t, 0.0));
    rig.sync();
    rig.engine.play (0);
    const int block = 256;
    auto before = rig.run (4 * block, block);
    REQUIRE (rig.session.setTrackVolume (t, -20.0));
    rig.sync();
    auto after = rig.run (4 * block, block);
    std::vector<float> joined (before);
    joined.insert (joined.end(), after.begin(), after.end());
    const float step = 0.5f - 0.05f;
    CHECK (maxDelta (joined, static_cast<std::size_t> (block) + 1) < step / static_cast<float> (block) * 1.01f);
    CHECK (after[static_cast<std::size_t> (block)] == Catch::Approx (0.05).epsilon (1e-4)); // settled after one buffer
}

// AT-MIX-3 ---------------------------------------------------------------------------------

TEST_CASE ("AT-MIX-3: a mono source pans at -3 dB; a stereo source uses balance", "[E][mixer][AT-MIX-3]")
{
    Rig rig;
    addMedia (rig, "b3:one", { constant (1.0f) });
    const auto mono = trackPlaying (rig, "Mono", "b3:one", 1);
    const auto sides = [&] (double pan) {
        REQUIRE (rig.session.setTrackPan (mono, pan));
        rig.sync();
        auto [l, r] = playBoth (rig, 1000);
        return std::pair { l[800], r[800] };
    };
    auto [cl, cr] = sides (0.0);
    CHECK (db (cl) == Catch::Approx (-3.01).margin (0.01));
    CHECK (db (cr) == Catch::Approx (-3.01).margin (0.01));
    auto [ll, lr] = sides (-1.0);
    CHECK (ll == Catch::Approx (1.0).epsilon (1e-6));
    CHECK (std::abs (lr) < 1e-7f);
    auto [rl, rr] = sides (1.0);
    CHECK (std::abs (rl) < 1e-7f);
    CHECK (rr == Catch::Approx (1.0).epsilon (1e-6));

    Rig stereoRig;
    addMedia (stereoRig, "b3:lr", { constant (0.5f), constant (0.5f) });
    const auto stereo = trackPlaying (stereoRig, "Stereo", "b3:lr");
    REQUIRE (stereoRig.session.setTrackPan (stereo, -1.0));
    stereoRig.sync();
    auto [sl, sr] = playBoth (stereoRig, 1000);
    CHECK (sl[800] == 0.5f);
    CHECK (sr[800] == 0.0f);
}

// AT-MIX-4 ---------------------------------------------------------------------------------

TEST_CASE ("AT-MIX-4: meters show the peak, latch clips until reset and hold the peak for 2 s", "[E][mixer][meters][AT-MIX-4]")
{
    Rig rig;
    const auto amp = static_cast<float> (std::pow (10.0, -6.0 / 20.0));
    addMedia (rig, "b3:sine", { sine (1000, amp), sine (1000, amp) });
    const auto t = trackPlaying (rig, "Sine", "b3:sine");
    rig.engine.takeMeter (t);
    playBoth (rig, 4800);
    const auto m = rig.engine.takeMeter (t);
    CHECK (db (m.output) == Catch::Approx (-6.0).margin (0.05));
    CHECK_FALSE (m.clip);

    // One sample of 1.1 lights the clip indicator until it's clicked.
    auto spike = constant (0.0f, 4800);
    spike[100] = 1.1f;
    addMedia (rig, "b3:spike", { spike, spike });
    const auto s = trackPlaying (rig, "Spike", "b3:spike");
    playBoth (rig, 1000);
    CHECK (rig.engine.takeMeter (s).clip);
    CHECK (rig.engine.takeMeter ({}).clip); // the master clipped too
    playBoth (rig, 50);                      // nothing over 1 now
    CHECK (rig.engine.takeMeter (s).clip);   // still lit
    rig.engine.resetClip (s);
    CHECK_FALSE (rig.engine.takeMeter (s).clip);

    // RMS (D-UI-15): a steady sine settles at amplitude / sqrt 2, 3 dB under its peak.
    addMedia (rig, "b3:long", { sine (1000, amp, 96000), sine (1000, amp, 96000) });
    const auto l = trackPlaying (rig, "Long", "b3:long");
    playBoth (rig, 72000);
    CHECK (db (rig.engine.takeMeter (l).rms) == Catch::Approx (-9.03).margin (0.1));

    // Peak hold: the highest peak shows for 2 s, then follows the level again.
    MeterBallistics meter;
    meter.update (amp, false, 0.0);
    CHECK (meter.holdDb() == Catch::Approx (-6.0).margin (0.01));
    meter.update (0.01f, false, 1.0);
    meter.update (0.01f, false, 1.9);
    CHECK (meter.holdDb() == Catch::Approx (-6.0).margin (0.01));
    meter.update (0.01f, false, 2.1);
    CHECK (meter.holdDb() < -30.0f);
    meter.update (1.2f, true, 2.2);
    meter.update (0.01f, false, 9.0);
    CHECK (meter.clip());
    meter.resetClip();
    CHECK_FALSE (meter.clip());
}

// AT-MIX-5 ---------------------------------------------------------------------------------

TEST_CASE ("AT-MIX-5: inserts process in slot order, bypass is bit-exact, and a strip holds 15", "[E][mixer][inserts][AT-MIX-5]")
{
    Rig rig;
    const auto input = sine (1000, 0.2f, barSamples);
    addMedia (rig, "b3:sine", { input, input });
    const auto t = trackPlaying (rig, "A", "b3:sine");
    const auto frames = std::int64_t { 9600 };
    const auto dry = playBoth (rig, frames).first;

    // Gain +6 dB then EQ (+6 dB at 1 kHz): output equals EQ(gain(x)).
    const auto gainId = rig.session.addInsert (t, builtin (WeftGain::pluginUid)).value();
    const auto eqId = rig.session.addInsert (t, builtin (WeftEq::pluginUid)).value();
    rig.sync();
    std::dynamic_pointer_cast<WeftGain> (rig.engine.processor (gainId))->setValue ("gain", 6.0);
    std::dynamic_pointer_cast<WeftEq> (rig.engine.processor (eqId))->setValue ("mid1Freq", 1000);
    std::dynamic_pointer_cast<WeftEq> (rig.engine.processor (eqId))->setValue ("mid1Gain", 6);
    const auto chain = playBoth (rig, frames).first;
    {
        WeftGain g;
        WeftEq e;
        g.prepare (rate, 256);
        e.prepare (rate, 256);
        g.setValue ("gain", 6.0);
        e.setValue ("mid1Freq", 1000);
        e.setValue ("mid1Gain", 6);
        auto l = std::vector<float> (input.begin(), input.begin() + frames), r = l;
        processAll (g, l, r);
        processAll (e, l, r);
        for (std::size_t i = 0; i < l.size(); i += 97)
            REQUIRE (chain[i] == Catch::Approx (l[i]).margin (1e-6));
    }

    // Order matters for a non-linear chain: gain into a compressor differs from the reverse.
    REQUIRE (rig.session.removeInsert (t, eqId));
    const auto compId = rig.session.addInsert (t, builtin (WeftCompressor::pluginUid)).value();
    rig.sync();
    const auto reference = [&] (bool gainFirst) {
        WeftGain g;
        WeftCompressor c;
        g.prepare (rate, 256);
        c.prepare (rate, 256);
        g.setValue ("gain", 6.0);
        auto l = std::vector<float> (input.begin(), input.begin() + frames), r = l;
        if (gainFirst)
            processAll (g, l, r), processAll (c, l, r);
        else
            processAll (c, l, r), processAll (g, l, r);
        return l;
    };
    const auto gainThenComp = reference (true), compThenGain = reference (false);
    REQUIRE (std::abs (gainThenComp[9012] - compThenGain[9012]) > 0.01f);
    rig.engine.processor (compId)->prepare (rate, 256); // fresh detector state, as in the reference
    auto out = playBoth (rig, frames).first;
    CHECK (out[9012] == Catch::Approx (gainThenComp[9012]).margin (1e-5));
    REQUIRE (rig.session.moveInsert (t, compId, 0));
    rig.sync();
    rig.engine.processor (compId)->prepare (rate, 256);
    out = playBoth (rig, frames).first;
    CHECK (out[9012] == Catch::Approx (compThenGain[9012]).margin (1e-5));

    // Bypass passes audio unchanged, bit for bit; removing restores the dry signal.
    REQUIRE (rig.session.setInsertBypass (t, compId, true));
    REQUIRE (rig.session.setInsertBypass (t, gainId, true));
    rig.sync();
    CHECK (playBoth (rig, frames).first == dry);
    REQUIRE (rig.session.removeInsert (t, compId));
    REQUIRE (rig.session.removeInsert (t, gainId));
    rig.sync();
    CHECK (playBoth (rig, frames).first == dry);

    // At most 15 inserts.
    for (int i = 0; i < 15; ++i)
        REQUIRE (rig.session.addInsert (t, builtin (WeftGain::pluginUid)));
    CHECK_FALSE (rig.session.addInsert (t, builtin (WeftGain::pluginUid)));
    CHECK (rig.session.lastError().find ("15") != std::string::npos);
}

// AT-MIX-6 ---------------------------------------------------------------------------------

TEST_CASE ("AT-MIX-6: the instrument slot hosts Weft Keys or another instrument; replacing it keeps regions and inserts", "[E][mixer][AT-MIX-6]")
{
    Rig rig;
    auto probe = std::make_shared<ProbeInstrument>();
    rig.engine.setPluginFactory (testPlugins (probe));
    const auto keys = rig.session.createTrack (TrackKind::instrument, "Keys").value();
    const auto region = rig.session.createMidiRegion (keys, 0, barTicks).value();
    model::Note n;
    n.pos = 0;
    n.length = 960;
    n.pitch = 60;
    n.velocity = 127;
    REQUIRE (rig.session.addNote (region, n));
    const auto insert = rig.session.addInsert (keys, builtin (WeftGain::pluginUid)).value();
    rig.sync();
    REQUIRE (dynamic_cast<WeftKeys*> (&rig.rack.find (keys)->instrument()) != nullptr);
    CHECK (peakOf (playBoth (rig, 4800).first) > 0.05f);

    REQUIRE (rig.session.setInstrument (keys, hosted ("test.probe")));
    rig.sync();
    CHECK (&rig.rack.find (keys)->instrument() == probe.get());
    const auto out = playBoth (rig, 4800).first;
    CHECK (out[1000] == Catch::Approx (0.1).epsilon (1e-4)); // the probe plays 0.1 per held note at velocity 127
    CHECK (rig.session.model().regionsOnTrack (keys).size() == 1);
    REQUIRE (rig.session.model().track (keys)->strip.inserts.size() == 1);
    CHECK (rig.session.model().track (keys)->strip.inserts[0].id == insert);
}

// AT-MIX-7 ---------------------------------------------------------------------------------

TEST_CASE ("AT-MIX-7: post sends follow the fader, pre sends don't; limits on sends", "[E][mixer][sends][AT-MIX-7]")
{
    Rig rig;
    addMedia (rig, "b3:half", { constant (0.5f), constant (0.5f) });
    const auto t = trackPlaying (rig, "A", "b3:half");
    const auto bus = rig.session.newBus (t, true).value();
    CHECK (rig.session.model().track (bus)->name == "Bus 1");
    const auto sendId = rig.session.model().track (t)->strip.sends.at (0).id;
    REQUIRE (rig.session.setTrackVolume (t, -6.0206));
    rig.sync();

    const auto busPeak = [&] {
        rig.engine.takeMeter (bus);
        playBoth (rig, 2000);
        return rig.engine.takeMeter (bus).output;
    };
    CHECK (busPeak() == Catch::Approx (0.25).epsilon (1e-4)); // post-fader: -6 dB
    REQUIRE (rig.session.setSendPre (t, sendId, true));
    rig.sync();
    CHECK (busPeak() == Catch::Approx (0.5).epsilon (1e-4)); // pre-fader: 0 dB whatever the fader
    REQUIRE (rig.session.setSendLevel (t, sendId, minusInfinityDb));
    rig.sync();
    CHECK (busPeak() == 0.0f);

    // A second send to the same bus is refused; so is a ninth send.
    CHECK_FALSE (rig.session.addSend (t, bus));
    std::vector<Id> buses;
    for (int i = 0; i < 7; ++i)
        REQUIRE (rig.session.newBus (t, true));
    CHECK (rig.session.model().track (t)->strip.sends.size() == 8);
    const auto extra = rig.session.createTrack (TrackKind::bus, "").value();
    CHECK_FALSE (rig.session.addSend (t, extra));
    CHECK_FALSE (rig.session.newBus (t, true));
}

// AT-MIX-11 --------------------------------------------------------------------------------

TEST_CASE ("AT-MIX-11: outputs sum at the master; the master fader and inserts act on the whole mix", "[E][mixer][master][AT-MIX-11]")
{
    Rig rig;
    addMedia (rig, "b3:a", { constant (0.25f), constant (0.25f) });
    addMedia (rig, "b3:b", { constant (0.125f), constant (0.125f) });
    trackPlaying (rig, "A", "b3:a");
    trackPlaying (rig, "B", "b3:b");
    OfflineRenderOptions options;
    options.frames = 4800;
    const auto bounce = [&] { return renderOffline (rig.session.model(), rig.media, options).channels[0][4000]; };
    CHECK (bounce() == 0.375f);

    const auto master = rig.session.model().info.masterTrackId;
    REQUIRE (rig.session.setTrackVolume (master, -6.0206));
    CHECK (bounce() == Catch::Approx (0.1875).epsilon (1e-4));

    REQUIRE (rig.session.setTrackVolume (master, 0.0));
    auto gain = builtin (WeftGain::pluginUid);
    WeftGain g;
    g.setValue ("gain", -6.0206);
    gain.state = Value (Value::Bytes (g.state()));
    REQUIRE (rig.session.addInsert (master, gain));
    CHECK (bounce() == Catch::Approx (0.1875).epsilon (1e-4));
}

// AT-MIX-12 --------------------------------------------------------------------------------

TEST_CASE ("AT-MIX-12: delay compensation aligns paths with and without plug-in latency", "[E][mixer][pdc][AT-MIX-12]")
{
    Rig rig;
    rig.engine.setPluginFactory (testPlugins());
    auto impulse = constant (0.0f, 4800);
    impulse[0] = 0.5f;
    auto quieter = impulse;
    quieter[0] = 0.25f;
    addMedia (rig, "b3:imp", { impulse, impulse });
    addMedia (rig, "b3:imp2", { quieter, quieter });
    const auto a = trackPlaying (rig, "A", "b3:imp", 2, barTicks); // impulses at bar 2
    trackPlaying (rig, "B", "b3:imp2", 2, barTicks);
    const auto latency = rig.session.addInsert (a, hosted ("test.latency")).value();
    rig.sync();
    CHECK (rig.engine.compensationLatency() == 1000);

    const auto impulsesIn = [] (const std::vector<float>& v) {
        std::vector<std::pair<std::int64_t, float>> hits;
        for (std::size_t i = 0; i < v.size(); ++i)
            if (v[i] != 0.0f)
                hits.emplace_back (static_cast<std::int64_t> (i), v[i]);
        return hits;
    };
    auto hits = impulsesIn (playBoth (rig, barSamples + 4000).first);
    REQUIRE (hits.size() == 1);
    CHECK (hits[0].first == barSamples + 1000); // both arrive together, 1,000 samples late
    CHECK (hits[0].second == 0.75f);

    // A send from A to a bus is aligned with B as well.
    const auto bus = rig.session.newBus (a, true).value();
    rig.sync();
    (void) bus;
    hits = impulsesIn (playBoth (rig, barSamples + 4000).first);
    REQUIRE (hits.size() == 1);
    CHECK (hits[0].first == barSamples + 1000);
    CHECK (hits[0].second == 1.25f);

    // Without the plug-in everything is aligned again with no delay.
    REQUIRE (rig.session.removeInsert (a, latency));
    rig.sync();
    CHECK (rig.engine.compensationLatency() == 0);
    playBoth (rig, 2000); // let the compensation crossfade finish
    hits = impulsesIn (playBoth (rig, barSamples + 4000).first);
    REQUIRE (hits.size() == 1);
    CHECK (hits[0].first == barSamples);
    CHECK (hits[0].second == 1.25f);
}

// AT-MIX-13 --------------------------------------------------------------------------------

TEST_CASE ("AT-MIX-13: changing compensation while playing crossfades instead of jumping", "[E][mixer][pdc][AT-MIX-13]")
{
    Rig rig;
    rig.engine.setPluginFactory (testPlugins());
    addMedia (rig, "b3:sine", { sine (1000, 0.5f, 4 * barSamples), sine (1000, 0.5f, 4 * barSamples) });
    const auto silent = rig.session.createTrack (TrackKind::audio, "Latent", 2).value();
    trackPlaying (rig, "Sine", "b3:sine");
    const auto plugin = rig.session.addInsert (silent, hosted ("test.latency")).value();
    rig.sync();

    const int block = 256;
    rig.engine.play (0);
    auto out = rig.run (20 * block, block);
    REQUIRE (rig.session.removeInsert (silent, plugin));
    rig.sync();
    const auto after = rig.run (20 * block, block);
    out.insert (out.end(), after.begin(), after.end());
    // A sine's own largest step, plus what a 10 ms crossfade between two positions can add.
    const float natural = static_cast<float> (2.0 * std::numbers::pi * 1000.0 / rate * 0.5);
    const float crossfade = 1.0f / 480.0f;
    CHECK (maxDelta (out, 2000) <= natural + crossfade);
    // Without a crossfade the 1,000-sample jump would leap by up to the full swing.
    CHECK (rig.engine.compensationLatency() == 0);
}

// AT-PLG-6 ---------------------------------------------------------------------------------

TEST_CASE ("AT-PLG-6: plug-in state round-trips through save and load; big state goes to a blob", "[E][plugins][state][AT-PLG-6]")
{
    TempFolder tmp;
    project::ProjectFolder folder (tmp.path / "State.weft");
    REQUIRE (folder.create());
    const auto blobs = tmp.path / "State.weft" / "Blobs";
    Id track, gainId, bigId;
    std::vector<std::uint8_t> big (100 * 1024);
    for (std::size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<std::uint8_t> ((i * 31) % 251);
    {
        Rig rig;
        rig.engine.setProjectFolder (tmp.path / "State.weft");
        rig.engine.setPluginFactory (testPlugins());
        track = rig.session.createTrack (TrackKind::audio, "A", 2).value();
        gainId = rig.session.addInsert (track, builtin (WeftGain::pluginUid)).value();
        bigId = rig.session.addInsert (track, hosted ("test.big")).value();
        rig.sync();
        // The user sets +3.5 dB; the client writes the state chunk (D-DM-5).
        auto gain = rig.engine.processor (gainId);
        std::dynamic_pointer_cast<WeftGain> (gain)->setValue ("gain", 3.5);
        REQUIRE (rig.session.setPluginState (track, gainId, *media::pluginStateValue (gain->state(), blobs)));
        const auto stateValue = media::pluginStateValue (big, blobs).value();
        CHECK (stateValue.isString()); // over 64 KB: a blob hash
        CHECK (std::filesystem::exists (media::blobFile (blobs, stateValue.asString())));
        REQUIRE (rig.session.setPluginState (track, bigId, stateValue));
        REQUIRE (folder.save (rig.session.document()));
    }

    auto opened = folder.open();
    REQUIRE (opened.document);
    commands::Session reloaded (std::move (*opened.document), "userA");
    Rig rig;
    rig.engine.setProjectFolder (tmp.path / "State.weft");
    rig.engine.setPluginFactory (testPlugins());
    rig.engine.setProject (reloaded.model());
    auto gain = std::dynamic_pointer_cast<WeftGain> (rig.engine.processor (gainId));
    REQUIRE (gain);
    CHECK (gain->value (0) == Catch::Approx (3.5).margin (1e-9));
    auto restored = std::dynamic_pointer_cast<BigStateProcessor> (rig.engine.processor (bigId));
    REQUIRE (restored);
    CHECK (restored->bytes == big);
    CHECK (restored->loads == 1);
}

// AT-PLG-7 ---------------------------------------------------------------------------------

TEST_CASE ("AT-PLG-7: Weft Keys presets sound, 16 voices steal the oldest, 8 loud notes stay under 0 dBFS", "[E][plugins][keys][AT-PLG-7]")
{
    const auto render = [] (WeftKeys& keys, std::vector<MidiEvent> events, int frames) {
        std::vector<float> l (static_cast<std::size_t> (frames)), r (l.size());
        float* io[2] = { l.data(), r.data() };
        keys.process (events, io, 2, frames);
        return l;
    };
    for (std::size_t preset = 0; preset < WeftKeys::presets().size(); ++preset)
    {
        WeftKeys keys;
        keys.prepare (rate, 48000);
        keys.setValue ("preset", static_cast<double> (preset));
        const auto out = render (keys, { MidiEvent::noteOn (0, 1, 60, 100) }, 48000);
        INFO (WeftKeys::presets()[preset].name);
        CHECK (peakOf (out) > 0.01f);
    }

    WeftKeys keys;
    keys.prepare (rate, 4800);
    std::vector<MidiEvent> notes;
    for (int i = 0; i < 17; ++i)
        notes.push_back (MidiEvent::noteOn (i, 1, 40 + i, 100));
    render (keys, notes, 4800);
    CHECK (keys.activeVoices() == 16);
    CHECK_FALSE (keys.sounding (1, 40)); // the oldest was stolen
    CHECK (keys.sounding (1, 56));

    for (std::size_t preset = 0; preset < WeftKeys::presets().size(); ++preset)
    {
        WeftKeys loud;
        loud.prepare (rate, 48000);
        loud.setValue ("preset", static_cast<double> (preset));
        loud.setValue ("level", 6.0);
        std::vector<MidiEvent> chord;
        for (const int p : { 36, 43, 48, 52, 55, 60, 64, 67 })
            chord.push_back (MidiEvent::noteOn (0, 1, p, 127));
        CHECK (peakOf (render (loud, chord, 48000)) < 1.0f);
    }
}

// AT-PLG-8 ---------------------------------------------------------------------------------

TEST_CASE ("AT-PLG-8: built-in EQ, compressor, delay, reverb and gain measure as specified", "[E][plugins][builtin][AT-PLG-8]")
{
    {
        WeftEq eq;
        eq.setValue ("mid1Freq", 1000);
        eq.setValue ("mid1Gain", 6);
        eq.setValue ("mid1Q", 1);
        CHECK (sineGainDb (eq, 1000) == Catch::Approx (6.0).margin (0.2));
        CHECK (std::abs (sineGainDb (eq, 100)) < 0.5);
    }
    {
        WeftEq eq;
        eq.setValue ("hpOn", 1);
        eq.setValue ("hpFreq", 100);
        CHECK (sineGainDb (eq, 50) <= -9.0);
    }
    {
        WeftCompressor comp;
        comp.setValue ("ratio", 4);
        comp.setValue ("threshold", -20);
        const auto amp = static_cast<float> (std::pow (10.0, -8.0 / 20.0));
        CHECK (db (amp) + sineGainDb (comp, 1000, amp) == Catch::Approx (-17.0).margin (0.5));
    }
    {
        WeftDelay delay;
        delay.prepare (rate, 256);
        delay.setValue ("time", 500);
        delay.setValue ("feedback", 0);
        delay.setValue ("mix", 100);
        std::vector<float> l (30000), r (30000);
        l[0] = r[0] = 1.0f;
        processAll (delay, l, r);
        std::vector<std::size_t> hits;
        for (std::size_t i = 0; i < l.size(); ++i)
            if (l[i] != 0.0f)
                hits.push_back (i);
        REQUIRE (hits.size() == 1);
        CHECK (hits[0] == 24000);
    }
    {
        // Reverb: the impulse response falls below -60 dB (relative to its loudest 10 ms) within the decay time ± 20%.
        WeftReverb reverb;
        reverb.prepare (rate, 256);
        reverb.setValue ("decay", 2.0);
        reverb.setValue ("damping", 0);
        reverb.setValue ("preDelay", 0);
        reverb.setValue ("mix", 100);
        std::vector<float> l (static_cast<std::size_t> (4 * rate)), r (l.size());
        l[0] = r[0] = 1.0f;
        processAll (reverb, l, r);
        const std::size_t window = 480;
        std::vector<double> rms;
        for (std::size_t at = 0; at + window <= l.size(); at += window)
        {
            double sum = 0;
            for (std::size_t i = at; i < at + window; ++i)
                sum += static_cast<double> (l[i]) * l[i];
            rms.push_back (std::sqrt (sum / window));
        }
        const double loudest = *std::max_element (rms.begin(), rms.end());
        std::size_t last = 0;
        for (std::size_t i = 0; i < rms.size(); ++i)
            if (rms[i] > 0 && db (rms[i] / loudest) > -60.0)
                last = i;
        const double seconds = static_cast<double> ((last + 1) * window) / rate;
        CHECK (seconds == Catch::Approx (2.0).margin (0.4));
    }
    {
        WeftGain gain;
        gain.setValue ("gain", 6.0206);
        CHECK (sineGainDb (gain, 440) == Catch::Approx (6.0206).margin (0.001));
        gain.prepare (rate, 1);
        float v = 0.25f, w = 0.25f;
        float* io[2] = { &v, &w };
        gain.process ({}, io, 2, 1);
        CHECK (v == Catch::Approx (0.5).epsilon (1e-5));
    }
}

// AT-PLG-9 ---------------------------------------------------------------------------------

TEST_CASE ("AT-PLG-9: built-in effects report 0 latency and stay finite and fast after 60 s of silence", "[E][plugins][builtin][AT-PLG-9]")
{
    for (const auto& info : builtinPlugins())
    {
        if (info.instrument)
            continue;
        INFO (info.name);
        auto p = createBuiltin (info.uid);
        REQUIRE (p);
        p->prepare (rate, 512);
        CHECK (p->latencySamples() == 0);
        if (auto* d = dynamic_cast<WeftDelay*> (p.get()))
            d->setValue ("feedback", 95);
        std::vector<float> l (512), r (512);
        float* io[2] = { l.data(), r.data() };
        // A short burst first so feedback paths decay toward the denormal range during the silence.
        std::fill (l.begin(), l.end(), 0.5f);
        std::fill (r.begin(), r.end(), 0.5f);
        p->process ({}, io, 2, 512);
        const auto started = std::chrono::steady_clock::now();
        const int silentBlocks = static_cast<int> (60 * rate / 512);
        for (int b = 0; b < silentBlocks; ++b)
        {
            std::fill (l.begin(), l.end(), 0.0f);
            std::fill (r.begin(), r.end(), 0.0f);
            p->process ({}, io, 2, 512);
        }
        const std::chrono::duration<double> silentTime = std::chrono::steady_clock::now() - started;
        CHECK (silentTime.count() < 6.0); // ten times faster than real time, even unoptimised
        std::mt19937 noise (7);
        std::uniform_real_distribution<float> uniform (-1.0f, 1.0f);
        bool finite = true;
        for (int b = 0; b < 100; ++b)
        {
            for (std::size_t i = 0; i < 512; ++i)
                l[i] = uniform (noise), r[i] = uniform (noise);
            p->process ({}, io, 2, 512);
            for (std::size_t i = 0; i < 512; ++i)
                finite = finite && std::isfinite (l[i]) && std::isfinite (r[i]);
        }
        CHECK (finite);
    }
}

// AT-PLG-11 --------------------------------------------------------------------------------

TEST_CASE ("AT-PLG-11: a plug-in that isn't installed becomes a placeholder that passes audio and keeps its state", "[E][plugins][placeholder][AT-PLG-11]")
{
    Rig rig;
    const auto input = sine (440, 0.3f);
    addMedia (rig, "b3:sine", { input, input });
    const auto t = trackPlaying (rig, "A", "b3:sine");
    const auto dry = playBoth (rig, 4800).first;

    auto missing = hosted ("0123456789ABCDEF0123456789ABCDEF");
    missing.name = "Missing Verb";
    const Value::Bytes original { 1, 2, 3, 250, 0, 7 };
    missing.state = Value (original);
    missing.params["gain"] = 0.25;
    const auto id = rig.session.addInsert (t, missing).value();
    rig.sync();
    REQUIRE (rig.engine.processor (id)->isPlaceholder());
    CHECK (playBoth (rig, 4800).first == dry);

    TempFolder tmp;
    project::ProjectFolder folder (tmp.path / "Missing.weft");
    REQUIRE (folder.create());
    REQUIRE (folder.save (rig.session.document()));
    auto opened = folder.open();
    REQUIRE (opened.document);
    commands::Session reloaded (std::move (*opened.document), "userA");
    const auto& plugin = reloaded.model().track (t)->strip.inserts.at (0);
    CHECK (plugin.name == "Missing Verb");
    CHECK (plugin.state.asBytes() == original);
    CHECK (plugin.params.at ("gain") == 0.25);
}
