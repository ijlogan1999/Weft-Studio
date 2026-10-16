// AT-RT-2: random document edits during playback, from this user and from a collaborator,
// with the null device timing every callback against its real-time period.

#include "EngineFixtures.h"

#include "weft/engine/dsp/BuiltinEffects.h"
#include "weft/commands/Notes.h"
#include "weft/rt/AudioThread.h"

#include <catch2/catch_get_random_seed.hpp>

#include <cmath>

using namespace weft;
using namespace weft::test;

namespace
{
model::PluginInstance builtinPlugin (const std::string& uid)
{
    model::PluginInstance p;
    p.format = "builtin";
    p.pluginUid = uid;
    p.name = uid;
    return p;
}

/** Picks random edits from what the document holds now. */
class Fuzzer
{
public:
    Fuzzer (Session& s, std::uint32_t seed) : session (s), rng (seed)
    {
        for (const auto& info : engine::builtinPlugins())
            if (! info.instrument)
                if (const auto p = engine::createBuiltin (info.uid))
                {
                    effects.push_back (info.uid);
                    for (const auto& param : p->parameters())
                        params[info.uid].push_back (param.id);
                }
    }

    // Straight from the engine's bits rather than std distributions, so a seed replays the same
    // edits with every standard library.
    int pick (int n) { return static_cast<int> (rng() % static_cast<std::uint32_t> (n)); }
    double unit() { return static_cast<double> (rng()) / 4294967296.0; }
    bool coin() { return pick (2) == 0; }

    template <typename Map>
    std::optional<typename Map::key_type> any (const Map& m)
    {
        if (m.empty())
            return std::nullopt;
        return std::next (m.begin(), pick (static_cast<int> (m.size())))->first;
    }

    std::vector<Id> tracks (std::optional<model::TrackKind> kind = {}) const
    {
        std::vector<Id> out;
        for (const auto& id : session.model().trackOrder)
            if (const auto* t = session.model().track (id); t != nullptr && (! kind || t->kind == *kind))
                out.push_back (id);
        return out;
    }
    std::optional<Id> anyTrack (std::optional<model::TrackKind> kind = {})
    {
        const auto ts = tracks (kind);
        if (ts.empty())
            return std::nullopt;
        return ts[static_cast<std::size_t> (pick (static_cast<int> (ts.size())))];
    }
    std::optional<Id> anyMixable()
    {
        auto ts = tracks();
        for (const auto& [id, t] : session.model().tracks)
            if (t->kind == model::TrackKind::bus)
                ts.push_back (id);
        if (ts.empty())
            return std::nullopt;
        return ts[static_cast<std::size_t> (pick (static_cast<int> (ts.size())))];
    }
    /** Shared, so it outlives the edit that replaces the model's copy. */
    std::shared_ptr<const model::Region> anyRegion (std::optional<bool> midi = {})
    {
        std::vector<std::shared_ptr<const model::Region>> rs;
        for (const auto& [id, r] : session.model().regions)
            if (! midi || r->isMidi == *midi)
                rs.push_back (r);
        return rs.empty() ? nullptr : rs[static_cast<std::size_t> (pick (static_cast<int> (rs.size())))];
    }
    Ticks anyPos (int bars = 12) { return pick (bars * 16) * 240; }

    /** One random edit; returns its name (for the failure message). */
    const char* edit()
    {
        auto& s = session;
        const auto& m = s.model();
        switch (pick (24))
        {
            case 0:
                if (const auto r = anyRegion())
                {
                    auto to = coin() ? std::optional<Id> {} : anyTrack (r->isMidi ? model::TrackKind::instrument : model::TrackKind::audio);
                    (void) s.moveRegion (r->id, anyPos(), to);
                }
                return "move region";
            case 1:
                if (const auto r = anyRegion())
                    (void) s.trimRegionLength (r->id, r->isMidi ? 240 + pick (32) * 240 : 4800 + pick (40) * 4800);
                return "trim region";
            case 2:
                if (const auto r = anyRegion())
                    (void) s.splitRegion (r->id, r->start + 240 + pick (8) * 240);
                return "split region";
            case 3:
                if (m.regions.size() > 6)
                    if (const auto r = anyRegion())
                        (void) s.deleteRegions ({ r->id });
                return "delete region";
            case 4:
                if (coin())
                {
                    if (const auto t = anyTrack (model::TrackKind::audio))
                        (void) s.createAudioRegion (*t, anyPos(), "b3:ramp", pick (10) * 4800, 4800 + pick (40) * 4800);
                }
                else if (const auto t = anyTrack (model::TrackKind::instrument))
                    (void) s.createMidiRegion (*t, anyPos(), 960 + pick (16) * 240);
                return "create region";
            case 5:
                if (const auto r = anyRegion())
                {
                    (void) s.setRegionGain (r->id, -24.0 + unit() * 30.0);
                    if (coin())
                        (void) s.setRegionMuted (r->id, coin());
                }
                return "region gain/mute";
            case 6:
                if (const auto r = anyRegion (false))
                {
                    model::Fade in, out;
                    static const char* curves[] = { "linear", "equalPower", "sCurve" };
                    in.length = pick (4) * 2400;
                    out.length = pick (4) * 2400;
                    in.curve = curves[pick (3)];
                    out.curve = curves[pick (3)];
                    (void) s.setRegionFades (r->id, in, out);
                }
                return "region fades";
            case 7:
                if (const auto r = anyRegion())
                    (void) s.setRegionLoop (r->id, coin() ? std::optional<Ticks> {} : std::optional<Ticks> (r->start + r->length * (1 + pick (3))));
                return "region loop";
            case 8:
                if (const auto r = anyRegion (true))
                    for (int i = 0, n = 1 + pick (8); i < n; ++i)
                    {
                        model::Note n2;
                        n2.pos = pick (32) * 60;
                        n2.length = 60 + pick (16) * 60;
                        n2.pitch = 36 + pick (48);
                        n2.velocity = 1 + pick (127);
                        (void) s.addNote (r->id, n2);
                    }
                return "add notes";
            case 9:
                if (const auto r = anyRegion (true); r != nullptr && m.sequence (r->midi.sequenceId) != nullptr
                                                      && ! m.sequence (r->midi.sequenceId)->notes.empty())
                {
                    const auto& notes = m.sequence (r->midi.sequenceId)->notes;
                    const auto note = std::next (notes.begin(), pick (static_cast<int> (notes.size())))->first;
                    if (coin())
                        (void) s.deleteNotes (r->id, std::vector<Id> { note });
                    else
                        (void) s.moveNotes ({ commands::NoteRef { r->id, note } }, (pick (9) - 4) * 60, pick (13) - 6);
                }
                return "edit notes";
            case 10:
                if (const auto t = anyMixable())
                    switch (pick (3))
                    {
                        case 0: (void) s.setTrackVolume (*t, -40.0 + unit() * 46.0); break;
                        case 1: (void) s.setTrackPan (*t, unit() * 2.0 - 1.0); break;
                        default: (void) s.setTrackMute (*t, coin()); break;
                    }
                return "track volume/pan/mute";
            case 11:
                if (const auto t = anyMixable())
                {
                    const auto* track = m.track (*t);
                    if (track == nullptr)
                        return "inserts";
                    if (track->strip.inserts.size() < 3 || coin())
                        (void) s.addInsert (*t, builtinPlugin (effects[static_cast<std::size_t> (pick (static_cast<int> (effects.size())))]));
                    else
                        (void) s.removeInsert (*t, Id (track->strip.inserts[static_cast<std::size_t> (pick (static_cast<int> (track->strip.inserts.size())))].id));
                }
                return "inserts";
            case 12:
                if (const auto t = anyMixable())
                    if (const auto* track = m.track (*t); track != nullptr && ! track->strip.inserts.empty())
                    {
                        const auto p = track->strip.inserts[static_cast<std::size_t> (pick (static_cast<int> (track->strip.inserts.size())))];
                        const auto& ids = params[p.pluginUid];
                        if (coin() && ! ids.empty())
                            (void) s.setPluginParam (*t, p.id, ids[static_cast<std::size_t> (pick (static_cast<int> (ids.size())))], unit());
                        else
                            (void) s.setInsertBypass (*t, p.id, coin());
                    }
                return "plug-in parameter/bypass";
            case 13:
                if (const auto t = anyTrack())
                {
                    std::vector<Id> buses;
                    for (const auto& [id, b] : m.tracks)
                        if (b->kind == model::TrackKind::bus)
                            buses.push_back (id);
                    const auto* track = m.track (*t);
                    if (track != nullptr && ! track->strip.sends.empty() && coin())
                    {
                        const auto send = track->strip.sends[static_cast<std::size_t> (pick (static_cast<int> (track->strip.sends.size())))];
                        if (coin())
                            (void) s.setSendLevel (*t, send.id, -30.0 + unit() * 36.0);
                        else
                            (void) s.removeSend (*t, send.id);
                    }
                    else if (! buses.empty())
                        (void) s.addSend (*t, buses[static_cast<std::size_t> (pick (static_cast<int> (buses.size())))], -12.0 + unit() * 12.0, coin());
                }
                return "sends";
            case 14:
                if (const auto t = anyTrack())
                {
                    model::AutomationTarget target;
                    target.kind = coin() ? "volume" : "pan";
                    (void) s.addAutomationPoint (*t, target, anyPos(), unit(), unit() * 2.0 - 1.0);
                    if (coin())
                        (void) s.setTrackAutomationMode (*t, coin() ? "read" : "off");
                }
                return "automation";
            case 15:
                if (const auto laneId = any (m.automationLanes))
                    if (const auto& lane = m.automationLanes.at (*laneId); ! lane->points.empty())
                    {
                        const auto point = *any (lane->points);
                        if (coin())
                            (void) s.deleteAutomationPoint (*laneId, point);
                        else
                            (void) s.moveAutomationPoint (*laneId, point, anyPos(), unit());
                    }
                return "automation points";
            case 16:
                switch (pick (3))
                {
                    case 0: (void) s.addTempoPoint (anyPos() + 240, 60.0 + unit() * 140.0); break;
                    case 1:
                        if (const auto& points = m.tempo.points(); points.size() > 1)
                            (void) s.deleteTempoPoint (Id (points[static_cast<std::size_t> (1 + pick (static_cast<int> (points.size()) - 1))].id));
                        break;
                    default: (void) s.setTempoAt (0, 80.0 + unit() * 80.0); break;
                }
                return "tempo";
            case 17:
                (void) s.setSignatureAt (pick (8), 2 + pick (6), coin() ? 4 : 8);
                return "signature";
            case 18:
                if (tracks().size() < 12 && coin())
                    (void) s.createTrack (coin() ? model::TrackKind::audio : model::TrackKind::instrument, "Fuzz", 1 + pick (2));
                else if (tracks().size() > 4)
                    if (const auto t = anyTrack())
                        (void) s.deleteTrack (*t);
                return "create/delete track";
            case 19:
                if (coin() && s.canUndo())
                    (void) s.undo();
                else if (s.canRedo())
                    (void) s.redo();
                return "undo/redo";
            case 20:
                if (const auto t = anyTrack (model::TrackKind::instrument))
                {
                    model::PluginInstance keys;
                    keys.format = "builtin";
                    keys.pluginUid = "weft.keys";
                    keys.name = "Weft Keys";
                    keys.params["preset"] = static_cast<double> (pick (4)) / 3.0;
                    (void) s.setInstrument (*t, keys);
                }
                return "instrument";
            case 21:
                if (coin())
                    (void) s.setCycle (coin(), pick (4) * 3840, (5 + pick (8)) * 3840);
                else
                    (void) s.addMarker (anyPos(), "M");
                return "cycle/marker";
            case 22:
                if (const auto r = anyRegion (true))
                {
                    model::CcEvent cc;
                    cc.pos = pick (32) * 60;
                    cc.number = coin() ? 1 : 64;
                    cc.value = pick (128);
                    (void) s.addController (r->id, cc);
                }
                return "controllers";
            default:
                if (const auto t = anyTrack(); t && tracks().size() > 1)
                    (void) s.moveTrack (*t, static_cast<std::size_t> (pick (static_cast<int> (tracks().size()))));
                return "move track";
        }
    }

    Session& session;
    std::mt19937 rng;
    std::vector<std::string> effects;
    std::map<std::string, std::vector<std::string>> params;
};

/** Two copies of the document kept in step with the sync protocol, as the relay would. */
struct Link
{
    doc::SyncState toRemote, toLocal;
    void exchange (Session& a, Session& b)
    {
        for (int round = 0; round < 8; ++round)
        {
            bool any = false;
            if (const auto msg = a.document().generateSyncMessage (toRemote))
            {
                REQUIRE (b.receiveSyncMessage (toLocal, *msg) == doc::Status::ok);
                any = true;
            }
            if (const auto msg = b.document().generateSyncMessage (toLocal))
            {
                REQUIRE (a.receiveSyncMessage (toRemote, *msg) == doc::Status::ok);
                any = true;
            }
            if (! any)
                return;
        }
    }
};
} // namespace

TEST_CASE ("AT-RT-2 10,000 random edits during playback, local and remote, make no glitches and no crashes", "[E][rt][fuzz][AT-RT-2]")
{
    Rig rig (256);
    auto& s = rig.session;
    // Four audio and four instrument tracks with regions, and a bus.
    std::vector<Id> audio;
    for (int i = 0; i < 4; ++i)
    {
        audio.push_back (s.createTrack (TrackKind::audio, "Audio", 1 + i % 2).value());
        rig.addAudioRegion (audio.back(), i * barTicks, 2 * barSamples);
    }
    for (int i = 0; i < 4; ++i)
    {
        const auto keys = s.createTrack (TrackKind::instrument, "Keys").value();
        const auto region = s.createMidiRegion (keys, i * barTicks, 4 * barTicks).value();
        for (int n = 0; n < 32; ++n)
        {
            model::Note note;
            note.pos = n * 480;
            note.length = 400;
            note.pitch = 48 + (n * 7 + i) % 24;
            REQUIRE (s.addNote (region, note));
        }
    }
    REQUIRE (s.newBus (audio.front(), true));
    rig.sync();

    // A collaborator with a copy of the document.
    auto copy = doc::Document::load (s.document().save());
    REQUIRE (copy);
    Session remote (std::move (*copy), "userB", [] { return std::int64_t { 1'700'000'000'000 }; }, 2);

    const auto seed = Catch::getSeed();
    INFO ("seed " << seed << " (rerun with --rng-seed " << seed << ")");
    Fuzzer local (s, seed), theirs (remote, seed + 1);
    Link link;

    rig.device.setCaptureCapacity (0);
    rig.device.setDeadlineSimulation (true);
    rig.engine.play (0);
    REQUIRE (rig.device.render (256 * 8));
    const auto missesBefore = rig.device.stats().deadlineMisses;
    weft::rt::resetViolations();

    for (int i = 0; i < 10'000; ++i)
    {
        // Mostly local edits; one in five comes from the collaborator. The two copies meet every
        // few edits, so some edits are concurrent with ones the other side hasn't seen yet.
        const bool fromRemote = local.pick (5) == 0;
        const char* what = (fromRemote ? theirs : local).edit();
        INFO ("edit " << i << ": " << (fromRemote ? "remote " : "") << what);
        if (i % 8 == 0)
            link.exchange (s, remote);
        rig.sync();

        // Now and then the transport moves too.
        switch (local.pick (200))
        {
            case 0: rig.engine.stop(); break;
            case 1: rig.engine.play (local.anyPos()); break;
            case 2: rig.engine.locate (local.anyPos()); break;
            default: break;
        }
        const auto missed = rig.device.stats().deadlineMisses;
        REQUIRE (rig.device.render (256));
        CHECK (rig.device.stats().deadlineMisses == missed);
        if (i % 500 == 0)
            rig.engine.collectGarbage();
    }
    rig.engine.stop();
    REQUIRE (rig.device.render (256 * 4));

    CHECK (rig.device.stats().deadlineMisses == missesBefore);
    const auto v = weft::rt::violations();
    CHECK (v.allocations == 0);
    CHECK (v.blockingCalls == 0);
    // Both copies end up identical.
    link.exchange (s, remote);
    CHECK (s.document().heads() == remote.document().heads());
}
