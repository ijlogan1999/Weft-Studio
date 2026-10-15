#pragma once

// Helpers for engine tests: a session, engine and NullDevice wired together, test
// media with a known waveform, and instruments that record or burn time.

#include "weft/commands/Session.h"
#include "weft/engine/devices/NullDevice.h"
#include "weft/engine/transport/Engine.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>
#include <vector>

namespace weft::test
{
using namespace weft::engine;
using commands::Session;
using model::TrackKind;

inline constexpr Ticks barTicks = 3840;
inline constexpr std::int64_t barSamples = 96000; // P0: 48 kHz, 120 BPM, 4/4

inline Session newP0()
{
    auto s = Session::createNew ({}, "userA", nullptr, [] { return std::int64_t { 1'700'000'000'000 }; }, 1);
    REQUIRE (s.has_value());
    return std::move (*s);
}

/** The test waveform: every frame non-zero and exactly representable, period 9,973 frames. */
inline float rampValue (std::int64_t frame) { return static_cast<float> ((frame % 9973) + 1) / 16384.0f; }

inline media::AudioData rampMedia (std::int64_t frames, double rate = 48000.0)
{
    media::AudioData a;
    a.sampleRate = rate;
    a.channels.assign (1, std::vector<float> (static_cast<std::size_t> (frames)));
    for (std::int64_t i = 0; i < frames; ++i)
        a.channels[0][static_cast<std::size_t> (i)] = rampValue (i);
    return a;
}

/** Records MIDI with absolute frame positions and outputs 0.1 x velocity/127 per held note. */
class ProbeInstrument final : public Instrument
{
public:
    struct Seen
    {
        std::int64_t frame;
        MidiEvent event;
    };

    void prepare (double, int) override
    {
        seen.clear();
        seen.reserve (4096);
        frames = 0;
        held.fill (0);
    }

    void process (std::span<const MidiEvent> events, float* const* outputs, int numChannels, int numFrames) noexcept override
    {
        std::size_t next = 0;
        for (int i = 0; i < numFrames; ++i)
        {
            for (; next < events.size() && events[next].offset <= i; ++next)
            {
                const auto& e = events[next];
                if (seen.size() < seen.capacity())
                    seen.push_back ({ frames + e.offset, e });
                if (e.isNoteOn())
                    held[e.data1] = e.data2;
                else if (e.isNoteOff())
                    held[e.data1] = 0;
            }
            float level = 0;
            for (const auto v : held)
                level += 0.1f * static_cast<float> (v) / 127.0f;
            for (int ch = 0; ch < numChannels; ++ch)
                outputs[ch][i] = level;
        }
        frames += numFrames;
    }

    std::vector<Seen> seen;
    std::int64_t frames = 0;
    std::array<int, 128> held {};
};

/** Busy-waits for a fraction of each block's real-time period (stands in for a heavy plug-in, AT-TRN-17). */
class BurnInstrument final : public Instrument
{
public:
    void prepare (double sampleRate, int) override { rate = sampleRate; }
    void process (std::span<const MidiEvent>, float* const* outputs, int numChannels, int numFrames) noexcept override
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double> (fraction.load() * numFrames / rate);
        while (std::chrono::steady_clock::now() < until)
        {
        }
        for (int ch = 0; ch < numChannels; ++ch)
            std::fill_n (outputs[ch], numFrames, 0.0f);
    }
    std::atomic<double> fraction { 0.8 };
    double rate = 48000;
};

/** A fresh directory under the system temp folder, removed afterwards. */
struct TempFolder
{
    std::filesystem::path path;
    TempFolder()
    {
        std::random_device rd;
        path = std::filesystem::temp_directory_path() / ("weft-engine-" + std::to_string (rd()) + std::to_string (rd()));
        std::filesystem::create_directories (path);
    }
    ~TempFolder()
    {
        std::error_code ec;
        std::filesystem::remove_all (path, ec);
    }
    TempFolder (const TempFolder&) = delete;
    TempFolder& operator= (const TempFolder&) = delete;
};

/** A project session, an engine and a NullDevice driving it. */
struct Rig
{
    TempFolder folder; // the project folder (Recovery/, Media/)
    Session session = newP0();
    MediaPool media;
    InstrumentRack rack;
    Engine engine { media, rack };
    NullDevice device;

    explicit Rig (int blockSize = 256, double rate = 48000.0, int inputs = 2)
    {
        TransportSettings quiet;
        quiet.metronomeOn = false;
        engine.setSettings (quiet);
        engine.setProjectFolder (folder.path);
        engine.setRecordCapacitySeconds (60); // renders run faster than real time
        open (blockSize, rate, inputs);
    }

    void open (int blockSize, double rate = 48000.0, int inputs = 2)
    {
        DeviceConfig c;
        c.sampleRate = rate;
        c.maxBlockSize = blockSize;
        c.numInputChannels = inputs;
        c.numOutputChannels = 2;
        REQUIRE (device.open (c));
        REQUIRE (device.start (engine));
        engine.setProject (session.model());
    }

    void reopen (int blockSize, double rate = 48000.0, int inputs = 2)
    {
        device.close();
        open (blockSize, rate, inputs);
    }

    /** Stops the transport and commits the recording pass. */
    std::optional<RecordingOutcome> finishRecording()
    {
        engine.stop();
        run (256);
        return engine.finishRecording (session);
    }

    std::filesystem::path mediaFile (const std::string& hash) const { return folder.path / "Media" / media::mediaFileName (hash); }

    /** The decoded file a region plays. */
    media::AudioData regionFile (const Id& regionId) const
    {
        const auto* r = session.model().region (regionId);
        REQUIRE (r != nullptr);
        auto audio = media::readWav (mediaFile (r->audio.media));
        REQUIRE (audio.has_value());
        return std::move (*audio);
    }

    std::vector<const model::Region*> regionsOn (const Id& trackId) const { return session.model().regionsOnTrack (trackId); }

    void sync() { engine.setProject (session.model()); }

    /** A stereo audio track, so mono test media plays at unity on both sides (no pan law). */
    Id audioTrack()
    {
        if (stereoTrack.empty())
        {
            stereoTrack = session.createTrack (TrackKind::audio, "Stereo", 2).value();
            sync();
        }
        return stereoTrack;
    }
    Id stereoTrack;

    /** Adds ramp media and an audio region using it from source frame 0. */
    Id addAudioRegion (const Id& track, Ticks start, std::int64_t frames, const std::string& hash = "b3:ramp")
    {
        if (! media.find (hash))
        {
            media.add (hash, rampMedia (frames));
            model::MediaRef m;
            m.hash = hash;
            m.originalName = "ramp.wav";
            m.lengthSamples = frames;
            REQUIRE (session.addMedia (m));
        }
        const auto id = session.createAudioRegion (track, start, hash, 0, frames);
        REQUIRE (id);
        sync();
        return *id;
    }

    /** Renders and returns one output channel. */
    std::vector<float> run (std::int64_t frames, int blockSize = 0, int channel = 0)
    {
        device.setCaptureCapacity (frames);
        REQUIRE (device.render (frames, blockSize));
        const auto out = device.capturedOutput (channel);
        return { out.begin(), out.end() };
    }
};

inline std::int64_t firstNonSilent (const std::vector<float>& v)
{
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i] != 0.0f)
            return static_cast<std::int64_t> (i);
    return -1;
}

/** Frames where a click starts: non-zero after at least `gap` silent frames. */
inline std::vector<std::int64_t> onsets (const std::vector<float>& v, std::size_t gap = 100)
{
    std::vector<std::int64_t> out;
    std::size_t silent = gap;
    for (std::size_t i = 0; i < v.size(); ++i)
    {
        if (v[i] == 0.0f)
        {
            ++silent;
            continue;
        }
        if (silent >= gap)
            out.push_back (static_cast<std::int64_t> (i));
        silent = 0;
    }
    return out;
}

inline float peak (const std::vector<float>& v, std::int64_t from, std::int64_t length)
{
    float p = 0;
    for (auto i = from; i < from + length && i < static_cast<std::int64_t> (v.size()); ++i)
        p = std::max (p, std::abs (v[static_cast<std::size_t> (i)]));
    return p;
}

/** Sign changes in [from, from + length): a pitch measure for clicks. */
inline int zeroCrossings (const std::vector<float>& v, std::int64_t from, std::int64_t length)
{
    int n = 0;
    for (auto i = from + 1; i < from + length; ++i)
        if ((v[static_cast<std::size_t> (i - 1)] < 0) != (v[static_cast<std::size_t> (i)] < 0))
            ++n;
    return n;
}
} // namespace weft::test
