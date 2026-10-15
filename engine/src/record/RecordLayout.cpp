#include "weft/engine/record/RecordLayout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace weft::engine
{
namespace
{
Ticks tickAt (const TempoMap& tempo, std::int64_t sample) { return static_cast<Ticks> (std::llround (tempo.sampleToTick (static_cast<double> (sample)))); }
std::int64_t sampleAt (const TempoMap& tempo, Ticks tick) { return std::llround (tempo.tickToSample (static_cast<double> (tick))); }

/** The first tick at or after the sample. */
Ticks tickAtOrAfter (const TempoMap& tempo, std::int64_t sample)
{
    auto t = static_cast<Ticks> (std::ceil (tempo.sampleToTick (static_cast<double> (sample)) - 1e-6));
    while (sampleAt (tempo, t) < sample)
        ++t;
    return t;
}

Ticks barFloor (const SignatureMap& sig, Ticks t) { return sig.barStart (sig.barAt (t).bar); }
Ticks barCeil (const SignatureMap& sig, Ticks t)
{
    const auto b = sig.barAt (t);
    return b.offset == 0 ? t : sig.barStart (b.bar + 1);
}
} // namespace

AudioLayout layoutAudio (const RecordPass& pass, std::int64_t frames, const TempoMap& tempo, const SignatureMap& signatures)
{
    AudioLayout out;
    const auto first = pass.firstSample.load();
    const auto rate = tempo.sampleRate();
    const auto minFrames = static_cast<std::int64_t> (std::ceil (0.1 * rate)); // W-1a step 9
    if (first == LLONG_MIN || frames - pass.latency < minFrames)
        return out;

    const auto L = pass.latency;
    // Frame f was captured at timeline P(f) and holds what was played at P(f) - L, so a region
    // starting at timeline T reads from the frame captured at T + L.
    const auto numWraps = std::min (pass.numWraps.load(), RecordPass::maxWraps);
    if (! pass.cycle || numWraps == 0)
    {
        const auto dataStart = first - L; // timeline of frame 0's content
        const auto startTick = tickAtOrAfter (tempo, std::max (dataStart, pass.recordStart));
        const auto offset = sampleAt (tempo, startTick) - dataStart;
        const auto length = frames - offset;
        if (length < minFrames)
            return out;
        commands::RecordedTake take { startTick, offset, length };
        if (pass.punch)
        {
            // D-W1-5: 10 ms handles, faded against the material underneath.
            take.fadeIn = take.fadeOut = std::min<std::int64_t> (std::llround (0.010 * rate), length / 2);
        }
        out.takes.push_back (take);
        return out;
    }

    // Cycle recording (W-1c): one take per pass, all in one file.
    std::vector<std::int64_t> bounds { 0 };
    for (int i = 0; i < numWraps; ++i)
        bounds.push_back (pass.wraps[static_cast<std::size_t> (i)]);
    bounds.push_back (frames);

    const auto cycleStartTick = tickAt (tempo, pass.cycleStart);
    const auto cycleEndTick = tickAt (tempo, pass.cycleEnd);
    const auto& sig = signatures.signatureAtBar (signatures.barAt (cycleStartTick).bar);
    const auto beat = sampleAt (tempo, cycleStartTick + 4 * ticksPerQuarter / sig.denominator) - pass.cycleStart;

    for (std::size_t k = 0; k + 1 < bounds.size(); ++k)
    {
        const bool last = k + 2 == bounds.size();
        const auto segment = bounds[k + 1] - bounds[k];
        if (last && (k > 0 && segment < beat)) // W-1c step 4: a final partial pass under 1 beat is dropped
            break;
        const auto timelineStart = k == 0 ? first : pass.cycleStart;
        const auto startTick = tickAtOrAfter (tempo, timelineStart);
        const auto skip = sampleAt (tempo, startTick) - timelineStart;
        const auto offset = bounds[k] + skip + L;
        const auto length = std::min (segment - skip, frames - offset);
        if (length < model::minAudioRegionLength)
            continue;
        out.takes.push_back ({ startTick, offset, length });
    }
    if (out.takes.empty())
        return out;
    out.takeFolder = true;
    out.folderStart = std::min (out.takes.front().start, cycleStartTick);
    out.folderEnd = cycleEndTick;
    return out;
}

commands::RecordedMidi layoutMidi (const RecordPass& pass, const RecordPass::MidiLane& lane, const TempoMap& tempo, const SignatureMap& signatures,
                                   bool mergeLinear, bool mergeCycle)
{
    commands::RecordedMidi out;
    out.trackId = lane.trackId;
    out.trackName = lane.trackName;
    const auto count = std::min (lane.count.load(), lane.events.size());
    const auto stop = pass.stopSample.load();

    struct Held
    {
        std::int64_t sample;
        int velocity;
    };
    std::array<std::array<std::optional<Held>, 128>, 16> held {};
    std::array<bool, 16> sustain {};
    std::optional<Ticks> lo, hi;
    const auto extend = [&] (Ticks a, Ticks b) {
        lo = lo ? std::min (*lo, a) : a;
        hi = hi ? std::max (*hi, b) : b;
    };
    const auto close = [&] (int ch, int pitch, std::int64_t at, int releaseVelocity) {
        auto& h = held[static_cast<std::size_t> (ch)][static_cast<std::size_t> (pitch)];
        if (! h)
            return;
        model::Note n;
        n.pos = tickAt (tempo, h->sample);
        n.length = std::max<Ticks> (1, tickAt (tempo, at) - n.pos);
        n.pitch = pitch;
        n.velocity = std::clamp (h->velocity, 1, 127);
        n.releaseVelocity = releaseVelocity;
        n.channel = ch + 1;
        extend (n.pos, n.pos + n.length);
        out.notes.push_back (n);
        h.reset();
    };
    const auto closeAll = [&] (std::int64_t at) {
        for (int ch = 0; ch < 16; ++ch)
            for (int p = 0; p < 128; ++p)
                close (ch, p, at, 64);
    };
    const auto controller = [&] (std::int64_t at, const char* kind, int number, int value, int ch) {
        model::CcEvent c;
        c.pos = tickAt (tempo, at);
        c.kind = kind;
        c.number = number;
        c.value = value;
        c.channel = ch + 1;
        extend (c.pos, c.pos + 1);
        out.controllers.push_back (c);
    };

    for (std::size_t i = 0; i < count; ++i)
    {
        const auto& e = lane.events[i];
        if (e.status == 0)
        {
            closeAll (e.sample); // a cycle wrap ends the notes of that pass
            continue;
        }
        const int type = e.status & 0xF0, ch = e.status & 0x0F;
        switch (type)
        {
            case 0x90:
                if (e.data2 > 0)
                {
                    close (ch, e.data1 & 0x7F, e.sample, 64);
                    held[static_cast<std::size_t> (ch)][e.data1 & 0x7F] = Held { e.sample, e.data2 };
                    break;
                }
                close (ch, e.data1 & 0x7F, e.sample, 64);
                break;
            case 0x80: close (ch, e.data1 & 0x7F, e.sample, e.data2); break;
            case 0xB0:
                if (e.data1 == 64)
                    sustain[static_cast<std::size_t> (ch)] = e.data2 >= 64;
                controller (e.sample, "cc", e.data1, e.data2, ch);
                break;
            case 0xE0: controller (e.sample, "pitchBend", 0, (e.data1 | (e.data2 << 7)) - 8192, ch); break; // signed, 03 §6
            case 0xD0: controller (e.sample, "channelPressure", 0, e.data1, ch); break;
            case 0xA0: controller (e.sample, "polyPressure", e.data1, e.data2, ch); break;
            case 0xC0: controller (e.sample, "programChange", e.data1, 0, ch); break;
            default: break;
        }
    }
    // W-2 step 6 and edge cases: hanging notes close at stop, a held pedal gets released.
    closeAll (stop);
    for (int ch = 0; ch < 16; ++ch)
        if (sustain[static_cast<std::size_t> (ch)])
            controller (stop, "cc", 64, 0, ch);

    if (! lo)
        return out; // W-2: nothing played, no region

    const bool cycle = pass.cycle && pass.numWraps.load() > 0;
    out.regionStart = barFloor (signatures, *lo);
    out.regionEnd = std::max (barCeil (signatures, *hi), out.regionStart + 1);
    if (cycle)
    {
        out.regionStart = std::min (out.regionStart, tickAt (tempo, pass.cycleStart));
        out.regionEnd = std::max (out.regionEnd, tickAt (tempo, pass.cycleEnd));
    }
    out.merge = cycle ? mergeCycle : mergeLinear;
    return out;
}
} // namespace weft::engine
