#include "weft/engine/transport/Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace weft::engine
{
namespace
{
/** Catmull-Rom (cubic Hermite) read between samples; outside the data is silence. */
float readCubic (const std::vector<float>& data, double pos) noexcept
{
    const auto n = static_cast<std::int64_t> (data.size());
    const auto i = static_cast<std::int64_t> (std::floor (pos));
    const auto frac = static_cast<float> (pos - static_cast<double> (i));
    auto at = [&] (std::int64_t k) { return k >= 0 && k < n ? data[static_cast<std::size_t> (k)] : 0.0f; };
    if (frac == 0.0f)
        return at (i);
    const float p0 = at (i - 1), p1 = at (i), p2 = at (i + 1), p3 = at (i + 2);
    const float c1 = 0.5f * (p2 - p0);
    const float c2 = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
    const float c3 = 0.5f * (p3 - p0) + 1.5f * (p1 - p2);
    return ((c3 * frac + c2) * frac + c1) * frac + p1;
}

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

Engine::Engine (MediaPool& media, InstrumentRack& instruments) : mediaPool (media), rack (instruments) { setSettings (currentSettings); }

Engine::~Engine() { current.store (nullptr); }

// Message thread ------------------------------------------------------------------

void Engine::setProject (const model::ProjectModel& model)
{
    lastModel = model;
    refresh();
}

void Engine::refresh()
{
    if (lastModel)
        publish (compileSnapshot (*lastModel, rate, mediaPool, rack));
}

void Engine::publish (std::unique_ptr<EngineSnapshot> snapshot)
{
    snapshot->serial = nextSerial++;
    current.store (snapshot.get(), std::memory_order_release);
    if (live)
        retired.push_back ({ std::move (live) });
    live = std::move (snapshot);
    collectGarbage();
}

void Engine::collectGarbage()
{
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
void Engine::play (Ticks from) { send ({ CommandType::play, toSamples (from) }); }
void Engine::stop() { send ({ CommandType::stop }); }
void Engine::locate (Ticks to) { send ({ CommandType::locate, toSamples (to) }); }

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
    if (armed.empty())
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
    Command c { CommandType::record, toSamples (from), currentSettings.countInBars };
    c.capture = true;
    result.started = result.recording = send (c);
    return result;
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

void Engine::setRecordCapacitySeconds (double seconds) { recordSeconds = std::max (0.0, seconds); }

std::optional<RecordedTake> Engine::takeRecording()
{
    if (! published.captureReady.exchange (false, std::memory_order_acq_rel))
        return std::nullopt;
    RecordedTake take;
    take.startSample = published.captureStart.load (std::memory_order_acquire);
    take.startTick = live ? static_cast<Ticks> (std::llround (live->tempo.sampleToTick (static_cast<double> (take.startSample)))) : 0;
    take.droppedFrames = published.droppedFrames.load (std::memory_order_acquire);
    const auto frames = static_cast<std::size_t> (published.capturedFrames.load (std::memory_order_acquire));
    take.audio.sampleRate = rate;
    for (const auto& ch : capture)
        take.audio.channels.emplace_back (ch.begin(), ch.begin() + static_cast<std::ptrdiff_t> (std::min (frames, ch.size())));
    return take;
}

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
    scratch.assign (2, std::vector<float> (block));
    mixPtrs = { mix[0].data(), mix[1].data(), mix[2].data() };
    scratchPtrs = { scratch[0].data(), scratch[1].data() };
    capture.assign (static_cast<std::size_t> (numInputs), std::vector<float> (static_cast<std::size_t> (recordSeconds * rate)));
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
}

void Engine::release()
{
    running = false;
    collectGarbage();
}

// Audio thread ------------------------------------------------------------------------

void Engine::flushAllNotes (const EngineSnapshot* snap) noexcept
{
    if (snap == nullptr)
        return;
    for (const auto& t : snap->tracks)
        if (t.instrument)
            t.instrument->flushPending = true;
}

void Engine::finishCapture() noexcept
{
    if (capturing && transport == TransportState::recording)
        published.captureReady.store (true, std::memory_order_release);
    capturing = false;
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
            capturing = c.capture;
            published.capturedFrames.store (0, std::memory_order_relaxed);
            published.droppedFrames.store (0, std::memory_order_relaxed);
            published.captureStart.store (recordStart, std::memory_order_relaxed);
            published.captureReady.store (false, std::memory_order_relaxed);
            transport = position < recordStart ? TransportState::countIn : TransportState::recording;
            flushAllNotes (snap);
            break;
        }
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
                    renderInstruments (*snap, position, offset, remaining, false);
                    renderClicks (*snap, position, offset, remaining, false);
                    offset = numFrames;
                    break;

                case TransportState::countIn:
                {
                    // Count-in: click only (D-ARCH-13); regions start playing at the record position.
                    const auto n = static_cast<int> (std::min<std::int64_t> (remaining, recordStart - position));
                    if (n > 0)
                    {
                        renderInstruments (*snap, position, offset, n, false);
                        renderClicks (*snap, position, offset, n, true);
                        position += n;
                        offset += n;
                    }
                    if (position >= recordStart)
                        transport = capturing ? TransportState::recording : TransportState::playing;
                    break;
                }

                case TransportState::playing:
                case TransportState::recording:
                {
                    auto end = position + remaining;
                    // Cycle (F-TRN-6): wrap when playback reaches the end; the end is exclusive.
                    // Recording passes don't cycle until take folders arrive (M3).
                    const bool cycling = transport == TransportState::playing && snap->cycleEnabled && position < snap->cycleEnd;
                    if (cycling)
                        end = std::min (end, snap->cycleEnd);
                    const auto n = static_cast<int> (end - position);
                    renderTimeline (*snap, position, offset, n);
                    if (transport == TransportState::recording)
                        captureInput (inputs, offset, n);
                    position = end;
                    offset += n;
                    if (cycling && position >= snap->cycleEnd)
                    {
                        position = snap->cycleStart;
                        flushAllNotes (snap);
                    }
                    break;
                }
            }
        }

        const float master = snap->masterGain;
        for (int ch = 0; ch < numOutputs && ch < 2; ++ch)
            for (int i = 0; i < numFrames; ++i)
                outputs[ch][i] = mix[static_cast<std::size_t> (ch)][static_cast<std::size_t> (i)] * master + mix[2][static_cast<std::size_t> (i)];
    }

    published.state.store (static_cast<int> (transport), std::memory_order_release);
    published.position.store (position, std::memory_order_release);

    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
    const double load = elapsed.count() * rate / numFrames;
    loadAverage += 0.2 * (load - loadAverage);
    published.load.store (loadAverage, std::memory_order_relaxed);
    if (load > 1.0)
        published.dropouts.fetch_add (1, std::memory_order_relaxed);
}

void Engine::renderTimeline (const EngineSnapshot& snap, std::int64_t from, int offset, int frames) noexcept
{
    const auto to = from + frames;
    for (const auto& track : snap.tracks)
    {
        if (track.muted || track.segments.empty())
            continue;
        auto it = std::lower_bound (track.segments.begin(), track.segments.end(), from,
                                    [] (const AudioSegment& s, std::int64_t t) { return s.end <= t; });
        for (; it != track.segments.end() && it->start < to; ++it)
        {
            const auto& seg = *it;
            const auto& data = *seg.media;
            const auto s = std::max (from, seg.start), e = std::min (to, seg.end);
            const bool direct = seg.sourceStep == 1.0 && seg.sourceAtRegionStart == std::floor (seg.sourceAtRegionStart);
            const auto numCh = data.numChannels();
            const float baseGain = seg.gain * track.gain;
            for (auto t = s; t < e; ++t)
            {
                float g = baseGain;
                const auto intoRegion = t - seg.regionStart;
                if (intoRegion < seg.fadeIn)
                    g *= fadeGain (seg.fadeInCurve, static_cast<double> (intoRegion) / static_cast<double> (seg.fadeIn));
                const auto toEnd = seg.regionEnd - t;
                if (toEnd <= seg.fadeOut)
                    g *= fadeGain (seg.fadeOutCurve, static_cast<double> (toEnd) / static_cast<double> (seg.fadeOut));
                const double src = seg.sourceAtRegionStart + static_cast<double> (intoRegion) * seg.sourceStep;
                const auto out = static_cast<std::size_t> (offset + (t - from));
                for (int ch = 0; ch < 2; ++ch)
                {
                    const auto& chan = data.channels[static_cast<std::size_t> (std::min (ch, numCh - 1))];
                    float v;
                    if (direct)
                    {
                        const auto k = static_cast<std::int64_t> (src);
                        v = k >= 0 && k < static_cast<std::int64_t> (chan.size()) ? chan[static_cast<std::size_t> (k)] : 0.0f;
                    }
                    else
                        v = readCubic (chan, src);
                    mix[static_cast<std::size_t> (ch)][out] += v * g;
                }
            }
        }
    }
    renderInstruments (snap, from, offset, frames, true);
    const bool audible = clickOn && (transport == TransportState::recording ? clickInRecord : clickInPlay);
    renderClicks (snap, from, offset, frames, audible);
}

void Engine::renderInstruments (const EngineSnapshot& snap, std::int64_t from, int offset, int frames, bool timeline) noexcept
{
    const auto to = from + frames;
    const auto capacity = events.capacity();
    auto add = [this, capacity] (const MidiEvent& e) {
        if (events.size() < capacity) // never grow on the audio thread
            events.push_back (e);
    };

    for (const auto& track : snap.tracks)
    {
        if (! track.instrument)
            continue;
        auto& slot = *track.instrument;
        events.clear();

        if (slot.flushPending)
        {
            for (std::size_t i = 0; i < slot.numActive; ++i)
                add (MidiEvent::noteOff (0, slot.active[i].channel, slot.active[i].pitch));
            slot.numActive = 0;
            slot.flushPending = false;
        }

        if (timeline)
        {
            auto it = std::lower_bound (track.notes.begin(), track.notes.end(), from,
                                        [] (const ScheduledNote& n, std::int64_t t) { return n.on < t; });
            for (; it != track.notes.end() && it->on < to; ++it)
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
        }

        slot.instrument().process (events, scratchPtrs.data(), 2, frames);
        if (track.muted)
            continue;
        for (std::size_t ch = 0; ch < 2; ++ch)
            for (int i = 0; i < frames; ++i)
                mix[ch][static_cast<std::size_t> (offset + i)] += scratch[ch][static_cast<std::size_t> (i)] * track.gain;
    }
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

void Engine::captureInput (const float* const* inputs, int offset, int frames) noexcept
{
    if (! capturing || capture.empty())
        return;
    const auto have = published.capturedFrames.load (std::memory_order_relaxed);
    const auto room = static_cast<std::int64_t> (capture[0].size()) - have;
    const auto n = std::clamp<std::int64_t> (frames, 0, room);
    for (std::size_t ch = 0; ch < capture.size(); ++ch)
        std::copy_n (inputs[ch] + offset, n, capture[ch].begin() + have);
    published.capturedFrames.store (have + n, std::memory_order_release);
    if (n < frames)
        published.droppedFrames.fetch_add (frames - n, std::memory_order_relaxed);
}
} // namespace weft::engine
