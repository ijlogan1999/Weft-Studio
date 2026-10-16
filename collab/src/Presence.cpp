#include "weft/collab/Presence.h"

#include <algorithm>
#include <cmath>

namespace weft::collab
{
namespace
{
const char* viewName (Presence::View v)
{
    switch (v)
    {
        case Presence::View::arrange: return "arrange";
        case Presence::View::pianoRoll: return "pianoRoll";
        case Presence::View::audioEditor: return "audioEditor";
        case Presence::View::mixer: return "mixer";
    }
    return "arrange";
}

Presence::View viewFrom (const std::string& s)
{
    if (s == "pianoRoll")
        return Presence::View::pianoRoll;
    if (s == "audioEditor")
        return Presence::View::audioEditor;
    if (s == "mixer")
        return Presence::View::mixer;
    return Presence::View::arrange;
}

doc::Value ids (const std::vector<Id>& v)
{
    auto l = doc::Value::list();
    for (std::size_t i = 0; i < std::min (v.size(), Presence::maxSelectionIds); ++i)
        l.listRef().push_back (v[i]);
    return l;
}

std::vector<Id> idsFrom (const doc::Value& v)
{
    std::vector<Id> out;
    for (const auto& id : v.asList())
        if (id.isString() && out.size() < Presence::maxSelectionIds)
            out.push_back (id.asString());
    return out;
}
} // namespace

const char* statusName (Presence::Status s)
{
    switch (s)
    {
        case Presence::Status::active: return "active";
        case Presence::Status::idle: return "idle";
        case Presence::Status::recording: return "recording";
        case Presence::Status::offlineEditing: return "offline-editing";
    }
    return "active";
}

doc::Value Presence::toValue() const
{
    auto v = doc::Value::map()
                 .set ("status", statusName (status))
                 .set ("view", viewName (view))
                 .set ("focus", doc::Value::map().set ("trackId", focusTrack).set ("regionId", focusRegion))
                 .set ("playhead", doc::Value::map().set ("pos", playhead.pos).set ("playing", playhead.playing).set ("at", playhead.at));
    if (pointer)
        v.set ("pointer", doc::Value::map()
                              .set ("area", viewName (pointer->area))
                              .set ("pos", pointer->pos)
                              .set ("track", pointer->track)
                              .set ("pitch", pointer->pitch));
    auto sel = doc::Value::map();
    const auto total = std::max<std::int64_t> (selectionCount, static_cast<std::int64_t> (regions.size() + notes.size() + points.size()));
    if (total > static_cast<std::int64_t> (maxSelectionIds))
        sel.set ("count", total);
    sel.set ("regions", ids (regions)).set ("notes", ids (notes)).set ("automationPoints", ids (points));
    v.set ("selection", sel);
    if (! writingAutomationOn.empty())
        v.set ("writingAutomationOn", writingAutomationOn);
    return v;
}

Presence Presence::fromValue (const doc::Value& v)
{
    Presence p;
    const auto status = v["status"].asString();
    p.status = status == "idle"              ? Status::idle
             : status == "recording"         ? Status::recording
             : status == "offline-editing"   ? Status::offlineEditing
                                             : Status::active;
    p.view = viewFrom (v["view"].asString());
    p.focusTrack = v["focus"]["trackId"].asString();
    p.focusRegion = v["focus"]["regionId"].asString();
    p.playhead.pos = v["playhead"]["pos"].asInt();
    p.playhead.playing = v["playhead"]["playing"].asBool();
    p.playhead.at = v["playhead"]["at"].asInt();
    if (const auto* ptr = v.find ("pointer"); ptr != nullptr && ptr->isMap())
    {
        Pointer q;
        q.area = viewFrom ((*ptr)["area"].asString());
        q.pos = (*ptr)["pos"].asInt();
        q.track = (*ptr)["track"].asString();
        q.pitch = static_cast<int> ((*ptr)["pitch"].asInt (-1));
        p.pointer = q;
    }
    const auto& sel = v["selection"];
    p.regions = idsFrom (sel["regions"]);
    p.notes = idsFrom (sel["notes"]);
    p.points = idsFrom (sel["automationPoints"]);
    p.selectionCount = sel["count"].asInt (static_cast<std::int64_t> (p.regions.size() + p.notes.size() + p.points.size()));
    p.writingAutomationOn = v["writingAutomationOn"].asString();
    return p;
}

Ticks extrapolatePlayhead (const Presence::Playhead& p, const TempoMap& tempo, std::int64_t nowMs, std::int64_t receivedAtMs)
{
    if (! p.playing)
        return p.pos;
    // Clocks on two machines rarely agree to the millisecond, but a skew of seconds means
    // the sender's clock is wrong: count from when we heard instead (D-COL-23).
    const auto since = std::abs (receivedAtMs - p.at) > 5000 ? nowMs - receivedAtMs : nowMs - p.at;
    const auto sample = tempo.tickToSample (static_cast<double> (p.pos)) + static_cast<double> (std::max<std::int64_t> (0, since)) * tempo.sampleRate() / 1000.0;
    return static_cast<Ticks> (std::llround (tempo.sampleToTick (sample)));
}
} // namespace weft::collab
