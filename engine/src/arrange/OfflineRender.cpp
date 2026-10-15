#include "weft/engine/arrange/OfflineRender.h"

#include "weft/engine/devices/NullDevice.h"
#include "weft/engine/transport/Engine.h"
#include "weft/media/MediaStore.h"

#include <algorithm>
#include <cmath>

namespace weft::engine
{
media::AudioData renderOffline (const model::ProjectModel& model, MediaPool& media, const OfflineRenderOptions& options)
{
    media::AudioData out;
    out.sampleRate = model.info.sampleRate;
    InstrumentRack rack;
    Engine engine (media, rack);
    TransportSettings settings;
    settings.metronomeOn = options.metronome;
    settings.metronomeDuring = ClickDuring::both;
    engine.setSettings (settings);
    engine.setDiskThread (false);

    DeviceConfig config;
    config.sampleRate = model.info.sampleRate;
    config.maxBlockSize = std::max (options.blockSize, 32);
    config.numInputChannels = 0;
    config.numOutputChannels = 2;
    NullDevice device;
    if (options.frames <= 0 || ! device.open (config) || ! device.start (engine))
        return out;
    engine.setProject (model);
    engine.play (options.from);
    device.setCaptureCapacity (options.frames);
    // Render a chunk at a time and load the streamed blocks ahead of each one.
    constexpr std::int64_t chunk = 8192;
    for (std::int64_t done = 0; done < options.frames; done += chunk)
    {
        engine.pumpDisk();
        device.render (std::min (chunk, options.frames - done));
    }
    device.stop();
    for (int ch = 0; ch < 2; ++ch)
    {
        const auto c = device.capturedOutput (ch);
        out.channels.emplace_back (c.begin(), c.end());
    }
    return out;
}

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
    options.from = rs[first].start;
    options.frames = endSample - startSample;
    result.audio = renderOffline (m, media, options);
    if (result.audio.channels.size() != 2 || static_cast<std::int64_t> (result.audio.channels[0].size()) != options.frames)
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
} // namespace weft::engine
