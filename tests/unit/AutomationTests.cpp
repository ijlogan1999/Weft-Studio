// Automation in the model: lane values, curves, the fader taper, the parameter list, point
// edits and live-write thinning (W-8).

#include "Fixtures.h"

#include "weft/model/Automation.h"

#include <cmath>

using namespace weft;
using namespace weft::test;

namespace
{
model::AutoPoint point (Ticks pos, double value, double curve = 0.0)
{
    model::AutoPoint p;
    p.id = std::to_string (pos);
    p.pos = pos;
    p.value = value;
    p.curve = curve;
    return p;
}

bool near (double a, double b, double tolerance = 1e-9) { return std::abs (a - b) <= tolerance; }

std::vector<std::pair<std::string, std::string>> fakeParams (const model::PluginInstance& p)
{
    if (p.pluginUid == "weft.eq")
        return { { "lowGain", "Low Gain" }, { "highGain", "High Gain" } };
    return { { "gain", "Gain" } };
}

model::AutomationTarget kind (const std::string& k)
{
    model::AutomationTarget t;
    t.kind = k;
    return t;
}
} // namespace

TEST_CASE ("Fader taper: +6 dB at the top, 0 dB near 0.794, -inf at the bottom", "[U][automation]")
{
    CHECK (near (automation::toDb (1.0), 6.0));
    CHECK (near (automation::toDb (0.0), minusInfinityDb));
    CHECK (near (automation::toDb (automation::fromDb (0.0)), 0.0));
    CHECK (near (automation::fromDb (0.0), 0.7943, 1e-4));
    CHECK (near (automation::fromDb (-200.0), 0.0));
    for (double db : { -60.0, -24.0, -6.0, 0.0, 3.0 })
        CHECK (near (automation::toDb (automation::fromDb (db)), db, 1e-9));
    CHECK (near (automation::toPan (0.0), -1.0));
    CHECK (near (automation::toPan (1.0), 1.0));
    CHECK (near (automation::fromPan (0.0), 0.5));
    CHECK (automation::toMute (0.5));
    CHECK_FALSE (automation::toMute (0.49));
}

TEST_CASE ("Lane values: hold before and after, linear and curved segments", "[U][automation]")
{
    const std::vector<model::AutoPoint> linear { point (3840, 1.0), point (7680, 0.0) };
    CHECK (near (automation::valueAt (linear, 0), 1.0));
    CHECK (near (automation::valueAt (linear, 5760), 0.5));
    CHECK (near (automation::valueAt (linear, 10000), 0.0));

    const std::vector<model::AutoPoint> curved { point (3840, 1.0, 0.5), point (7680, 0.0) };
    CHECK_FALSE (near (automation::valueAt (curved, 5760), 0.5, 1e-3));
    double previous = 2.0;
    for (Ticks t = 3840; t <= 7680; t += 16)
    {
        const auto v = automation::valueAt (curved, static_cast<double> (t));
        CHECK (v <= previous + 1e-12);
        previous = v;
    }
    for (double c : { -1.0, -0.3, 0.0, 0.7, 1.0 })
    {
        CHECK (near (automation::shape (0.0, c), 0.0));
        CHECK (near (automation::shape (1.0, c), 1.0));
    }
    CHECK (near (automation::shape (0.5, 1.0), 0.0625));
    CHECK (near (automation::shape (0.25, -1.0), std::sqrt (0.5)));
}

TEST_CASE ("AT-AUT-2 the parameter selector lists mixer and plug-in parameters on every kind of track", "[U][automation]")
{
    Scene s;
    const auto& m = s.session.model();
    const auto choices = automation::targetsFor (m, s.instTrack, fakeParams);
    std::vector<std::string> names;
    for (const auto& c : choices)
        names.push_back (c.name);
    REQUIRE (names.size() >= 6);
    CHECK (names[0] == "Volume");
    CHECK (names[1] == "Pan");
    CHECK (names[2] == "Mute");
    CHECK (names[3] == "Send to Verb");
    CHECK (std::find (names.begin(), names.end(), "EQ: Low Gain") != names.end());
    CHECK (std::find (names.begin(), names.end(), "EQ: High Gain") != names.end());
    // The instrument track's instrument parameters come too.
    if (m.track (s.instTrack)->strip.instrument)
        CHECK (std::find_if (names.begin(), names.end(), [&] (const auto& n) { return n == m.track (s.instTrack)->strip.instrument->name + ": Gain"; }) != names.end());

    // Buses and the master offer the same.
    for (const auto& id : { s.bus, m.info.masterTrackId })
    {
        REQUIRE (! id.empty());
        model::PluginInstance p;
        p.format = "builtin";
        p.pluginUid = "weft.gain";
        p.name = "Gain";
        REQUIRE (s.session.addInsert (id, p));
        const auto list = automation::targetsFor (s.session.model(), id, fakeParams);
        REQUIRE (list.size() >= 4);
        CHECK (list[0].name == "Volume");
        CHECK (list[1].name == "Pan");
        CHECK (list[2].name == "Mute");
        CHECK (list.back().name == "Gain: Gain");
        CHECK (automation::targetExists (s.session.model(), id, list.back().target));
    }
    // A removed send's target no longer exists.
    model::AutomationTarget send;
    send.kind = "sendLevel";
    send.sendId = s.send;
    CHECK (automation::targetExists (s.session.model(), s.instTrack, send));
    REQUIRE (s.session.removeSend (s.instTrack, s.send));
    CHECK_FALSE (automation::targetExists (s.session.model(), s.instTrack, send));
}

TEST_CASE ("AT-AUT-3 point edits: add, move several, bend, delete several, each one undo step", "[U][automation]")
{
    Scene s;
    const auto volume = kind ("volume");
    const auto a = s.session.addAutomationPoint (s.audioTrack, volume, 0, 0.8);
    REQUIRE (a);
    const auto* lane = s.session.model().lane (s.audioTrack, volume);
    REQUIRE (lane != nullptr);
    const auto laneId = lane->id;
    const auto b = s.session.addAutomationPoint (s.audioTrack, volume, 3840, 0.2).value();
    const auto c = s.session.addAutomationPoint (s.audioTrack, volume, 7680, 0.6).value();
    CHECK (s.session.model().lane (s.audioTrack, volume)->points.size() == 3);

    REQUIRE (s.session.moveAutomationPoints (laneId, { { b, 4800, 0.3 }, { c, 8640, 0.7 } }));
    auto points = automation::sortedPoints (*s.session.model().lane (s.audioTrack, volume));
    CHECK (points[1].pos == 4800);
    CHECK (near (points[1].value, 0.3));
    CHECK (points[2].pos == 8640);
    REQUIRE (s.session.undo().done);
    points = automation::sortedPoints (*s.session.model().lane (s.audioTrack, volume));
    CHECK (points[1].pos == 3840);
    CHECK (points[2].pos == 7680);

    REQUIRE (s.session.setAutomationCurve (laneId, *a, 0.4));
    CHECK (near (s.session.model().lane (s.audioTrack, volume)->points.at (*a).curve, 0.4));
    REQUIRE (s.session.setAutomationCurve (laneId, *a, 3.0));
    CHECK (near (s.session.model().lane (s.audioTrack, volume)->points.at (*a).curve, 1.0));

    REQUIRE (s.session.deleteAutomationPoints (laneId, { b, c }));
    CHECK (s.session.model().lane (s.audioTrack, volume)->points.size() == 1);
    REQUIRE (s.session.undo().done);
    CHECK (s.session.model().lane (s.audioTrack, volume)->points.size() == 3);
    CHECK_FALSE (s.session.moveAutomationPoints (laneId, { { b, -10, 0.3 } }));
}

TEST_CASE ("A write pass replaces points in its range and switches Write to Latch as one undo step", "[U][automation]")
{
    Scene s;
    const auto volume = kind ("volume");
    for (Ticks t : { 0, 3840, 7680, 15360 })
        REQUIRE (s.session.addAutomationPoint (s.audioTrack, volume, t, 0.25));
    REQUIRE (s.session.setTrackAutomationMode (s.audioTrack, "write"));
    Session::AutomationWrite write;
    write.trackId = s.audioTrack;
    write.target = volume;
    write.from = 3000;
    write.to = 8000;
    write.points = { { 3000, 0.9 }, { 8000, 0.9 } };
    Session::AutomationWrite pan;
    pan.trackId = s.audioTrack;
    pan.target = kind ("pan");
    pan.from = 3000;
    pan.to = 8000;
    pan.points = { { 3000, 0.1 }, { 8000, 0.1 } };
    REQUIRE (s.session.writeAutomation ({ write, pan }, { s.audioTrack }));
    auto points = automation::sortedPoints (*s.session.model().lane (s.audioTrack, volume));
    REQUIRE (points.size() == 4);
    CHECK (points[0].pos == 0);
    CHECK (points[1].pos == 3000);
    CHECK (near (points[1].value, 0.9));
    CHECK (points[2].pos == 8000);
    CHECK (points[3].pos == 15360);
    REQUIRE (s.session.model().lane (s.audioTrack, kind ("pan")) != nullptr);
    CHECK (s.session.model().track (s.audioTrack)->automationMode == "latch");
    CHECK (s.session.undoName() == "Write Automation");

    REQUIRE (s.session.undo().done);
    CHECK (s.session.model().lane (s.audioTrack, volume)->points.size() == 4);
    CHECK (s.session.model().lane (s.audioTrack, kind ("pan")) == nullptr);
    CHECK (s.session.model().track (s.audioTrack)->automationMode == "write");
}

TEST_CASE ("AT-AUT-11 live write thinning: a constant gives two points, a ramp at most one per 50 ms", "[U][automation]")
{
    // 4 bars at 120 BPM = 8 s, one value per 10 ms.
    {
        automation::Recorder r;
        for (int i = 0; i <= 800; ++i)
            r.add (i * 0.01, i * 19, 0.6);
        r.finish();
        CHECK (r.points().size() <= 2);
        CHECK (r.points().size() == 2);
        CHECK (r.points().front().first == 0);
        CHECK (r.points().back().first == 800 * 19);
    }
    {
        automation::Recorder r;
        std::vector<double> seconds;
        for (int i = 0; i <= 800; ++i)
            r.add (i * 0.01, i * 19, i / 800.0);
        r.finish();
        const auto& pts = r.points();
        CHECK (pts.size() > 10);
        for (std::size_t i = 1; i < pts.size(); ++i)
        {
            const auto dt = static_cast<double> (pts[i].first - pts[i - 1].first) / 19.0 * 0.01;
            CHECK (dt >= 0.05 - 1e-9);
        }
        CHECK (near (pts.back().second, 1.0));
    }
}
