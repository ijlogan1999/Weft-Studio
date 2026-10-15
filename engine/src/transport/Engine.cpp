#include "weft/engine/transport/Engine.h"

#include "weft/commands/Session.h"
#include "weft/engine/dsp/WeftKeys.h"
#include "weft/media/Resample.h"
#include "weft/engine/record/RecordLayout.h"
#include "weft/media/Hash.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>

namespace weft::engine
{
namespace
{
ClickDuring duringFromString (const std::string& s)
{
    if (s == "play")
        return ClickDuring::play;
    if (s == "both")
        return ClickDuring::both;
    return ClickDuring::record;
}
} // namespace

TransportSettings TransportSettings::fromDefaults (const model::MetronomeSettings& m)
{
    TransportSettings s;
    s.metronomeOn = m.enabled;
    s.metronomeLevelDb = m.levelDb;
    s.metronomeDuring = duringFromString (m.during);
    s.countInBars = std::clamp (m.countInBars, 0, 2);
    return s;
}

Engine::Engine (MediaPool& media, InstrumentRack& instruments)
    : mediaPool (media),
      rack (instruments),
      streamer ({ [this] { return published.position.load (std::memory_order_acquire); },
                  [this] { return published.frames.load (std::memory_order_acquire); },
                  [this] { return running.load(); } })
{
    setSettings (currentSettings);
    instrumentFactory = [] (const model::Track& track) -> std::shared_ptr<Instrument> {
        const auto& inst = track.strip.instrument;
        if (! inst || (inst->format == "builtin" && inst->pluginUid == WeftKeys::pluginUid))
            return std::make_shared<WeftKeys>();
        return nullptr; // hosted instruments: M6
    };
}

Engine::~Engine()
{
    streamer.stopThread();
    current.store (nullptr);
    for (auto& p : passes)
        p.writer->finish();
}

// Message thread ------------------------------------------------------------------

void Engine::setProject (const model::ProjectModel& model)
{
    lastModel = model;
    refresh();
}

void Engine::refresh()
{
    if (lastModel && instrumentFactory)
        for (const auto& [id, track] : lastModel->tracks)
            if (track->kind == model::TrackKind::instrument && ! rack.find (id))
                if (auto instrument = instrumentFactory (*track))
                    rack.set (id, std::move (instrument));
    if (lastModel)
        publish (compileSnapshot (*lastModel, rate, std::max (1, maxBlock), mediaPool, rack, local));
}

void Engine::publish (std::unique_ptr<EngineSnapshot> snapshot)
{
    // Tell the disk thread where streamed media plays.
    auto plan = std::make_shared<MediaStreamer::Plan>();
    plan->cycle = snapshot->cycleEnabled;
    plan->cycleStart = snapshot->cycleStart;
    plan->cycleEnd = snapshot->cycleEnd;
    for (const auto& ch : snapshot->channels)
        for (const auto& seg : ch.segments)
            if (! seg.media->resident())
            {
                const auto it = std::find_if (snapshot->media.begin(), snapshot->media.end(), [&] (const auto& m) { return m.get() == seg.media; });
                if (it != snapshot->media.end())
                    plan->windows.push_back ({ *it, seg.start, seg.end, seg.sourceAtRegionStart + static_cast<double> (seg.start - seg.regionStart) * seg.sourceStep,
                                               seg.sourceStep });
            }
    streamer.setPlan (std::move (plan));

    snapshot->serial = nextSerial++;
    current.store (snapshot.get(), std::memory_order_release);
    if (live)
        retired.push_back ({ std::move (live) });
    live = std::move (snapshot);
    collectGarbage();
}

void Engine::preview (std::shared_ptr<const media::AudioData> audio)
{
    std::shared_ptr<const media::AudioData> playable = audio;
    if (audio && audio->sampleRate != rate)
        playable = std::make_shared<const media::AudioData> (media::resample (*audio, rate));
    const auto serial = previewSerial.load() + 1;
    previews.push_back ({ serial, playable });
    previewNext.store (playable.get(), std::memory_order_release);
    previewSerial.store (serial, std::memory_order_release);
    collectGarbage();
}

void Engine::collectGarbage()
{
    {
        // Previews older than the one the audio thread last picked up are no longer read.
        const auto acked = running ? previewAcked.load (std::memory_order_acquire) : previewSerial.load();
        std::erase_if (previews, [acked] (const Preview& p) { return p.serial < acked; });
    }
    if (! running)
    {
        retired.clear();
        return;
    }
    // The audio thread acknowledges the serial of the snapshot it loaded at the start of
    // its latest block; anything older can't be in use any more.
    const auto acked = ackedSerial.load (std::memory_order_acquire);
    std::erase_if (retired, [acked] (const Retired& r) { return r.snapshot->serial < acked; });
}

bool Engine::send (const Command& c) { return commands.push (c); }

std::int64_t Engine::toSamples (Ticks t) const { return live ? live->tickToSample (t) : 0; }

double Engine::positionTicks() const noexcept { return live ? live->tempo.sampleToTick (static_cast<double> (positionSamples())) : 0.0; }

void Engine::play() { send ({ CommandType::play, fromPlayhead }); }
void Engine::play (Ticks from)
{
    streamer.hint (toSamples (from));
    send ({ CommandType::play, toSamples (from) });
}
void Engine::stop() { send ({ CommandType::stop }); }
void Engine::locate (Ticks to)
{
    streamer.hint (toSamples (to));
    send ({ CommandType::locate, toSamples (to) });
}

void Engine::setDiskThread (bool on)
{
    diskThread = on;
    if (! on)
        streamer.stopThread();
    else if (running)
        streamer.startThread();
}

void Engine::togglePlay()
{
    if (state() == TransportState::stopped)
        play();
    else
        stop();
}

RecordResult Engine::record() { return record (static_cast<Ticks> (std::llround (positionTicks()))); }

RecordResult Engine::record (Ticks from)
{
    RecordResult result;
    if (local.armed.empty())
    {
        // AT-TRN-4: record with nothing armed just plays.
        play (from);
        result.started = true;
        result.message = "No tracks armed";
        return result;
    }
    if (lastModel && lastModel->info.sampleRate != static_cast<int> (std::lround (rate)))
    {
        // D-W0-1: playback is resampled, recording waits until the rates match.
        result.message = "Recording is off: the audio device isn't running at the project's sample rate.";
        return result;
    }
    if (! live)
        return result;

    auto newPass = std::make_unique<RecordPass>();
    const auto fifoFrames = static_cast<std::int64_t> (std::ceil (std::max (1.0, recordSeconds) * rate));
    for (const auto& ch : live->channels)
    {
        if (! ch.armed)
            continue;
        const auto* track = lastModel->track (ch.trackId);
        const auto name = track != nullptr ? track->name : std::string();
        if (ch.kind == model::TrackKind::audio && ch.inputFirst >= 0 && ch.inputFirst < numInputs)
        {
            auto lane = std::make_unique<RecordPass::AudioLane>();
            lane->trackId = ch.trackId;
            lane->trackName = name;
            lane->inputFirst = ch.inputFirst;
            lane->channels = ch.sourceChannels == 1 ? 1 : 2;
            lane->fifo = std::make_unique<AudioFifo> (lane->channels, fifoFrames);
            newPass->audio.push_back (std::move (lane));
        }
        else if (ch.kind == model::TrackKind::instrument)
        {
            auto lane = std::make_unique<RecordPass::MidiLane>();
            lane->trackId = ch.trackId;
            lane->trackName = name;
            lane->midiChannel = ch.midiChannel;
            lane->events.resize (65536);
            newPass->midi.push_back (std::move (lane));
        }
    }
    if (newPass->audio.empty() && newPass->midi.empty())
    {
        play (from);
        result.started = true;
        result.message = "No armed track has an input";
        return result;
    }

    newPass->recordStart = toSamples (from);
    newPass->latency = recordLatency();
    if (lastModel->info.punch.enabled && lastModel->info.punch.end > lastModel->info.punch.start)
    {
        // W-1d: write from punch-in to punch-out with 10 ms handles; capture runs `latency` later
        // than the timeline it represents.
        const auto handle = std::llround (0.010 * rate);
        newPass->punch = true;
        newPass->captureStart = toSamples (lastModel->info.punch.start) - handle + newPass->latency;
        newPass->captureEnd = toSamples (lastModel->info.punch.end) + handle + newPass->latency;
    }
    if (live->cycleEnabled && ! newPass->punch)
    {
        newPass->cycle = true;
        newPass->cycleStart = live->cycleStart;
        newPass->cycleEnd = live->cycleEnd;
    }

    std::unique_ptr<RecordWriter> writer;
    if (! newPass->audio.empty())
    {
        if (projectFolder.empty())
        {
            result.message = "Save the project before recording audio.";
            return result;
        }
        media::BextInfo bext;
        const auto now = std::time (nullptr);
        std::tm localTime {};
#if defined(_WIN32)
        localtime_s (&localTime, &now);
#else
        localtime_r (&now, &localTime);
#endif
        char date[16] {}, clock[16] {};
        std::strftime (date, sizeof date, "%Y-%m-%d", &localTime);
        std::strftime (clock, sizeof clock, "%H:%M:%S", &localTime);
        bext.date = date;
        bext.time = clock;
        bext.timeReference = static_cast<std::uint64_t> (std::max<std::int64_t> (0, newPass->recordStart));
        writer = std::make_unique<RecordWriter> (*newPass, projectFolder / "Recovery");
        std::string error;
        if (! writer->open (rate, media::formatForBitDepth (lastModel->info.recordBitDepth), bext, &error))
        {
            result.message = "Couldn't create the recording file: " + error;
            return result;
        }
    }

    streamer.hint (newPass->recordStart);
    Command c { CommandType::record, newPass->recordStart, currentSettings.countInBars };
    c.pass = newPass.get();
    if (! send (c))
        return result;
    if (writer && writerThread)
        writer->startThread();
    passes.push_back ({ std::move (newPass), std::move (writer) });
    result.started = result.recording = true;
    return result;
}

void Engine::stopRecording() { send ({ CommandType::stopRecording }); }

bool Engine::sendMidi (std::uint8_t status, std::uint8_t data1, std::uint8_t data2, std::int64_t deviceFrame) noexcept
{
    return midiIn.push ({ deviceFrame, status, data1, data2 });
}

bool Engine::audition (const Id& trackId, int channel, int pitch, int velocity, double seconds)
{
    const auto slot = rack.find (trackId);
    if (slot == nullptr)
        return false;
    InstrumentSlot::Audition a;
    a.channel = static_cast<std::uint8_t> (std::clamp (channel, 1, 16));
    a.pitch = static_cast<std::uint8_t> (std::clamp (pitch, 0, 127));
    a.velocity = static_cast<std::uint8_t> (std::clamp (velocity, 1, 127));
    a.frames = static_cast<std::int32_t> (std::clamp (seconds, 0.0, 1.0) * rate);
    return slot->auditions.push (a);
}

std::optional<RecordingOutcome> Engine::finishRecording (commands::Session& session)
{
    if (passes.empty() || ! passes.front().pass->finished.load (std::memory_order_acquire))
        return std::nullopt;
    auto entry = std::move (passes.front());
    passes.erase (passes.begin());
    auto& p = *entry.pass;

    RecordingOutcome out;
    const auto& tempo = live->tempo;
    const auto& signatures = live->signatures;
    std::vector<commands::RecordedAudio> audio;
    std::vector<commands::RecordedMidi> midi;
    std::vector<std::string> problems;

    if (entry.writer)
    {
        if (! entry.writer->finish())
            problems.push_back ("Some audio couldn't be written to disk.");
        const auto mediaDir = projectFolder / "Media";
        std::error_code ec;
        std::filesystem::create_directories (mediaDir, ec);
        for (std::size_t i = 0; i < p.audio.size(); ++i)
        {
            const auto& lane = *p.audio[i];
            out.droppedFrames += lane.dropped.load();
            const auto frames = entry.writer->framesWritten (i);
            const auto& file = entry.writer->file (i);
            const auto layout = layoutAudio (p, frames, tempo, signatures);
            if (layout.takes.empty())
            {
                std::filesystem::remove (file, ec); // W-1a step 9
                continue;
            }
            std::string error;
            const auto hash = media::hashFile (file, &error);
            if (! hash)
            {
                problems.push_back ("Couldn't read the recording back: " + error);
                continue;
            }
            const auto dest = mediaDir / media::mediaFileName (*hash);
            if (std::filesystem::exists (dest, ec))
                std::filesystem::remove (file, ec);
            else
            {
                std::filesystem::rename (file, dest, ec);
                if (ec)
                {
                    ec.clear();
                    std::filesystem::copy_file (file, dest, ec);
                    if (ec)
                    {
                        problems.push_back ("Couldn't move the recording into Media: " + ec.message());
                        continue;
                    }
                    std::filesystem::remove (file, ec);
                }
            }
            mediaPool.load (mediaDir, *hash);

            commands::RecordedAudio r;
            r.trackId = lane.trackId;
            r.trackName = lane.trackName;
            r.trackChannels = lane.channels;
            r.media.hash = *hash;
            r.media.sampleRate = static_cast<int> (std::lround (rate));
            r.media.channels = lane.channels;
            r.media.lengthSamples = frames;
            r.media.bitDepth = lastModel ? lastModel->info.recordBitDepth : 24;
            r.media.sizeBytes = static_cast<std::int64_t> (std::filesystem::file_size (dest, ec));
            r.takes = layout.takes;
            r.takeFolder = layout.takeFolder;
            r.folderStart = layout.folderStart;
            r.folderEnd = layout.folderEnd;
            audio.push_back (std::move (r));
            ++out.files;
        }
    }
    for (const auto& lane : p.midi)
    {
        auto m = layoutMidi (p, *lane, tempo, signatures, currentSettings.midiMergeLinear, currentSettings.midiMergeCycle);
        if (! m.notes.empty() || ! m.controllers.empty())
            midi.push_back (std::move (m));
    }

    if (! audio.empty() || ! midi.empty())
    {
        const auto result = session.commitRecording (audio, midi);
        out.committed = result.ok;
        if (! result.ok)
            problems.push_back (result.error);
        setProject (session.model());
    }
    if (out.droppedFrames > 0)
        problems.push_back ("The disk couldn't keep up: " + std::to_string (out.droppedFrames) + " samples were lost.");
    for (const auto& m : problems)
        out.message += (out.message.empty() ? "" : " ") + m;
    return out;
}

void Engine::setSettings (const TransportSettings& s)
{
    currentSettings = s;
    Command c { CommandType::settings };
    c.returnToStart = s.returnToStartOnStop;
    c.clickOn = s.metronomeOn;
    c.clickInPlay = s.metronomeDuring != ClickDuring::record;
    c.clickInRecord = s.metronomeDuring != ClickDuring::play;
    c.clickGain = dbToGain (s.metronomeLevelDb);
    send (c);
}

void Engine::setArmedTracks (std::set<Id> trackIds)
{
    if (lastModel)
        std::erase_if (trackIds, [this] (const Id& id) {
            const auto* t = lastModel->track (id);
            return t != nullptr && ! LocalMixState::canArm (*t); // buses and the master can't record (F-TRK-11)
        });
    local.armed = std::move (trackIds);
    refresh();
}

void Engine::setLocalView (const LocalMixState& view)
{
    local.soloed = view.soloed;
    local.selectedTrack = view.selectedTrack;
    setArmedTracks (view.armed); // recompiles
}

void Engine::setSoloedTracks (std::set<Id> trackIds)
{
    local.soloed = std::move (trackIds);
    refresh();
}

void Engine::setSelectedTrack (const Id& trackId)
{
    local.selectedTrack = trackId;
    refresh();
}

Engine::Meter Engine::takeMeter (const Id& trackId)
{
    if (! live)
        return {};
    if (trackId.empty())
        return { live->masterMeter.output.exchange (0), 0 };
    const auto i = live->channelIndex (trackId);
    if (i < 0)
        return {};
    auto& m = live->meters[static_cast<std::size_t> (i)];
    return { m.output.exchange (0), m.input.exchange (0) };
}

void Engine::setRecordCapacitySeconds (double seconds) { recordSeconds = std::max (0.0, seconds); }

// Device lifecycle ------------------------------------------------------------------

void Engine::prepare (const DeviceConfig& config)
{
    const double oldRate = rate;
    rate = config.sampleRate;
    maxBlock = config.maxBlockSize;
    numInputs = config.numInputChannels;
    numOutputs = config.numOutputChannels;

    const auto block = static_cast<std::size_t> (maxBlock);
    mix.assign (3, std::vector<float> (block)); // left, right, click
    mixPtrs = { mix[0].data(), mix[1].data(), mix[2].data() };
    deviceLatency = config.inputLatency + config.outputLatency;
    events.clear();
    events.reserve (1024);
    metronome.prepare (rate);
    rack.prepare (rate, maxBlock);

    if (oldRate != rate)
    {
        // Positions are samples at the device rate; ticks stay where they were.
        const double k = rate / oldRate;
        const auto rescale = [k] (std::int64_t s) { return std::llround (static_cast<double> (s) * k); };
        position = rescale (position);
        playStart = rescale (playStart);
        recordStart = rescale (recordStart);
        published.position.store (position);
    }
    running = true;
    if (lastModel && (! live || live->sampleRate != rate))
        refresh();
    if (diskThread)
        streamer.startThread();
}

void Engine::release()
{
    running = false;
    streamer.stopThread();
    collectGarbage();
}

// Audio thread ------------------------------------------------------------------------

void Engine::flushAllNotes (const EngineSnapshot* snap) noexcept
{
    if (snap == nullptr)
        return;
    for (const auto& c : snap->channels)
        if (c.instrument)
            c.instrument->flushPending = true;
}

void Engine::finishCapture() noexcept
{
    if (pass == nullptr)
        return;
    pass->stopSample.store (position, std::memory_order_relaxed);
    pass->finished.store (true, std::memory_order_release);
    pass = nullptr;
}

void Engine::apply (const Command& c, const EngineSnapshot* snap) noexcept
{
    switch (c.type)
    {
        case CommandType::play:
            finishCapture();
            if (c.a != fromPlayhead)
                position = c.a;
            playStart = position;
            transport = TransportState::playing;
            flushAllNotes (snap);
            break;
        case CommandType::stop:
            if (transport == TransportState::stopped)
                break;
            finishCapture();
            transport = TransportState::stopped;
            if (returnToStart)
                position = playStart;
            flushAllNotes (snap);
            break;
        case CommandType::locate:
            finishCapture();
            if (transport == TransportState::countIn || transport == TransportState::recording)
                transport = TransportState::playing;
            position = playStart = c.a;
            flushAllNotes (snap);
            metronome.silence();
            break;
        case CommandType::record:
        {
            if (snap == nullptr)
                break;
            finishCapture();
            recordStart = c.a == fromPlayhead ? position : c.a;
            const auto recordTick = static_cast<Ticks> (std::llround (snap->tempo.sampleToTick (static_cast<double> (recordStart))));
            const auto bars = std::clamp<std::int64_t> (c.b, 0, 2);
            const auto barLength = snap->signatures.barLength (snap->signatures.barAt (recordTick).bar);
            position = bars > 0 ? snap->tickToSample (recordTick - bars * barLength) : recordStart;
            playStart = recordStart;
            pass = c.pass;
            transport = position < recordStart ? TransportState::countIn : TransportState::recording;
            flushAllNotes (snap);
            break;
        }
        case CommandType::stopRecording:
            finishCapture();
            if (transport == TransportState::recording || transport == TransportState::countIn)
                transport = TransportState::playing;
            break;
        case CommandType::settings:
            returnToStart = c.returnToStart;
            clickOn = c.clickOn;
            clickInPlay = c.clickInPlay;
            clickInRecord = c.clickInRecord;
            clickGain = c.clickGain;
            break;
    }
}

void Engine::process (const float* const* inputs, float* const* outputs, int numFrames) noexcept
{
    const auto started = std::chrono::steady_clock::now();
    for (int ch = 0; ch < numOutputs; ++ch)
        std::fill_n (outputs[ch], numFrames, 0.0f);
    if (numFrames <= 0 || numFrames > maxBlock)
        return;

    const auto* snap = current.load (std::memory_order_acquire);
    if (snap != nullptr)
        ackedSerial.store (snap->serial, std::memory_order_release);
    while (auto c = commands.pop())
        apply (*c, snap);
    takeLiveMidi (numFrames);

    if (snap != nullptr)
    {
        for (auto& m : mix)
            std::fill_n (m.data(), numFrames, 0.0f);

        int offset = 0;
        while (offset < numFrames)
        {
            const int remaining = numFrames - offset;
            switch (transport)
            {
                case TransportState::stopped:
                    renderSpan (*snap, inputs, position, offset, remaining, false);
                    renderClicks (*snap, position, offset, remaining, false);
                    offset = numFrames;
                    break;

                case TransportState::countIn:
                {
                    // Count-in: click only (D-ARCH-13); regions start playing at the record position.
                    const auto n = static_cast<int> (std::min<std::int64_t> (remaining, recordStart - position));
                    if (n > 0)
                    {
                        renderSpan (*snap, inputs, position, offset, n, false);
                        renderClicks (*snap, position, offset, n, true);
                        position += n;
                        offset += n;
                    }
                    if (position >= recordStart)
                        transport = pass != nullptr ? TransportState::recording : TransportState::playing;
                    break;
                }

                case TransportState::playing:
                case TransportState::recording:
                {
                    auto end = position + remaining;
                    // Cycle (F-TRN-6): wrap when playback reaches the end; the end is exclusive.
                    // Recording cycles too: each pass becomes a take (W-1c).
                    const bool cycling = snap->cycleEnabled && position < snap->cycleEnd;
                    if (cycling)
                        end = std::min (end, snap->cycleEnd);
                    const auto n = static_cast<int> (end - position);
                    renderSpan (*snap, inputs, position, offset, n, true);
                    const bool audible = clickOn && (transport == TransportState::recording ? clickInRecord : clickInPlay);
                    renderClicks (*snap, position, offset, n, audible);
                    if (transport == TransportState::recording && pass != nullptr)
                        captureSpan (inputs, position, offset, n);
                    position = end;
                    offset += n;
                    if (cycling && position >= snap->cycleEnd)
                    {
                        if (transport == TransportState::recording && pass != nullptr)
                            captureWrap (position);
                        position = snap->cycleStart;
                        flushAllNotes (snap);
                    }
                    break;
                }
            }
        }

        const float master = snap->masterGain;
        float masterPeak = 0;
        for (std::size_t ch = 0; ch < 2; ++ch)
            for (int i = 0; i < numFrames; ++i)
            {
                const float v = mix[ch][static_cast<std::size_t> (i)] * master;
                masterPeak = std::max (masterPeak, std::abs (v));
                if (static_cast<int> (ch) < numOutputs)
                    outputs[ch][i] = v + mix[2][static_cast<std::size_t> (i)];
            }
        if (numOutputs == 1) // mono device: fold right into left
            for (int i = 0; i < numFrames; ++i)
                outputs[0][i] += mix[1][static_cast<std::size_t> (i)] * master;
        if (masterPeak > snap->masterMeter.output.load (std::memory_order_relaxed))
            snap->masterMeter.output.store (masterPeak, std::memory_order_relaxed);
    }

    // File preview, after the master fader.
    if (const auto serial = previewSerial.load (std::memory_order_acquire); serial != previewSeen)
    {
        previewSeen = serial;
        previewAudio = previewNext.load (std::memory_order_acquire);
        previewPos = 0;
        previewAcked.store (serial, std::memory_order_release);
    }
    if (previewAudio != nullptr)
    {
        const auto& chans = previewAudio->channels;
        const auto n = static_cast<int> (std::clamp<std::int64_t> (previewAudio->numFrames() - previewPos, 0, numFrames));
        for (int ch = 0; ch < std::min (numOutputs, 2); ++ch)
        {
            const auto& src = chans[std::min<std::size_t> (static_cast<std::size_t> (ch), chans.size() - 1)];
            for (int i = 0; i < n; ++i)
                outputs[ch][i] += src[static_cast<std::size_t> (previewPos + i)];
        }
        previewPos += n;
        if (previewPos >= previewAudio->numFrames())
            previewAudio = nullptr;
    }
    published.previewing.store (previewAudio != nullptr, std::memory_order_relaxed);

    frameCounter += numFrames;
    published.state.store (static_cast<int> (transport), std::memory_order_release);
    published.position.store (position, std::memory_order_release);
    published.frames.store (frameCounter, std::memory_order_release);

    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
    const double load = elapsed.count() * rate / numFrames;
    loadAverage += 0.2 * (load - loadAverage);
    published.load.store (loadAverage, std::memory_order_relaxed);
    if (load > 1.0)
        published.dropouts.fetch_add (1, std::memory_order_relaxed);
}

void Engine::mixSegments (const ChannelPlan& channel, std::int64_t from, int frames, float* left, float* right) noexcept
{
    const auto to = from + frames;
    // Segments overlap only where regions crossfade, so any segment reaching `from` starts
    // within the longest segment's length before it.
    auto it = std::lower_bound (channel.segments.begin(), channel.segments.end(), from - channel.longestSegment,
                                [] (const AudioSegment& s, std::int64_t t) { return s.start < t; });
    bool underrun = false;
    for (; it != channel.segments.end() && it->start < to; ++it)
    {
        const auto& seg = *it;
        if (seg.end <= from)
            continue;
        const auto& source = *seg.media;
        const auto s = std::max (from, seg.start), e = std::min (to, seg.end);
        const auto numCh = source.numChannels();
        const bool direct = seg.sourceStep == 1.0 && seg.sourceAtRegionStart == std::floor (seg.sourceAtRegionStart);
        const bool mixDown = channel.sourceChannels == 1 && numCh > 1;
        const bool stereo = channel.sourceChannels != 1;

        auto gainAt = [&seg] (std::int64_t t) noexcept {
            float g = seg.gain;
            const auto intoRegion = t - seg.regionStart;
            if (intoRegion < seg.fadeIn)
                g *= fadeGain (seg.fadeInCurve, static_cast<double> (intoRegion) / static_cast<double> (seg.fadeIn));
            const auto toEnd = seg.regionEnd - t;
            if (toEnd <= seg.fadeOut)
                g *= fadeGain (seg.fadeOutCurve, static_cast<double> (toEnd) / static_cast<double> (seg.fadeOut));
            if (seg.duckLength > 0)
            {
                const auto into = static_cast<double> (t - seg.duckFrom) / static_cast<double> (seg.duckLength);
                g *= fadeGain (seg.duckCurve, seg.duckRising ? 1.0 - into : into);
            }
            return g;
        };
        auto put = [&] (std::int64_t t, float l, float r) noexcept {
            const auto k = static_cast<std::size_t> (t - from);
            const float g = gainAt (t);
            if (! stereo)
                left[k] += (mixDown ? 0.5f * (l + r) : l) * g;
            else
            {
                left[k] += l * g;
                right[k] += r * g;
            }
        };

        if (direct)
        {
            // Runs of frames within one block of the media.
            const auto offset = static_cast<std::int64_t> (seg.sourceAtRegionStart) - seg.regionStart;
            for (auto t = s; t < e;)
            {
                const auto k = t + offset;
                if (k < 0)
                {
                    t = std::min (e, t - k);
                    continue;
                }
                if (k >= source.numFrames())
                    break;
                const auto blockIndex = k / MediaSource::blockFrames;
                const auto within = k % MediaSource::blockFrames;
                const auto run = std::min ({ e - t, MediaSource::blockFrames - within, source.numFrames() - k });
                const float* p0 = source.block (0, blockIndex);
                if (p0 == nullptr)
                {
                    underrun = true;
                    t += run;
                    continue;
                }
                const float* p1 = numCh > 1 ? source.block (1, blockIndex) : p0;
                for (std::int64_t i = 0; i < run; ++i)
                    put (t + i, p0[within + i], p1[within + i]);
                t += run;
            }
            continue;
        }

        // Resampled read (D-ARCH-15): Catmull-Rom between samples.
        for (auto t = s; t < e; ++t)
        {
            const double pos = seg.sourceAtRegionStart + static_cast<double> (t - seg.regionStart) * seg.sourceStep;
            const auto i = static_cast<std::int64_t> (std::floor (pos));
            const auto frac = static_cast<float> (pos - static_cast<double> (i));
            float v[2] = {};
            for (int ch = 0; ch < std::min (numCh, 2); ++ch)
            {
                const float p1 = source.sample (ch, i, underrun);
                if (frac == 0.0f)
                {
                    v[ch] = p1;
                    continue;
                }
                const float p0 = source.sample (ch, i - 1, underrun), p2 = source.sample (ch, i + 1, underrun), p3 = source.sample (ch, i + 2, underrun);
                const float c1 = 0.5f * (p2 - p0);
                const float c2 = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
                const float c3 = 0.5f * (p3 - p0) + 1.5f * (p1 - p2);
                v[ch] = ((c3 * frac + c2) * frac + c1) * frac + p1;
            }
            put (t, v[0], numCh > 1 ? v[1] : v[0]);
        }
    }
    if (underrun)
        published.underruns.fetch_add (1, std::memory_order_relaxed);
}

bool Engine::monitoring (const ChannelPlan& channel) const noexcept
{
    switch (channel.monitor)
    {
        case MonitorMode::on:
            return true;
        case MonitorMode::off:
            return false;
        case MonitorMode::autoMode:
            break;
    }
    // D-W1-1: armed and stopped, or recording (a count-in counts as recording).
    return channel.armed && transport != TransportState::playing;
}

void Engine::renderSpan (const EngineSnapshot& snap, const float* const* inputs, std::int64_t from, int offset, int frames, bool timeline) noexcept
{
    const auto count = snap.channels.size();
    for (std::size_t b = 0; b < count * 2; ++b)
        std::fill_n (snap.buffers[b].data(), frames, 0.0f);

    for (std::size_t i = 0; i < count; ++i)
    {
        const auto& ch = snap.channels[i];
        float* left = snap.buffers[2 * i].data();
        float* right = snap.buffers[2 * i + 1].data();
        const bool mono = ch.sourceChannels == 1;

        // Source.
        if (timeline && ! ch.segments.empty())
            mixSegments (ch, from, frames, left, right);
        if (ch.instrument)
            renderInstrument (ch, from, offset, frames, timeline, left, right);
        if (ch.inputFirst >= 0 && ch.inputFirst < numInputs)
        {
            const float* inL = inputs[ch.inputFirst] + offset;
            const float* inR = ! mono && ch.inputFirst + 1 < numInputs ? inputs[ch.inputFirst + 1] + offset : inL;
            if (ch.armed)
            {
                float p = 0;
                for (int k = 0; k < frames; ++k)
                    p = std::max ({ p, std::abs (inL[k]), std::abs (inR[k]) });
                auto& m = snap.meters[i].input;
                if (p > m.load (std::memory_order_relaxed))
                    m.store (p, std::memory_order_relaxed);
            }
            if (monitoring (ch))
                for (int k = 0; k < frames; ++k)
                {
                    left[k] += inL[k];
                    if (! mono)
                        right[k] += inR[k];
                }
        }
        if (! ch.audible)
            continue; // muted or solo-silenced: no output and no sends (D-W6-2)

        const auto sendTo = [&] (const SendPlan& send, bool stereo) noexcept {
            float* tl = snap.buffers[2 * static_cast<std::size_t> (send.target)].data();
            float* tr = snap.buffers[2 * static_cast<std::size_t> (send.target) + 1].data();
            for (int k = 0; k < frames; ++k)
            {
                tl[k] += left[k] * send.gain;
                tr[k] += (stereo ? right[k] : left[k]) * send.gain;
            }
        };
        for (const auto& send : ch.sends)
            if (send.pre)
                sendTo (send, ! mono);

        // Fader and pan: the channel becomes stereo here.
        const float gl = ch.gain * ch.panLeft, gr = ch.gain * ch.panRight;
        for (int k = 0; k < frames; ++k)
        {
            const float l = left[k], r = mono ? left[k] : right[k];
            left[k] = l * gl;
            right[k] = r * gr;
        }
        for (const auto& send : ch.sends)
            if (! send.pre)
                sendTo (send, true);

        float p = 0;
        for (int k = 0; k < frames; ++k)
            p = std::max ({ p, std::abs (left[k]), std::abs (right[k]) });
        auto& m = snap.meters[i].output;
        if (p > m.load (std::memory_order_relaxed))
            m.store (p, std::memory_order_relaxed);

        float* outL = ch.output >= 0 ? snap.buffers[2 * static_cast<std::size_t> (ch.output)].data() : mix[0].data() + offset;
        float* outR = ch.output >= 0 ? snap.buffers[2 * static_cast<std::size_t> (ch.output) + 1].data() : mix[1].data() + offset;
        for (int k = 0; k < frames; ++k)
        {
            outL[k] += left[k];
            outR[k] += right[k];
        }
    }
}

void Engine::renderInstrument (const ChannelPlan& channel, std::int64_t from, int offset, int frames, bool timeline, float* left, float* right) noexcept
{
    const auto to = from + frames;
    const auto capacity = events.capacity();
    auto add = [this, capacity] (const MidiEvent& e) {
        if (events.size() < capacity) // never grow on the audio thread
            events.push_back (e);
    };

    auto& slot = *channel.instrument;
    events.clear();

    if (slot.flushPending)
    {
        for (std::size_t i = 0; i < slot.numActive; ++i)
            add (MidiEvent::noteOff (0, slot.active[i].channel, slot.active[i].pitch));
        slot.numActive = 0;
        slot.flushPending = false;
    }

    // Audition (F-PRL-11): sounds whatever the transport does; ends after its length.
    for (std::size_t i = 0; i < slot.numAuditioning;)
    {
        auto& a = slot.auditioning[i];
        if (a.framesLeft < frames)
        {
            add (MidiEvent::noteOff (static_cast<int> (std::max<std::int64_t> (0, a.framesLeft)), a.channel, a.pitch));
            a = slot.auditioning[--slot.numAuditioning];
        }
        else
        {
            a.framesLeft -= frames;
            ++i;
        }
    }
    while (auto a = slot.auditions.pop())
    {
        for (std::size_t i = 0; i < slot.numAuditioning; ++i)
            if (slot.auditioning[i].channel == a->channel && slot.auditioning[i].pitch == a->pitch)
            {
                add (MidiEvent::noteOff (0, a->channel, a->pitch));
                slot.auditioning[i] = slot.auditioning[--slot.numAuditioning];
                break;
            }
        if (slot.numAuditioning == InstrumentSlot::maxAuditions)
            continue;
        add (MidiEvent::noteOn (0, a->channel, a->pitch, a->velocity));
        if (a->frames < frames)
            add (MidiEvent::noteOff (a->frames, a->channel, a->pitch));
        else
            slot.auditioning[slot.numAuditioning++] = { a->frames - frames, a->channel, a->pitch };
    }

    if (timeline)
    {
        auto it = std::lower_bound (channel.notes.begin(), channel.notes.end(), from,
                                    [] (const ScheduledNote& n, std::int64_t t) { return n.on < t; });
        for (; it != channel.notes.end() && it->on < to; ++it)
        {
            const int at = static_cast<int> (it->on - from);
            // D-DM-8: a note-on for a pitch already sounding sends a note-off first.
            for (std::size_t i = 0; i < slot.numActive; ++i)
                if (slot.active[i].channel == it->channel && slot.active[i].pitch == it->pitch)
                {
                    add (MidiEvent::noteOff (at, it->channel, it->pitch));
                    slot.active[i] = slot.active[--slot.numActive];
                    break;
                }
            if (slot.numActive == InstrumentSlot::maxActiveNotes)
                continue;
            add (MidiEvent::noteOn (at, it->channel, it->pitch, it->velocity));
            slot.active[slot.numActive++] = { it->off, it->channel, it->pitch };
        }
        // Controllers, pitch bend, pressure and program changes (F-PRL-12).
        auto ev = std::lower_bound (channel.controllers.begin(), channel.controllers.end(), from,
                                    [] (const ScheduledEvent& e, std::int64_t t) { return e.at < t; });
        for (; ev != channel.controllers.end() && ev->at < to; ++ev)
            add ({ static_cast<int> (ev->at - from), ev->status, ev->data1, ev->data2 });
        for (std::size_t i = 0; i < slot.numActive;)
        {
            const auto& n = slot.active[i];
            if (n.endSample < to)
            {
                add (MidiEvent::noteOff (static_cast<int> (std::max<std::int64_t> (0, n.endSample - from)), n.channel, n.pitch));
                slot.active[i] = slot.active[--slot.numActive];
            }
            else
                ++i;
        }
    }

    // Live MIDI (D-W2-1): the selected or armed instrument tracks play what comes in.
    if (channel.liveMidi)
        for (int i = 0; i < numLive; ++i)
        {
            const auto& e = liveEvents[static_cast<std::size_t> (i)];
            if (e.offset < offset || e.offset >= offset + frames)
                continue;
            if (channel.midiChannel != 0 && (e.status & 0x0f) + 1 != channel.midiChannel)
                continue;
            auto copy = e;
            copy.offset -= offset;
            add (copy);
        }

    // Order by offset; at one offset, note-offs before note-ons. Insertion sort:
    // std::stable_sort may allocate, and the list is short and nearly sorted.
    const auto before = [] (const MidiEvent& a, const MidiEvent& b) {
        if (a.offset != b.offset)
            return a.offset < b.offset;
        return a.isNoteOff() && ! b.isNoteOff();
    };
    for (std::size_t i = 1; i < events.size(); ++i)
        for (std::size_t j = i; j > 0 && before (events[j], events[j - 1]); --j)
            std::swap (events[j], events[j - 1]);

    float* out[2] = { left, right };
    slot.instrument().process (events, out, 2, frames);
}

void Engine::renderClicks (const EngineSnapshot& snap, std::int64_t from, int offset, int frames, bool audible) noexcept
{
    int count = 0;
    if (audible)
    {
        count = Metronome::clicksIn (snap, from, from + frames, clickScratch.data(), maxClicksPerSpan);
        for (int i = 0; i < count; ++i)
        {
            clickOffsets[static_cast<std::size_t> (i)] = static_cast<int> (clickScratch[static_cast<std::size_t> (i)].sample - from);
            clickAccents[static_cast<std::size_t> (i)] = clickScratch[static_cast<std::size_t> (i)].accent;
        }
    }
    float* click = mix[2].data();
    metronome.render (&click, 1, offset, frames, clickOffsets.data(), clickAccents.data(), count, clickGain);
}

void Engine::takeLiveMidi (int numFrames) noexcept
{
    numLive = 0;
    const auto blockEnd = frameCounter + numFrames;
    while (numLive < maxLiveEvents)
    {
        LiveMidiIn in;
        if (laterMidi)
            in = *laterMidi;
        else if (auto next = midiIn.pop())
            in = *next;
        else
            break;
        if (in.frame >= blockEnd)
        {
            laterMidi = in; // arrives in a later block
            break;
        }
        laterMidi.reset();
        const auto at = in.frame < frameCounter ? 0 : static_cast<int> (in.frame - frameCounter);
        liveEvents[static_cast<std::size_t> (numLive++)] = { at, in.status, in.data1, in.data2 };
    }
}

void Engine::captureSpan (const float* const* inputs, std::int64_t from, int offset, int frames) noexcept
{
    auto& p = *pass;
    auto s = from, e = from + frames;
    if (p.punch)
    {
        if (p.punchDone)
            return;
        s = std::max (s, p.captureStart);
        e = std::min (e, p.captureEnd);
    }

    // MIDI is stamped with the timeline position it arrived at (W-2 step 4); the punch window
    // without the audio latency applies to it.
    for (const auto& lane : p.midi)
    {
        const auto ms = p.punch ? std::max (from, p.captureStart - p.latency) : from;
        const auto me = p.punch ? std::min (from + frames, p.captureEnd - p.latency) : from + frames;
        for (int i = 0; i < numLive; ++i)
        {
            const auto& ev = liveEvents[static_cast<std::size_t> (i)];
            const auto at = from + (ev.offset - offset);
            if (ev.offset < offset || ev.offset >= offset + frames || at < ms || at >= me)
                continue;
            if (lane->midiChannel != 0 && (ev.status & 0x0f) + 1 != lane->midiChannel)
                continue;
            const auto n = lane->count.load (std::memory_order_relaxed);
            if (n >= lane->events.size())
            {
                lane->dropped.fetch_add (1, std::memory_order_relaxed);
                continue;
            }
            lane->events[n] = { at, ev.status, ev.data1, ev.data2 };
            lane->count.store (n + 1, std::memory_order_release);
        }
    }

    if (s >= e || p.audio.empty())
        return;
    const auto off = offset + static_cast<int> (s - from);
    const auto count = e - s;
    if (p.firstSample.load (std::memory_order_relaxed) == LLONG_MIN)
        p.firstSample.store (s, std::memory_order_relaxed);
    for (const auto& lane : p.audio)
    {
        const float* src[2] = { inputs[lane->inputFirst], inputs[std::min (lane->inputFirst + 1, numInputs - 1)] };
        const auto written = lane->fifo->write (src, off, count);
        if (written < count)
            lane->dropped.fetch_add (count - written, std::memory_order_relaxed);
    }
    p.frames.store (p.frames.load (std::memory_order_relaxed) + count, std::memory_order_release);
}

void Engine::captureWrap (std::int64_t at) noexcept
{
    auto& p = *pass;
    if (p.punch)
        p.punchDone = true; // punch records the first pass through the range
    if (! p.cycle)
        return;
    const auto n = p.numWraps.load (std::memory_order_relaxed);
    if (n < RecordPass::maxWraps)
    {
        p.wraps[static_cast<std::size_t> (n)] = p.frames.load (std::memory_order_relaxed);
        p.numWraps.store (n + 1, std::memory_order_release);
    }
    for (const auto& lane : p.midi)
    {
        const auto k = lane->count.load (std::memory_order_relaxed);
        if (k < lane->events.size())
        {
            lane->events[k] = { at, 0, 0, 0 };
            lane->count.store (k + 1, std::memory_order_release);
        }
    }
}
} // namespace weft::engine
