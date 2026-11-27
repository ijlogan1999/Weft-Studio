#include "weft/engine/arrange/OfflineRender.h"

#include "weft/engine/devices/NullDevice.h"
#include "weft/engine/transport/Engine.h"
#include "weft/media/MediaStore.h"
#include "weft/engine/render/Bounce.h"
#include "weft/media/WavFile.h"

#include <string_view>

#include <algorithm>
#include <cmath>

namespace weft::engine
{
OfflineRenderer::OfflineRenderer (const model::ProjectModel& model, MediaPool& media, const OfflineRenderOptions& options)
    : engine (media, rack)
{
    TransportSettings settings;
    settings.metronomeOn = options.metronome;
    settings.metronomeDuring = ClickDuring::both;
    engine.setSettings (settings);
    engine.setDiskThread (false);
    engine.setPluginFactory (options.plugins);
    engine.setProjectFolder (options.projectFolder);
    if (! options.soloed.empty())
        engine.setSoloedTracks (options.soloed);

    DeviceConfig config;
    config.sampleRate = model.info.sampleRate;
    config.maxBlockSize = std::max (options.blockSize, 32);
    config.numInputChannels = 0;
    config.numOutputChannels = 2;
    if (! device.open (config) || ! device.start (engine))
        return;
    engine.setProject (model);
    engine.play (options.from);
    compensation = toSkip = engine.compensationLatency();
    started = true;
}

OfflineRenderer::~OfflineRenderer()
{
    if (started)
        device.stop();
}

bool OfflineRenderer::render (std::int64_t frames, std::vector<float>& left, std::vector<float>& right)
{
    left.clear();
    right.clear();
    if (! started || frames <= 0)
        return started;
    left.reserve (static_cast<std::size_t> (frames));
    right.reserve (static_cast<std::size_t> (frames));
    // Render a chunk at a time and load the streamed blocks ahead of each one.
    constexpr std::int64_t chunk = 8192;
    while (static_cast<std::int64_t> (left.size()) < frames)
    {
        const auto want = frames - static_cast<std::int64_t> (left.size()) + toSkip;
        const auto n = std::min (chunk, want);
        engine.pumpDisk();
        device.setCaptureCapacity (n);
        if (! device.render (n))
            return false;
        const auto l = device.capturedOutput (0), r = device.capturedOutput (1);
        const auto skip = static_cast<std::size_t> (std::min<std::int64_t> (toSkip, static_cast<std::int64_t> (l.size())));
        toSkip -= static_cast<std::int64_t> (skip);
        left.insert (left.end(), l.begin() + static_cast<std::ptrdiff_t> (skip), l.end());
        right.insert (right.end(), r.begin() + static_cast<std::ptrdiff_t> (skip), r.end());
    }
    return true;
}

media::AudioData renderOffline (const model::ProjectModel& model, MediaPool& media, const OfflineRenderOptions& options)
{
    media::AudioData out;
    out.sampleRate = model.info.sampleRate;
    if (options.frames <= 0)
        return out;
    OfflineRenderer renderer (model, media, options);
    std::vector<float> left, right;
    if (! renderer.render (options.frames, left, right))
        return out;
    out.channels.push_back (std::move (left));
    out.channels.push_back (std::move (right));
    return out;
}

namespace
{
/** The regions (all on trackId) as they play, without any strip, from `from` for `frames`. */
media::AudioData renderPlain (const model::ProjectModel& original, std::vector<model::Region> rs, const Id& trackId, Ticks from,
                              std::int64_t frames, MediaPool& media)
{
    // A model with just these regions on a plain stereo track into a plain master.
    model::ProjectModel m = original;
    auto plain = [] (model::Track t) {
        t.strip = {};
        t.mute = false;
        t.output = {};
        t.automationMode = "off";
        return t;
    };
    auto track = plain (*original.track (trackId));
    track.kind = model::TrackKind::audio;
    track.channels = 2;
    m.tracks.clear();
    m.syntheticTracks.clear();
    m.tracks.emplace (trackId, std::make_shared<model::Track> (track));
    if (const auto* master = original.track (original.info.masterTrackId))
        m.tracks.emplace (master->id, std::make_shared<model::Track> (plain (*master)));
    m.trackOrder = { trackId };
    m.recoveredTrackFor.clear();
    m.regions.clear();
    for (auto& r : rs)
    {
        r.trackId = trackId;
        r.takeFolderId.clear();
        m.regions.emplace (r.id, std::make_shared<model::Region> (r));
    }
    m.automationLanes.clear();
    m.storedLanes.clear();
    m.disabledEdges.clear();

    OfflineRenderOptions options;
    options.from = from;
    options.frames = frames;
    return renderOffline (m, media, options);
}
} // namespace

std::optional<RenderedJoin> renderJoin (const model::ProjectModel& original, const std::vector<Id>& regionIds, MediaPool& media, std::string* error)
{
    auto fail = [&] (std::string why) -> std::optional<RenderedJoin> {
        if (error != nullptr)
            *error = std::move (why);
        return std::nullopt;
    };
    std::vector<model::Region> rs;
    for (const auto& id : regionIds)
        if (const auto* r = original.region (id); r != nullptr && std::none_of (rs.begin(), rs.end(), [&] (const model::Region& x) { return x.id == id; }))
            rs.push_back (*r);
    if (rs.size() < 2)
        return fail ("Select two or more regions to join.");
    const auto trackId = original.effectiveTrackId (rs.front());
    for (const auto& r : rs)
        if (r.isMidi || original.effectiveTrackId (r) != trackId)
            return fail ("Only audio regions on one track can be joined this way.");

    // The span, and the outer fades that the joined region keeps (D-ARR-4).
    auto toSample = [&] (Ticks t) { return std::llround (original.tempo.tickToSample (static_cast<double> (t))); };
    std::size_t first = 0, last = 0;
    bool stereo = false;
    for (std::size_t i = 0; i < rs.size(); ++i)
    {
        if (std::tie (rs[i].start, rs[i].id) < std::tie (rs[first].start, rs[first].id))
            first = i;
        if (toSample (rs[i].start) + rs[i].length > toSample (rs[last].start) + rs[last].length)
            last = i;
        if (const auto* m = original.mediaRef (rs[i].audio.media))
            stereo = stereo || m->channels > 1;
    }
    const auto startSample = toSample (rs[first].start);
    const auto endSample = toSample (rs[last].start) + rs[last].length;
    RenderedJoin result;
    result.fadeIn = rs[first].audio.fadeIn;
    result.fadeOut = rs[last].audio.fadeOut;
    rs[first].audio.fadeIn.length = 0;
    rs[last].audio.fadeOut.length = 0;

    result.audio = renderPlain (original, rs, trackId, rs[first].start, endSample - startSample, media);
    if (result.audio.channels.size() != 2 || static_cast<std::int64_t> (result.audio.channels[0].size()) != endSample - startSample)
        return fail ("Couldn't render the regions.");
    if (! stereo)
        result.audio.channels.resize (1);
    return result;
}

commands::EditResult joinRegions (commands::Session& session, const std::vector<Id>& regionIds, MediaPool& media, const std::filesystem::path& mediaDir)
{
    if (auto joined = session.joinRegions (regionIds))
        return joined;
    bool audio = ! regionIds.empty();
    for (const auto& id : regionIds)
        if (const auto* r = session.model().region (id); r == nullptr || r->isMidi)
            audio = false;
    if (! audio)
        return commands::EditResult::fail (session.lastError());

    std::string error;
    auto rendered = renderJoin (session.model(), regionIds, media, &error);
    if (! rendered)
        return commands::EditResult::fail (error);
    const auto bitDepth = session.model().info.recordBitDepth;
    const auto hash = media::storeMedia (rendered->audio, media::formatForBitDepth (bitDepth), mediaDir, &error);
    if (! hash)
        return commands::EditResult::fail (error);
    media.load (mediaDir, *hash);

    model::MediaRef ref;
    ref.hash = *hash;
    ref.originalName = session.model().region (regionIds.front())->name + " (joined).wav";
    ref.sampleRate = session.model().info.sampleRate;
    ref.channels = static_cast<int> (rendered->audio.channels.size());
    ref.lengthSamples = static_cast<Samples> (rendered->audio.channels[0].size());
    ref.bitDepth = bitDepth;
    std::error_code ec;
    ref.sizeBytes = static_cast<std::int64_t> (std::filesystem::file_size (mediaDir / media::mediaFileName (*hash), ec));
    return session.joinRegionsAs (regionIds, ref, rendered->fadeIn, rendered->fadeOut);
}

std::optional<media::AudioData> renderRegion (const model::ProjectModel& model, const Id& regionId, MediaPool& media, std::string* error)
{
    const auto* r = model.region (regionId);
    if (r == nullptr || r->isMidi)
    {
        if (error != nullptr)
            *error = "That isn't an audio region.";
        return std::nullopt;
    }
    auto audio = renderPlain (model, { *r }, model.effectiveTrackId (*r), r->start, r->length, media);
    if (audio.channels.size() != 2 || static_cast<std::int64_t> (audio.channels[0].size()) != r->length)
    {
        if (error != nullptr)
            *error = "Couldn't render the region.";
        return std::nullopt;
    }
    if (const auto* m = model.mediaRef (r->audio.media); m == nullptr || m->channels < 2)
        audio.channels.resize (1);
    return audio;
}

namespace
{
/** A MIDI region's notes and controllers as they play, `shift` ticks after the file's start. */
void appendRegionEvents (media::MidiFileTrack& track, const model::ProjectModel& model, const model::Region& r, Ticks shift)
{
    const auto offset = r.midi.sequenceOffset;
    const auto end = offset + r.length;
    const auto* seq = model.sequence (r.midi.sequenceId);
    if (seq == nullptr)
        return;
    for (const auto* n : seq->sortedNotes())
    {
        if (n->muted || n->pos < offset || n->pos >= end)
            continue;
        media::MidiFileNote out;
        out.tick = n->pos - offset + shift;
        out.length = std::max<std::int64_t> (1, std::min (n->length, end - n->pos));
        out.channel = n->channel;
        out.pitch = std::clamp (n->pitch + r.midi.transpose, 0, 127);
        out.velocity = std::clamp (n->velocity + r.midi.velocityOffset, 1, 127);
        out.releaseVelocity = n->releaseVelocity;
        track.notes.push_back (out);
    }
    for (const auto& [id, c] : seq->controllers)
        if (c.pos >= offset && c.pos < end)
            track.controllers.push_back ({ c.pos - offset + shift, c.kind, c.number, c.value, c.channel });
}

/** The key, tempo and meter in force at `from`, and every key and tempo change before `to`. */
void addConductor (media::MidiFileData& file, const model::ProjectModel& model, Ticks from, Ticks to)
{
    const auto startKey = model.keyAt (from); // and each key change inside the span (D-UI-44)
    file.keys.push_back ({ 0, startKey.fifths(), startKey.minor });
    for (const auto* k : model.keyChangesInOrder())
        if (k->pos > from && k->pos < to)
            file.keys.push_back ({ k->pos - from, k->key.fifths(), k->key.minor });
    file.tempos.push_back ({ 0, model.tempo.bpmAt (from) });
    for (const auto& p : model.storedTempoPoints)
        if (p.pos > from && p.pos < to)
            file.tempos.push_back ({ p.pos - from, p.bpm });
    const auto at = model.signatures.barAt (from);
    const auto& sig = model.signatures.signatureAtBar (at.bar);
    file.signatures.push_back ({ 0, sig.numerator, sig.denominator });
}
} // namespace

media::MidiFileData midiRegionFile (const model::ProjectModel& model, const Id& regionId)
{
    media::MidiFileData file;
    file.format = 1;
    file.ticksPerQuarter = static_cast<int> (ticksPerQuarter);
    const auto* r = model.region (regionId);
    if (r == nullptr || ! r->isMidi)
        return file;
    media::MidiFileTrack track;
    track.name = r->name;
    appendRegionEvents (track, model, *r, 0);
    std::stable_sort (track.controllers.begin(), track.controllers.end(), [] (const auto& a, const auto& b) { return a.tick < b.tick; });
    file.tracks.push_back (std::move (track));
    // The project key (F-TMP-10), then the tempo and meter in force at the region start and
    // every change inside it.
    addConductor (file, model, r->start, r->start + r->length);
    return file;
}

media::MidiFileData midiSelectionFile (const model::ProjectModel& model, const std::vector<Id>& regionIds)
{
    media::MidiFileData file;
    file.format = 1;
    file.ticksPerQuarter = static_cast<int> (ticksPerQuarter);
    std::vector<const model::Region*> regions;
    for (const auto& id : regionIds)
        if (const auto* r = model.region (id); r != nullptr && r->isMidi)
            regions.push_back (r);
    if (regions.empty())
        return file;
    Ticks from = regions.front()->start, to = 0;
    for (const auto* r : regions)
    {
        from = std::min (from, r->start);
        to = std::max (to, r->start + r->length);
    }
    for (const auto& trackId : model.trackOrder)
    {
        const auto* t = model.track (trackId);
        if (t == nullptr)
            continue;
        media::MidiFileTrack track;
        track.name = t->name;
        bool any = false;
        for (const auto* r : regions)
            if (r->trackId == t->id)
            {
                appendRegionEvents (track, model, *r, r->start - from);
                any = true;
            }
        if (! any)
            continue;
        std::stable_sort (track.notes.begin(), track.notes.end(), [] (const auto& a, const auto& b) { return a.tick < b.tick; });
        std::stable_sort (track.controllers.begin(), track.controllers.end(), [] (const auto& a, const auto& b) { return a.tick < b.tick; });
        file.tracks.push_back (std::move (track));
    }
    addConductor (file, model, from, to);
    return file;
}

RegionExport exportRegions (const model::ProjectModel& model, const std::vector<Id>& regionIds, MediaPool& media,
                            const std::filesystem::path& folder, int bitDepth)
{
    RegionExport out;
    std::vector<std::string> failed;
    std::error_code ec;
    std::filesystem::create_directories (folder, ec);
    for (const auto& id : regionIds)
    {
        const auto* r = model.region (id);
        if (r == nullptr)
            continue;
        auto base = r->name.empty() ? std::string ("Region") : r->name;
        for (auto& ch : base)
            if (std::string_view ("\\/:*?\"<>|").find (ch) != std::string_view::npos)
                ch = '_';
        std::string error;
        bool ok = false;
        std::filesystem::path file;
        if (r->isMidi)
        {
            file = keepBothPath (folder / (base + ".mid"));
            ok = media::writeMidiFile (file, midiRegionFile (model, id), &error);
        }
        else if (auto audio = renderRegion (model, id, media, &error))
        {
            audio->sampleRate = model.info.sampleRate;
            file = keepBothPath (folder / (base + ".wav"));
            ok = media::writeWav (file, *audio, media::formatForBitDepth (bitDepth), &error);
        }
        if (ok)
            out.files.push_back (file);
        else
            failed.push_back (r->name);
    }
    if (! out.files.empty())
        out.message = "Exported " + std::to_string (out.files.size()) + (out.files.size() == 1 ? " region" : " regions") + " to "
                    + folder.filename().string() + ".";
    if (! failed.empty())
        out.message += std::string (out.message.empty() ? "" : " ") + "Couldn't export " + std::to_string (failed.size())
                     + (failed.size() == 1 ? " region." : " regions.");
    return out;
}

commands::EditResult reverseRegions (commands::Session& session, const std::vector<Id>& regionIds, MediaPool& media,
                                     const std::filesystem::path& mediaDir)
{
    const auto& model = session.model();
    std::vector<std::pair<Id, model::MediaRef>> reversed;
    for (const auto& id : regionIds)
    {
        const auto* r = model.region (id);
        const auto* ref = r != nullptr && ! r->isMidi ? model.mediaRef (r->audio.media) : nullptr;
        if (ref == nullptr)
            continue;
        // The region's part of the file: from disk when it's there, else from memory.
        media::AudioData audio;
        audio.sampleRate = model.info.sampleRate;
        media::WavReader reader;
        if (reader.open (mediaDir / media::mediaFileName (ref->hash)))
        {
            audio.channels.assign (static_cast<std::size_t> (reader.info().channels), std::vector<float> (static_cast<std::size_t> (r->length)));
            std::vector<float*> dest;
            for (auto& c : audio.channels)
                dest.push_back (c.data());
            reader.read (r->audio.sourceOffset, r->length, dest.data());
        }
        else if (const auto source = media.find (ref->hash); source != nullptr && source->resident())
        {
            audio.channels.assign (static_cast<std::size_t> (source->numChannels()), std::vector<float> (static_cast<std::size_t> (r->length)));
            bool missing = false;
            for (int c = 0; c < source->numChannels(); ++c)
                for (std::int64_t i = 0; i < r->length; ++i)
                    audio.channels[static_cast<std::size_t> (c)][static_cast<std::size_t> (i)] = source->sample (c, r->audio.sourceOffset + i, missing);
        }
        else
            return commands::EditResult::fail ("The audio for " + r->name + " isn't here yet.");
        for (auto& c : audio.channels)
            std::reverse (c.begin(), c.end());
        std::string error;
        const auto hash = media::storeMedia (audio, media::formatForBitDepth (model.info.recordBitDepth), mediaDir, &error);
        if (! hash)
            return commands::EditResult::fail (error);
        if (media.find (*hash) == nullptr && ! media.load (mediaDir, *hash))
            media.add (*hash, audio);
        model::MediaRef out;
        out.hash = *hash;
        out.originalName = r->name + " (reversed).wav";
        out.sampleRate = model.info.sampleRate;
        out.channels = audio.numChannels();
        out.lengthSamples = audio.numFrames();
        out.bitDepth = model.info.recordBitDepth;
        std::error_code ec;
        out.sizeBytes = static_cast<std::int64_t> (std::filesystem::file_size (mediaDir / media::mediaFileName (*hash), ec));
        reversed.emplace_back (id, out);
    }
    return session.reverseRegionsAs (reversed);
}

std::string fileEditName (FileEdit edit)
{
    switch (edit)
    {
        case FileEdit::reverse: return "Reverse";
        case FileEdit::invert: return "Invert";
        case FileEdit::normalize: return "Normalize";
        case FileEdit::gain: return "Change Gain";
        case FileEdit::fadeIn: return "Fade In";
        case FileEdit::fadeOut: return "Fade Out";
        case FileEdit::silence: return "Silence";
        case FileEdit::removeDc: return "Remove DC Offset";
        case FileEdit::trim: return "Trim";
        case FileEdit::remove: return "Delete";
        case FileEdit::insert: return "Paste";
    }
    return {};
}

std::optional<media::AudioData> readMediaFile (const model::ProjectModel& model, const std::string& hash, MediaPool& media,
                                               const std::filesystem::path& mediaDir, std::string* error)
{
    media::AudioData audio;
    audio.sampleRate = model.info.sampleRate;
    media::WavReader reader;
    if (reader.open (mediaDir / media::mediaFileName (hash)))
    {
        const auto frames = reader.info().frames;
        audio.sampleRate = reader.info().sampleRate;
        audio.channels.assign (static_cast<std::size_t> (reader.info().channels), std::vector<float> (static_cast<std::size_t> (frames)));
        std::vector<float*> dest;
        for (auto& c : audio.channels)
            dest.push_back (c.data());
        reader.read (0, frames, dest.data());
        return audio;
    }
    if (const auto source = media.find (hash); source != nullptr && source->resident())
    {
        const auto frames = source->numFrames();
        audio.channels.assign (static_cast<std::size_t> (source->numChannels()), std::vector<float> (static_cast<std::size_t> (frames)));
        bool missing = false;
        for (int c = 0; c < source->numChannels(); ++c)
            for (std::int64_t i = 0; i < frames; ++i)
                audio.channels[static_cast<std::size_t> (c)][static_cast<std::size_t> (i)] = source->sample (c, i, missing);
        return audio;
    }
    if (error != nullptr)
        *error = "The audio file isn't here yet.";
    return std::nullopt;
}

std::optional<media::AudioData> copyMediaFrames (const model::ProjectModel& model, const std::string& hash, std::int64_t from, std::int64_t to,
                                                 MediaPool& media, const std::filesystem::path& mediaDir, std::string* error)
{
    auto whole = readMediaFile (model, hash, media, mediaDir, error);
    if (! whole)
        return std::nullopt;
    from = std::clamp<std::int64_t> (from, 0, whole->numFrames());
    to = std::clamp<std::int64_t> (to, from, whole->numFrames());
    media::AudioData out;
    out.sampleRate = whole->sampleRate;
    for (const auto& c : whole->channels)
        out.channels.emplace_back (c.begin() + from, c.begin() + to);
    return out;
}

commands::EditResult editAudioFile (commands::Session& session, const std::string& hash, FileEdit edit, std::int64_t from, std::int64_t to,
                                    MediaPool& media, const std::filesystem::path& mediaDir, double gainDb, const media::AudioData* insert)
{
    using commands::EditResult;
    const auto& model = session.model();
    const auto* ref = model.mediaRef (hash);
    if (ref == nullptr)
        return EditResult::fail ("That audio file isn't in the project.");
    std::string error;
    auto audio = readMediaFile (model, hash, media, mediaDir, &error);
    if (! audio)
        return EditResult::fail (error);
    const auto frames = audio->numFrames();
    from = std::clamp<std::int64_t> (from, 0, frames);
    to = std::clamp<std::int64_t> (to, from, frames);
    const bool keepsLength = edit != FileEdit::trim && edit != FileEdit::remove && edit != FileEdit::insert;
    if (keepsLength && from == to)
        from = 0, to = frames; // nothing selected: the whole file
    if ((edit == FileEdit::trim || edit == FileEdit::remove) && from == to)
        return EditResult::fail ("Select part of the file first.");
    if (edit == FileEdit::insert && (insert == nullptr || insert->numFrames() == 0))
        return EditResult::fail ("Copy some audio first.");
    const auto begin = static_cast<std::size_t> (from), end = static_cast<std::size_t> (to);
    std::int64_t inserted = to - from;
    switch (edit)
    {
        case FileEdit::reverse:
            for (auto& c : audio->channels)
                std::reverse (c.begin() + static_cast<std::ptrdiff_t> (begin), c.begin() + static_cast<std::ptrdiff_t> (end));
            break;
        case FileEdit::invert:
            for (auto& c : audio->channels)
                for (auto i = begin; i < end; ++i)
                    c[i] = -c[i];
            break;
        case FileEdit::normalize:
        case FileEdit::gain:
        {
            float factor = static_cast<float> (std::pow (10.0, gainDb / 20.0));
            if (edit == FileEdit::normalize)
            {
                float peak = 0.0f;
                for (const auto& c : audio->channels)
                    for (auto i = begin; i < end; ++i)
                        peak = std::max (peak, std::abs (c[i]));
                if (peak <= 0.0f)
                    return EditResult::fail ("The selection is silent.");
                factor = 1.0f / peak;
            }
            for (auto& c : audio->channels)
                for (auto i = begin; i < end; ++i)
                    c[i] *= factor;
            break;
        }
        case FileEdit::fadeIn:
        case FileEdit::fadeOut:
        {
            const auto n = static_cast<double> (std::max<std::int64_t> (1, to - from - 1));
            for (auto& c : audio->channels)
                for (auto i = begin; i < end; ++i)
                {
                    const auto t = static_cast<double> (i - begin) / n;
                    c[i] *= static_cast<float> (edit == FileEdit::fadeIn ? t : 1.0 - t);
                }
            break;
        }
        case FileEdit::silence:
            for (auto& c : audio->channels)
                std::fill (c.begin() + static_cast<std::ptrdiff_t> (begin), c.begin() + static_cast<std::ptrdiff_t> (end), 0.0f);
            break;
        case FileEdit::removeDc:
            for (auto& c : audio->channels)
            {
                double sum = 0.0;
                for (auto i = begin; i < end; ++i)
                    sum += c[i];
                const auto mean = static_cast<float> (sum / static_cast<double> (end - begin));
                for (auto i = begin; i < end; ++i)
                    c[i] -= mean;
            }
            break;
        case FileEdit::trim:
            for (auto& c : audio->channels)
                c = std::vector<float> (c.begin() + static_cast<std::ptrdiff_t> (begin), c.begin() + static_cast<std::ptrdiff_t> (end));
            break;
        case FileEdit::remove:
            for (auto& c : audio->channels)
                c.erase (c.begin() + static_cast<std::ptrdiff_t> (begin), c.begin() + static_cast<std::ptrdiff_t> (end));
            inserted = 0;
            break;
        case FileEdit::insert:
        {
            // The pasted audio takes the file's channels: a mono clip goes to every channel; extra channels are dropped.
            for (std::size_t ch = 0; ch < audio->channels.size(); ++ch)
            {
                const auto& src = insert->channels[std::min (ch, insert->channels.size() - 1)];
                auto& c = audio->channels[ch];
                c.erase (c.begin() + static_cast<std::ptrdiff_t> (begin), c.begin() + static_cast<std::ptrdiff_t> (end));
                c.insert (c.begin() + static_cast<std::ptrdiff_t> (begin), src.begin(), src.end());
            }
            inserted = insert->numFrames();
            break;
        }
    }
    if (audio->numFrames() < model::minAudioRegionLength)
        return EditResult::fail ("The file would be too short.");
    const auto stored = media::storeMedia (*audio, media::formatForBitDepth (ref->bitDepth), mediaDir, &error);
    if (! stored)
        return EditResult::fail (error);
    if (*stored == hash)
        return EditResult::fail ("The edit doesn't change the file.");
    if (media.find (*stored) == nullptr && ! media.load (mediaDir, *stored))
        media.add (*stored, *audio);
    auto out = *ref;
    out.hash = *stored;
    out.lengthSamples = audio->numFrames();
    std::error_code ec;
    out.sizeBytes = static_cast<std::int64_t> (std::filesystem::file_size (mediaDir / media::mediaFileName (*stored), ec));
    if (edit == FileEdit::trim)
        return session.trimMediaFile (hash, out, from, to);
    return session.replaceMediaFile (hash, out, from, to, inserted, "Edit File: " + fileEditName (edit));
}

std::int64_t nearestZeroCrossing (const MediaSource& source, std::int64_t frame, std::int64_t radius)
{
    const auto at = [&] (std::int64_t f) {
        bool missing = false;
        float sum = 0.0f;
        for (int c = 0; c < source.numChannels(); ++c)
            sum += source.sample (c, f, missing);
        return sum;
    };
    // A crossing "at f" means the sample at f is 0 or has the other sign from f - 1.
    const auto crosses = [&] (std::int64_t f) {
        if (f <= 0 || f >= source.numFrames())
            return f == 0 || f == source.numFrames();
        const auto a = at (f - 1), b = at (f);
        return b == 0.0f || (a < 0.0f) != (b < 0.0f);
    };
    for (std::int64_t d = 0; d <= radius; ++d)
    {
        if (crosses (frame - d))
            return frame - d;
        if (crosses (frame + d))
            return frame + d;
    }
    return frame;
}
} // namespace weft::engine
