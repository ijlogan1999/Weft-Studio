// VST3 hosting acceptance tests with the fixture plug-ins (spec 06 §10): AT-PLG-2, 3, 6 and
// the fixture versions of AT-MIX-5, 6 and 12. They host through Weft's own VST3 host and
// scanner, the way the app does.

#include "../engine/EngineFixtures.h"

#include "weft/engine/dsp/BuiltinEffects.h"
#include "weft/engine/plugins/HostedPlugin.h"
#include "weft/engine/plugins/PluginCatalog.h"
#include "weft/engine/plugins/Vst3Host.h"
#include "weft/media/Blobs.h"
#include "weft/project/ProjectFolder.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace weft;
using namespace weft::test;
using doc::Value;

namespace
{
constexpr double rate = 48000.0;
constexpr int block = 512;

/** The fixtures copied into one folder, as a user's VST3 folder would hold them. */
struct FixtureFolder
{
    TempFolder tmp;
    std::filesystem::path path = tmp.path / "VST3";
    explicit FixtureFolder (std::initializer_list<const char*> bundles)
    {
        std::filesystem::create_directories (path);
        for (const auto* b : bundles)
        {
            const std::filesystem::path from (b);
            std::filesystem::copy (from, path / from.filename(), std::filesystem::copy_options::recursive);
        }
    }
};

/** A catalog with the working fixtures scanned in. */
struct Hosting
{
    FixtureFolder folder { WEFT_FIXTURE_TESTGAIN, WEFT_FIXTURE_TESTLATENCY, WEFT_FIXTURE_TESTSYNTH };
    PluginCatalog catalog;
    Vst3Host host { catalog };
    Hosting()
    {
        PluginCatalog::ScanOptions options;
        options.folders = { folder.path };
        options.scanner = WEFT_SCANNER;
        const auto result = catalog.scan (options);
        REQUIRE (result.found == 3);
    }
    model::PluginInstance instance (const std::string& name) const
    {
        for (const auto& p : catalog.plugins())
            if (p.name == name)
            {
                model::PluginInstance i;
                i.format = "vst3";
                i.pluginUid = p.uid;
                i.name = p.name;
                i.vendor = p.vendor;
                return i;
            }
        FAIL ("not scanned: " << name);
        return {};
    }
    Engine::PluginFactory factory()
    {
        return [this] (const model::PluginInstance& p, bool instrument) { return host.create (p, instrument); };
    }
};

/** A hosted parameter by its display name (VST3 parameter ids are numbers). */
int paramNamed (const Processor& p, const std::string& name)
{
    const auto params = p.parameters();
    for (std::size_t i = 0; i < params.size(); ++i)
        if (params[i].name == name)
            return static_cast<int> (i);
    return -1;
}

std::vector<float> run (Processor& p, std::vector<MidiEvent> events, float fill, int frames = block)
{
    std::vector<float> l (static_cast<std::size_t> (frames), fill), r (l);
    float* io[2] = { l.data(), r.data() };
    p.process (events, io, 2, frames);
    return l;
}

void addMedia (Rig& rig, const std::string& hash, std::vector<float> samples)
{
    media::AudioData a;
    a.sampleRate = 48000;
    a.channels = { samples, samples };
    rig.media.add (hash, a);
    model::MediaRef m;
    m.hash = hash;
    m.originalName = hash + ".wav";
    m.lengthSamples = static_cast<std::int64_t> (samples.size());
    REQUIRE (rig.session.addMedia (m));
}
} // namespace

TEST_CASE ("AT-PLG-2: TestGain applies its gain parameter and TestSynth responds to MIDI through the VST3 host", "[E][plugins][vst3][AT-PLG-2]")
{
    PluginCatalog empty;
    Vst3Host host (empty);
    auto gain = host.createFromFile (WEFT_FIXTURE_TESTGAIN, "TestGain");
    REQUIRE (gain);
    gain->prepare (rate, block);
    const auto index = paramNamed (*gain, "Gain");
    REQUIRE (index >= 0);
    CHECK_FALSE (gain->parameters()[static_cast<std::size_t> (index)].id.empty());
    CHECK (run (*gain, {}, 0.25f)[100] == Catch::Approx (0.25).epsilon (1e-5)); // 0 dB default

    // A change made between blocks applies from the first sample of the next block (D-W6-5).
    gain->setParameter (index, (6.0 + 60.0) / 72.0);
    const auto out = run (*gain, {}, 0.25f);
    CHECK (out[0] == Catch::Approx (0.25 * std::pow (10.0, 6.0 / 20.0)).epsilon (1e-4));
    CHECK (gain->parameterText (index, gain->parameter (index)).find ("6") != std::string::npos);

    auto synth = host.createFromFile (WEFT_FIXTURE_TESTSYNTH, "TestSynth");
    REQUIRE (synth);
    synth->prepare (rate, block);
    CHECK (run (*synth, {}, 0.0f) == std::vector<float> (block, 0.0f));
    const auto first = run (*synth, { MidiEvent::noteOn (100, 1, 69, 64) }, 0.0f);
    CHECK (first[99] == 0.0f); // nothing before the note's offset
    CHECK (first[101] != 0.0f);
    float peak = 0;
    for (int b = 0; b < 10; ++b)
        for (const auto v : run (*synth, {}, 0.0f))
            peak = std::max (peak, std::abs (v));
    CHECK (peak == Catch::Approx (64.0 / 127.0).epsilon (0.005));
}

TEST_CASE ("AT-PLG-3: scanning lists working plug-ins and marks one that crashes as failed", "[E][plugins][scan][AT-PLG-3]")
{
    FixtureFolder folder { WEFT_FIXTURE_TESTGAIN, WEFT_FIXTURE_TESTSYNTH, WEFT_FIXTURE_TESTCRASHSCAN };
    TempFolder settings;
    PluginCatalog catalog (settings.path / "plugins.json");
    PluginCatalog::ScanOptions options;
    options.folders = { folder.path };
    options.scanner = WEFT_SCANNER;
    auto result = catalog.scan (options);
    CHECK (result.scanned == 3);
    CHECK (result.found == 2);
    CHECK (result.failed == 1);

    std::map<std::string, PluginInfo> byName;
    for (const auto& p : catalog.plugins())
        byName[p.name] = p;
    REQUIRE (byName.contains ("TestGain"));
    REQUIRE (byName.contains ("TestSynth"));
    REQUIRE (byName.contains ("TestCrashScan"));
    CHECK (byName["TestGain"].status == PluginStatus::ok);
    CHECK (byName["TestGain"].uid.size() == 32);
    CHECK (byName["TestGain"].vendor == "Weft Test Fixtures");
    CHECK_FALSE (byName["TestGain"].instrument);
    CHECK (byName["TestSynth"].instrument);
    CHECK (byName["TestCrashScan"].status == PluginStatus::failed);
    INFO (byName["TestCrashScan"].error);
    CHECK (byName["TestCrashScan"].error.find ("crashed") != std::string::npos);
    CHECK (catalog.menu (false).size() == 1);
    CHECK (catalog.menu (true).size() == 1);

    // The catalog persists; a rescan skips everything known, including the failed plug-in.
    PluginCatalog reloaded (settings.path / "plugins.json");
    REQUIRE (reloaded.load());
    CHECK (reloaded.plugins().size() == 3);
    result = reloaded.scan (options);
    CHECK (result.scanned == 0);
    CHECK (result.skipped == 3);

    // "Retry failed" scans it again (and it fails again).
    options.retryFailed = true;
    result = reloaded.scan (options);
    CHECK (result.scanned == 1);
    CHECK (result.failed == 1);

    // Disabling hides a plug-in from the menus and survives a rescan of its file.
    reloaded.setEnabled (byName["TestGain"].uid, false);
    CHECK (reloaded.menu (false).empty());
    std::filesystem::last_write_time (folder.path / "TestGain.vst3", std::filesystem::file_time_type::clock::now());
    reloaded.scan (options);
    REQUIRE (reloaded.find (byName["TestGain"].uid) != nullptr);
    CHECK (reloaded.find (byName["TestGain"].uid)->status == PluginStatus::disabled);
}

TEST_CASE ("AT-PLG-6: TestGain at +3.5 dB survives save and reload", "[E][plugins][state][AT-PLG-6]")
{
    Hosting hosting;
    TempFolder tmp;
    const auto projectDir = tmp.path / "Gain.weft";
    project::ProjectFolder folder (projectDir);
    REQUIRE (folder.create());
    Id track, pluginId;
    {
        Rig rig;
        rig.engine.setPluginFactory (hosting.factory());
        rig.engine.setProjectFolder (projectDir);
        track = rig.session.createTrack (TrackKind::audio, "A", 2).value();
        pluginId = rig.session.addInsert (track, hosting.instance ("TestGain")).value();
        rig.sync();
        auto plugin = rig.engine.processor (pluginId);
        REQUIRE (plugin);
        REQUIRE_FALSE (plugin->isPlaceholder());
        const auto index = paramNamed (*plugin, "Gain");
        plugin->setParameter (index, (3.5 + 60.0) / 72.0);
        REQUIRE (rig.session.setPluginState (track, pluginId, *media::pluginStateValue (plugin->state(), projectDir / "Blobs")));
        REQUIRE (rig.session.setPluginParam (track, pluginId, plugin->parameters()[static_cast<std::size_t> (index)].id, plugin->parameter (index)));
        REQUIRE (folder.save (rig.session.document()));
    }
    auto opened = folder.open();
    REQUIRE (opened.document);
    commands::Session reloaded (std::move (*opened.document), "userA");
    Rig rig;
    rig.engine.setPluginFactory (hosting.factory());
    rig.engine.setProjectFolder (projectDir);
    rig.engine.setProject (reloaded.model());
    auto plugin = rig.engine.processor (pluginId);
    REQUIRE (plugin);
    CHECK (plugin->parameter (paramNamed (*plugin, "Gain")) * 72.0 - 60.0 == Catch::Approx (3.5).margin (1e-4));
}

TEST_CASE ("AT-MIX-5 with TestGain: a hosted insert then a built-in EQ", "[E][mixer][inserts][vst3][AT-MIX-5]")
{
    Hosting hosting;
    Rig rig;
    rig.engine.setPluginFactory (hosting.factory());
    std::vector<float> input (static_cast<std::size_t> (barSamples));
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = 0.2f * static_cast<float> (std::sin (2.0 * std::numbers::pi * 1000.0 * static_cast<double> (i) / rate));
    addMedia (rig, "b3:sine", input);
    const auto t = rig.session.createTrack (TrackKind::audio, "A", 2).value();
    REQUIRE (rig.session.createAudioRegion (t, 0, "b3:sine", 0, barSamples));
    auto gain = hosting.instance ("TestGain");
    {
        auto probe = hosting.host.create (gain, false);
        REQUIRE (probe);
        gain.params[probe->parameters()[static_cast<std::size_t> (paramNamed (*probe, "Gain"))].id] = (6.0 + 60.0) / 72.0;
    }
    REQUIRE (rig.session.addInsert (t, gain));
    model::PluginInstance eq;
    eq.pluginUid = WeftEq::pluginUid;
    const auto eqId = rig.session.addInsert (t, eq).value();
    rig.sync();
    std::dynamic_pointer_cast<WeftEq> (rig.engine.processor (eqId))->setValue ("mid1Freq", 1000);
    std::dynamic_pointer_cast<WeftEq> (rig.engine.processor (eqId))->setValue ("mid1Gain", 6);

    rig.engine.play (0);
    const auto out = rig.run (9600, 256);
    WeftEq reference;
    reference.prepare (rate, 256);
    reference.setValue ("mid1Freq", 1000);
    reference.setValue ("mid1Gain", 6);
    std::vector<float> l (input.begin(), input.begin() + 9600), r;
    for (auto& v : l)
        v *= static_cast<float> (std::pow (10.0, 6.0 / 20.0));
    r = l;
    for (std::size_t at = 0; at < l.size(); at += 256)
    {
        float* io[2] = { l.data() + at, r.data() + at };
        reference.process ({}, io, 2, static_cast<int> (std::min<std::size_t> (256, l.size() - at)));
    }
    for (std::size_t i = 0; i < l.size(); i += 101)
        REQUIRE (out[i] == Catch::Approx (l[i]).margin (1e-5));
}

TEST_CASE ("AT-MIX-6 with TestSynth: the instrument slot hosts a VST3 instrument", "[E][mixer][vst3][AT-MIX-6]")
{
    Hosting hosting;
    Rig rig;
    rig.engine.setPluginFactory (hosting.factory());
    const auto keys = rig.session.createTrack (TrackKind::instrument, "Synth").value();
    const auto region = rig.session.createMidiRegion (keys, 0, barTicks).value();
    model::Note n;
    n.pos = 0;
    n.length = 960;
    n.pitch = 69;
    n.velocity = 64;
    REQUIRE (rig.session.addNote (region, n));
    REQUIRE (rig.session.setInstrument (keys, hosting.instance ("TestSynth")));
    rig.sync();
    REQUIRE (dynamic_cast<HostedPlugin*> (&rig.rack.find (keys)->instrument()) != nullptr);
    rig.engine.play (0);
    const auto out = rig.run (4800);
    float peak = 0;
    for (const auto v : out)
        peak = std::max (peak, std::abs (v));
    CHECK (peak == Catch::Approx (64.0 / 127.0).epsilon (0.01)); // stereo instrument on a stereo channel: unity
    CHECK (rig.session.model().regionsOnTrack (keys).size() == 1);
}

TEST_CASE ("AT-MIX-12 with TestLatency: impulses on two tracks reach the master together", "[E][mixer][pdc][vst3][AT-MIX-12]")
{
    Hosting hosting;
    Rig rig;
    rig.engine.setPluginFactory (hosting.factory());
    std::vector<float> a (4800, 0.0f), b (4800, 0.0f);
    a[0] = 0.5f;
    b[0] = 0.25f;
    addMedia (rig, "b3:a", a);
    addMedia (rig, "b3:b", b);
    const auto ta = rig.session.createTrack (TrackKind::audio, "A", 2).value();
    const auto tb = rig.session.createTrack (TrackKind::audio, "B", 2).value();
    REQUIRE (rig.session.createAudioRegion (ta, barTicks, "b3:a", 0, 4800));
    REQUIRE (rig.session.createAudioRegion (tb, barTicks, "b3:b", 0, 4800));
    REQUIRE (rig.session.addInsert (ta, hosting.instance ("TestLatency")));
    rig.sync();
    CHECK (rig.engine.compensationLatency() == 1000);
    rig.engine.play (0);
    const auto out = rig.run (barSamples + 4000, 256);
    std::vector<std::pair<std::int64_t, float>> hits;
    for (std::size_t i = 0; i < out.size(); ++i)
        if (std::abs (out[i]) > 1e-6f)
            hits.emplace_back (static_cast<std::int64_t> (i), out[i]);
    REQUIRE (hits.size() == 1);
    CHECK (hits[0].first == barSamples + 1000);
    CHECK (hits[0].second == Catch::Approx (0.75).epsilon (1e-5));
}
