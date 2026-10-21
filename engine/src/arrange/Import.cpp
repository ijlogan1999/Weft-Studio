#include "weft/engine/arrange/Import.h"

#include "weft/media/MediaStore.h"
#include "weft/media/MidiFile.h"
#include "weft/media/Resample.h"

#include <algorithm>
#include <cmath>

namespace weft::engine
{
namespace
{
struct DecodedAudio
{
    std::filesystem::path file;
    model::MediaRef ref;
};

struct ParsedMidi
{
    std::filesystem::path file;
    media::MidiFileData data;
};

Ticks toProjectTicks (std::int64_t fileTicks, int ppq)
{
    return static_cast<Ticks> (std::llround (static_cast<double> (fileTicks) * static_cast<double> (ticksPerQuarter) / ppq));
}

/** The first bar line at or after t. */
Ticks barCeil (const SignatureMap& sigs, Ticks t)
{
    const auto pos = sigs.barAt (t);
    return pos.offset == 0 ? t : sigs.barStart (pos.bar + 1);
}
} // namespace

bool isMidiFile (const std::filesystem::path& file)
{
    const auto ext = media::lowerExtension (file);
    return ext == ".mid" || ext == ".midi" || ext == ".smf";
}

ImportOutcome importFiles (commands::Session& session, MediaPool& pool, const std::filesystem::path& mediaDir,
                           const media::DecoderSet& decoders, const std::vector<std::filesystem::path>& files, DropTarget target)
{
    ImportOutcome out;
    const auto& info = session.model().info;
    std::vector<DecodedAudio> audio;
    std::vector<ParsedMidi> midi;

    std::vector<std::string> missing, narrowed;
    // Read everything first, so a bad file doesn't leave half a drop behind.
    for (const auto& file : files)
    {
        std::string error;
        if (std::error_code ec; ! std::filesystem::is_regular_file (file, ec))
        {
            out.rejected.push_back (file.filename().string());
            missing.push_back (file.filename().string());
            continue;
        }
        if (isMidiFile (file))
        {
            if (auto data = media::readMidiFile (file, &error))
                midi.push_back ({ file, std::move (*data) });
            else
                out.rejected.push_back (file.filename().string());
            continue;
        }
        if (! decoders.canDecode (file))
        {
            out.rejected.push_back (file.filename().string());
            continue;
        }
        auto decoded = decoders.decode (file, &error);
        if (! decoded)
        {
            out.rejected.push_back (file.filename().string());
            continue;
        }
        if (decoded->numChannels() > 2)
        {
            decoded->channels.resize (2); // D-ARR-7: tracks are mono or stereo
            narrowed.push_back (file.filename().string());
        }
        auto converted = media::resample (*decoded, info.sampleRate);
        const auto hash = media::storeMedia (converted, media::formatForBitDepth (info.recordBitDepth), mediaDir, &error);
        if (! hash)
        {
            out.rejected.push_back (file.filename().string());
            continue;
        }
        pool.load (mediaDir, *hash);
        DecodedAudio a;
        a.file = file;
        a.ref.hash = *hash;
        a.ref.originalName = file.filename().string();
        a.ref.format = "wav";
        a.ref.sampleRate = info.sampleRate;
        a.ref.channels = converted.numChannels();
        a.ref.lengthSamples = converted.numFrames();
        a.ref.bitDepth = info.recordBitDepth;
        std::error_code ec;
        a.ref.sizeBytes = static_cast<std::int64_t> (std::filesystem::file_size (mediaDir / media::mediaFileName (*hash), ec));
        a.ref.addedBy = session.user();
        audio.push_back (std::move (a));
    }
    // Status text: missing files, then unreadable ones, then multichannel files cut to stereo.
    auto names = [] (const std::vector<std::string>& list) {
        std::string text;
        for (std::size_t i = 0; i < list.size(); ++i)
            text += (i > 0 ? ", " : "") + list[i];
        return text;
    };
    auto append = [&] (const std::string& sentence) { out.message += (out.message.empty() ? "" : " ") + sentence; };
    std::vector<std::string> unreadable;
    for (const auto& name : out.rejected)
        if (std::find (missing.begin(), missing.end(), name) == missing.end())
            unreadable.push_back (name);
    if (! missing.empty())
        append ("Couldn't find " + names (missing) + ".");
    if (! unreadable.empty())
        append ("Couldn't import " + names (unreadable) + ": unsupported or damaged file" + std::string (unreadable.size() > 1 ? "s." : "."));
    if (! narrowed.empty())
        append ("Kept the first two channels of " + names (narrowed) + ".");
    if (audio.empty() && midi.empty())
        return out;

    const bool emptyProject = session.model().regions.empty();
    const auto at = std::max<Ticks> (target.at, 0);
    session.beginGesture ("Import");

    // Audio ----------------------------------------------------------------------------
    const auto* dropTrack = session.model().track (target.trackId);
    const bool ontoTrack = dropTrack != nullptr && dropTrack->kind == model::TrackKind::audio;
    Ticks next = at;
    for (const auto& a : audio)
    {
        session.addMedia (a.ref);
        Id track = target.trackId;
        Ticks start = at;
        if (ontoTrack)
            start = next;
        else
        {
            const auto created = session.createTrack (model::TrackKind::audio, a.file.stem().string(), a.ref.channels);
            if (! created)
                continue;
            track = *created;
            out.tracks.push_back (track);
        }
        if (const auto region = session.createAudioRegion (track, start, a.ref.hash, 0, a.ref.lengthSamples))
        {
            out.regions.push_back (*region);
            next = static_cast<Ticks> (std::ceil (session.model().regionEndTick (*session.model().region (*region))));
        }
    }

    // MIDI -----------------------------------------------------------------------------
    for (const auto& m : midi)
    {
        const auto ppq = m.data.ticksPerQuarter;
        if (emptyProject && (! m.data.tempos.empty() || ! m.data.signatures.empty()))
        {
            // D-AT-1: an empty project takes the file's tempo map.
            for (const auto& t : m.data.tempos)
            {
                const auto tick = toProjectTicks (t.tick, ppq);
                if (tick == 0)
                    session.setTempoAt (0, t.bpm);
                else
                    session.addTempoPoint (at + tick, t.bpm);
            }
            // Signature changes sit on bar lines; count bars with the file's own meters.
            int bar = 0;
            Ticks barTick = 0, barLength = 4 * ticksPerQuarter;
            for (const auto& s : m.data.signatures)
            {
                const auto tick = toProjectTicks (s.tick, ppq);
                bar += static_cast<int> ((tick - barTick + barLength / 2) / barLength);
                barTick = tick;
                barLength = static_cast<Ticks> (s.numerator) * 4 * ticksPerQuarter / s.denominator;
                const auto atBar = at > 0 ? session.model().signatures.barAt (at).bar : 0;
                session.setSignatureAt (atBar + bar, s.numerator, s.denominator);
            }
        }
        int number = 0;
        for (const auto& t : m.data.tracks)
        {
            if (t.notes.empty())
                continue;
            ++number;
            auto name = t.name.empty() ? m.file.stem().string() + " " + std::to_string (number) : t.name;
            const auto track = session.createTrack (model::TrackKind::instrument, name);
            if (! track)
                continue;
            out.tracks.push_back (*track);
            Ticks end = 1;
            for (const auto& n : t.notes)
                end = std::max (end, toProjectTicks (n.tick + n.length, ppq));
            for (const auto& c : t.controllers)
                end = std::max (end, toProjectTicks (c.tick, ppq) + 1);
            const auto length = barCeil (session.model().signatures, at + end) - at;
            const auto region = session.createMidiRegion (*track, at, std::max<Ticks> (length, 1));
            if (! region)
                continue;
            out.regions.push_back (*region);
            const auto* r = session.model().region (*region);
            model::MidiSequence seq;
            seq.id = r->midi.sequenceId;
            for (const auto& n : t.notes)
            {
                model::Note note;
                note.id = session.newId();
                note.pos = toProjectTicks (n.tick, ppq);
                note.length = std::max<Ticks> (1, toProjectTicks (n.tick + n.length, ppq) - note.pos);
                note.pitch = n.pitch;
                note.velocity = std::clamp (n.velocity, 1, 127);
                note.releaseVelocity = n.releaseVelocity;
                note.channel = n.channel;
                seq.notes.emplace (note.id, note);
            }
            for (const auto& c : t.controllers)
            {
                model::CcEvent e;
                e.id = session.newId();
                e.pos = toProjectTicks (c.tick, ppq);
                e.kind = c.kind;
                e.number = c.number;
                e.value = c.value;
                e.channel = c.channel;
                seq.controllers.emplace (e.id, e);
            }
            session.edit ("Import", [&] (commands::TxnWriter& w) {
                return w.put (doc::path ({ model::keys::midiSequences, seq.id }), seq.toValue()) ? commands::EditResult {}
                                                                                             : commands::EditResult::fail ("The document rejected the edit.");
            });
        }
    }
    session.endGesture();
    return out;
}
} // namespace weft::engine
