// weft-cli: headless driver for engine, project and sync tests.

#include "weft/commands/Session.h"
#include "weft/core/Version.h"
#include "weft/engine/devices/NullDevice.h"
#include "weft/engine/transport/Engine.h"
#include "weft/media/WavFile.h"
#include "weft/time/Position.h"
#include "weft/project/ProjectFolder.h"

#include <chrono>
#include <cmath>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numbers>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
class TestTone final : public weft::engine::AudioCallback
{
public:
    void prepare (const weft::engine::DeviceConfig& config) override { sampleRate = config.sampleRate; }

    void process (const float* const*, float* const* outputs, int numFrames) noexcept override
    {
        for (int i = 0; i < numFrames; ++i)
        {
            const auto sample = static_cast<float> (0.25 * std::sin (phase));
            phase += 2.0 * std::numbers::pi * 440.0 / sampleRate;
            outputs[0][i] = sample;
            peak = std::max (peak, std::abs (sample));
        }
    }

    double sampleRate = 48000.0;
    double phase = 0.0;
    float peak = 0.0f;
};

int usage()
{
    std::fputs ("usage: weft-cli version\n"
                "       weft-cli null-render [seconds] [sample-rate] [block-size]\n"
                "       weft-cli new <parent-dir> <name> [sample-rate]\n"
                "       weft-cli demo-edits <project.weft> [--dump <file.json>] [--hold]\n"
                "       weft-cli open <project.weft> [--recover | --discard] [--dump <file.json>]\n"
                "       weft-cli render <project.weft> <out.wav> [--from <bar beat>] [--seconds <s>] [--metronome]\n",
                stderr);
    return 2;
}

using weft::commands::Session;
using weft::project::ProjectFolder;
namespace fs = std::filesystem;

constexpr const char* cliUser = "cli";

struct Options
{
    std::vector<std::string_view> positional;
    std::string_view dump, from;
    double seconds = 0;
    bool hold = false, recover = false, discard = false, metronome = false;
};

Options parse (int argc, char** argv)
{
    Options o;
    for (int i = 2; i < argc; ++i)
    {
        const std::string_view a = argv[i];
        if (a == "--dump" && i + 1 < argc)
            o.dump = argv[++i];
        else if (a == "--from" && i + 1 < argc)
            o.from = argv[++i];
        else if (a == "--seconds" && i + 1 < argc)
            o.seconds = std::atof (argv[++i]);
        else if (a == "--metronome")
            o.metronome = true;
        else if (a == "--hold")
            o.hold = true;
        else if (a == "--recover")
            o.recover = true;
        else if (a == "--discard")
            o.discard = true;
        else
            o.positional.push_back (a);
    }
    return o;
}

bool dump (const weft::doc::Document& d, std::string_view file)
{
    if (file.empty())
        return true;
    std::ofstream out { fs::path (std::string (file)), std::ios::binary };
    out << weft::doc::toJson (d.root(), true) << "\n";
    return static_cast<bool> (out);
}

int fail (const std::string& message)
{
    std::fprintf (stderr, "error: %s\n", message.c_str());
    return 1;
}

int newProject (const Options& o)
{
    if (o.positional.size() < 2)
        return usage();
    weft::commands::NewProjectOptions options;
    options.name = std::string (o.positional[1]);
    if (o.positional.size() > 2)
        options.sampleRate = std::atoi (std::string (o.positional[2]).c_str());
    std::string error;
    auto session = Session::createNew (options, cliUser, &error);
    if (! session)
        return fail (error);
    ProjectFolder folder (ProjectFolder::folderFor (fs::path (std::string (o.positional[0])), options.name));
    if (! folder.create (&error) || ! folder.save (session->document(), {}, &error))
        return fail (error);
    std::printf ("%s\n", folder.root().string().c_str());
    return 0;
}

/** A fixed set of edits that touches most of the schema; used by the kill-recovery test. */
int demoEdits (const Options& o)
{
    if (o.positional.empty())
        return usage();
    ProjectFolder folder { fs::path (std::string (o.positional[0])) };
    auto opened = folder.open();
    if (! opened.document)
        return fail (opened.error);
    Session session (std::move (*opened.document), cliUser);
    session.onLocalChange = [&] (const weft::doc::Bytes& change, std::int64_t t) { folder.log().append (change, t); };

    using weft::model::TrackKind;
    const auto keys = session.createTrack (TrackKind::instrument, "Keys");
    const auto bus = session.createTrack (TrackKind::bus, "Reverb");
    if (! keys || ! bus)
        return fail (session.lastError());
    const auto region = session.createMidiRegion (*keys, 3840, 7680);
    if (! region)
        return fail (session.lastError());
    for (int i = 0; i < 16; ++i)
    {
        weft::model::Note n;
        n.pos = i * 480 + 7;
        n.pitch = 60 + (i % 12);
        session.addNote (*region, n);
    }
    session.quantizeNotes (*region, {}, 240, 1.0, 0.0);
    session.splitRegion (*region, 7680);
    session.addSend (*keys, *bus, -12.0);
    session.addTempoPoint (15360, 90.0);
    session.addSignaturePoint (4, 3, 4);
    session.addMarker (0, "Intro");
    session.renameProject ("Demo After Edits");
    session.beginGesture ("Volume");
    for (int i = 0; i < 50; ++i)
        session.setTrackVolume (*keys, -0.1 * i);
    session.endGesture();

    if (! dump (session.document(), o.dump))
        return fail ("couldn't write the dump");
    std::printf ("READY\n");
    std::fflush (stdout);
    while (o.hold)
        std::this_thread::sleep_for (std::chrono::seconds (1));
    return 0;
}

int openProject (const Options& o)
{
    if (o.positional.empty())
        return usage();
    ProjectFolder folder { fs::path (std::string (o.positional[0])) };
    auto opened = folder.open();
    if (! opened.document)
        return fail (opened.error);
    std::printf ("recovery: %s\n", opened.recoveryAvailable() ? "available" : "none");
    std::string error;
    if (o.recover && ! folder.recover (opened, &error))
        return fail (error);
    if (o.discard && ! folder.discard (opened, weft::commands::systemClockMs()))
        return fail ("couldn't archive the recovery log");
    if (! dump (*opened.document, o.dump))
        return fail ("couldn't write the dump");
    return 0;
}
/** Plays the saved project through the engine and the null device into a WAV file. */
int renderProject (const Options& o)
{
    if (o.positional.size() < 2)
        return usage();
    ProjectFolder folder { fs::path (std::string (o.positional[0])) };
    auto opened = folder.open();
    if (! opened.document)
        return fail (opened.error);
    Session session (std::move (*opened.document), cliUser);
    const auto& model = session.model();

    weft::engine::MediaPool media;
    media.setResidentLimit (INT64_MAX); // offline render runs faster than real time: no streaming
    for (const auto& [hash, ref] : model.media)
    {
        std::string error;
        if (! media.load (folder.mediaDir(), hash, &error))
            std::fprintf (stderr, "warning: %s plays as silence (%s)\n", ref->originalName.c_str(), error.c_str());
    }
    weft::engine::InstrumentRack rack;
    weft::engine::Engine engine (media, rack);
    auto settings = weft::engine::TransportSettings::fromDefaults (model.info.metronomeDefaults);
    settings.metronomeOn = o.metronome;
    settings.metronomeDuring = weft::engine::ClickDuring::both;
    engine.setSettings (settings);

    weft::Ticks from = 0;
    if (! o.from.empty())
    {
        const auto parsed = weft::parseBarsBeats (o.from, model.signatures);
        if (! parsed)
            return fail ("couldn't read the --from position");
        from = *parsed;
    }
    const double seconds = o.seconds > 0 ? o.seconds
                                         : std::max (1.0, (model.tempo.tickToSample (static_cast<double> (weft::commands::projectEndTick (model)))
                                                           - model.tempo.tickToSample (static_cast<double> (from)))
                                                              / model.info.sampleRate);

    weft::engine::DeviceConfig config;
    config.sampleRate = model.info.sampleRate;
    config.maxBlockSize = 512;
    config.numInputChannels = 0;
    config.numOutputChannels = 2;
    weft::engine::NullDevice device;
    if (! device.open (config) || ! device.start (engine))
        return fail ("couldn't start the null device");
    engine.setProject (model);
    engine.play (from);
    const auto frames = static_cast<std::int64_t> (std::llround (seconds * config.sampleRate));
    device.setCaptureCapacity (frames);
    device.render (frames);
    device.stop();

    weft::media::AudioData out;
    out.sampleRate = config.sampleRate;
    float peak = 0;
    for (int ch = 0; ch < 2; ++ch)
    {
        const auto c = device.capturedOutput (ch);
        out.channels.emplace_back (c.begin(), c.end());
        for (const float v : c)
            peak = std::max (peak, std::abs (v));
    }
    std::string error;
    if (! weft::media::writeWav (fs::path (std::string (o.positional[1])), out, weft::media::SampleFormat::float32, &error))
        return fail (error);
    std::printf ("frames=%lld peak=%.4f\n", static_cast<long long> (frames), static_cast<double> (peak));
    return 0;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 2)
        return usage();

    const std::string_view command = argv[1];
    if (command == "version")
    {
        std::printf ("%.*s %.*s\n", static_cast<int> (weft::productName.size()), weft::productName.data(),
                     static_cast<int> (weft::versionString().size()), weft::versionString().data());
        return 0;
    }

    if (command == "null-render")
    {
        const double seconds = argc > 2 ? std::atof (argv[2]) : 1.0;
        weft::engine::DeviceConfig config;
        config.sampleRate = argc > 3 ? std::atof (argv[3]) : 48000.0;
        config.maxBlockSize = argc > 4 ? std::atoi (argv[4]) : 256;
        config.numInputChannels = 0;
        config.numOutputChannels = 1;

        weft::engine::NullDevice device;
        TestTone tone;
        if (! device.open (config) || ! device.start (tone))
            return usage();
        device.setDeadlineSimulation (true);
        device.render (static_cast<std::int64_t> (seconds * config.sampleRate));
        device.stop();

        const auto& s = device.stats();
        std::printf ("blocks=%lld frames=%lld deadline_misses=%lld peak=%.4f\n", static_cast<long long> (s.blocksProcessed),
                     static_cast<long long> (s.framesProcessed), static_cast<long long> (s.deadlineMisses), static_cast<double> (tone.peak));
        return 0;
    }

    const auto options = parse (argc, argv);
    if (command == "new")
        return newProject (options);
    if (command == "demo-edits")
        return demoEdits (options);
    if (command == "open")
        return openProject (options);
    if (command == "render")
        return renderProject (options);

    return usage();
}
