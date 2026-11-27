#include "weft/engine/render/Bounce.h"

#include "weft/media/Resample.h"

#include <weft_codec.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <system_error>

namespace weft::engine
{
namespace
{
std::string utf8 (const std::filesystem::path& p)
{
    const auto s = p.u8string();
    return { s.begin(), s.end() };
}

std::filesystem::path fromUtf8 (const std::string& s) { return std::filesystem::path (std::u8string (s.begin(), s.end())); }
} // namespace

BounceRange defaultBounceRange (const model::ProjectModel& model)
{
    const auto& cycle = model.info.cycle;
    if (cycle.enabled && cycle.end > cycle.start)
        return { cycle.start, cycle.end };
    if (model.info.endMarker && *model.info.endMarker > 0)
        return { 0, *model.info.endMarker };
    Ticks end = 0;
    for (const auto& [id, r] : model.regions)
        end = std::max (end, static_cast<Ticks> (std::ceil (model.regionEndTick (*r))));
    return { 0, end };
}

bool dithers (DitherMode mode, int bitDepth) noexcept
{
    if (bitDepth >= 32)
        return false;
    switch (mode)
    {
        case DitherMode::automatic:
            return bitDepth < 24;
        case DitherMode::on:
            return true;
        case DitherMode::off:
            break;
    }
    return false;
}

double mp3SampleRate (const Mp3Format& format, double projectRate) noexcept
{
    if (format.sampleRate > 0)
        return format.sampleRate;
    const bool supported = std::abs (projectRate - 44100.0) < 0.5 || std::abs (projectRate - 48000.0) < 0.5;
    return supported ? projectRate : 44100.0;
}

std::filesystem::path keepBothPath (const std::filesystem::path& wanted)
{
    std::error_code ec;
    if (! std::filesystem::exists (wanted, ec))
        return wanted;
    for (int n = 2;; ++n)
    {
        auto name = wanted.stem();
        name += " (" + std::to_string (n) + ")";
        name += wanted.extension();
        auto candidate = wanted.parent_path() / name;
        if (! std::filesystem::exists (candidate, ec))
            return candidate;
    }
}

std::vector<std::filesystem::path> bounceTargets (const BounceSettings& s)
{
    std::vector<std::filesystem::path> out;
    const auto base = s.folder / fromUtf8 (s.name);
    const auto pcm = [&] (const char* ext) {
        if (s.channels == FileChannels::split)
        {
            out.push_back (std::filesystem::path (base).concat (std::string (".L") + ext));
            out.push_back (std::filesystem::path (base).concat (std::string (".R") + ext));
        }
        else
            out.push_back (std::filesystem::path (base).concat (ext));
    };
    if (s.wav)
        pcm (".wav");
    if (s.aiff)
        pcm (".aif");
    if (s.flac)
        pcm (".flac");
    if (s.mp3)
        out.push_back (std::filesystem::path (base).concat (".mp3"));
    return out;
}

// Tail -----------------------------------------------------------------------------------------

TailDetector::TailDetector (double sampleRate) noexcept
    : threshold (static_cast<float> (std::pow (10.0, thresholdDb / 20.0))),
      quietFrames (std::llround (quietSeconds * sampleRate)),
      maxFrames (std::llround (maxSeconds * sampleRate))
{
}

void TailDetector::add (const float* left, const float* right, std::int64_t frames) noexcept
{
    for (std::int64_t i = 0; i < frames && ! done(); ++i)
    {
        const auto k = static_cast<std::size_t> (i);
        if (std::max (std::abs (left[k]), std::abs (right[k])) >= threshold)
        {
            quietRun = 0;
            lastLoud = fed + 1;
        }
        else
            ++quietRun;
        ++fed;
    }
}

std::int64_t TailDetector::length() const noexcept
{
    if (quietRun >= quietFrames)
        return lastLoud;
    return std::min (fed, maxFrames);
}

// Dither and clipping ---------------------------------------------------------------------------

void addDither (media::AudioData& audio, int bitDepth, std::uint32_t seed)
{
    // xorshift32: small, fast, and the same on every platform (D-W9-2).
    std::uint32_t state = seed != 0 ? seed : 1u;
    auto uniform = [&state] {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<double> (state >> 8) / 16777216.0; // [0, 1)
    };
    const double lsb = 1.0 / std::ldexp (1.0, bitDepth - 1);
    for (auto& channel : audio.channels)
        for (auto& x : channel)
        {
            const double a = uniform(), b = uniform();
            x = static_cast<float> (static_cast<double> (x) + (a - b) * lsb); // triangular, +-1 LSB
        }
}

std::string clipMessage (double peakDb)
{
    std::array<char, 64> text {};
    std::snprintf (text.data(), text.size(), "Bounce clipped (peak %+.1f dBFS)", peakDb);
    return text.data();
}

// Job ------------------------------------------------------------------------------------------

namespace
{
constexpr std::int64_t sliceFrames = 32768;

std::int64_t sampleAt (const model::ProjectModel& m, Ticks t) { return std::llround (m.tempo.tickToSample (static_cast<double> (t))); }

const model::PluginInstance* realtimeOnlyPlugin (const model::ProjectModel& m, const std::function<bool (const model::PluginInstance&)>& realtimeOnly)
{
    if (! realtimeOnly)
        return nullptr;
    for (const auto& [id, track] : m.tracks)
    {
        if (track->strip.instrument && realtimeOnly (*track->strip.instrument))
            return &*track->strip.instrument;
        for (const auto& p : track->strip.inserts)
            if (realtimeOnly (p))
                return &p;
    }
    return nullptr;
}
} // namespace

BounceJob::BounceJob (const model::ProjectModel& model, BounceSettings settings, Context context)
    : project (model), config (std::move (settings)), ctx (std::move (context)), tail (model.info.sampleRate)
{
    // The bounce plays its own range straight through (or twice round, for the second cycle pass).
    project.info.cycle.enabled = false;
    if (! config.soloTracks.empty())
        config.realtime = false; // the live engine plays the whole mix: a track export renders offline
    else if (const auto* p = realtimeOnlyPlugin (project, ctx.realtimeOnly); p != nullptr && ! config.realtime)
    {
        if (config.forceOffline)
            why = p->name + " says it can only render in real time; Offline mode renders it offline anyway.";
        else
        {
            config.realtime = true;
            why = p->name + " can only render in real time, so this bounce plays in real time.";
        }
    }
    rangeFrames = std::max<std::int64_t> (0, sampleAt (project, config.range.to) - sampleAt (project, config.range.from));
    if (config.secondCyclePass)
    {
        if (config.realtime)
        {
            config.secondCyclePass = false;
            why += std::string (why.empty() ? "" : " ") + "A real-time bounce plays the range once (no second cycle pass).";
        }
        else
        {
            project.info.cycle.enabled = true;
            project.info.cycle.start = config.range.from;
            project.info.cycle.end = config.range.to;
            config.includeTail = false; // the tail wraps into the start instead
            firstPass = rangeFrames;
        }
    }
    audio.sampleRate = project.info.sampleRate;
    audio.channels.resize (2);
    for (auto& c : audio.channels)
        c.reserve (static_cast<std::size_t> (rangeFrames));
    if (! config.realtime && ctx.media != nullptr && rangeFrames > 0)
    {
        OfflineRenderOptions options;
        options.from = config.range.from;
        options.plugins = ctx.plugins;
        options.projectFolder = ctx.projectFolder;
        options.soloed = config.soloTracks;
        renderer = std::make_unique<OfflineRenderer> (project, *ctx.media, options);
    }
}

BounceJob::~BounceJob() { endRealtime(); }

void BounceJob::append (const float* left, const float* right, std::int64_t frames)
{
    auto& l = audio.channels[0];
    auto& r = audio.channels[1];
    const auto have = static_cast<std::int64_t> (l.size());
    l.insert (l.end(), left, left + frames);
    r.insert (r.end(), right, right + frames);
    const auto inRange = std::clamp<std::int64_t> (rangeFrames - have, 0, frames);
    if (config.includeTail && frames > inRange)
        tail.add (left + inRange, right + inRange, frames - inRange);
    if (have + frames >= rangeFrames && (! config.includeTail || tail.done()))
    {
        const auto keep = static_cast<std::size_t> (rangeFrames + (config.includeTail ? tail.length() : 0));
        l.resize (keep);
        r.resize (keep);
        complete = true;
    }
}

bool BounceJob::step()
{
    if (complete || stopped)
        return false;
    if (rangeFrames <= 0)
    {
        complete = true;
        return false;
    }
    return config.realtime ? stepRealtime() : stepOffline();
}

void BounceJob::render()
{
    while (step())
    {
    }
}

bool BounceJob::stepOffline()
{
    if (renderer == nullptr || ! renderer->ok())
    {
        stopped = true;
        return false;
    }
    std::vector<float> left, right;
    if (firstPass > 0)
    {
        // The first time round the cycle only fills the effects; it isn't kept.
        const auto n = std::min (sliceFrames, firstPass);
        if (! renderer->render (n, left, right))
        {
            stopped = true;
            return false;
        }
        firstPass -= n;
        return true;
    }
    const auto have = static_cast<std::int64_t> (audio.channels[0].size());
    const auto n = have < rangeFrames ? std::min (sliceFrames, rangeFrames - have) : sliceFrames;
    if (! renderer->render (n, left, right))
    {
        stopped = true;
        return false;
    }
    append (left.data(), right.data(), static_cast<std::int64_t> (left.size()));
    return ! complete;
}

void BounceJob::startRealtime()
{
    auto& live = *ctx.live;
    live.stop();
    live.setCycleSuppressed (true);
    latency = live.compensationLatency();
    capture = std::make_shared<Engine::Capture>();
    // The live engine runs at the device rate; finish() converts to the file's rate.
    const double scale = live.sampleRate() / static_cast<double> (project.info.sampleRate);
    auto at = [&] (Ticks t) { return std::llround (project.tempo.tickToSample (static_cast<double> (t)) * scale); };
    capture->startSample = at (config.range.from);
    rangeFrames = std::max<std::int64_t> (0, at (config.range.to) - capture->startSample);
    tail = TailDetector (live.sampleRate());
    audio.sampleRate = live.sampleRate();
    const auto capacity = static_cast<std::size_t> (rangeFrames + latency + (config.includeTail ? tail.limit() : 0));
    capture->left.assign (capacity, 0.0f);
    capture->right.assign (capacity, 0.0f);
    live.startCapture (capture);
    live.play (config.range.from);
    playing = true;
}

void BounceJob::endRealtime()
{
    if (! playing || ctx.live == nullptr)
        return;
    playing = false;
    ctx.live->stop();
    ctx.live->stopCapture();
    ctx.live->setCycleSuppressed (false);
}

bool BounceJob::stepRealtime()
{
    if (ctx.live == nullptr)
    {
        stopped = true;
        return false;
    }
    if (! playing)
    {
        startRealtime();
        return true;
    }
    const auto written = capture->written.load (std::memory_order_acquire);
    if (written > taken)
    {
        const auto from = std::max (taken, latency);
        if (written > from)
            append (capture->left.data() + from, capture->right.data() + from, written - from);
        taken = written;
    }
    if (complete)
    {
        endRealtime();
        return false;
    }
    const auto capacity = static_cast<std::int64_t> (capture->left.size());
    const bool stoppedOutside = rolled && ctx.live->state() == TransportState::stopped;
    rolled = rolled || ctx.live->state() != TransportState::stopped;
    if (taken >= capacity || stoppedOutside)
    {
        // Stopped from outside (or out of room): finish with what was played, if the range is in.
        if (static_cast<std::int64_t> (audio.channels[0].size()) >= rangeFrames)
        {
            auto& l = audio.channels[0];
            const auto keep = static_cast<std::size_t> (rangeFrames + (config.includeTail ? tail.length() : 0));
            l.resize (std::min (l.size(), keep));
            audio.channels[1].resize (l.size());
            complete = true;
        }
        else
            stopped = true;
        endRealtime();
        return false;
    }
    return true;
}

double BounceJob::progress() const noexcept
{
    if (complete)
        return 1.0;
    if (rangeFrames <= 0)
        return 0.0;
    const auto done = static_cast<double> (audio.channels[0].size());
    if (config.secondCyclePass)
        return std::min (1.0, (static_cast<double> (rangeFrames - firstPass) + done) / (2.0 * static_cast<double> (rangeFrames)));
    return std::min (1.0, done / static_cast<double> (rangeFrames));
}

void BounceJob::cancel()
{
    stopped = true;
    endRealtime();
}

namespace
{
/** WAV, AIFF or FLAC: `mix` holds the channels this file gets (one for split and mono files). */
bool writePcmFile (const std::filesystem::path& file, const media::AudioData& mix, const WavFormat& format, const BounceSettings& s,
                   double projectRate, const AudioEncoder* encoder, std::string& error)
{
    const auto ext = file.extension().string();
    const bool wav = ext == ".wav";
    const int bits = wav ? format.bitDepth : std::min (format.bitDepth, 24);
    const double rate = format.sampleRate > 0 ? format.sampleRate : projectRate;
    media::AudioData data = std::abs (rate - mix.sampleRate) > 0.5 ? media::resample (mix, rate) : mix;
    // Sample-rate conversion comes before dither (W-9 step 4).
    if (dithers (s.dither, bits))
        addDither (data, bits, s.ditherSeed);
    if (wav)
        return media::writeWav (file, data, media::formatForBitDepth (bits), &error);
    if (encoder == nullptr)
    {
        error = "this build can't write " + ext.substr (1) + " files";
        return false;
    }
    return encoder->encode (file, data, bits, error);
}

bool writeMp3File (const std::filesystem::path& file, const media::AudioData& mix, const Mp3Format& format, double projectRate, std::string& error)
{
    const auto rate = mp3SampleRate (format, projectRate);
    const media::AudioData data = std::abs (rate - mix.sampleRate) > 0.5 ? media::resample (mix, rate) : mix;
    WeftMp3Settings settings {};
    settings.sample_rate = static_cast<int> (std::lround (rate));
    settings.channels = static_cast<int> (std::clamp<std::size_t> (data.channels.size(), 1, 2));
    const auto bpm = format.bpm > 0 ? std::to_string (static_cast<int> (std::lround (format.bpm))) : std::string();
    settings.bpm = bpm.c_str();
    settings.vbr = format.vbr ? 1 : 0;
    settings.bitrate_kbps = format.bitrateKbps;
    settings.vbr_quality = format.vbrQuality;
    settings.title = format.title.c_str();
    settings.artist = format.artist.c_str();
    settings.album = format.album.c_str();
    settings.year = format.year.c_str();
    std::array<char, 256> err {};
    auto* enc = weft_mp3_open (utf8 (file).c_str(), &settings, err.data(), err.size());
    if (enc == nullptr)
    {
        error = err.data();
        return false;
    }
    const auto frames = static_cast<std::size_t> (data.numFrames());
    const auto* right = data.channels.size() > 1 ? data.channels[1].data() : data.channels[0].data();
    if (frames > 0 && weft_mp3_write (enc, data.channels[0].data(), right, frames, err.data(), err.size()) != 0)
    {
        weft_mp3_abort (enc);
        error = err.data();
        return false;
    }
    if (weft_mp3_finish (enc, err.data(), err.size()) != 0)
    {
        error = err.data();
        return false;
    }
    return true;
}
} // namespace

BounceResult BounceJob::finish()
{
    BounceResult result;
    if (stopped || ! complete)
    {
        result.cancelled = true;
        result.error = stopped ? "Bounce cancelled." : "The bounce hasn't finished rendering.";
        return result;
    }
    result.frames = audio.numFrames(); // at the render rate
    media::AudioData mix = audio;
    if (config.channels == FileChannels::mono && mix.channels.size() == 2)
    {
        for (std::size_t i = 0; i < mix.channels[0].size(); ++i)
            mix.channels[0][i] = 0.5f * (mix.channels[0][i] + mix.channels[1][i]);
        mix.channels.resize (1);
    }
    float peak = 0;
    for (const auto& c : mix.channels)
        for (const auto x : c)
            peak = std::max (peak, std::abs (x));
    result.peakDb = peak > 0 ? 20.0 * std::log10 (static_cast<double> (peak)) : -1000.0;
    // Normalize (D-W9-7) to a 0 dBFS peak, before conversion and dither.
    if (peak > 0 && (config.normalize == Normalize::on || (config.normalize == Normalize::overloadOnly && peak > 1.0f)))
    {
        const auto gain = 1.0f / peak;
        for (auto& c : mix.channels)
            for (auto& x : c)
                x *= gain;
        result.gainDb = -result.peakDb;
        peak = 1.0f;
    }
    result.clipped = peak > 1.0f;

    std::error_code ec;
    std::filesystem::create_directories (config.folder, ec);
    const auto targets = bounceTargets (config);
    for (const auto& wanted : targets)
    {
        const auto file = config.replace ? wanted : keepBothPath (wanted);
        std::string error;
        const auto ext = wanted.extension().string();
        const double projectRate = project.info.sampleRate;
        bool ok = false;
        if (ext == ".mp3")
        {
            auto format = *config.mp3;
            if (config.includeTempo)
                format.bpm = project.tempo.bpmAt (config.range.from);
            ok = writeMp3File (file, mix, format, projectRate, error);
        }
        else
        {
            const auto& format = ext == ".wav" ? *config.wav : ext == ".aif" ? *config.aiff : *config.flac;
            const auto* encoder = ctx.encoder.get();
            if (config.channels == FileChannels::split && mix.channels.size() == 2)
            {
                // "name.L.wav" and "name.R.wav": the stem's last letter says which.
                const auto stem = wanted.stem().string();
                media::AudioData one;
                one.sampleRate = mix.sampleRate;
                one.channels.push_back (mix.channels[! stem.empty() && stem.back() == 'R' ? 1 : 0]);
                ok = writePcmFile (file, one, format, config, projectRate, encoder, error);
            }
            else
                ok = writePcmFile (file, mix, format, config, projectRate, encoder, error);
        }
        if (! ok)
        {
            std::filesystem::remove (file, ec);
            result.error = "Couldn't write " + utf8 (file.filename()) + ": " + error;
            return result;
        }
        result.files.push_back (file);
    }
    result.ok = true;
    std::string names;
    for (const auto& f : result.files)
        names += (names.empty() ? "" : ", ") + utf8 (f.filename());
    result.message = "Bounced " + names + ".";
    if (std::abs (result.gainDb) > 0.05)
    {
        std::array<char, 64> text {};
        std::snprintf (text.data(), text.size(), " Normalized by %+.1f dB.", result.gainDb);
        result.message += text.data();
    }
    if (result.clipped)
        result.message += " " + clipMessage (result.peakDb) + ".";
    return result;
}

int regionsMissingMedia (const model::ProjectModel& model, const MediaPool& media)
{
    int missing = 0;
    for (const auto& [id, r] : model.regions)
    {
        if (r->isMidi)
            continue;
        const auto* ref = model.mediaRef (r->audio.media);
        if (ref == nullptr || ! media.available (ref->hash))
            ++missing;
    }
    return missing;
}

bool isRealtimeOnlyCategory (const std::string& category)
{
    std::size_t start = 0;
    while (start <= category.size())
    {
        const auto end = std::min (category.find ('|', start), category.size());
        if (category.compare (start, end - start, "OnlyRT") == 0)
            return true;
        start = end + 1;
    }
    return false;
}
} // namespace weft::engine
