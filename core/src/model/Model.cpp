#include "weft/model/Model.h"

#include <algorithm>
#include <cmath>

namespace weft::model
{
namespace
{
template <typename T>
T clampInt (const Value& v, T fallback, T lo, T hi)
{
    return static_cast<T> (std::clamp<std::int64_t> (v.asInt (fallback), lo, hi));
}

double clampDouble (const Value& v, double fallback, double lo, double hi) { return std::clamp (v.asDouble (fallback), lo, hi); }

std::string oneOf (const Value& v, std::initializer_list<std::string_view> allowed, std::string_view fallback)
{
    const auto s = v.asString();
    for (auto a : allowed)
        if (s == a)
            return s;
    return std::string (fallback);
}

Value fadeToValue (const Fade& f) { return Value::map().set ("length", f.length).set ("curve", f.curve); }

Fade fadeFromValue (const Value& v)
{
    Fade f;
    f.length = std::max<std::int64_t> (0, v["length"].asInt (0));
    f.curve = oneOf (v["curve"], { "linear", "equalPower", "sCurve" }, "equalPower");
    return f;
}

std::vector<Id> idList (const Value& v)
{
    std::vector<Id> out;
    for (const auto& item : v.asList())
        if (item.isString() && std::find (out.begin(), out.end(), item.asString()) == out.end())
            out.push_back (item.asString()); // duplicates: keep first (D-DM-18)
    return out;
}

Value idListValue (const std::vector<Id>& ids)
{
    Value::List l;
    for (const auto& id : ids)
        l.emplace_back (id);
    return Value (std::move (l));
}
} // namespace

// Color ---------------------------------------------------------------------

Color Color::fromValue (const Value& v)
{
    Color c;
    if (v.isString())
    {
        const auto s = v.asString();
        if (s.size() == 7 && s[0] == '#')
            c.rgb = s;
    }
    else
        c.palette = clampInt<int> (v, 0, 0, 23);
    return c;
}

Value Color::toValue() const { return rgb.empty() ? Value (palette) : Value (rgb); }

// Track ---------------------------------------------------------------------

std::string_view toString (TrackKind k)
{
    switch (k)
    {
        case TrackKind::audio: return "audio";
        case TrackKind::instrument: return "instrument";
        case TrackKind::bus: return "bus";
        case TrackKind::master: return "master";
    }
    return "audio";
}

std::optional<TrackKind> trackKindFromString (std::string_view s)
{
    for (auto k : { TrackKind::audio, TrackKind::instrument, TrackKind::bus, TrackKind::master })
        if (toString (k) == s)
            return k;
    return std::nullopt;
}

InputAssignment InputAssignment::fromValue (const Value& v)
{
    InputAssignment in;
    const auto kind = v["kind"].asString();
    if (kind == "audio")
    {
        in.kind = Kind::audio;
        in.firstChannel = clampInt<int> (v["firstChannel"], 0, 0, 1023);
    }
    else if (kind == "midi")
    {
        in.kind = Kind::midi;
        in.device = v["device"].asString ("all");
        in.channel = v["channel"].isString() ? 0 : clampInt<int> (v["channel"], 0, 0, 16);
    }
    return in;
}

Value InputAssignment::toValue() const
{
    switch (kind)
    {
        case Kind::audio: return Value::map().set ("kind", "audio").set ("firstChannel", firstChannel);
        case Kind::midi:
            return Value::map().set ("kind", "midi").set ("device", device).set ("channel", channel == 0 ? Value ("all") : Value (channel));
        case Kind::none: break;
    }
    return Value();
}

OutputAssignment OutputAssignment::fromValue (const Value& v)
{
    OutputAssignment out;
    if (v["kind"].asString() == "bus")
        out.busId = v["trackId"].asString();
    return out;
}

Value OutputAssignment::toValue() const
{
    if (toMaster())
        return Value::map().set ("kind", "master");
    return Value::map().set ("kind", "bus").set ("trackId", busId);
}

PluginInstance PluginInstance::fromValue (const Value& v)
{
    PluginInstance p;
    p.id = v["id"].asString();
    p.format = oneOf (v["format"], { "builtin", "vst3" }, "builtin");
    p.pluginUid = v["pluginUid"].asString();
    p.name = v["name"].asString();
    p.vendor = v["vendor"].asString();
    p.version = v["version"].asString();
    p.bypass = v["bypass"].asBool();
    p.state = v["state"];
    for (const auto& [k, value] : v["params"].asMap())
        p.params[k] = std::clamp (value.asDouble(), 0.0, 1.0);
    p.addedBy = v["addedBy"].asString();
    return p;
}

Value PluginInstance::toValue() const
{
    Value paramsValue = Value::map();
    for (const auto& [k, value] : params)
        paramsValue.set (k, value);
    auto v = Value::map()
                 .set ("id", id)
                 .set ("format", format)
                 .set ("pluginUid", pluginUid)
                 .set ("name", name)
                 .set ("vendor", vendor)
                 .set ("version", version)
                 .set ("bypass", bypass)
                 .set ("state", state)
                 .set ("params", std::move (paramsValue));
    if (! addedBy.empty())
        v.set ("addedBy", addedBy);
    return v;
}

Send Send::fromValue (const Value& v)
{
    Send s;
    s.id = v["id"].asString();
    s.targetBusId = v["targetBusId"].asString();
    s.levelDb = clampDouble (v["levelDb"], 0.0, minusInfinityDb, 6.0);
    s.pre = v["mode"].asString() == "pre";
    s.muted = v["muted"].asBool();
    return s;
}

Value Send::toValue() const
{
    return Value::map().set ("id", id).set ("targetBusId", targetBusId).set ("levelDb", levelDb).set ("mode", pre ? "pre" : "post").set ("muted", muted);
}

ChannelStrip ChannelStrip::fromValue (const Value& v)
{
    ChannelStrip s;
    s.volumeDb = clampDouble (v["volumeDb"], 0.0, minusInfinityDb, 6.0);
    s.pan = clampDouble (v["pan"], 0.0, -1.0, 1.0);
    if (v["instrument"].isMap())
        s.instrument = PluginInstance::fromValue (v["instrument"]);
    for (const auto& item : v["inserts"].asList())
    {
        auto p = PluginInstance::fromValue (item);
        const bool duplicate = std::any_of (s.inserts.begin(), s.inserts.end(), [&] (const auto& q) { return q.id == p.id; });
        if (! duplicate && s.inserts.size() < static_cast<std::size_t> (maxInserts))
            s.inserts.push_back (std::move (p));
    }
    for (const auto& item : v["sends"].asList())
    {
        auto send = Send::fromValue (item);
        const bool duplicate = std::any_of (s.sends.begin(), s.sends.end(), [&] (const auto& q) { return q.id == send.id; });
        if (! duplicate && s.sends.size() < static_cast<std::size_t> (maxSends))
            s.sends.push_back (std::move (send));
    }
    return s;
}

Value ChannelStrip::toValue() const
{
    Value::List insertList, sendList;
    for (const auto& p : inserts)
        insertList.push_back (p.toValue());
    for (const auto& s : sends)
        sendList.push_back (s.toValue());
    return Value::map()
        .set ("volumeDb", volumeDb)
        .set ("pan", pan)
        .set ("instrument", instrument ? instrument->toValue() : Value())
        .set ("inserts", Value (std::move (insertList)))
        .set ("sends", Value (std::move (sendList)));
}

Track Track::fromValue (const Id& id, const Value& v)
{
    Track t;
    t.id = id;
    t.kind = trackKindFromString (v["kind"].asString()).value_or (TrackKind::audio);
    t.name = v["name"].asString();
    t.color = Color::fromValue (v["color"]);
    t.channels = t.kind == TrackKind::audio ? clampInt<int> (v["channels"], 1, 1, 2) : 2;
    t.input = InputAssignment::fromValue (v["input"]);
    t.output = t.kind == TrackKind::master ? OutputAssignment {} : OutputAssignment::fromValue (v["output"]);
    t.mute = v["mute"].asBool();
    t.monitor = oneOf (v["monitor"], { "auto", "on", "off" }, "auto");
    if (v["locked"].isMap())
        t.locked = LockInfo { v["locked"]["by"].asString(), v["locked"]["at"].asInt(), v["locked"]["note"].asString() };
    t.strip = ChannelStrip::fromValue (v["strip"]);
    if (t.kind == TrackKind::master)
        t.strip.sends.clear(); // master has no sends (spec 03 §3)
    if (t.kind != TrackKind::instrument)
        t.strip.instrument.reset();
    t.automationMode = oneOf (v["automationMode"], { "off", "read", "touch", "latch", "write" }, "read");
    return t;
}

Value Track::toValue() const
{
    Value lock;
    if (locked)
        lock = Value::map().set ("by", locked->by).set ("at", locked->at).set ("note", locked->note);
    return Value::map()
        .set ("id", id)
        .set ("kind", std::string (toString (kind)))
        .set ("name", name)
        .set ("color", color.toValue())
        .set ("channels", channels)
        .set ("input", input.toValue())
        .set ("output", output.toValue())
        .set ("mute", mute)
        .set ("monitor", monitor)
        .set ("locked", lock)
        .set ("strip", strip.toValue())
        .set ("automationMode", automationMode);
}

// Region --------------------------------------------------------------------

Region Region::fromValue (const Id& id, const Value& v)
{
    Region r;
    r.id = id;
    r.trackId = v["trackId"].asString();
    r.isMidi = v["kind"].asString() == "midi";
    r.start = std::max<std::int64_t> (0, v["start"].asInt());
    r.length = std::max<std::int64_t> (r.isMidi ? minMidiRegionLength : minAudioRegionLength, v["length"].asInt());
    r.name = v["name"].asString();
    if (! v["color"].isNull())
        r.color = Color::fromValue (v["color"]);
    r.muted = v["muted"].asBool();
    r.z = v["z"].asInt();
    if (v["loop"].isMap())
        r.loop = RegionLoop { v["loop"]["enabled"].asBool(), v["loop"]["endTicks"].asInt() };
    r.takeFolderId = v["takeFolderId"].asString();
    if (r.isMidi)
    {
        const auto& m = v["midi"];
        r.midi.sequenceId = m["sequenceId"].asString();
        r.midi.sequenceOffset = m["sequenceOffset"].asInt();
        if (m["quantize"].isMap())
        {
            const auto& q = m["quantize"];
            r.midi.quantize = RegionQuantize { std::max<std::int64_t> (1, q["grid"].asInt (240)),
                                               clampDouble (q["strength"], 1.0, 0.0, 1.0),
                                               clampDouble (q["swing"], 0.0, 0.0, 1.0) };
        }
        r.midi.transpose = clampInt<int> (m["transpose"], 0, -127, 127);
        r.midi.velocityOffset = clampInt<int> (m["velocityOffset"], 0, -127, 127);
    }
    else
    {
        const auto& a = v["audio"];
        r.audio.media = a["media"].asString();
        r.audio.sourceOffset = std::max<std::int64_t> (0, a["sourceOffset"].asInt());
        r.audio.gainDb = clampDouble (a["gainDb"], 0.0, minusInfinityDb, 24.0);
        r.audio.fadeIn = fadeFromValue (a["fadeIn"]);
        r.audio.fadeOut = fadeFromValue (a["fadeOut"]);
        // Fades never exceed the region (W-3h step 4); shrink proportionally.
        const auto total = r.audio.fadeIn.length + r.audio.fadeOut.length;
        if (total > r.length)
        {
            const double scale = static_cast<double> (r.length) / static_cast<double> (total);
            r.audio.fadeIn.length = static_cast<Samples> (std::floor (static_cast<double> (r.audio.fadeIn.length) * scale));
            r.audio.fadeOut.length = static_cast<Samples> (std::floor (static_cast<double> (r.audio.fadeOut.length) * scale));
        }
    }
    r.createdBy = v["createdBy"].asString();
    r.modifiedBy = v["modifiedBy"].asString();
    r.modifiedAt = v["modifiedAt"].asInt();
    return r;
}

Value Region::toValue() const
{
    Value v = Value::map()
                  .set ("id", id)
                  .set ("trackId", trackId)
                  .set ("kind", isMidi ? "midi" : "audio")
                  .set ("start", start)
                  .set ("length", length)
                  .set ("name", name)
                  .set ("color", color ? color->toValue() : Value())
                  .set ("muted", muted)
                  .set ("z", z)
                  .set ("loop", loop ? Value::map().set ("enabled", loop->enabled).set ("endTicks", loop->endTicks) : Value())
                  .set ("takeFolderId", takeFolderId.empty() ? Value() : Value (takeFolderId))
                  .set ("createdBy", createdBy)
                  .set ("modifiedBy", modifiedBy)
                  .set ("modifiedAt", modifiedAt);
    if (isMidi)
    {
        Value q;
        if (midi.quantize)
            q = Value::map().set ("grid", midi.quantize->grid).set ("strength", midi.quantize->strength).set ("swing", midi.quantize->swing);
        v.set ("midi", Value::map()
                           .set ("sequenceId", midi.sequenceId)
                           .set ("sequenceOffset", midi.sequenceOffset)
                           .set ("quantize", q)
                           .set ("transpose", midi.transpose)
                           .set ("velocityOffset", midi.velocityOffset));
    }
    else
    {
        v.set ("audio", Value::map()
                            .set ("media", audio.media)
                            .set ("sourceOffset", audio.sourceOffset)
                            .set ("gainDb", audio.gainDb)
                            .set ("fadeIn", fadeToValue (audio.fadeIn))
                            .set ("fadeOut", fadeToValue (audio.fadeOut)));
    }
    return v;
}

TakeFolder TakeFolder::fromValue (const Id& id, const Value& v)
{
    TakeFolder f;
    f.id = id;
    f.trackId = v["trackId"].asString();
    f.start = v["start"].asInt();
    f.endTicks = std::max (f.start, v["endTicks"].asInt());
    f.takeIds = idList (v["takeIds"]);
    f.activeTakeId = v["activeTakeId"].asString();
    return f;
}

Value TakeFolder::toValue() const
{
    return Value::map()
        .set ("id", id)
        .set ("trackId", trackId)
        .set ("start", start)
        .set ("endTicks", endTicks)
        .set ("takeIds", idListValue (takeIds))
        .set ("activeTakeId", activeTakeId);
}

// MIDI ----------------------------------------------------------------------

Note Note::fromValue (const Id& id, const Value& v)
{
    Note n;
    n.id = id;
    n.pos = std::max<std::int64_t> (0, v["pos"].asInt());
    n.length = std::max<std::int64_t> (1, v["length"].asInt (240));
    n.pitch = clampInt<int> (v["pitch"], 60, 0, 127);
    n.velocity = clampInt<int> (v["velocity"], 100, 1, 127);
    n.releaseVelocity = clampInt<int> (v["releaseVelocity"], 64, 0, 127);
    n.channel = clampInt<int> (v["channel"], 1, 1, 16);
    n.muted = v["muted"].asBool();
    return n;
}

Value Note::toValue() const
{
    return Value::map()
        .set ("pos", pos)
        .set ("length", length)
        .set ("pitch", pitch)
        .set ("velocity", velocity)
        .set ("releaseVelocity", releaseVelocity)
        .set ("channel", channel)
        .set ("muted", muted);
}

CcEvent CcEvent::fromValue (const Id& id, const Value& v)
{
    CcEvent e;
    e.id = id;
    e.pos = std::max<std::int64_t> (0, v["pos"].asInt());
    e.kind = oneOf (v["kind"], { "cc", "pitchBend", "channelPressure", "polyPressure", "programChange" }, "cc");
    e.number = clampInt<int> (v["number"], 0, 0, 127);
    e.value = e.kind == "pitchBend" ? clampInt<int> (v["value"], 0, -8192, 8191) : clampInt<int> (v["value"], 0, 0, 127);
    e.channel = clampInt<int> (v["channel"], 1, 1, 16);
    return e;
}

Value CcEvent::toValue() const
{
    return Value::map().set ("pos", pos).set ("kind", kind).set ("number", number).set ("value", value).set ("channel", channel);
}

std::vector<const Note*> MidiSequence::sortedNotes() const
{
    std::vector<const Note*> out;
    out.reserve (notes.size());
    for (const auto& entry : notes)
        out.push_back (&entry.second);
    std::sort (out.begin(), out.end(), [] (const Note* a, const Note* b) {
        return std::tie (a->pos, a->pitch, a->id) < std::tie (b->pos, b->pitch, b->id);
    });
    return out;
}

MidiSequence MidiSequence::fromValue (const Id& id, const Value& v)
{
    MidiSequence s;
    s.id = id;
    for (const auto& [noteId, nv] : v["notes"].asMap())
        s.notes.emplace (noteId, Note::fromValue (noteId, nv));
    for (const auto& [ccId, cv] : v["controllers"].asMap())
        s.controllers.emplace (ccId, CcEvent::fromValue (ccId, cv));
    return s;
}

Value MidiSequence::toValue() const
{
    Value notesValue = Value::map(), ccValue = Value::map();
    for (const auto& [noteId, n] : notes)
        notesValue.set (noteId, n.toValue());
    for (const auto& [ccId, c] : controllers)
        ccValue.set (ccId, c.toValue());
    return Value::map().set ("id", id).set ("notes", std::move (notesValue)).set ("controllers", std::move (ccValue));
}

// Media, markers, automation --------------------------------------------------

MediaRef MediaRef::fromValue (const std::string& hash, const Value& v)
{
    MediaRef m;
    m.hash = hash;
    m.originalName = v["originalName"].asString();
    m.format = oneOf (v["format"], { "wav", "aiff", "flac", "mp3" }, "wav");
    m.sampleRate = static_cast<int> (v["sampleRate"].asInt (48000));
    m.channels = clampInt<int> (v["channels"], 1, 1, 64);
    m.lengthSamples = std::max<std::int64_t> (0, v["lengthSamples"].asInt());
    m.bitDepth = static_cast<int> (v["bitDepth"].asInt (24));
    m.sizeBytes = v["sizeBytes"].asInt();
    m.addedBy = v["addedBy"].asString();
    m.addedAt = v["addedAt"].asInt();
    return m;
}

Value MediaRef::toValue() const
{
    return Value::map()
        .set ("hash", hash)
        .set ("originalName", originalName)
        .set ("format", format)
        .set ("sampleRate", sampleRate)
        .set ("channels", channels)
        .set ("lengthSamples", lengthSamples)
        .set ("bitDepth", bitDepth)
        .set ("sizeBytes", sizeBytes)
        .set ("addedBy", addedBy)
        .set ("addedAt", addedAt);
}

Marker Marker::fromValue (const Id& id, const Value& v)
{
    Marker m;
    m.id = id;
    m.pos = std::max<std::int64_t> (0, v["pos"].asInt());
    m.name = v["name"].asString();
    if (! v["color"].isNull())
        m.color = Color::fromValue (v["color"]);
    m.createdBy = v["createdBy"].asString();
    return m;
}

Value Marker::toValue() const
{
    return Value::map().set ("id", id).set ("pos", pos).set ("name", name).set ("color", color ? color->toValue() : Value()).set ("createdBy", createdBy);
}

std::string AutomationTarget::key() const
{
    if (kind == "sendLevel")
        return kind + ":" + sendId;
    if (kind == "pluginParam")
        return kind + ":" + pluginId + ":" + paramId;
    return kind;
}

std::optional<AutomationTarget> AutomationTarget::fromKey (const std::string& key)
{
    AutomationTarget t;
    if (key == "volume" || key == "pan" || key == "mute")
    {
        t.kind = key;
        return t;
    }
    const auto colon = key.find (':');
    if (colon == std::string::npos)
        return std::nullopt;
    t.kind = key.substr (0, colon);
    const auto rest = key.substr (colon + 1);
    if (t.kind == "sendLevel" && ! rest.empty())
    {
        t.sendId = rest;
        return t;
    }
    const auto second = rest.find (':');
    if (t.kind == "pluginParam" && second != std::string::npos && second > 0 && second + 1 < rest.size())
    {
        t.pluginId = rest.substr (0, second);
        t.paramId = rest.substr (second + 1);
        return t;
    }
    return std::nullopt;
}

AutomationTarget AutomationTarget::fromValue (const Value& v)
{
    AutomationTarget t;
    t.kind = oneOf (v["kind"], { "volume", "pan", "mute", "sendLevel", "pluginParam" }, "volume");
    t.sendId = v["sendId"].asString();
    t.pluginId = v["pluginId"].asString();
    t.paramId = v["paramId"].asString();
    return t;
}

Value AutomationTarget::toValue() const
{
    Value v = Value::map().set ("kind", kind);
    if (kind == "sendLevel")
        v.set ("sendId", sendId);
    if (kind == "pluginParam")
        v.set ("pluginId", pluginId).set ("paramId", paramId);
    return v;
}

AutomationLane AutomationLane::fromValue (const Id& id, const Value& v)
{
    AutomationLane lane;
    lane.id = id;
    lane.trackId = v["trackId"].asString();
    lane.target = AutomationTarget::fromValue (v["target"]);
    for (const auto& [pointId, pv] : v["points"].asMap())
        lane.points.emplace (pointId, AutoPoint { pointId, std::max<std::int64_t> (0, pv["pos"].asInt()),
                                                  clampDouble (pv["value"], 0.0, 0.0, 1.0), clampDouble (pv["curve"], 0.0, -1.0, 1.0) });
    return lane;
}

Value AutomationLane::toValue() const
{
    Value pointsValue = Value::map();
    for (const auto& [pointId, p] : points)
        pointsValue.set (pointId, Value::map().set ("pos", p.pos).set ("value", p.value).set ("curve", p.curve));
    return Value::map().set ("id", id).set ("trackId", trackId).set ("target", target.toValue()).set ("points", std::move (pointsValue));
}

TimeRange TimeRange::fromValue (const Value& v, TimeRange fallback)
{
    if (! v.isMap())
        return fallback;
    TimeRange r;
    r.enabled = v["enabled"].asBool();
    r.start = std::max<std::int64_t> (0, v["start"].asInt (fallback.start));
    r.end = v["end"].asInt (fallback.end);
    if (r.end <= r.start)
        r.end = r.start + 1; // invariant start < end (spec 03 §2)
    return r;
}

Value TimeRange::toValue() const { return Value::map().set ("enabled", enabled).set ("start", start).set ("end", end); }

MetronomeSettings MetronomeSettings::fromValue (const Value& v)
{
    MetronomeSettings m;
    if (! v.isMap())
        return m;
    m.enabled = v["enabled"].asBool (true);
    m.levelDb = clampDouble (v["levelDb"], 0.0, minusInfinityDb, 6.0);
    m.during = oneOf (v["during"], { "record", "play", "both" }, "record");
    m.countInBars = clampInt<int> (v["countInBars"], 1, 0, 2);
    return m;
}

Value MetronomeSettings::toValue() const
{
    return Value::map().set ("enabled", enabled).set ("levelDb", levelDb).set ("during", during).set ("countInBars", countInBars);
}

bool isValidSampleRate (int rate) noexcept { return rate == 44100 || rate == 48000 || rate == 88200 || rate == 96000; }

bool isValidBitDepth (int bits) noexcept { return bits == 16 || bits == 24 || bits == 32; }
} // namespace weft::model
