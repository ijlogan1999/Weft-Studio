// Read-time repair (spec 03 §10.4): concurrent edits that leave the document in
// a state no single client would write are shown the same way on every client.

#include "Fixtures.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace weft;
using namespace weft::test;
using doc::Value;

namespace
{
void checkSameView (const Session& a, const Session& b)
{
    CHECK (a.document().root() == b.document().root());
    CHECK (a.model().trackOrder == b.model().trackOrder);
    CHECK (a.model().disabledEdges == b.model().disabledEdges);
    CHECK (a.model().recoveredTrackFor == b.model().recoveredTrackFor);
}
} // namespace

TEST_CASE ("Repair 1 and 2: trackOrder is deduped and completed", "[U][repair]")
{
    Scene scene;
    auto& a = scene.session;
    auto b = joinSession (a, 7, "userB");
    // Both users move the same track at once: it appears twice in the stored list.
    REQUIRE (a.moveTrack (scene.bus, 0));
    REQUIRE (b.moveTrack (scene.bus, 2));
    sync (a, b);
    const auto& stored = a.model().storedTrackOrder;
    CHECK (std::count (stored.begin(), stored.end(), scene.bus) == 2);
    const auto& order = a.model().trackOrder;
    CHECK (std::count (order.begin(), order.end(), scene.bus) == 1);
    CHECK (order.size() == 3);
    checkSameView (a, b);

    // A track missing from trackOrder is appended.
    const auto t = a.createTrack (TrackKind::audio, "Late").value();
    REQUIRE (a.document().put (doc::path ({ "trackOrder" }), Value::list()) == doc::Status::ok);
    a.document().commit ("test", 0);
    a.reproject();
    auto expected = std::vector<Id> { scene.audioTrack, scene.instTrack, scene.bus, t };
    std::sort (expected.begin(), expected.end());
    CHECK (a.model().trackOrder == expected);
}

TEST_CASE ("Repair 3: orphan regions go to a deterministic Recovered track", "[U][repair]")
{
    Scene scene;
    auto& a = scene.session;
    auto b = joinSession (a, 7, "userB");
    REQUIRE (a.deleteTrack (scene.instTrack));
    const auto added = b.createMidiRegion (scene.instTrack, 15360, 960);
    REQUIRE (added);
    sync (a, b);
    const auto rid = model::recoveredTrackId (scene.instTrack);
    REQUIRE (a.model().region (*added) != nullptr);
    CHECK (a.model().effectiveTrackId (*a.model().region (*added)) == rid);
    REQUIRE (a.model().track (rid) != nullptr);
    CHECK (a.model().track (rid)->name == "Recovered");
    CHECK (std::find (a.model().trackOrder.begin(), a.model().trackOrder.end(), rid) != a.model().trackOrder.end());
    checkSameView (a, b);

    // Both clients write the real track; they agree on its id, so there is one.
    REQUIRE (a.materializeRecoveredTracks());
    REQUIRE (b.materializeRecoveredTracks());
    sync (a, b);
    CHECK (a.model().tracks.contains (rid));
    CHECK (a.model().syntheticTracks.empty());
    CHECK (a.model().region (*added)->trackId == rid);
    CHECK (std::count (a.model().trackOrder.begin(), a.model().trackOrder.end(), rid) == 1);
    checkSameView (a, b);
}

TEST_CASE ("Repair 4: two tempo points at one position, greater Id wins", "[U][repair]")
{
    Scene scene;
    auto& a = scene.session;
    auto b = joinSession (a, 7, "userB");
    const auto pa = a.addTempoPoint (7680, 100.0).value();
    const auto pb = b.addTempoPoint (7680, 140.0).value();
    sync (a, b);
    CHECK (a.model().storedTempoPoints.size() == 3);
    const auto winner = std::max (pa, pb);
    const double bpm = winner == pa ? 100.0 : 140.0;
    CHECK (a.model().tempo.bpmAt (7680) == bpm);
    CHECK (b.model().tempo.bpmAt (7680) == bpm);
    const auto& pts = a.model().tempo.points();
    CHECK (std::find_if (pts.begin(), pts.end(), [&] (const auto& p) { return p.id == winner && p.conflicted; }) != pts.end());
}

TEST_CASE ("Repair 5: a concurrent routing cycle disables the newest edge", "[U][repair][routing]")
{
    Scene scene;
    auto& a = scene.session;
    const auto bus2 = a.createTrack (TrackKind::bus, "Bus B").value();
    auto b = joinSession (a, 7, "userB");
    const auto s1 = a.addSend (scene.bus, bus2);
    const auto s2 = b.addSend (bus2, scene.bus);
    REQUIRE (s1);
    REQUIRE (s2);
    sync (a, b);
    REQUIRE (a.model().disabledEdges.size() == 1);
    checkSameView (a, b);
    const auto disabled = *a.model().disabledEdges.begin();
    // The disabled edge is the one written last (greater op stamp).
    auto stampOf = [&] (const Id& track, const Id& send) {
        const auto op = a.document().opId (doc::path ({ "tracks", track, "strip", "sends" }) / doc::PathSegment::id (send) / "targetBusId");
        REQUIRE (op);
        return doc::OpStamp::parse (*op).value();
    };
    const auto newest = stampOf (scene.bus, *s1) > stampOf (bus2, *s2) ? model::RoutingEdge { scene.bus, bus2, *s1 } : model::RoutingEdge { bus2, scene.bus, *s2 };
    CHECK (disabled == newest.key());

    // Removing the disabled send leaves an acyclic graph.
    REQUIRE (a.removeSend (newest.fromTrackId, newest.sendId));
    CHECK (a.model().disabledEdges.empty());
}

TEST_CASE ("Repair 6: duplicate automation lanes are unioned", "[U][repair]")
{
    Scene scene;
    auto& a = scene.session;
    auto b = joinSession (a, 7, "userB");
    model::AutomationTarget pan;
    pan.kind = "pan";
    REQUIRE (a.addAutomationPoint (scene.audioTrack, pan, 0, 0.2));
    REQUIRE (b.addAutomationPoint (scene.audioTrack, pan, 960, 0.9));
    sync (a, b);
    CHECK (a.model().storedLanes.size() == 3);
    const auto* lane = a.model().lane (scene.audioTrack, pan);
    REQUIRE (lane != nullptr);
    CHECK (lane->points.size() == 2);
    CHECK (lane->mergedLaneIds.size() == 1);
    CHECK (lane->id < lane->mergedLaneIds[0]);
    // A point that lives in the merged lane can still be edited through the canonical one.
    const auto otherPoint = std::find_if (lane->points.begin(), lane->points.end(), [] (const auto& kv) { return kv.second.pos == 960; })->first;
    REQUIRE (a.moveAutomationPoint (lane->id, otherPoint, 1920, 0.5));
    CHECK (a.model().lane (scene.audioTrack, pan)->points.at (otherPoint).pos == 1920);
}

TEST_CASE ("Repair 7: out-of-range values are clamped at read time", "[U][repair]")
{
    Scene scene;
    auto& a = scene.session;
    auto b = joinSession (a, 7, "userB");
    // A trims the region short while B sets a long fade.
    REQUIRE (a.trimRegionLength (scene.audioRegion, 10000));
    REQUIRE (b.setRegionFades (scene.audioRegion, std::nullopt, model::Fade { 48000, "linear" }));
    sync (a, b);
    const auto* r = a.model().region (scene.audioRegion);
    CHECK (r->length == 10000);
    CHECK (r->audio.fadeIn.length + r->audio.fadeOut.length <= 10000);
    // Garbage values are clamped too.
    REQUIRE (a.document().put (doc::path ({ "regions", scene.audioRegion, "audio", "gainDb" }), Value (999.0)) == doc::Status::ok);
    REQUIRE (a.document().put (doc::path ({ "tracks", scene.audioTrack, "strip", "pan" }), Value ("left")) == doc::Status::ok);
    a.document().commit ("test", 0);
    a.reproject();
    CHECK (a.model().region (scene.audioRegion)->audio.gainDb == 24.0);
    CHECK (a.model().track (scene.audioTrack)->strip.pan == 0.0);
}
