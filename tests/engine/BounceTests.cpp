// Bounce and export (spec 06 §13): range, offline and real-time renders, WAV bit depths,
// dither, tail, clipping and MP3 (AT-EXP-1, 3 to 10). AT-EXP-2 reads the files back with
// JUCE's WAV reader in JuceBounceTests.cpp; AT-EXP-11 is a [UI] test.

#include "EngineFixtures.h"

#include "weft/engine/dsp/BuiltinEffects.h"
#include "weft/engine/render/Bounce.h"
#include "weft/media/MediaStore.h"
#include "weft/rt/AudioThread.h"

#include <weft_codec.h>

#include <catch2/catch_approx.hpp>

#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <numbers>

using namespace weft;
using namespace weft::test;

namespace
{
constexpr double rate = 48000.0;

/** Mono media under `hash` from a generator, and an audio region of it on a new stereo track. */
template <typename F>
Id toneTrack (Rig& rig, const std::string& hash, std::int64_t frames, F&& value, Ticks start = 0)
{
    media::AudioData a;
    a.sampleRate = rate;
    a.channels.assign (1, std::vector<float> (static_cast<std::size_t> (frames)));
    for (std::int64_t i = 0; i < frames; ++i)
        a.channels[0][static_cast<std::size_t> (i)] = value (i);
    rig.media.add (hash, a);
    model::MediaRef m;
    m.hash = hash;
    m.originalName = hash + ".wav";
    m.lengthSamples = frames;
    REQUIRE (rig.session.addMedia (m));
    const auto t = rig.session.createTrack (TrackKind::audio, "Tone", 2).value();
    REQUIRE (rig.session.createAudioRegion (t, start, hash, 0, frames));
    rig.sync();
    return t;
}

float sine (std::int64_t i, double freq, double amplitude)
{
    return static_cast<float> (amplitude * std::sin (2.0 * std::numbers::pi * freq * static_cast<double> (i) / rate));
}

Id addBuiltin (Rig& rig, const Id& track, const char* uid, const char* name, std::map<std::string, double> params = {})
{
    model::PluginInstance p;
    p.format = "builtin";
    p.pluginUid = uid;
    p.name = name;
    p.params = std::move (params);
    const auto id = rig.session.addInsert (track, p);
    REQUIRE (id);
    rig.sync();
    return *id;
}

BounceSettings wavSettings (const TempFolder& out, Ticks bars, int bitDepth = 24, bool tail = false)
{
    BounceSettings s;
    s.range = { 0, bars * barTicks };
    s.includeTail = tail;
    s.wav = WavFormat { bitDepth, 0 };
    s.folder = out.path;
    s.name = "Mix";
    return s;
}

BounceJob::Context context (Rig& rig) { return { &rig.media, {}, rig.folder.path, &rig.engine, {} }; }

BounceResult bounce (Rig& rig, const BounceSettings& s)
{
    BounceJob job (rig.session.model(), s, context (rig));
    job.render();
    return job.finish();
}

/** Runs a real-time job: steps it while the device plays, as the window's timer would. */
BounceResult bounceRealtime (Rig& rig, BounceJob& job, int block = 256)
{
    int guard = 0;
    while (job.step() && ++guard < 100000)
        REQUIRE (rig.device.render (block, block));
    return job.finish();
}

media::AudioData readBack (const std::filesystem::path& file)
{
    auto a = media::readWav (file);
    REQUIRE (a.has_value());
    return std::move (*a);
}

double rmsDb (const std::vector<float>& v, std::size_t from, std::size_t to)
{
    double sum = 0;
    for (auto i = from; i < to; ++i)
        sum += static_cast<double> (v[i]) * static_cast<double> (v[i]);
    return 10.0 * std::log10 (sum / static_cast<double> (to - from));
}

struct Decoded
{
    std::vector<float> samples; // interleaved
    std::size_t frames = 0;
    int channels = 0, sampleRate = 0;
    std::string title, artist;
};

Decoded decodeMp3 (const std::filesystem::path& file)
{
    WeftMp3Decoded d {};
    std::array<char, 256> err {};
    const auto path = file.u8string();
    REQUIRE (weft_mp3_decode (reinterpret_cast<const char*> (path.c_str()), &d, err.data(), err.size()) == 0);
    Decoded out;
    out.samples.assign (d.samples, d.samples + d.frames * static_cast<std::size_t> (d.channels));
    out.frames = d.frames;
    out.channels = d.channels;
    out.sampleRate = d.sample_rate;
    out.title = d.title;
    out.artist = d.artist;
    weft_mp3_free (&d);
    return out;
}

/** Goertzel power of channel 0 at `freq` over the decoded audio. */
double power (const Decoded& d, double freq)
{
    const double w = 2.0 * std::numbers::pi * freq / d.sampleRate, k = 2.0 * std::cos (w);
    double s1 = 0, s2 = 0;
    for (std::size_t i = 0; i < d.frames; ++i)
    {
        const double s = d.samples[i * static_cast<std::size_t> (d.channels)] + k * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return s1 * s1 + s2 * s2 - k * s1 * s2;
}

std::vector<char> bytes (const std::filesystem::path& file)
{
    std::ifstream in (file, std::ios::binary);
    return { std::istreambuf_iterator<char> (in), {} };
}

bool contains (const std::vector<char>& data, std::size_t from, std::size_t to, const std::string& what)
{
    const auto end = data.begin() + static_cast<std::ptrdiff_t> (std::min (to, data.size()));
    return std::search (data.begin() + static_cast<std::ptrdiff_t> (from), end, what.begin(), what.end()) != end;
}
} // namespace

TEST_CASE ("AT-EXP-1 a 4-bar WAV bounce has 384,000 samples equal to a real-time capture", "[E][bounce]")
{
    Rig rig (256);
    rig.addAudioRegion (rig.audioTrack(), 0, 6 * barSamples);
    TempFolder out;
    auto s = wavSettings (out, 4);
    const auto offline = bounce (rig, s);
    REQUIRE (offline.ok);
    REQUIRE (offline.files.size() == 1);
    CHECK (offline.files[0].filename() == "Mix.wav");
    const auto file = readBack (offline.files[0]);
    CHECK (file.numFrames() == 384000);
    CHECK (file.numChannels() == 2);
    CHECK (file.sampleRate == 48000.0);
    CHECK (file.channels[0][1000] == Catch::Approx (rampValue (1000)).margin (1.0 / 8388608));

    // The same range captured from the live engine while it plays (real time).
    s.realtime = true;
    s.name = "Live";
    BounceJob job (rig.session.model(), s, context (rig));
    const auto live = bounceRealtime (rig, job);
    REQUIRE (live.ok);
    const auto captured = readBack (live.files[0]);
    REQUIRE (captured.numFrames() == 384000);
    int beyondOneLsb = 0;
    for (int ch = 0; ch < 2; ++ch)
        for (std::size_t i = 0; i < 384000; ++i)
            if (std::abs (captured.channels[static_cast<std::size_t> (ch)][i] - file.channels[static_cast<std::size_t> (ch)][i]) > 1.5f / 8388608.0f)
                ++beyondOneLsb;
    CHECK (beyondOneLsb == 0);
    REQUIRE (rig.device.render (256));
    CHECK (rig.engine.state() == TransportState::stopped);
}

TEST_CASE ("AT-EXP-3 a 320 kbps MP3 of a 10 s sine decodes to 10 s at the sine's frequency with its tags", "[E][bounce][mp3]")
{
    Rig rig (256);
    toneTrack (rig, "b3:sine", 10 * 48000, [] (std::int64_t i) { return sine (i, 1000.0, 0.5); });
    TempFolder out;
    BounceSettings s;
    s.range = { 0, 5 * barTicks }; // 10 s at 120 BPM
    s.includeTail = false;
    s.wav.reset();
    s.mp3 = Mp3Format {};
    s.mp3->title = "Sine Song";
    s.mp3->artist = "Weft Tester";
    s.folder = out.path;
    s.name = "Sine";
    const auto result = bounce (rig, s);
    REQUIRE (result.ok);
    REQUIRE (result.files.size() == 1);
    CHECK (result.files[0].extension() == ".mp3");
    const auto d = decodeMp3 (result.files[0]);
    CHECK (d.sampleRate == 48000);
    CHECK (d.channels == 2);
    CHECK (std::abs (static_cast<double> (d.frames) - 480000.0) <= 1152.0);
    CHECK (d.title == "Sine Song");
    CHECK (d.artist == "Weft Tester");
    const auto at1k = power (d, 1000.0);
    for (const double other : { 500.0, 900.0, 1100.0, 2000.0, 3000.0 })
        CHECK (at1k > 1000.0 * power (d, other));
    // A CBR file starts with an Info header from LAME.
    const auto data = bytes (result.files[0]);
    CHECK (contains (data, 0, 4096, "Info"));
}

TEST_CASE ("AT-EXP-4 VBR quality 2 makes a valid MP3 with a Xing/LAME header", "[E][bounce][mp3]")
{
    Rig rig (256);
    toneTrack (rig, "b3:sine", 4 * 48000, [] (std::int64_t i) { return sine (i, 440.0, 0.3) + sine (i, 3000.0, 0.1); });
    TempFolder out;
    BounceSettings s;
    s.range = { 0, 2 * barTicks };
    s.includeTail = false;
    s.wav.reset();
    s.mp3 = Mp3Format {};
    s.mp3->vbr = true;
    s.mp3->vbrQuality = 2;
    s.mp3->sampleRate = 44100;
    s.folder = out.path;
    s.name = "Vbr";
    const auto result = bounce (rig, s);
    REQUIRE (result.ok);
    const auto data = bytes (result.files[0]);
    CHECK (contains (data, 0, 4096, "Xing"));
    CHECK (contains (data, 0, 4096, "LAME"));
    const auto d = decodeMp3 (result.files[0]);
    CHECK (d.sampleRate == 44100);
    CHECK (std::abs (static_cast<double> (d.frames) - 4.0 * 44100.0) <= 1152.0);
}

TEST_CASE ("AT-EXP-5 an offline bounce of a 3-minute project is faster than real time and bit-identical twice", "[E][bounce][slow]")
{
    Rig rig (256);
    const std::int64_t frames = 180 * 48000;
    const auto a = toneTrack (rig, "b3:a", frames, [] (std::int64_t i) { return sine (i, 220.0, 0.3) * static_cast<float> ((i / 24000) % 2); });
    const auto b = toneTrack (rig, "b3:b", frames, [] (std::int64_t i) { return rampValue (i) * 0.5f; });
    addBuiltin (rig, a, WeftReverb::pluginUid, "Weft Reverb");
    addBuiltin (rig, a, "weft.eq", "Weft EQ");
    addBuiltin (rig, b, "weft.compressor", "Weft Compressor");
    addBuiltin (rig, b, "weft.delay", "Weft Delay");
    TempFolder out;
    auto s = wavSettings (out, 90, 16); // 3 minutes, with dither
    s.includeTail = true;
    s.replace = true;
    const auto started = std::chrono::steady_clock::now();
    BounceJob first (rig.session.model(), s, context (rig));
    first.render();
    const auto r1 = first.finish();
    const std::chrono::duration<double> took = std::chrono::steady_clock::now() - started;
    REQUIRE (r1.ok);
    CHECK (took.count() < 180.0);
    const auto file1 = bytes (r1.files[0]);
    BounceJob second (rig.session.model(), s, context (rig));
    second.render();
    const auto r2 = second.finish();
    REQUIRE (r2.ok);
    CHECK (first.mix().channels == second.mix().channels);
    CHECK (bytes (r2.files[0]) == file1);
    WARN ("3-minute offline bounce took " << took.count() << " s");
}

TEST_CASE ("AT-EXP-6 a realtime-only plug-in forces a real-time bounce with a notice", "[E][bounce]")
{
    CHECK (isRealtimeOnlyCategory ("Fx|OnlyRT"));
    CHECK (isRealtimeOnlyCategory ("OnlyRT"));
    CHECK_FALSE (isRealtimeOnlyCategory ("Fx|Reverb"));
    CHECK_FALSE (isRealtimeOnlyCategory ("Fx|OnlyRTX"));

    Rig rig (256);
    const auto track = rig.audioTrack();
    rig.addAudioRegion (track, 0, 2 * barSamples);
    addBuiltin (rig, track, "weft.gain", "Hardware Insert");
    TempFolder out;
    auto ctx = context (rig);
    ctx.realtimeOnly = [] (const model::PluginInstance& p) { return p.name == "Hardware Insert"; };
    BounceJob job (rig.session.model(), wavSettings (out, 1), ctx);
    CHECK (job.realtime());
    CHECK (job.notice() == "Hardware Insert can only render in real time, so this bounce plays in real time.");
    const auto result = bounceRealtime (rig, job);
    REQUIRE (result.ok);
    CHECK (readBack (result.files[0]).numFrames() == barSamples);

    // Without such a plug-in the bounce stays offline.
    BounceJob plain (rig.session.model(), wavSettings (out, 1), context (rig));
    CHECK_FALSE (plain.realtime());
    CHECK (plain.notice().empty());
}

TEST_CASE ("AT-EXP-7 the default range is the cycle, else the end marker, else the end of the last region", "[U][bounce]")
{
    Rig rig (256);
    const auto track = rig.audioTrack();
    rig.addAudioRegion (track, 2 * barTicks, 3 * barSamples);
    const auto& m = rig.session.model();
    auto range = defaultBounceRange (m);
    CHECK (range.from == 0);
    CHECK (range.to == 5 * barTicks);

    REQUIRE (rig.session.setEndMarker (8 * barTicks));
    range = defaultBounceRange (rig.session.model());
    CHECK (range.from == 0);
    CHECK (range.to == 8 * barTicks);

    REQUIRE (rig.session.setCycle (true, barTicks, 3 * barTicks));
    range = defaultBounceRange (rig.session.model());
    CHECK (range.from == barTicks);
    CHECK (range.to == 3 * barTicks);

    REQUIRE (rig.session.setCycle (false, barTicks, 3 * barTicks));
    CHECK (defaultBounceRange (rig.session.model()).to == 8 * barTicks);
}

TEST_CASE ("AT-EXP-8 Auto dither adds TPDF noise at 16 bits, none at 24; Off at 16 only requantizes", "[E][bounce]")
{
    Rig rig (256);
    // A -20 dBFS tone for the first bar, then digital silence.
    toneTrack (rig, "b3:tone", barSamples, [] (std::int64_t i) { return sine (i, 997.0, 0.1); });
    TempFolder out;

    auto s = wavSettings (out, 2, 16);
    s.name = "Auto16";
    BounceJob job (rig.session.model(), s, context (rig));
    job.render();
    const auto mix = job.mix(); // the float mix before dither
    const auto auto16 = job.finish();
    REQUIRE (auto16.ok);
    const auto dithered = readBack (auto16.files[0]);
    const auto silence = static_cast<std::size_t> (barSamples + 4800);
    const auto end = static_cast<std::size_t> (2 * barSamples);
    // TPDF of +-1 LSB, then rounding: 0.5 LSB RMS = -96.3 dB re full scale (D-W9-4).
    const double noise = rmsDb (dithered.channels[0], silence, end);
    CHECK (noise > -98.3);
    CHECK (noise < -94.3);
    // The tone is still the tone, give or take the dither.
    double worst = 0;
    for (std::size_t i = 0; i < static_cast<std::size_t> (barSamples); ++i)
        worst = std::max (worst, std::abs (static_cast<double> (dithered.channels[0][i] - mix.channels[0][i])));
    CHECK (worst <= 1.5 / 32768.0);

    s = wavSettings (out, 2, 24);
    s.name = "Auto24";
    const auto auto24 = bounce (rig, s);
    REQUIRE (auto24.ok);
    const auto plain24 = readBack (auto24.files[0]);
    int differ = 0;
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (std::size_t i = 0; i < end; ++i)
        {
            const auto expected = std::lround (std::clamp (static_cast<double> (mix.channels[ch][i]), -1.0, 1.0) * 8388608.0);
            if (std::lround (static_cast<double> (plain24.channels[ch][i]) * 8388608.0) != expected)
                ++differ;
        }
    CHECK (differ == 0);

    s = wavSettings (out, 2, 16);
    s.dither = DitherMode::off;
    s.name = "Off16";
    const auto off16 = bounce (rig, s);
    REQUIRE (off16.ok);
    const auto plain16 = readBack (off16.files[0]);
    differ = 0;
    for (std::size_t i = 0; i < end; ++i)
        if (std::lround (static_cast<double> (plain16.channels[0][i]) * 32768.0) != std::lround (static_cast<double> (mix.channels[0][i]) * 32768.0))
            ++differ;
    CHECK (differ == 0);
    CHECK (rmsDb (plain16.channels[0], silence, end) < -300.0); // exact silence

    CHECK (dithers (DitherMode::automatic, 16));
    CHECK_FALSE (dithers (DitherMode::automatic, 24));
    CHECK (dithers (DitherMode::on, 24));
    CHECK_FALSE (dithers (DitherMode::on, 32));
    CHECK_FALSE (dithers (DitherMode::off, 16));
}

TEST_CASE ("AT-EXP-9 the tail runs until -90 dBFS for 200 ms, at most 10 s; off ends at the range", "[E][bounce]")
{
    Rig rig (256);
    const auto track = toneTrack (rig, "b3:burst", 4 * barSamples, [] (std::int64_t i) { return sine (i, 330.0, 0.5); });
    addBuiltin (rig, track, WeftReverb::pluginUid, "Weft Reverb", { { "mix", 0.5 } });
    TempFolder out;

    auto s = wavSettings (out, 4, 32, true);
    const auto withTail = bounce (rig, s);
    REQUIRE (withTail.ok);
    const auto rangeEnd = 4 * barSamples;
    CHECK (withTail.frames > rangeEnd + 24000);
    CHECK (withTail.frames <= rangeEnd + 10 * 48000);
    CHECK (readBack (withTail.files[0]).numFrames() == withTail.frames);

    // Against a plain render running on past the file's end: the 200 ms after the end are all
    // below -90 dBFS, and the sample just before it isn't.
    OfflineRenderOptions o;
    o.frames = rangeEnd + 11 * 48000;
    const auto longer = renderOffline (rig.session.model(), rig.media, o);
    const float threshold = static_cast<float> (std::pow (10.0, -90.0 / 20.0));
    const auto end = static_cast<std::size_t> (withTail.frames);
    float after = 0;
    for (auto i = end; i < end + 9600; ++i)
        after = std::max ({ after, std::abs (longer.channels[0][i]), std::abs (longer.channels[1][i]) });
    CHECK (after < threshold);
    CHECK (std::max (std::abs (longer.channels[0][end - 1]), std::abs (longer.channels[1][end - 1])) >= threshold);

    s = wavSettings (out, 4, 32, false);
    s.name = "NoTail";
    const auto noTail = bounce (rig, s);
    REQUIRE (noTail.ok);
    CHECK (noTail.frames == rangeEnd);
    CHECK (readBack (noTail.files[0]).numFrames() == rangeEnd);

    // A tail that never dies stops at 10 s.
    TailDetector loud (48000.0);
    std::vector<float> one (48000, 0.5f);
    for (int i = 0; i < 12 && ! loud.done(); ++i)
        loud.add (one.data(), one.data(), 48000);
    CHECK (loud.done());
    CHECK (loud.length() == 480000);
}

TEST_CASE ("AT-EXP-10 a mix peaking at +2 dBFS reports clipping; float keeps the overs, 24-bit clips", "[E][bounce]")
{
    Rig rig (256);
    const double amp = std::pow (10.0, 2.0 / 20.0);
    const auto track = toneTrack (rig, "b3:hot", barSamples, [amp] (std::int64_t i) { return sine (i, 250.0, amp); });
    TempFolder out;
    auto s = wavSettings (out, 1, 32);
    s.name = "Float";
    const auto f = bounce (rig, s);
    REQUIRE (f.ok);
    CHECK (f.clipped);
    CHECK (f.peakDb == Catch::Approx (2.0).margin (0.05));
    CHECK (clipMessage (f.peakDb) == "Bounce clipped (peak +2.0 dBFS)");
    CHECK (f.message.find ("Bounce clipped (peak +2.0 dBFS)") != std::string::npos);
    const auto floats = readBack (f.files[0]);
    CHECK (*std::max_element (floats.channels[0].begin(), floats.channels[0].end()) > 1.2f);

    s = wavSettings (out, 1, 24);
    s.name = "Int";
    const auto i24 = bounce (rig, s);
    REQUIRE (i24.ok);
    const auto ints = readBack (i24.files[0]);
    CHECK (*std::max_element (ints.channels[0].begin(), ints.channels[0].end()) == 8388607.0f / 8388608.0f);
    CHECK (*std::min_element (ints.channels[0].begin(), ints.channels[0].end()) == -1.0f);

    // A quiet bounce doesn't mention clipping.
    s = wavSettings (out, 1, 24);
    s.name = "Quiet";
    REQUIRE (rig.session.setTrackVolume (track, -6.0));
    rig.sync();
    const auto quiet = bounce (rig, s);
    REQUIRE (quiet.ok);
    CHECK_FALSE (quiet.clipped);
    CHECK (quiet.message == "Bounced Quiet.wav.");
}

TEST_CASE ("Bounce names: Keep both adds (2), (3); Replace overwrites", "[U][bounce]")
{
    TempFolder out;
    const auto wanted = out.path / "Song.wav";
    CHECK (keepBothPath (wanted) == wanted);
    std::ofstream (wanted) << "x";
    CHECK (keepBothPath (wanted) == out.path / "Song (2).wav");
    std::ofstream (out.path / "Song (2).wav") << "x";
    CHECK (keepBothPath (wanted) == out.path / "Song (3).wav");

    BounceSettings s;
    s.folder = out.path;
    s.name = "Song";
    s.mp3 = Mp3Format {};
    const auto targets = bounceTargets (s);
    REQUIRE (targets.size() == 2);
    CHECK (targets[0] == wanted);
    CHECK (targets[1] == out.path / "Song.mp3");

    Mp3Format mp3;
    CHECK (mp3SampleRate (mp3, 48000) == 48000);
    CHECK (mp3SampleRate (mp3, 96000) == 44100);
    mp3.sampleRate = 44100;
    CHECK (mp3SampleRate (mp3, 48000) == 44100);
}

TEST_CASE ("A bounce can be cancelled, and D-W9-1 counts regions without media", "[E][bounce]")
{
    Rig rig (256);
    const auto track = rig.audioTrack();
    rig.addAudioRegion (track, 0, 8 * barSamples);
    TempFolder out;
    BounceJob job (rig.session.model(), wavSettings (out, 8), context (rig));
    CHECK (job.step());
    CHECK (job.progress() > 0.0);
    CHECK (job.progress() < 1.0);
    job.cancel();
    CHECK_FALSE (job.step());
    const auto r = job.finish();
    CHECK (r.cancelled);
    CHECK_FALSE (r.ok);
    CHECK (std::filesystem::is_empty (out.path));

    CHECK (regionsMissingMedia (rig.session.model(), rig.media) == 0);
    model::MediaRef ghost;
    ghost.hash = "b3:not-downloaded";
    ghost.lengthSamples = 1000;
    REQUIRE (rig.session.addMedia (ghost));
    REQUIRE (rig.session.createAudioRegion (track, 9 * barTicks, ghost.hash, 0, 1000));
    CHECK (regionsMissingMedia (rig.session.model(), rig.media) == 1);
}

TEST_CASE ("AT-COL-18 regions whose media is still downloading count as not yet downloaded for the bounce warning", "[E][bounce][AT-COL-18]")
{
    Rig rig (256);
    const auto track = rig.audioTrack();
    // A collaborator's recording, half of it here so far.
    const auto frames = 4 * barSamples;
    const auto hash = media::storeMedia (rampMedia (frames), media::SampleFormat::int24, rig.folder.path / "Media");
    REQUIRE (hash);
    const auto whole = rig.mediaFile (*hash);
    const auto part = rig.folder.path / "part.wav";
    {
        std::ifstream in (whole, std::ios::binary);
        std::vector<char> bytes ((std::istreambuf_iterator<char> (in)), {});
        std::ofstream out (part, std::ios::binary);
        out.write (bytes.data(), static_cast<std::streamsize> (bytes.size() / 2));
    }
    model::MediaRef ref;
    ref.hash = *hash;
    ref.lengthSamples = frames;
    ref.channels = 1;
    REQUIRE (rig.session.addMedia (ref));
    REQUIRE (rig.session.createAudioRegion (track, 0, *hash, 0, frames));
    REQUIRE (rig.session.createAudioRegion (track, 8 * barTicks, *hash, 0, frames));
    REQUIRE (rig.media.loadPartial (part, *hash, frames));
    CHECK (rig.media.find (*hash)->partial());
    // The dialog says "2 regions have media not yet downloaded" from this count (D-W9-1).
    CHECK (regionsMissingMedia (rig.session.model(), rig.media) == 2);
    REQUIRE (rig.media.completeDownload (rig.folder.path / "Media", *hash));
    CHECK (regionsMissingMedia (rig.session.model(), rig.media) == 0);
}

TEST_CASE ("AT-RT-1 (bounce) capturing a real-time bounce allocates nothing on the audio thread", "[E][rt][bounce]")
{
    Rig rig (256);
    rig.addAudioRegion (rig.audioTrack(), 0, 2 * barSamples);
    TempFolder out;
    auto s = wavSettings (out, 1);
    s.realtime = true;
    BounceJob job (rig.session.model(), s, context (rig));
    REQUIRE (job.step()); // starts the transport and the capture
    weft::rt::resetViolations();
    int guard = 0;
    while (job.step() && ++guard < 10000)
        REQUIRE (rig.device.render (256, 256));
    const auto v = weft::rt::violations();
    CHECK (v.allocations == 0);
    CHECK (v.blockingCalls == 0);
    CHECK (job.finish().ok);
}

TEST_CASE ("A real-time bounce on a 44.1 kHz device writes the project rate", "[E][bounce]")
{
    Rig rig (256, 44100.0);
    rig.addAudioRegion (rig.audioTrack(), 0, 2 * barSamples);
    TempFolder out;
    auto s = wavSettings (out, 1);
    s.realtime = true;
    BounceJob job (rig.session.model(), s, context (rig));
    const auto result = bounceRealtime (rig, job);
    REQUIRE (result.ok);
    CHECK (result.frames == 88200);
    const auto file = readBack (result.files[0]);
    CHECK (file.sampleRate == 48000.0);
    CHECK (std::abs (file.numFrames() - barSamples) <= 1);
}
