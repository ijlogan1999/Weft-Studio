#include "Studio.h"

#include "Theme.h"

#include "weft/engine/dsp/BuiltinEffects.h"
#include "weft/media/Blobs.h"
#include "weft/media/Decoder.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>

namespace weft::ui
{
namespace fs = std::filesystem;

namespace
{
/** This computer's id (D-COL-45): read from `file`, or made up once and written there. */
std::string loadDeviceId (const fs::path& file)
{
    if (const auto bytes = project::readFile (file))
    {
        std::string id (bytes->begin(), bytes->end());
        id.erase (std::remove_if (id.begin(), id.end(), [] (char c) { return ! std::isxdigit (static_cast<unsigned char> (c)); }), id.end());
        if (id.size() == 16)
            return id;
    }
    juce::Random random;
    const auto id = juce::String::toHexString (random.nextInt64()).paddedLeft ('0', 16).substring (0, 16).toStdString();
    std::error_code ec;
    fs::create_directories (file.parent_path(), ec);
    project::writeFileAtomic (file, { reinterpret_cast<const std::uint8_t*> (id.data()), id.size() });
    return id;
}
} // namespace

juce::String toolName (Tool t)
{
    switch (t)
    {
        case Tool::pointer: return "Pointer";
        case Tool::pencil: return "Pencil";
        case Tool::scissors: return "Scissors";
        case Tool::glue: return "Glue";
        case Tool::eraser: return "Eraser";
        case Tool::mute: return "Mute";
        case Tool::text: return "Text";
        case Tool::zoom: return "Zoom";
        case Tool::fade: return "Fade";
        case Tool::gain: return "Gain";
        case Tool::quantize: return "Quantize";
    }
    return {};
}

std::optional<Tool> toolForKey (juce::juce_wchar key)
{
    switch (juce::CharacterFunctions::toLowerCase (key))
    {
        case 't': return Tool::pointer;
        case 'p': return Tool::pencil;
        case 'i': return Tool::scissors;
        case 'j': return Tool::glue;
        case 'e': return Tool::eraser;
        case 'm': return Tool::mute;
        case 'n': return Tool::text;
        case 'z': return Tool::zoom;
        case 'a': return Tool::fade;
        case 'g': return Tool::gain;
        case 'q': return Tool::quantize;
        default: return std::nullopt;
    }
}

juce::String toolKey (Tool t)
{
    for (const auto c : juce::String ("tpijemnzagq"))
        if (toolForKey (c) == t)
            return juce::String::charToString (juce::CharacterFunctions::toUpperCase (c));
    return {};
}

// Layout ---------------------------------------------------------------------------

juce::String PanelLayout::toString() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("inspector", inspector);
    o->setProperty ("sidePanel", sidePanel);
    o->setProperty ("globalTracks", globalTracks);
    o->setProperty ("bottom", static_cast<int> (bottom));
    o->setProperty ("inspectorWidth", inspectorWidth);
    o->setProperty ("sidePanelWidth", sidePanelWidth);
    o->setProperty ("bottomHeight", bottomHeight);
    o->setProperty ("darkTheme", darkTheme);
    return juce::JSON::toString (juce::var (o), true);
}

PanelLayout PanelLayout::fromString (const juce::String& text)
{
    PanelLayout l;
    const auto v = juce::JSON::parse (text);
    if (! v.isObject())
        return l;
    auto get = [&] (const char* key, auto fallback) {
        const auto p = v.getProperty (key, juce::var());
        return p.isVoid() ? fallback : static_cast<decltype (fallback)> (p);
    };
    l.inspector = get ("inspector", l.inspector);
    l.sidePanel = get ("sidePanel", l.sidePanel);
    l.globalTracks = get ("globalTracks", l.globalTracks);
    l.bottom = static_cast<BottomTab> (std::clamp (get ("bottom", 0), 0, 3));
    l.inspectorWidth = std::clamp (get ("inspectorWidth", l.inspectorWidth), 160, 600);
    l.sidePanelWidth = std::clamp (get ("sidePanelWidth", l.sidePanelWidth), 200, 700);
    l.bottomHeight = std::clamp (get ("bottomHeight", l.bottomHeight), 160, 2000);
    l.darkTheme = get ("darkTheme", l.darkTheme);
    return l;
}

// Studio -------------------------------------------------------------------------------

Studio::Studio (std::unique_ptr<engine::AudioDevice> device, const fs::path& projectParent_, engine::DeviceConfig config)
    : audioDevice (std::move (device)), deviceConfig (config)
{
    eng = std::make_unique<engine::Engine> (pool, rack);
    live = std::make_unique<engine::LiveAutomation> (*eng);
    midiRouter = std::make_unique<engine::MidiInputRouter> (*eng);
    catalog = std::make_unique<engine::PluginCatalog> (settingsFolder() / "plugins.json");
    catalog->load();
    host = std::make_unique<engine::Vst3Host> (*catalog);
    pluginFolders = engine::PluginCatalog::defaultFolders();
    eng->setPluginFactory ([this] (const model::PluginInstance& p, bool instrument) { return host->create (p, instrument); });
    scannerPath = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                      .getSiblingFile (juce::String ("weft-plugin-scanner") + (juce::SystemStats::getOperatingSystemType() & juce::SystemStats::Windows ? ".exe" : ""))
                      .getFullPathName()
                      .toStdString();
    userDisplayName = juce::SystemStats::getFullUserName().toStdString();
    projectParent = projectParent_;
    recent = project::RecentProjects (settingsFolder() / "recent-projects.txt");
    recent.load();
    loadAccount();
    deviceIdText = loadDeviceId (settingsFolder() / "device-id.txt");
    std::string error;
    const auto made = newProject (projectParent, {}, &error, true);
    jassert (made);
    juce::ignoreUnused (made);
    audioOk = audioDevice != nullptr && audioDevice->open (config) && audioDevice->start (*eng);
    if (! audioOk)
    {
        status = "No audio device: Weft is running without sound.";
        startSilentClock();
    }
    syncEngine();
}

Studio::~Studio()
{
    if (silentClock != nullptr)
        silentClock->stop();
    if (audioDevice != nullptr)
    {
        audioDevice->stop();
        audioDevice->close();
    }
    releaseProject();
}

void Studio::syncEngine()
{
    local.prune (model());
    selection.prune (model());
    eng->setLocalView (local);
    eng->setProject (model());
}

void Studio::edited()
{
    // Notes deleted here or by an undo leave the piano roll selection.
    for (auto it = piano.selection.begin(); it != piano.selection.end();)
        it = commands::findNote (model(), *it) == nullptr ? piano.selection.erase (it) : std::next (it);
    syncEngine();
    ++editCount;
    mediaDirty = true;
    sendChangeMessage();
}

void Studio::setStatus (const juce::String& message)
{
    status = message;
    sendChangeMessage();
}

bool Studio::check (const commands::EditResult& result)
{
    if (! result)
        setStatus (juce::String (result.error));
    edited();
    return result.ok;
}

engine::Engine::Meter Studio::takeMeter (const Id& trackId, const void* reader)
{
    const auto m = eng->takeMeter (trackId);
    auto& readers = unreadMeters[trackId];
    readers.try_emplace (reader);
    for (auto& [who, kept] : readers)
    {
        kept.output = std::max (kept.output, m.output);
        kept.input = std::max (kept.input, m.input);
    }
    auto out = readers[reader];
    out.rms = m.rms;
    out.clip = m.clip;
    readers[reader] = {};
    return out;
}

void Studio::forgetMeterReader (const void* reader)
{
    for (auto& [id, readers] : unreadMeters)
        readers.erase (reader);
}

void Studio::selectTrack (const Id& trackId)
{
    local.selectedTrack = trackId;
    eng->setSelectedTrack (trackId);
    sendChangeMessage();
}

bool Studio::moveTrackTo (const Id& trackId, std::size_t index)
{
    // `index` is the position among the tracks as shown; the stored order can also hold ids the
    // projection hides (D-DM-18), so place the track before the shown track it lands on.
    const auto& shown = model().trackOrder;
    std::vector<Id> others;
    for (const auto& id : shown)
        if (id != trackId)
            others.push_back (id);
    if (others.size() == shown.size())
        return false;
    auto stored = model().storedTrackOrder;
    stored.erase (std::remove (stored.begin(), stored.end(), trackId), stored.end());
    auto at = stored.size();
    if (index < others.size())
        at = static_cast<std::size_t> (std::find (stored.begin(), stored.end(), others[index]) - stored.begin());
    if (std::find (shown.begin(), shown.end(), trackId) - shown.begin() == static_cast<std::ptrdiff_t> (std::min (index, others.size())))
        return false; // already there
    const bool ok = check (sess->moveTrack (trackId, at));
    edited();
    return ok;
}

void Studio::toggleArm (const Id& trackId)
{
    local.toggleArm (model(), trackId);
    edited();
}

void Studio::toggleMute (const Id& trackId)
{
    const auto* t = model().track (trackId);
    if (t == nullptr)
        return;
    model::AutomationTarget mutes;
    mutes.kind = "mute";
    if (automationLive (trackId, mutes))
    {
        // A click is a touch and a release: Latch and Write hold it until stop.
        const auto now = automationValue (trackId, mutes).value_or (t->mute ? 1.0 : 0.0);
        touchControl (trackId, mutes, automation::toMute (now) ? 0.0 : 1.0);
        releaseControl (trackId, mutes);
        return;
    }
    check (session().setTrackMute (trackId, ! t->mute));
    edited();
}

void Studio::toggleSolo (const Id& trackId, bool exclusive)
{
    if (exclusive)
        local.soloOnly (trackId);
    else
        local.toggleSolo (trackId);
    edited();
}

bool Studio::trackSwitch (TrackSwitch which, const Id& trackId) const
{
    switch (which)
    {
        case TrackSwitch::mute:
        {
            const auto* t = model().track (trackId);
            return t != nullptr && t->mute;
        }
        case TrackSwitch::solo: return local.soloed.contains (trackId);
        case TrackSwitch::arm: return local.armed.contains (trackId);
        case TrackSwitch::on:
        {
            const auto* t = model().track (trackId);
            return t != nullptr && ! t->off;
        }
        case TrackSwitch::monitor:
        {
            const auto* t = model().track (trackId);
            return t != nullptr && t->monitor == "on";
        }
    }
    return false;
}

void Studio::setTrackSwitch (TrackSwitch which, const Id& trackId, bool on)
{
    const auto* t = model().track (trackId);
    if (t == nullptr || t->kind == model::TrackKind::master || trackSwitch (which, trackId) == on)
        return;
    switch (which)
    {
        case TrackSwitch::mute: toggleMute (trackId); return;
        case TrackSwitch::solo: local.setSolo (trackId, on); break;
        case TrackSwitch::arm:
            if (! local.setArm (model(), trackId, on))
                return;
            break;
        case TrackSwitch::on:
            if (t->kind != model::TrackKind::audio && t->kind != model::TrackKind::instrument)
                return;
            check (sess->setTrackOff (trackId, ! on));
            return;
        case TrackSwitch::monitor:
            if (t->kind != model::TrackKind::audio)
                return;
            check (sess->setTrackMonitor (trackId, on ? "on" : "auto"));
            return;
    }
    edited();
}

void Studio::clickTrackSwitch (TrackSwitch which, const Id& trackId, juce::ModifierKeys mods)
{
    if (! mods.isCommandDown() && ! mods.isAltDown())
    {
        switch (which)
        {
            case TrackSwitch::mute: toggleMute (trackId); break;
            case TrackSwitch::solo: toggleSolo (trackId); break;
            case TrackSwitch::arm: toggleArm (trackId); break;
            case TrackSwitch::on:
            case TrackSwitch::monitor: setTrackSwitch (which, trackId, ! trackSwitch (which, trackId)); break;
        }
        return;
    }
    // Ctrl: this one on, the others off. Alt: every track takes the state this one is about to take.
    const bool only = mods.isCommandDown();
    const bool target = only || ! trackSwitch (which, trackId);
    std::vector<Id> ids;
    for (const auto& id : model().trackOrder)
        if (const auto* t = model().track (id); t != nullptr && t->kind != model::TrackKind::master)
            ids.push_back (id); // by value: a mute edit rebuilds the model
    // The switches saved in the project make one undo step; solo and record are local.
    const bool own = documentSwitch (which) && ! sess->inGesture();
    if (own)
        sess->beginGesture (which == TrackSwitch::on    ? (only ? "Turn On Only" : target ? "Turn All Tracks On" : "Turn All Tracks Off")
                            : which == TrackSwitch::monitor ? "Input Monitoring"
                            : only                          ? "Mute Only"
                            : target                        ? "Mute All"
                                                            : "Unmute All");
    for (const auto& id : ids)
        setTrackSwitch (which, id, only ? id == trackId : target);
    if (own)
        sess->endGesture();
    edited();
}

void Studio::beginSwitchSwipe (TrackSwitch which)
{
    swipeGesture = documentSwitch (which) && ! sess->inGesture();
    if (swipeGesture)
        sess->beginGesture (which == TrackSwitch::on ? "Turn Tracks On/Off" : which == TrackSwitch::monitor ? "Input Monitoring" : "Mute Tracks");
}

void Studio::endSwitchSwipe()
{
    if (std::exchange (swipeGesture, false))
        sess->endGesture();
    edited();
}

bool Studio::documentSwitch (TrackSwitch which) noexcept
{
    return which == TrackSwitch::mute || which == TrackSwitch::on || which == TrackSwitch::monitor;
}

bool Studio::setAllTracksOff (bool off)
{
    std::vector<Id> ids; // by value: each edit rebuilds the model
    for (const auto& id : model().trackOrder)
        if (const auto* t = model().track (id);
            t != nullptr && (t->kind == model::TrackKind::audio || t->kind == model::TrackKind::instrument) && t->off != off && ! lockedByOther (id))
            ids.push_back (id);
    if (ids.empty())
        return false;
    sess->beginGesture (off ? "Turn All Tracks Off" : "Turn All Tracks On");
    for (const auto& id : ids)
        sess->setTrackOff (id, off);
    sess->endGesture();
    edited();
    return true;
}

std::vector<Id> Studio::mixerOrder() const
{
    const auto& m = model();
    const auto kindShown = [&] (const model::Track& t) {
        switch (t.kind)
        {
            case model::TrackKind::audio: return mixerAudio;
            case model::TrackKind::instrument: return mixerInstrument;
            case model::TrackKind::bus: return mixerBus;
            case model::TrackKind::master: return false;
        }
        return false;
    };
    std::set<Id> wanted;
    if (mixerMode == MixerMode::single)
    {
        // The selected channel, then every bus its signal reaches through outputs and sends.
        std::vector<Id> todo;
        if (const auto* t = m.track (local.selectedTrack); t != nullptr && t->kind != model::TrackKind::master)
            todo.push_back (t->id);
        while (! todo.empty())
        {
            const auto id = todo.back();
            todo.pop_back();
            if (! wanted.insert (id).second)
                continue;
            if (const auto* t = m.track (id))
            {
                if (! t->output.toMaster())
                    todo.push_back (t->output.busId);
                for (const auto& send : t->strip.sends)
                    todo.push_back (send.targetBusId);
            }
        }
    }
    std::vector<Id> order;
    for (const bool buses : { false, true })
        for (const auto& tid : m.trackOrder)
        {
            const auto* t = m.track (tid);
            if (t == nullptr || t->kind == model::TrackKind::master || (t->kind == model::TrackKind::bus) != buses)
                continue;
            if (mixerMode == MixerMode::single ? wanted.contains (tid)
                                               : kindShown (*t) && (mixerMode == MixerMode::all || ! t->hidden || m.info.showHiddenTracks))
                order.push_back (tid);
        }
    return order;
}

bool Studio::copyChannelStrip (const Id& trackId)
{
    const auto* t = model().track (trackId);
    if (t == nullptr)
    {
        setStatus ("Select a track first.");
        return false;
    }
    for (const auto& p : t->strip.inserts)
        writePluginState (p.id); // the editors' last changes go with the copy
    t = model().track (trackId);
    stripClipboard = t->strip;
    setStatus ("Copied the channel strip of " + juce::String (t->name) + ".");
    return true;
}

bool Studio::pasteChannelStrip (const Id& trackId)
{
    const auto* t = model().track (trackId);
    if (t == nullptr || ! stripClipboard)
    {
        setStatus (t == nullptr ? "Select a track first." : "Copy a channel strip setting first.");
        return false;
    }
    // D-W6-13: volume, pan and the insert effects (fresh copies, settings and bypass kept); sends and routing stay.
    const auto from = *stripClipboard;
    std::vector<Id> old;
    for (const auto& p : t->strip.inserts)
        old.push_back (p.id);
    endPluginGesture();
    sess->beginGesture ("Paste Channel Strip Setting");
    sess->setTrackVolume (trackId, from.volumeDb);
    sess->setTrackPan (trackId, from.pan);
    for (const auto& id : old)
        sess->removeInsert (trackId, id);
    for (auto p : from.inserts)
    {
        p.id.clear(); // a fresh plug-in: the same one can be pasted onto several tracks
        sess->addInsert (trackId, p);
    }
    sess->endGesture();
    edited();
    return true;
}

bool Studio::toggleAllTracksOff()
{
    bool anyOff = false;
    for (const auto& id : model().trackOrder)
        if (const auto* t = model().track (id); t != nullptr && t->off)
            anyOff = true;
    if (! setAllTracksOff (! anyOff))
    {
        setStatus ("No audio or instrument tracks to turn on or off.");
        return false;
    }
    return true;
}

juce::PopupMenu Studio::headerOptionsMenu (int base) const
{
    juce::PopupMenu m;
    const auto& h = header;
    m.addSectionHeader ("Buttons");
    m.addItem (base + headerOnOff, "On/Off", true, h.onOff);
    m.addItem (base + headerMonitor, "Input Monitoring", true, h.monitor);
    m.addItem (base + headerHide, "Hide", true, h.hide);
    m.addSectionHeader ("Show");
    m.addItem (base + headerVolume, "Volume", true, h.volume);
    m.addItem (base + headerPan, "Pan", true, h.pan);
    m.addItem (base + headerNumbers, "Track Numbers", true, h.numbers);
    m.addItem (base + headerColorBars, "Color Bars", true, h.colorBars);
    m.addItem (base + headerIcons, "Track Icons", true, h.icons);
    return m;
}

void Studio::toggleHeaderOption (int option)
{
    auto& h = header;
    switch (option)
    {
        case headerMonitor: h.monitor = ! h.monitor; break;
        case headerOnOff: h.onOff = ! h.onOff; break;
        case headerHide: h.hide = ! h.hide; break;
        case headerVolume: h.volume = ! h.volume; break;
        case headerPan: h.pan = ! h.pan; break;
        case headerNumbers: h.numbers = ! h.numbers; break;
        case headerColorBars: h.colorBars = ! h.colorBars; break;
        case headerIcons: h.icons = ! h.icons; break;
        default: return;
    }
    sendChangeMessage();
}

void Studio::setHeaderWidth (int width)
{
    width = std::clamp (width, minHeaderWidth, maxHeaderWidth);
    if (width == header.width)
        return;
    header.width = width;
    sendChangeMessage();
}

bool Studio::silencedBySolo (const Id& trackId) const
{
    return ! local.soloed.empty() && ! commands::audibleWithSolo (model(), local.soloed).contains (trackId);
}

const model::Marker* Studio::markerAtPlayhead() const
{
    const model::Marker* at = nullptr;
    const auto now = playhead();
    for (const auto* marker : arrange::markersInOrder (model()))
        if (marker->pos <= now)
            at = marker;
    return at;
}

int Studio::trackHeight (const Id& trackId) const
{
    const auto it = heights.find (trackId);
    return it == heights.end() ? defaultTrackHeight : it->second;
}

void Studio::setTrackHeight (const Id& trackId, int height)
{
    heights[trackId] = std::clamp (height, minTrackHeight, maxTrackHeight);
    sendChangeMessage();
}

int Studio::rowHeight (const Id& trackId) const
{
    return trackHeight (trackId) + (automationView ? static_cast<int> (shownLanes (trackId).size()) * automationLaneHeight : 0);
}

// Automation ---------------------------------------------------------------------------

namespace
{
model::AutomationTarget volumeTarget()
{
    model::AutomationTarget t;
    t.kind = "volume";
    return t;
}
} // namespace

std::vector<model::AutomationTarget> Studio::shownLanes (const Id& trackId) const
{
    std::vector<model::AutomationTarget> out;
    if (const auto it = lanesShown.find (trackId); it != lanesShown.end())
        for (const auto& t : it->second)
            if (automation::targetExists (model(), trackId, t))
                out.push_back (t);
    if (out.empty())
        out.push_back (volumeTarget());
    return out;
}

void Studio::setShownLane (const Id& trackId, std::size_t index, const model::AutomationTarget& target)
{
    auto lanes = shownLanes (trackId);
    if (index < lanes.size())
        lanes[index] = target;
    lanesShown[trackId] = lanes;
    pointSelection = {};
    sendChangeMessage();
}

void Studio::removeShownLane (const Id& trackId, std::size_t index)
{
    auto lanes = shownLanes (trackId);
    if (index >= lanes.size())
        return;
    lanes.erase (lanes.begin() + static_cast<std::ptrdiff_t> (index));
    lanesShown[trackId] = lanes; // the last one closed leaves Volume showing
    pointSelection = {};
    sendChangeMessage();
}

bool Studio::cycleAutomationParameter (const Id& trackId)
{
    // D-W8-7: the first lane goes to the next parameter that has automation, in the parameter menu's order.
    if (model().track (trackId) == nullptr)
    {
        setStatus ("Select a track first.");
        return false;
    }
    std::vector<model::AutomationTarget> used;
    for (const auto& choice : automationTargets (trackId))
        if (const auto* lane = model().lane (trackId, choice.target); lane != nullptr && ! lane->points.empty())
            used.push_back (choice.target);
    if (used.empty())
    {
        setStatus ("This track has no automation yet.");
        return false;
    }
    const auto current = shownLanes (trackId).front().key();
    std::size_t next = 0;
    for (std::size_t i = 0; i < used.size(); ++i)
        if (used[i].key() == current)
            next = (i + 1) % used.size();
    automationView = true;
    setShownLane (trackId, 0, used[next]);
    return true;
}

bool Studio::setAllAutomationModes (const std::string& mode)
{
    std::vector<Id> ids; // by value: each edit rebuilds the model
    for (const auto& id : model().trackOrder)
        if (const auto* t = model().track (id); t != nullptr && t->automationMode != mode && ! lockedByOther (id))
            ids.push_back (id);
    if (ids.empty())
        return false;
    sess->beginGesture ("Automation Mode");
    for (const auto& id : ids)
        sess->setTrackAutomationMode (id, mode);
    sess->endGesture();
    edited();
    return true;
}

bool Studio::nudgeAutomationPoints (double by)
{
    const auto* lane = model().automationLanes.contains (pointSelection.laneId) ? &*model().automationLanes.at (pointSelection.laneId) : nullptr;
    if (lane == nullptr)
        return false;
    std::vector<commands::Session::PointMove> moves;
    for (const auto& id : pointSelection.points)
        if (const auto it = lane->points.find (id); it != lane->points.end())
            moves.push_back ({ id, it->second.pos, std::clamp (it->second.value + by, 0.0, 1.0) });
    return ! moves.empty() && check (sess->moveAutomationPoints (pointSelection.laneId, moves));
}

void Studio::addShownLane (const Id& trackId)
{
    auto lanes = shownLanes (trackId);
    for (const auto& choice : automationTargets (trackId))
        if (std::none_of (lanes.begin(), lanes.end(), [&] (const auto& t) { return t.key() == choice.target.key(); }))
        {
            lanes.push_back (choice.target);
            break;
        }
    lanesShown[trackId] = lanes;
    sendChangeMessage();
}

std::vector<automation::TargetChoice> Studio::automationTargets (const Id& trackId) const
{
    return automation::targetsFor (model(), trackId, [this] (const model::PluginInstance& p) {
        std::vector<std::pair<std::string, std::string>> out;
        if (const auto proc = eng->processor (p.id))
            for (const auto& info : proc->parameters())
                out.emplace_back (info.id, info.name);
        return out;
    });
}

juce::String Studio::automationTargetName (const Id& trackId, const model::AutomationTarget& target) const
{
    for (const auto& c : automationTargets (trackId))
        if (c.target.key() == target.key())
            return juce::String (c.name);
    return juce::String (automation::targetName (model(), trackId, target, {}));
}

bool Studio::automationLive (const Id& trackId, const model::AutomationTarget& target) const { return live->isLive (model(), trackId, target); }

void Studio::touchControl (const Id& trackId, const model::AutomationTarget& target, double normalized)
{
    if (automationLive (trackId, target))
    {
        live->touch (model(), trackId, target, normalized);
        return;
    }
    // Static value (mode Off, or nothing to follow while stopped).
    auto& s = *sess;
    if (target.kind == "volume")
        check (s.setTrackVolume (trackId, automation::toDb (normalized)));
    else if (target.kind == "pan")
        check (s.setTrackPan (trackId, automation::toPan (normalized)));
    else if (target.kind == "mute")
        check (s.setTrackMute (trackId, automation::toMute (normalized)));
    else if (target.kind == "sendLevel")
        check (s.setSendLevel (trackId, target.sendId, automation::toDb (normalized)));
    else if (target.kind == "pluginParam")
        pluginParamsChanged (target.pluginId, { { target.paramId, normalized } });
    edited();
}

void Studio::releaseControl (const Id& trackId, const model::AutomationTarget& target)
{
    live->release (model(), trackId, target);
    sendChangeMessage();
}

std::optional<double> Studio::automationValue (const Id& trackId, const model::AutomationTarget& target) const
{
    return eng->automationValue (trackId, target);
}

Studio::ViewSnapshot Studio::viewSnapshot() const
{
    ViewSnapshot v;
    v.originTick = view.originTick;
    v.pixelsPerTick = view.pixelsPerTick;
    v.heights = heights;
    v.scrollY = verticalScroll ? verticalScroll() : 0;
    return v;
}

void Studio::restoreView (const ViewSnapshot& v)
{
    view.originTick = v.originTick;
    view.pixelsPerTick = std::clamp (v.pixelsPerTick, arrange::TimelineView::minPixelsPerTick, arrange::TimelineView::maxPixelsPerTick);
    heights = v.heights;
    zoomedFrom.clear();
    if (setVerticalScroll)
        setVerticalScroll (v.scrollY);
    sendChangeMessage();
}

void Studio::rememberView()
{
    backViews.push_back (viewSnapshot());
    if (backViews.size() > 50)
        backViews.erase (backViews.begin());
    forwardViews.clear();
}

void Studio::scaleTrackHeights (double factor)
{
    for (const auto& id : model().trackOrder)
    {
        const auto h = trackHeight (id);
        auto next = juce::roundToInt (h * factor);
        if (next == h)
            next += factor > 1.0 ? 1 : -1; // always a visible step
        heights[id] = std::clamp (next, minTrackHeight, maxTrackHeight);
    }
    sendChangeMessage();
}

void Studio::setAllTrackHeights (int height)
{
    for (const auto& id : model().trackOrder)
        heights[id] = std::clamp (height, minTrackHeight, maxTrackHeight);
    sendChangeMessage();
}

// Transport ---------------------------------------------------------------------------

Ticks Studio::playhead() const
{
    if (locatedAt && eng->deviceFrames() == locatedFrame)
        return *locatedAt;
    return static_cast<Ticks> (std::llround (eng->positionTicks()));
}

void Studio::locate (Ticks t)
{
    t = std::max<Ticks> (t, 0);
    eng->locate (t);
    locatedAt = t;
    lastLocate = t;
    locatedFrame = eng->deviceFrames();
    sendChangeMessage();
}

bool Studio::playing() const { return eng->state() != engine::TransportState::stopped; }
bool Studio::recording() const { return eng->state() == engine::TransportState::recording || eng->state() == engine::TransportState::countIn; }

void Studio::tick()
{
    checkDevice();
    if (silentClock != nullptr)
    {
        // No-audio mode: the engine runs on the message thread, in step with the system clock.
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto frames = static_cast<std::int64_t> ((now - silentClockMs) * silentClock->config().sampleRate / 1000.0);
        if (frames > 0)
        {
            silentClock->render (std::min<std::int64_t> (frames, static_cast<std::int64_t> (silentClock->config().sampleRate)));
            silentClockMs += 1000.0 * static_cast<double> (frames) / silentClock->config().sampleRate;
        }
    }
    if (syncClient != nullptr)
    {
        updatePresence();
        syncClient->poll();
    }
    // 30 minutes without input ends a session: an automatic version if this user changed things (05 §9.3).
    if (lastInputMs > 0 && juce::Time::getMillisecondCounterHiRes() - lastInputMs >= idleSessionMs && ! previewing())
    {
        lastInputMs = 0;
        if (sess->autoVersion())
            edited();
    }
    if (mediaSyncer != nullptr)
    {
        mediaSyncer->setRecording (recording());
        std::vector<Ticks> heads { playhead() };
        for (const auto* who : collaborators())
            if (who->hasState)
                heads.push_back (collaboratorPlayhead (*who));
        mediaSyncer->setPlayheads (std::move (heads));
        // Edits can bring in audio to send or fetch: look again at most once a second.
        const auto now = juce::Time::getMillisecondCounterHiRes();
        if (mediaDirty && now - lastMediaRefresh >= 1000.0)
        {
            mediaDirty = false;
            lastMediaRefresh = now;
            mediaSyncer->refresh();
        }
        mediaSyncer->poll();
    }
    if (inSharedFolder())
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        pollSharedFolder (now);
        // A shared folder saves itself, so collaborators get this computer's changes.
        if (unsaved && ! previewing() && ! sess->inGesture() && now - lastSharedSave >= sharedFolderSaveMs)
            writeProject (false);
    }
    stepBounce();
    flushPluginStates();
    if (sess->inGesture() && lastParamChange > 0 && juce::Time::getMillisecondCounterHiRes() - lastParamChange > 500)
        endPluginGesture();
    eng->collectGarbage();
    // A live automation pass that just stopped becomes one undo step (W-8).
    if (auto pass = live->tick (model()))
    {
        if (check (sess->writeAutomation (pass->writes, pass->toLatch)))
            status = pass->writes.empty() ? "Automation mode is now Latch." : "Wrote automation.";
        edited();
    }
    if (eng->recordingPending())
        if (const auto outcome = eng->finishRecording (*sess))
        {
            if (! outcome->message.empty())
                status = outcome->message;
            if (discardRecordingFrom)
            {
                // Redo brings the take back; its files stay in Media/ until then.
                if (outcome->committed && sess->undoName() == "Record")
                    sess->undo();
                locate (*discardRecordingFrom);
                discardRecordingFrom.reset();
                status = "Discarded the recording.";
            }
            edited();
        }
}

// Bounce (M8) ------------------------------------------------------------------------

engine::BounceSettings Studio::defaultBounceSettings() const
{
    engine::BounceSettings s;
    s.range = engine::defaultBounceRange (model());
    s.folder = projectRoot() / "Bounces";
    s.name = model().info.name.empty() ? "Bounce" : model().info.name;
    s.mp3 = std::nullopt; // WAV only until MP3 is ticked
    return s;
}

engine::Mp3Format Studio::defaultMp3Format() const
{
    engine::Mp3Format mp3;
    mp3.title = model().info.name;
    mp3.artist = userDisplayName;
    return mp3;
}

bool Studio::startBounce (const engine::BounceSettings& settings)
{
    if (bounceJob != nullptr)
        return false;
    if (settings.range.to <= settings.range.from)
    {
        setStatus ("The bounce range is empty.");
        return false;
    }
    if (! settings.wav && ! settings.mp3)
    {
        setStatus ("Choose WAV, MP3 or both.");
        return false;
    }
    eng->stop();
    lastBounce.reset();
    engine::BounceJob::Context ctx;
    ctx.media = &pool;
    ctx.plugins = [this] (const model::PluginInstance& p, bool instrument) { return host->create (p, instrument); };
    ctx.projectFolder = projectRoot();
    ctx.live = eng.get();
    ctx.realtimeOnly = [this] (const model::PluginInstance& p) {
        if (p.format != "vst3")
            return false;
        const auto* info = catalog->find (p.pluginUid);
        return info != nullptr && engine::isRealtimeOnlyCategory (info->category);
    };
    bounceFrom = settings.range.from;
    bounceJob = std::make_unique<engine::BounceJob> (model(), settings, std::move (ctx));
    setStatus (bounceJob->notice().empty() ? juce::String ("Bouncing...") : juce::String (bounceJob->notice()));
    return true;
}

void Studio::stepBounce()
{
    if (bounceJob == nullptr)
        return;
    // Offline: render for up to ~40 ms per tick so the window stays responsive.
    const auto until = juce::Time::getMillisecondCounterHiRes() + 40.0;
    bool more = bounceJob->step();
    while (more && ! bounceJob->realtime() && juce::Time::getMillisecondCounterHiRes() < until)
        more = bounceJob->step();
    if (more)
        return;
    lastBounce = bounceJob->finish();
    bounceJob.reset();
    if (lastBounce->ok)
        setStatus (juce::String (lastBounce->message));
    else
        setStatus (juce::String (lastBounce->error));
    if (onBounceDone)
        onBounceDone();
}

void Studio::cancelBounce()
{
    if (bounceJob == nullptr)
        return;
    bounceJob->cancel();
    stepBounce();
}

bool Studio::addBounceToProject (const fs::path& file)
{
    auto outcome = engine::importFiles (*sess, pool, mediaDir(), decoders, { file }, { bounceFrom, {} });
    if (outcome.regions.empty())
    {
        setStatus (juce::String (outcome.message));
        return false;
    }
    if (const auto* r = model().region (outcome.regions.front()))
        sess->setTrackMute (r->trackId, true);
    selection.set (outcome.regions);
    setStatus ("Added the bounce as a muted track.");
    edited();
    return true;
}

// Commands ---------------------------------------------------------------------------

namespace
{
void toggleBottom (PanelLayout& l, BottomTab tab) { l.bottom = l.bottom == tab ? BottomTab::none : tab; }
} // namespace

void Studio::zoomToFit()
{
    const auto close = [] (double a, double b) { return std::abs (a - b) <= 1e-9 * std::max (1.0, std::abs (a)); };
    const auto selected = selection.span (model());
    // D-UI-27: Z fits the selection; Z again, with the selection and the view as Z left them, fits everything.
    const bool again = selected && fittedSelection && close (fittedSelection->span.first, selected->first)
                       && close (fittedSelection->span.second, selected->second) && close (fittedSelection->originTick, view.originTick)
                       && close (fittedSelection->pixelsPerTick, view.pixelsPerTick);
    if (selected && ! again)
    {
        view.fit (selected->first, selected->second, timelineWidth, 0.9);
        fittedSelection = FittedSelection { *selected, view.originTick, view.pixelsPerTick };
        return;
    }
    fittedSelection.reset();
    std::optional<std::pair<double, double>> span;
    if (const auto all = arrange::projectSpan (model()))
        span = std::pair { 0.0, all->second }; // the whole project, from its start (F-ARR-21)
    if (! span)
        span = std::pair { 0.0, 8.0 * 3840.0 };
    view.fit (span->first, span->second, timelineWidth, 0.9);
}

bool Studio::stepRegion (int direction, bool add)
{
    const auto& m = model();
    const model::Region* anchor = nullptr;
    for (const auto& id : selection.ids())
        if (const auto* r = m.region (id); r != nullptr && (anchor == nullptr || (direction > 0 ? r->start > anchor->start : r->start < anchor->start)))
            anchor = r;
    const auto track = anchor != nullptr ? m.effectiveTrackId (*anchor) : local.selectedTrack;
    if (track.empty())
    {
        setStatus ("Select a region or a track first.");
        return false;
    }
    std::vector<const model::Region*> lane;
    for (const auto& [id, r] : m.regions)
        if (m.effectiveTrackId (*r) == track)
            lane.push_back (&*r);
    std::sort (lane.begin(), lane.end(), [] (const auto* a, const auto* b) { return std::pair (a->start, a->id) < std::pair (b->start, b->id); });
    const model::Region* to = nullptr;
    if (anchor != nullptr)
    {
        const auto it = std::find (lane.begin(), lane.end(), anchor);
        if (direction > 0 && it != lane.end() && it + 1 != lane.end())
            to = *(it + 1);
        else if (direction < 0 && it != lane.begin() && it != lane.end())
            to = *(it - 1);
    }
    else
    {
        const auto now = playhead();
        for (const auto* r : lane)
            if (direction > 0 ? r->start >= now && to == nullptr : r->start < now)
                to = r;
    }
    if (to == nullptr)
        return false;
    if (add) // Add Previous/Next Region to Selection (D-W3-12)
    {
        auto now = selection.ids();
        now.push_back (to->id);
        selection.set (now);
    }
    else
        selection.set ({ to->id });
    sendChangeMessage();
    return true;
}

std::vector<Id> Studio::pianoRollRegions() const
{
    const auto& m = model();
    const auto ids = selection.ids();
    const auto* base = m.region (editorRegion);
    if (base == nullptr || ! base->isMidi)
    {
        base = nullptr;
        for (const auto& id : ids)
            if (const auto* r = m.region (id); r != nullptr && r->isMidi)
            {
                base = r;
                break;
            }
    }
    if (base == nullptr)
        return {};
    const auto track = m.effectiveTrackId (*base);
    std::vector<const model::Region*> shown { base };
    for (const auto& id : ids)
        if (const auto* r = m.region (id); r != nullptr && r != base && r->isMidi && m.effectiveTrackId (*r) == track)
            shown.push_back (r);
    std::sort (shown.begin(), shown.end(), [] (const auto* a, const auto* b) { return a->start < b->start; });
    std::vector<Id> out;
    for (const auto* r : shown)
        out.push_back (r->id);
    return out;
}

void Studio::auditionNote (const Id& regionId, int pitch, int velocity, int channel, Ticks length)
{
    const auto* r = model().region (regionId);
    if (! piano.audition || r == nullptr)
        return;
    const auto& tempo = model().tempo;
    const auto seconds = tempo.samplesBetween (0.0, static_cast<double> (length)) / tempo.sampleRate();
    eng->audition (model().effectiveTrackId (*r), channel, pitch, velocity, std::min (1.0, seconds));
}

std::optional<bool> Studio::performNotes (Cmd command)
{
    auto& s = *sess;
    const auto& m = model();
    const auto regions = pianoRollRegions();
    if (regions.empty())
        return std::nullopt;
    const std::vector<commands::NoteRef> notes (piano.selection.begin(), piano.selection.end());
    const auto needNotes = [&] {
        if (notes.empty())
            setStatus ("Select notes first.");
        return ! notes.empty();
    };
    // The shown notes (or only the selected ones) where they sit on the timeline (D-W4-14).
    const auto timed = [&] (bool selectedOnly) {
        std::vector<commands::TimedNote> out;
        for (const auto& id : regions)
        {
            const auto* r = m.region (id);
            if (const auto* seq = m.sequence (r->midi.sequenceId))
                for (const auto& [noteId, n] : seq->notes)
                    if (n.pos >= r->midi.sequenceOffset && n.pos < r->midi.sequenceOffset + r->length
                        && (! selectedOnly || piano.selection.contains ({ id, noteId })))
                        out.push_back ({ { id, noteId }, r->start + n.pos - r->midi.sequenceOffset, n.length, n.pitch });
        }
        return out;
    };
    const auto applySpans = [&] (const std::map<commands::NoteRef, commands::NoteSpan>& spans, const char* name) {
        if (spans.empty())
        {
            setStatus ("Nothing to change.");
            return false;
        }
        const bool own = ! s.inGesture();
        if (own)
            s.beginGesture (name);
        for (const auto& [ref, span] : spans)
            if (! s.setNoteSpan (ref, span.start, span.length))
            {
                if (own)
                    s.cancelGesture();
                setStatus (juce::String (s.lastError()));
                edited();
                return false;
            }
        if (own)
            s.endGesture();
        edited();
        return true;
    };
    const auto audition = [&] {
        if (! notes.empty())
            if (const auto* n = commands::findNote (m, notes.front()))
                auditionNote (notes.front().regionId, n->pitch, n->velocity, n->channel, n->length);
    };
    switch (command)
    {
        case Cmd::copy:
        case Cmd::cut:
            if (! needNotes())
                return false;
            piano.clipboard = commands::NoteClipboard::of (m, notes);
            return command == Cmd::copy || check (s.deleteNotes (notes));
        case Cmd::paste:
        {
            const auto pasted = s.pasteNotes (piano.clipboard, m.effectiveTrackId (*m.region (regions.front())), playhead());
            if (pasted.empty())
                setStatus (piano.clipboard.empty() ? juce::String ("Copy notes first.") : juce::String (s.lastError()));
            piano.selection = { pasted.begin(), pasted.end() };
            edited();
            return ! pasted.empty();
        }
        case Cmd::deleteSelection:
            if (const auto point = std::exchange (piano.point, std::nullopt))
                return check (s.deleteControllers (point->first, { point->second }));
            return needNotes() && check (s.deleteNotes (notes));
        case Cmd::selectAll:
            piano.selection.clear();
            for (const auto& id : regions)
            {
                const auto* r = m.region (id);
                if (const auto* seq = m.sequence (r->midi.sequenceId))
                    for (const auto& [noteId, n] : seq->notes)
                        if (n.pos >= r->midi.sequenceOffset && n.pos < r->midi.sequenceOffset + r->length)
                            piano.selection.insert ({ id, noteId });
            }
            sendChangeMessage();
            return true;
        case Cmd::deselectAll:
            piano.selection.clear();
            piano.point.reset();
            sendChangeMessage();
            return true;
        case Cmd::invertSelection:
        {
            // D-UI-18: every note of the shown regions that wasn't selected.
            std::set<commands::NoteRef> inverted;
            for (const auto& id : regions)
            {
                const auto* r = m.region (id);
                if (const auto* seq = m.sequence (r->midi.sequenceId))
                    for (const auto& [noteId, n] : seq->notes)
                        if (n.pos >= r->midi.sequenceOffset && n.pos < r->midi.sequenceOffset + r->length && ! piano.selection.contains ({ id, noteId }))
                            inverted.insert ({ id, noteId });
            }
            piano.selection = std::move (inverted);
            sendChangeMessage();
            return true;
        }
        case Cmd::muteNotes: return needNotes() && check (s.toggleNotesMuted (notes));
        case Cmd::quantize: return check (s.quantizeNotes (regions, notes, piano.quantizeGrid, piano.strength / 100.0, piano.swing));
        case Cmd::transposeUp:
        case Cmd::transposeDown:
        case Cmd::octaveUp:
        case Cmd::octaveDown:
        {
            if (! needNotes())
                return false;
            const int by = command == Cmd::transposeUp ? 1 : command == Cmd::transposeDown ? -1 : command == Cmd::octaveUp ? 12 : -12;
            const auto ok = check (s.moveNotes (notes, 0, by, "Transpose"));
            if (ok)
                audition();
            return ok;
        }
        case Cmd::notesLeft:
        case Cmd::notesRight:
        case Cmd::nudgeLeft:
        case Cmd::nudgeRight:
        {
            if (! needNotes())
                return false;
            const auto step = piano.grid > 0 ? piano.grid : Ticks (240);
            return check (s.moveNotes (notes, command == Cmd::notesRight || command == Cmd::nudgeRight ? step : -step, 0));
        }
        case Cmd::selectPrevious:
        case Cmd::selectNext:
        {
            // D-UI-28: through the shown notes in order of time, then pitch; from the playhead when none is selected.
            using Entry = std::tuple<Ticks, int, commands::NoteRef>;
            std::vector<Entry> all;
            for (const auto& id : regions)
            {
                const auto* r = m.region (id);
                if (const auto* seq = m.sequence (r->midi.sequenceId))
                    for (const auto& [noteId, n] : seq->notes)
                        if (n.pos >= r->midi.sequenceOffset && n.pos < r->midi.sequenceOffset + r->length)
                            all.emplace_back (r->start + n.pos - r->midi.sequenceOffset, n.pitch, commands::NoteRef { id, noteId });
            }
            std::sort (all.begin(), all.end());
            const bool forward = command == Cmd::selectNext;
            std::optional<Entry> anchor;
            for (const auto& e : all)
                if (piano.selection.contains (std::get<2> (e)) && (! anchor || (forward ? *anchor < e : e < *anchor)))
                    anchor = e;
            std::optional<Entry> to;
            if (anchor)
            {
                const auto it = std::find (all.begin(), all.end(), *anchor);
                if (forward && it + 1 != all.end())
                    to = *(it + 1);
                else if (! forward && it != all.begin())
                    to = *(it - 1);
            }
            else
                for (const auto& e : all)
                    if (forward ? std::get<0> (e) >= playhead() && ! to : std::get<0> (e) < playhead())
                        to = e;
            if (! to)
                return false;
            piano.selection = { std::get<2> (*to) };
            piano.point.reset();
            if (const auto* n = commands::findNote (m, std::get<2> (*to)))
                auditionNote (std::get<2> (*to).regionId, n->pitch, n->velocity, n->channel, n->length);
            sendChangeMessage();
            return true;
        }
        case Cmd::selectAllFollowing:
        {
            // D-W4-14: every shown note starting at or after the earliest selected one, or the playhead.
            Ticks from = playhead();
            bool any = false;
            for (const auto& n : timed (true))
                if (! any || n.start < from)
                    from = n.start, any = true;
            piano.selection.clear();
            for (const auto& n : timed (false))
                if (n.start >= from)
                    piano.selection.insert (n.ref);
            piano.point.reset();
            sendChangeMessage();
            return ! piano.selection.empty();
        }
        case Cmd::legato:
            return needNotes() && applySpans (commands::legatoSpans (timed (true)), "Legato");
        case Cmd::removeOverlaps:
            return needNotes() && applySpans (commands::overlapFreeSpans (timed (true), timed (false)), "Remove Overlaps");
        case Cmd::noteStartToPlayhead:
            return needNotes() && applySpans (commands::startToPlayheadSpans (timed (true), playhead()), "Note Start to Playhead");
        case Cmd::noteEndToPlayhead:
            return needNotes() && applySpans (commands::endToPlayheadSpans (timed (true), playhead()), "Note End to Playhead");
        case Cmd::lengthenNotes:
        case Cmd::shortenNotes:
        {
            if (! needNotes())
                return false;
            // By the snap value (a sixteenth with snap off); never shorter than one tick.
            const auto step = piano.grid > 0 ? piano.grid : Ticks (240);
            std::map<commands::NoteRef, commands::NoteSpan> spans;
            for (const auto& n : timed (true))
                if (const auto length = std::max<Ticks> (1, n.length + (command == Cmd::lengthenNotes ? step : -step)); length != n.length)
                    spans[n.ref] = { n.start, length };
            return applySpans (spans, "Note Length");
        }
        case Cmd::scaleQuantize:
        {
            if (! needNotes())
                return false;
            const auto& list = commands::scales();
            const auto degrees = list[static_cast<std::size_t> (std::clamp (piano.scale, 0, static_cast<int> (list.size()) - 1))].degrees;
            std::vector<model::Note> changed;
            std::vector<Id> regionOf;
            for (const auto& ref : notes)
                if (const auto* n = commands::findNote (m, ref))
                    if (const auto to = commands::scaleQuantizePitch (n->pitch, piano.scaleRoot, degrees); to != n->pitch)
                    {
                        changed.push_back (*n);
                        changed.back().pitch = to;
                        regionOf.push_back (ref.regionId);
                    }
            if (changed.empty())
            {
                setStatus ("The selected notes are already in the scale.");
                return false;
            }
            const bool own = ! s.inGesture();
            if (own)
                s.beginGesture ("Scale Quantize");
            for (std::size_t i = 0; i < changed.size(); ++i)
                if (! s.updateNote (regionOf[i], changed[i]))
                {
                    if (own)
                        s.cancelGesture();
                    setStatus (juce::String (s.lastError()));
                    edited();
                    return false;
                }
            if (own)
                s.endGesture();
            edited();
            return true;
        }
        case Cmd::escape:
            if (piano.selection.empty())
                return std::nullopt;
            piano.selection.clear();
            sendChangeMessage();
            return true;
        case Cmd::playStop: case Cmd::record: case Cmd::cycle: case Cmd::metronome: case Cmd::goToStart: case Cmd::goToEnd:
        case Cmd::previousBar: case Cmd::nextBar: case Cmd::autopunch: case Cmd::followPlayhead: case Cmd::save: case Cmd::newTrack:
        case Cmd::undo: case Cmd::redo: case Cmd::split: case Cmd::join: case Cmd::muteRegions:
        case Cmd::repeat: case Cmd::crossfade: case Cmd::loopRegion: case Cmd::setCycleToSelection: case Cmd::zoomToFit: case Cmd::zoomIn:
        case Cmd::zoomOut: case Cmd::toolMenu: case Cmd::toggleInspector: case Cmd::toggleSidePanel: case Cmd::toggleMixer:
        case Cmd::togglePianoRoll: case Cmd::toggleAudioEditor: case Cmd::toggleGlobalTracks: case Cmd::addMarker: case Cmd::previousMarker:
        case Cmd::nextMarker: case Cmd::marker1: case Cmd::marker2: case Cmd::marker3: case Cmd::marker4: case Cmd::marker5: case Cmd::marker6:
        case Cmd::marker7: case Cmd::marker8: case Cmd::marker9: case Cmd::snapToggle: case Cmd::keyboardMidi: case Cmd::toggleTheme:
        case Cmd::about: case Cmd::applyQuantize: case Cmd::deleteTrack: case Cmd::newBus: case Cmd::pluginManager:
        case Cmd::toggleAutomation: case Cmd::bounce: case Cmd::newProject: case Cmd::openProject: case Cmd::saveAs:
        case Cmd::projectSettings: case Cmd::audioSettings: case Cmd::shareProject: case Cmd::shareFolder: case Cmd::joinProject: case Cmd::signIn:
        case Cmd::signOut: case Cmd::goOffline: case Cmd::addComment: case Cmd::saveVersion: case Cmd::lockTrack:
        case Cmd::importAudio: case Cmd::quit: case Cmd::duplicateTrack: case Cmd::duplicateTrackWithRegions: case Cmd::moveTrackUp:
        case Cmd::moveTrackDown: case Cmd::pause: case Cmd::exportRegions: case Cmd::discardRecording:
        case Cmd::reverseRegions: case Cmd::snapZeroCrossings: case Cmd::tapTempo: case Cmd::showHiddenTracks:
        case Cmd::stop: case Cmd::countIn: case Cmd::muteTrack: case Cmd::soloTrack: case Cmd::armTrack: case Cmd::newAudioTrack:
        case Cmd::newInstrumentTrack: case Cmd::renameMarker: case Cmd::deleteMarker: case Cmd::marker10: case Cmd::marker11:
        case Cmd::marker12: case Cmd::marker13: case Cmd::marker14: case Cmd::marker15: case Cmd::marker16: case Cmd::marker17:
        case Cmd::marker18: case Cmd::marker19: case Cmd::searchPlugins: case Cmd::deleteUnusedTracks:
        case Cmd::nudgeByGrid: case Cmd::nudgeByBar: case Cmd::nudgeByBeat: case Cmd::nudgeByDivision: case Cmd::nudgeByTick: case Cmd::slipLeft: case Cmd::slipRight: case Cmd::shuffleLeft: case Cmd::shuffleRight: case Cmd::gridLines:
        case Cmd::nextTool: case Cmd::previousTool:
        case Cmd::newTrackNextInput: case Cmd::searchTracks: case Cmd::configureTrackHeader: case Cmd::toggleAllTracksOff: case Cmd::togglePluginWindows:
        case Cmd::unmuteAll: case Cmd::clearRecallSolo: case Cmd::soloSelectedRegions: case Cmd::monitorTrack: case Cmd::copyChannelStrip: case Cmd::pasteChannelStrip: case Cmd::cycleMixerMode:
        case Cmd::zoomVerticalIn: case Cmd::zoomVerticalOut: case Cmd::saveZoom1: case Cmd::saveZoom2: case Cmd::saveZoom3: case Cmd::recallZoom1: case Cmd::recallZoom2: case Cmd::recallZoom3: case Cmd::navigateBack: case Cmd::navigateForward: case Cmd::pageUp: case Cmd::pageDown: case Cmd::pageLeft: case Cmd::pageRight: case Cmd::zoomTrack:
        case Cmd::selectPreviousTrack: case Cmd::selectNextTrack: case Cmd::selectInsideCycle: case Cmd::selectMutedRegions: case Cmd::selectSameColor: case Cmd::selectFirst: case Cmd::selectLast: case Cmd::togglePreviousRegion: case Cmd::toggleNextRegion: case Cmd::scrollToSelection: case Cmd::moveToPlayhead: case Cmd::moveToPlayheadSelectNext: case Cmd::regionStartToPlayhead: case Cmd::regionEndToPlayhead: case Cmd::splitAtCycle:
        case Cmd::automationAllOff: case Cmd::automationAllRead: case Cmd::automationAllTouch: case Cmd::automationAllLatch: case Cmd::automationOffRead: case Cmd::automationLatchRead: case Cmd::cycleAutomationParameter:
        case Cmd::markerForSelection: case Cmd::cycleToMarker: case Cmd::cycleToPreviousMarker: case Cmd::cycleToNextMarker: case Cmd::previousMarkerSetLocators: case Cmd::nextMarkerSetLocators: case Cmd::goToMarkerNumber:
        case Cmd::applyRegionParameters: case Cmd::applyRegionParametersExceptDelay:
        case Cmd::goToCycleStart: case Cmd::goToCycleEnd: case Cmd::goToSelectionStart: case Cmd::goToSelectionEnd: case Cmd::goToLastLocate: case Cmd::goToPosition: case Cmd::playFromStart: case Cmd::playFromCycleStart: case Cmd::playFromSelection: case Cmd::playFromWindowEdge: case Cmd::playFromPreviousBar: case Cmd::playFromLastLocate: case Cmd::stopGoToStart: case Cmd::stopGoToCycleStart: case Cmd::stopGoToLastLocate: case Cmd::cycleForward: case Cmd::cycleBack: case Cmd::doubleCycle: case Cmd::halveCycle:
            break;
    }
    return std::nullopt;
}

bool Studio::perform (Cmd command)
{
    endPluginGesture();
    if (previewing() && changesDocument (command))
    {
        setStatus (readOnlyReason());
        return false;
    }
    if (notesFocused())
        if (const auto handled = performNotes (command))
            return *handled;
    auto& s = *sess;
    const auto& m = model();
    const auto ids = selection.ids();
    switch (command)
    {
        case Cmd::playStop:
            if (playing())
                eng->stop();
            else
            {
                lastLocate = m.info.cycle.enabled ? m.info.cycle.start : playhead();
                eng->play (lastLocate); // D-UI-17: the cycle plays from its start
            }
            locatedAt.reset();
            break;
        case Cmd::stop:
            // D-UI-17: Stop while stopped goes back to the start of the cycle, or of the project.
            if (playing())
            {
                eng->stop();
                locatedAt.reset();
            }
            else
                locate (m.info.cycle.enabled && playhead() != m.info.cycle.start ? m.info.cycle.start : 0);
            break;
        case Cmd::countIn:
        {
            // Off and back to the length last used (1 bar until one is chosen in Navigate > Count-in).
            auto settings = eng->settings();
            if (settings.countInBars > 0)
                countInWhenOn = std::exchange (settings.countInBars, 0);
            else
                settings.countInBars = countInWhenOn;
            eng->setSettings (settings);
            setStatus (settings.countInBars == 0 ? juce::String ("Count-in off.") : "Count-in: " + juce::String (settings.countInBars) + (settings.countInBars == 1 ? " bar." : " bars."));
            break;
        }
        case Cmd::deselectAll:
            selection.clear();
            pointSelection = {};
            break;
        case Cmd::invertSelection:
        {
            // D-UI-18: regions on tracks that have a row; the ones tucked away on hidden tracks stay out of it.
            std::vector<Id> inverted;
            for (const auto& [id, r] : m.regions)
                if (! selection.contains (id) && std::find (m.rows.begin(), m.rows.end(), m.effectiveTrackId (*r)) != m.rows.end())
                    inverted.push_back (id);
            selection.set (std::move (inverted));
            break;
        }
        case Cmd::muteTrack:
        case Cmd::soloTrack:
        case Cmd::armTrack:
        {
            const auto* t = m.track (local.selectedTrack);
            if (t == nullptr)
            {
                setStatus ("Select a track first.");
                return false;
            }
            if (command == Cmd::muteTrack)
                toggleMute (t->id);
            else if (command == Cmd::soloTrack)
                toggleSolo (t->id);
            else if (t->kind == model::TrackKind::audio || t->kind == model::TrackKind::instrument)
                toggleArm (t->id);
            else
                return false;
            return true;
        }
        case Cmd::newAudioTrack:
        case Cmd::newInstrumentTrack:
        {
            // D-UI-19: one track of that kind below the selected one, with the New Track dialog's defaults, selected.
            const auto made = s.createTracks (command == Cmd::newAudioTrack ? model::TrackKind::audio : model::TrackKind::instrument, 1, 2, local.selectedTrack);
            if (made.empty())
                setStatus (juce::String (s.lastError()));
            else
                selectTrack (made.front());
            edited();
            return ! made.empty();
        }
        case Cmd::newTrackNextInput:
            return newTrackNextInput().has_value();
        case Cmd::pause:
            // Stops where it is (F-TRN-3, D-INV-6); pressed again, plays on from there.
            if (playing())
                eng->pause();
            else
                eng->play (playhead());
            locatedAt.reset();
            break;
        case Cmd::discardRecording:
            // F-REC-18: stop, throw the take away (tick() undoes its Record step), back to where it started.
            if (! recording())
            {
                setStatus ("Discard Recording works while recording.");
                return false;
            }
            discardRecordingFrom = recordPressedAt;
            eng->stop();
            break;
        case Cmd::record:
        {
            if (recording())
            {
                eng->stopRecording();
                break;
            }
            if (! audioOk)
            {
                setStatus ("Recording needs an audio device: choose one in Audio & MIDI Settings.");
                return false;
            }
            recordPressedAt = playhead();
            const auto result = eng->record (recordPressedAt);
            locatedAt.reset();
            if (! result.message.empty())
                setStatus (juce::String (result.message));
            break;
        }
        case Cmd::cycle: return check (s.setCycle (! m.info.cycle.enabled, m.info.cycle.start, m.info.cycle.end));
        case Cmd::autopunch:
        {
            if (m.info.punch.enabled)
                return check (s.setPunch (false, m.info.punch.start, m.info.punch.end));
            // D-W1-9: turning it on places the range at the cycle when the cycle is on, else 4 bars from the bar at the playhead.
            if (m.info.cycle.enabled)
                return check (s.setPunch (true, m.info.cycle.start, m.info.cycle.end));
            const auto bar = m.signatures.barAt (playhead()).bar;
            return check (s.setPunch (true, m.signatures.barStart (bar), m.signatures.barStart (bar + 4)));
        }
        case Cmd::metronome:
        {
            auto settings = eng->settings();
            settings.metronomeOn = ! settings.metronomeOn;
            eng->setSettings (settings);
            // The default only clicks while recording (02 W-1a step 6): say so, or turning it on mid-playback seems to do nothing.
            if (settings.metronomeOn && settings.metronomeDuring == engine::ClickDuring::record)
                setStatus ("Click is on while recording. Navigate > Click During changes when it plays.");
            else
                setStatus (settings.metronomeOn ? "Click on." : "Click off.");
            break;
        }
        case Cmd::goToStart: locate (0); break;
        case Cmd::goToCycleStart:
        case Cmd::goToCycleEnd:
        case Cmd::goToSelectionStart:
        case Cmd::goToSelectionEnd:
        case Cmd::goToLastLocate:
        case Cmd::playFromStart:
        case Cmd::playFromCycleStart:
        case Cmd::playFromSelection:
        case Cmd::playFromWindowEdge:
        case Cmd::playFromPreviousBar:
        case Cmd::playFromLastLocate:
        case Cmd::stopGoToStart:
        case Cmd::stopGoToCycleStart:
        case Cmd::stopGoToLastLocate:
        {
            // D-UI-35: where each command goes; the play commands start playback there, the stop ones stop first.
            std::optional<Ticks> to;
            const auto& cycle = m.info.cycle;
            const bool hasCycle = cycle.end > cycle.start;
            const auto is = [command] (std::initializer_list<Cmd> cmds) { return std::find (cmds.begin(), cmds.end(), command) != cmds.end(); };
            if (is ({ Cmd::goToCycleStart, Cmd::playFromCycleStart, Cmd::stopGoToCycleStart }))
            {
                if (hasCycle)
                    to = cycle.start;
            }
            else if (command == Cmd::goToCycleEnd)
            {
                if (hasCycle)
                    to = cycle.end;
            }
            else if (is ({ Cmd::goToSelectionStart, Cmd::goToSelectionEnd, Cmd::playFromSelection }))
                to = selectionEdge (command == Cmd::goToSelectionEnd);
            else if (is ({ Cmd::goToLastLocate, Cmd::playFromLastLocate, Cmd::stopGoToLastLocate }))
                to = lastLocate;
            else if (is ({ Cmd::playFromStart, Cmd::stopGoToStart }))
                to = 0;
            else if (command == Cmd::playFromWindowEdge)
                to = std::max<Ticks> (0, static_cast<Ticks> (std::ceil (view.originTick)));
            else if (command == Cmd::playFromPreviousBar)
            {
                const auto pos = m.signatures.barAt (playhead());
                to = m.signatures.barStart (std::max (pos.bar - 1, 0));
            }
            if (! to)
            {
                setStatus (command == Cmd::goToSelectionStart || command == Cmd::goToSelectionEnd || command == Cmd::playFromSelection
                               ? "Select regions or notes first."
                               : "Set a cycle first.");
                return false;
            }
            const bool startPlaying = command == Cmd::playFromStart || command == Cmd::playFromCycleStart || command == Cmd::playFromSelection
                                      || command == Cmd::playFromWindowEdge || command == Cmd::playFromPreviousBar || command == Cmd::playFromLastLocate;
            if (startPlaying)
            {
                lastLocate = *to;
                eng->play (*to);
                locatedAt.reset();
            }
            else
            {
                if (playing())
                {
                    if (command == Cmd::stopGoToStart || command == Cmd::stopGoToCycleStart || command == Cmd::stopGoToLastLocate)
                        eng->pause(); // stops where it is; the locate below puts it in place
                }
                const auto keep = lastLocate;
                locate (*to);
                if (command == Cmd::goToLastLocate || command == Cmd::stopGoToLastLocate)
                    lastLocate = keep;
            }
            break;
        }
        case Cmd::cycleForward:
        case Cmd::cycleBack:
        case Cmd::doubleCycle:
        case Cmd::halveCycle:
        {
            const auto& cycle = m.info.cycle;
            const auto length = cycle.end - cycle.start;
            if (length <= 0)
            {
                setStatus ("Set a cycle first.");
                return false;
            }
            Ticks start = cycle.start, end = cycle.end;
            if (command == Cmd::cycleForward)
                start += length, end += length;
            else if (command == Cmd::cycleBack)
            {
                const auto by = std::min (length, cycle.start);
                if (by == 0)
                {
                    setStatus ("The cycle is at the start already.");
                    return false;
                }
                start -= by, end -= by;
            }
            else if (command == Cmd::doubleCycle)
                end = start + 2 * length;
            else if (length < 2)
                return false;
            else
                end = start + length / 2;
            return check (s.setCycle (cycle.enabled, start, end));
        }
        case Cmd::goToEnd: locate (commands::projectEndTick (m)); break;
        case Cmd::previousBar:
        case Cmd::nextBar:
        {
            const auto pos = m.signatures.barAt (playhead());
            const auto bar = command == Cmd::nextBar ? pos.bar + 1 : (pos.offset > 0 ? pos.bar : std::max (pos.bar - 1, 0));
            locate (m.signatures.barStart (bar));
            break;
        }
        case Cmd::followPlayhead: follow = ! follow; break;
        case Cmd::save: return save();
        case Cmd::undo:
        case Cmd::redo:
        {
            const auto r = command == Cmd::undo ? s.undo() : s.redo();
            if (! r.message.empty())
                status = juce::String (r.message);
            else if (! r.done)
                status = command == Cmd::undo ? "Nothing to undo." : "Nothing to redo.";
            edited();
            return r.done;
        }
        case Cmd::copy:
        case Cmd::cut:
            if (ids.empty())
                return false;
            clipboard = commands::RegionClipboard::of (m, ids);
            if (command == Cmd::cut)
                return check (s.deleteRegions (ids));
            break;
        case Cmd::paste:
        {
            auto target = local.selectedTrack;
            if (target.empty() && ! ids.empty())
                target = m.effectiveTrackId (*m.region (ids.front()));
            const auto pasted = s.pasteRegions (clipboard, playhead(), target);
            selection.set (pasted);
            if (pasted.empty())
                setStatus (juce::String (s.lastError()));
            edited();
            return ! pasted.empty();
        }
        case Cmd::deleteSelection:
            if (! pointSelection.points.empty())
            {
                const auto points = std::vector<Id> (pointSelection.points.begin(), pointSelection.points.end());
                const auto laneId = pointSelection.laneId;
                pointSelection = {};
                return check (s.deleteAutomationPoints (laneId, points));
            }
            if (ids.empty())
                return false;
            return check (s.deleteRegions (ids));
        case Cmd::selectAll: selection.selectAll (m); break;
        case Cmd::split:
        {
            auto targets = ids;
            if (targets.empty() && ! local.selectedTrack.empty())
                for (const auto* r : m.regionsOnTrack (local.selectedTrack))
                    targets.push_back (r->id);
            const auto pieces = s.splitRegionsAt (targets, playhead());
            if (pieces.empty())
                setStatus (juce::String (s.lastError()));
            edited();
            return ! pieces.empty();
        }
        case Cmd::join: return check (engine::joinRegions (s, ids, pool, mediaDir()));
        case Cmd::reverseRegions: return ! ids.empty() && check (engine::reverseRegions (s, ids, pool, mediaDir()));
        case Cmd::muteRegions: return ! ids.empty() && check (s.toggleRegionsMuted (ids));
        case Cmd::nudgeLeft:
        case Cmd::nudgeRight:
        {
            if (ids.empty())
                return false;
            const auto anchor = selection.span (m)->first;
            const auto from = static_cast<Ticks> (anchor);
            return check (s.nudgeRegions (ids, nudgeDelta (from, command == Cmd::nudgeRight ? 1 : -1)));
        }
        case Cmd::repeat:
        {
            const auto copies = s.repeatRegions (ids, 1);
            if (copies.empty())
                setStatus (juce::String (s.lastError()));
            edited();
            return ! copies.empty();
        }
        case Cmd::crossfade: return check (s.crossfadeRegions (ids));
        case Cmd::loopRegion:
            for (const auto& id : ids)
                if (const auto* r = m.region (id))
                {
                    // D-ARR-8: up to the next region on the track, else to the cycle end, else 3 repeats.
                    const auto end = static_cast<Ticks> (std::ceil (m.regionEndTick (*r)));
                    auto until = end + (end - r->start) * 3;
                    std::optional<Ticks> next;
                    for (const auto* other : m.regionsOnTrack (r->trackId))
                        if (other->id != id && other->start >= end && (! next || other->start < *next))
                            next = other->start;
                    if (next)
                        until = *next;
                    else if (m.info.cycle.enabled && m.info.cycle.end > end)
                        until = m.info.cycle.end;
                    if (! r->loop && until <= end)
                    {
                        setStatus ("There's no room to loop \"" + juce::String (r->name) + "\": the next region starts right after it.");
                        continue;
                    }
                    if (! check (s.setRegionLoop (id, r->loop ? std::nullopt : std::optional<Ticks> (until))))
                        return false;
                }
            break;
        case Cmd::setCycleToSelection: return check (s.setCycleToRegions (ids));
        case Cmd::zoomToFit: rememberView(); zoomToFit(); break;
        case Cmd::zoomIn: rememberView(); view.zoomAround (1.5, view.tickToX (static_cast<double> (playhead()))); break;
        case Cmd::zoomOut: rememberView(); view.zoomAround (1.0 / 1.5, view.tickToX (static_cast<double> (playhead()))); break;
        case Cmd::zoomVerticalIn:
        case Cmd::zoomVerticalOut:
            rememberView();
            scaleTrackHeights (command == Cmd::zoomVerticalIn ? 1.25 : 0.8);
            break;
        case Cmd::saveZoom1:
        case Cmd::saveZoom2:
        case Cmd::saveZoom3:
        {
            const auto slot = static_cast<std::size_t> (static_cast<int> (command) - static_cast<int> (Cmd::saveZoom1));
            savedZooms[slot] = viewSnapshot();
            setStatus ("Saved as Zoom " + juce::String (static_cast<int> (slot) + 1) + ".");
            return true;
        }
        case Cmd::recallZoom1:
        case Cmd::recallZoom2:
        case Cmd::recallZoom3:
        {
            const auto slot = static_cast<std::size_t> (static_cast<int> (command) - static_cast<int> (Cmd::recallZoom1));
            if (! savedZooms[slot])
            {
                setStatus ("Zoom " + juce::String (static_cast<int> (slot) + 1) + " isn't saved yet.");
                return false;
            }
            rememberView();
            restoreView (*savedZooms[slot]);
            break;
        }
        case Cmd::navigateBack:
        case Cmd::navigateForward:
        {
            auto& from = command == Cmd::navigateBack ? backViews : forwardViews;
            auto& to = command == Cmd::navigateBack ? forwardViews : backViews;
            if (from.empty())
                return false;
            to.push_back (viewSnapshot());
            const auto v = from.back();
            from.pop_back();
            restoreView (v);
            break;
        }
        case Cmd::pageLeft:
        case Cmd::pageRight:
            view.originTick = std::max (0.0, view.originTick + (command == Cmd::pageRight ? 1.0 : -1.0) * timelineWidth / view.pixelsPerTick);
            break;
        case Cmd::pageUp:
        case Cmd::pageDown:
            if (verticalScroll && setVerticalScroll)
                setVerticalScroll (std::max (0, verticalScroll() + juce::roundToInt ((command == Cmd::pageDown ? 1.0 : -1.0) * timelineHeight)));
            break;
        case Cmd::zoomTrack:
        {
            // D-UI-38: the selected track grows to fill most of the view; again, back to its height.
            const auto id = local.selectedTrack;
            if (model().track (id) == nullptr)
            {
                setStatus ("Select a track first.");
                return false;
            }
            if (const auto it = zoomedFrom.find (id); it != zoomedFrom.end())
            {
                heights[id] = it->second;
                zoomedFrom.erase (it);
            }
            else
            {
                zoomedFrom[id] = trackHeight (id);
                heights[id] = std::clamp (juce::roundToInt (timelineHeight * 0.75), std::max (trackHeight (id), minTrackHeight), maxTrackHeight);
            }
            break;
        }
        case Cmd::escape: tool = Tool::pointer; break;
        case Cmd::nextTool:
        case Cmd::previousTool:
        {
            // D-UI-36: through the Tool menu's order, wrapping.
            const int n = static_cast<int> (lastTool) + 1;
            tool = static_cast<Tool> ((static_cast<int> (tool) + (command == Cmd::nextTool ? 1 : n - 1)) % n);
            setStatus (toolName (tool) + " tool.");
            break;
        }
        case Cmd::toggleInspector: layout.inspector = ! layout.inspector; break;
        case Cmd::toggleSidePanel: layout.sidePanel = ! layout.sidePanel; break;
        case Cmd::toggleGlobalTracks: layout.globalTracks = ! layout.globalTracks; break;
        case Cmd::toggleAutomation:
            automationView = ! automationView;
            pointSelection = {};
            break;
        case Cmd::toggleMixer: toggleBottom (layout, BottomTab::mixer); break;
        case Cmd::togglePianoRoll: toggleBottom (layout, BottomTab::pianoRoll); break;
        case Cmd::toggleAudioEditor:
            toggleBottom (layout, BottomTab::audioEditor);
            if (editorRegion.empty() && ! ids.empty())
                editorRegion = ids.front();
            break;
        case Cmd::addMarker:
        {
            const auto id = s.addMarker (playhead(), "");
            edited();
            return id.has_value();
        }
        case Cmd::selectPrevious:
        case Cmd::selectNext: return stepRegion (command == Cmd::selectNext ? 1 : -1);
        case Cmd::deleteUnusedTracks:
        {
            // D-UI-32: audio and instrument tracks with no regions, as one undo step; a locked one stays.
            std::set<Id> used;
            for (const auto& [rid, r] : m.regions)
            {
                used.insert (r->trackId);
                used.insert (m.effectiveTrackId (*r));
            }
            std::vector<Id> unused;
            for (const auto& tid : m.trackOrder)
                if (const auto* t = m.track (tid);
                    t != nullptr && (t->kind == model::TrackKind::audio || t->kind == model::TrackKind::instrument) && ! used.contains (tid))
                    unused.push_back (tid);
            if (unused.empty())
            {
                setStatus ("No unused tracks.");
                return false;
            }
            endPluginGesture();
            const bool own = ! s.inGesture();
            if (own)
                s.beginGesture ("Delete Unused Tracks");
            int deleted = 0;
            for (const auto& tid : unused)
                if (s.deleteTrack (tid))
                {
                    ++deleted;
                    if (local.selectedTrack == tid)
                        local.selectedTrack.clear();
                }
            if (own)
                s.endGesture();
            edited();
            setStatus ("Deleted " + juce::String (deleted) + (deleted == 1 ? " unused track." : " unused tracks."));
            return deleted > 0;
        }
        case Cmd::deleteMarker:
        case Cmd::renameMarker:
        {
            const auto* marker = markerAtPlayhead();
            if (marker == nullptr)
            {
                setStatus ("No marker at or before the playhead.");
                return false;
            }
            if (command == Cmd::deleteMarker)
                return check (s.deleteMarker (marker->id));
            if (uiCommand)
                uiCommand (command);
            break;
        }
        case Cmd::previousMarker:
        case Cmd::nextMarker:
        {
            const auto to = command == Cmd::nextMarker ? arrange::nextMarker (m, playhead()) : arrange::previousMarker (m, playhead());
            if (! to)
                return false;
            locate (*to);
            break;
        }
        case Cmd::markerForSelection:
        {
            // D-UI-39: a marker at the selected regions' start, named after the first of them.
            const auto span = selection.span (m);
            if (! span)
            {
                setStatus ("Select regions first.");
                return false;
            }
            std::string name;
            Ticks first = 0;
            for (const auto& id : ids)
                if (const auto* r = m.region (id); r != nullptr && (name.empty() || r->start < first))
                    name = r->name, first = r->start;
            const auto id = s.addMarker (static_cast<Ticks> (span->first), name);
            edited();
            return id.has_value();
        }
        case Cmd::cycleToMarker:
        case Cmd::cycleToPreviousMarker:
        case Cmd::cycleToNextMarker:
        case Cmd::previousMarkerSetLocators:
        case Cmd::nextMarkerSetLocators:
        {
            // D-UI-39: a marker's range runs to the next marker (the project end after the last).
            const auto markers = arrange::markersInOrder (m);
            if (markers.empty())
            {
                setStatus ("There are no markers.");
                return false;
            }
            const auto now = playhead();
            int at = -1; // the marker at or before the playhead
            for (std::size_t i = 0; i < markers.size(); ++i)
                if (markers[i]->pos <= now)
                    at = static_cast<int> (i);
            int pick = at;
            if (command == Cmd::cycleToPreviousMarker || command == Cmd::previousMarkerSetLocators)
                pick = at < 0 ? -1 : (markers[static_cast<std::size_t> (at)]->pos == now || command == Cmd::cycleToPreviousMarker ? at - 1 : at);
            else if (command == Cmd::cycleToNextMarker || command == Cmd::nextMarkerSetLocators)
                pick = at + 1;
            if (pick < 0 || pick >= static_cast<int> (markers.size()))
                return false;
            const auto start = markers[static_cast<std::size_t> (pick)]->pos;
            const auto end = static_cast<std::size_t> (pick) + 1 < markers.size() ? markers[static_cast<std::size_t> (pick) + 1]->pos
                                                                                  : std::max (commands::projectEndTick (m), start + 3840);
            const bool enable = command == Cmd::cycleToMarker || command == Cmd::cycleToPreviousMarker || command == Cmd::cycleToNextMarker || m.info.cycle.enabled;
            if (! check (s.setCycle (enable, start, end)))
                return false;
            if (command != Cmd::cycleToMarker)
                locate (start);
            break;
        }
        case Cmd::snapToggle: snapEnabled = ! snapEnabled; break;
        case Cmd::tapTempo: return tapTempo (juce::Time::getMillisecondCounterHiRes());
        case Cmd::showHiddenTracks:
            if (! check (session().setShowHiddenTracks (! model().info.showHiddenTracks)))
                return false;
            break;
        case Cmd::snapZeroCrossings:
            snapZeroCrossings = ! snapZeroCrossings;
            setStatus (snapZeroCrossings ? "Audio editor trims snap to zero crossings." : "Zero-crossing snap off.");
            break;
        case Cmd::keyboardMidi:
        {
            std::vector<engine::KeyboardMidi::Message> out;
            keys.toggle (out);
            for (const auto& msg : out)
                midiRouter->deliver ({}, msg.status, msg.data1, msg.data2);
            status = keys.enabled() ? "Keyboard MIDI on: A-L play notes, Z/X octave, C/V velocity." : "Keyboard MIDI off.";
            break;
        }
        case Cmd::toggleTheme:
            layout.darkTheme = ! layout.darkTheme;
            Theme::get().setDark (layout.darkTheme);
            break;
        case Cmd::newTrack:
        case Cmd::toolMenu:
        case Cmd::about:
        case Cmd::searchPlugins:
        case Cmd::searchTracks:
        case Cmd::configureTrackHeader:
        case Cmd::togglePluginWindows:
        case Cmd::scrollToSelection:
        case Cmd::goToMarkerNumber:
        case Cmd::goToPosition:
        case Cmd::pluginManager:
        case Cmd::deleteTrack:
        case Cmd::bounce:
        case Cmd::newProject:
        case Cmd::openProject:
        case Cmd::saveAs:
        case Cmd::projectSettings:
        case Cmd::audioSettings:
        case Cmd::shareProject:
        case Cmd::shareFolder:
        case Cmd::joinProject:
        case Cmd::signIn:
        case Cmd::addComment:
        case Cmd::saveVersion:
        case Cmd::importAudio:
        case Cmd::quit:
            if (uiCommand)
                uiCommand (command);
            break;
        case Cmd::exportRegions:
            if (selection.ids().empty())
            {
                setStatus ("Select regions to export.");
                return false;
            }
            if (uiCommand)
                uiCommand (command);
            break;
        case Cmd::duplicateTrack:
        case Cmd::duplicateTrackWithRegions:
        {
            const auto* t = m.track (local.selectedTrack);
            if (t == nullptr || t->kind == model::TrackKind::master)
            {
                setStatus ("Select a track to duplicate.");
                return false;
            }
            const auto copy = sess->duplicateTrack (local.selectedTrack, command == Cmd::duplicateTrackWithRegions);
            if (! copy)
            {
                setStatus (juce::String (sess->lastError()));
                return false;
            }
            selectTrack (*copy);
            edited();
            break;
        }
        case Cmd::moveTrackUp:
        case Cmd::moveTrackDown:
        {
            const auto& order = m.trackOrder;
            const auto it = std::find (order.begin(), order.end(), local.selectedTrack);
            if (it == order.end())
            {
                setStatus ("Select a track to move.");
                return false;
            }
            const auto at = static_cast<std::size_t> (it - order.begin());
            if (command == Cmd::moveTrackUp ? at == 0 : at + 1 >= order.size())
                return false;
            return moveTrackTo (local.selectedTrack, command == Cmd::moveTrackUp ? at - 1 : at + 1);
        }
        case Cmd::lockTrack:
            if (local.selectedTrack.empty())
            {
                setStatus ("Select a track to lock.");
                return false;
            }
            return toggleLock (local.selectedTrack);
        case Cmd::signOut:
            signOut();
            break;
        case Cmd::goOffline:
            setOnline (! wantOnline);
            setStatus (wantOnline ? "Back online." : "Working offline. Your edits are sent when you go back online.");
            break;
        case Cmd::newBus:
        {
            // Mix > New Bus: the selected track's output goes to a new bus (W-6 "choosing outputs").
            const auto* t = m.track (local.selectedTrack);
            if (t == nullptr || t->kind == model::TrackKind::master)
            {
                setStatus ("Select a track to route to the new bus.");
                return false;
            }
            const auto bus = s.newBus (t->id, false);
            if (! bus)
                setStatus (juce::String (s.lastError()));
            edited();
            return bus.has_value();
        }
        case Cmd::selectAllFollowing:
        {
            // D-W3-12: in the arrange view, every region starting at or after the earliest selected one, or the playhead.
            Ticks from = playhead();
            bool any = false;
            for (const auto& id : ids)
                if (const auto* r = m.region (id); r != nullptr && (! any || r->start < from))
                    from = r->start, any = true;
            std::vector<Id> picked;
            for (const auto& [rid, r] : m.regions)
                if (r->start >= from)
                    picked.push_back (rid);
            selection.set (picked);
            sendChangeMessage();
            return ! picked.empty();
        }
        case Cmd::selectInsideCycle:
        case Cmd::selectMutedRegions:
        case Cmd::selectSameColor:
        {
            std::set<std::pair<int, std::string>> colours;
            for (const auto& id : ids)
                if (const auto* r = m.region (id))
                {
                    const auto* t = m.track (m.effectiveTrackId (*r));
                    const auto c = r->color ? *r->color : t != nullptr ? t->color : model::Color {};
                    colours.insert ({ c.palette, c.rgb });
                }
            if (command == Cmd::selectSameColor && colours.empty())
            {
                setStatus ("Select a region first.");
                return false;
            }
            const auto& cyc = m.info.cycle;
            std::vector<Id> picked;
            for (const auto& [rid, r] : m.regions)
            {
                bool take = false;
                if (command == Cmd::selectInsideCycle)
                    take = cyc.end > cyc.start && r->start >= cyc.start && m.regionEndTick (*r) <= static_cast<double> (cyc.end);
                else if (command == Cmd::selectMutedRegions)
                    take = r->muted;
                else
                {
                    const auto* t = m.track (m.effectiveTrackId (*r));
                    const auto c = r->color ? *r->color : t != nullptr ? t->color : model::Color {};
                    take = colours.contains ({ c.palette, c.rgb });
                }
                if (take)
                    picked.push_back (rid);
            }
            selection.set (picked);
            sendChangeMessage();
            return ! picked.empty();
        }
        case Cmd::selectFirst:
        case Cmd::selectLast:
        {
            // On the selected track, or across every track when none is selected.
            const model::Region* pick = nullptr;
            for (const auto& [rid, r] : m.regions)
                if (local.selectedTrack.empty() || m.effectiveTrackId (*r) == local.selectedTrack)
                    if (pick == nullptr || (command == Cmd::selectFirst ? r->start < pick->start : r->start > pick->start))
                        pick = &*r;
            if (pick == nullptr)
                return false;
            selection.set ({ pick->id });
            sendChangeMessage();
            return true;
        }
        case Cmd::togglePreviousRegion:
        case Cmd::toggleNextRegion: return stepRegion (command == Cmd::toggleNextRegion ? 1 : -1, true);
        case Cmd::moveToPlayhead:
        case Cmd::moveToPlayheadSelectNext:
        {
            Ticks earliest = 0;
            bool any = false;
            for (const auto& id : ids)
                if (const auto* r = m.region (id); r != nullptr && (! any || r->start < earliest))
                    earliest = r->start, any = true;
            if (! any)
            {
                setStatus ("Select regions first.");
                return false;
            }
            if (playhead() != earliest && ! check (s.moveRegions (ids, playhead() - earliest, 0)))
                return false;
            if (command == Cmd::moveToPlayheadSelectNext)
                stepRegion (1);
            return true;
        }
        case Cmd::regionStartToPlayhead:
        case Cmd::regionEndToPlayhead:
        {
            // D-W3-12: each selected region the playhead is inside is trimmed to it, as one undo step.
            const auto now = playhead();
            std::vector<Id> inside;
            for (const auto& id : ids)
                if (const auto* r = m.region (id); r != nullptr && r->start < now && static_cast<double> (now) < m.regionEndTick (*r))
                    inside.push_back (id);
            if (inside.empty())
            {
                setStatus ("The playhead isn't inside a selected region.");
                return false;
            }
            s.beginGesture (command == Cmd::regionStartToPlayhead ? "Set Region Start" : "Set Region End");
            for (const auto& id : inside)
            {
                if (command == Cmd::regionStartToPlayhead)
                    s.trimRegionStart (id, now);
                else if (const auto* r = s.model().region (id))
                    s.trimRegionLength (id, now - r->start);
            }
            s.endGesture();
            edited();
            return true;
        }
        case Cmd::splitAtCycle:
        {
            const auto& cyc = m.info.cycle;
            if (cyc.end <= cyc.start || ids.empty())
            {
                setStatus (ids.empty() ? "Select regions first." : "Set a cycle range first.");
                return false;
            }
            const auto start = cyc.start, end = cyc.end;
            s.beginGesture ("Split at Cycle");
            auto pieces = s.splitRegionsAt (ids, start);
            std::vector<Id> all = ids;
            all.insert (all.end(), pieces.begin(), pieces.end());
            const auto more = s.splitRegionsAt (all, end);
            if (pieces.empty() && more.empty())
            {
                s.cancelGesture();
                setStatus ("The cycle doesn't cross a selected region.");
                return false;
            }
            s.endGesture();
            edited();
            return true;
        }
        case Cmd::muteNotes:
        case Cmd::quantize:
        case Cmd::legato:
        case Cmd::removeOverlaps:
        case Cmd::noteStartToPlayhead:
        case Cmd::noteEndToPlayhead:
        case Cmd::scaleQuantize:
            setStatus ("Click in the piano roll to edit notes.");
            return false;
        case Cmd::nudgeByGrid: nudgeValue = NudgeValue::grid; break;
        case Cmd::nudgeByBar: nudgeValue = NudgeValue::bar; break;
        case Cmd::nudgeByBeat: nudgeValue = NudgeValue::beat; break;
        case Cmd::nudgeByDivision: nudgeValue = NudgeValue::division; break;
        case Cmd::nudgeByTick: nudgeValue = NudgeValue::tick; break;
        case Cmd::gridLines: gridLines = ! gridLines; break;
        case Cmd::toggleAllTracksOff: return toggleAllTracksOff();
        case Cmd::unmuteAll:
        {
            std::vector<Id> muted; // by value: each edit rebuilds the model
            for (const auto& id : m.trackOrder)
                if (const auto* t = m.track (id); t != nullptr && t->mute)
                    muted.push_back (id);
            if (muted.empty())
            {
                setStatus ("No track is muted.");
                return false;
            }
            s.beginGesture ("Mute Off for All");
            for (const auto& id : muted)
                s.setTrackMute (id, false);
            s.endGesture();
            edited();
            return true;
        }
        case Cmd::clearRecallSolo:
            // D-W6-13: with anything soloed the solos are kept aside and cleared; with none, the kept ones come back.
            if (! local.soloed.empty())
            {
                keptSolo = local.soloed;
                local.soloed.clear();
            }
            else if (! keptSolo.empty())
            {
                for (const auto& id : keptSolo)
                    if (m.track (id) != nullptr)
                        local.setSolo (id, true);
                keptSolo.clear();
            }
            else
            {
                setStatus ("Nothing is soloed.");
                return false;
            }
            edited();
            return true;
        case Cmd::soloSelectedRegions:
        {
            std::set<Id> tracks;
            for (const auto& id : selection.ids())
                if (const auto* r = m.region (id))
                    tracks.insert (m.effectiveTrackId (*r));
            if (tracks.empty())
            {
                setStatus ("Select regions first.");
                return false;
            }
            local.soloed = tracks;
            edited();
            return true;
        }
        case Cmd::monitorTrack:
        {
            const auto* t = m.track (local.selectedTrack);
            if (t == nullptr || t->kind != model::TrackKind::audio)
            {
                setStatus ("Select an audio track first.");
                return false;
            }
            setTrackSwitch (TrackSwitch::monitor, t->id, t->monitor != "on");
            return true;
        }
        case Cmd::cycleMixerMode:
            mixerMode = mixerMode == MixerMode::single ? MixerMode::tracks : mixerMode == MixerMode::tracks ? MixerMode::all : MixerMode::single;
            break;
        case Cmd::copyChannelStrip: return copyChannelStrip (local.selectedTrack);
        case Cmd::pasteChannelStrip: return pasteChannelStrip (local.selectedTrack);
        case Cmd::lengthenNotes:
        case Cmd::shortenNotes:
        case Cmd::slipLeft:
        case Cmd::slipRight:
        {
            // D-W3-11: the selected regions' length, or what plays inside them, by the nudge value.
            if (ids.empty())
            {
                setStatus ("Select regions first.");
                return false;
            }
            const int dir = command == Cmd::lengthenNotes || command == Cmd::slipRight ? 1 : -1;
            const bool slip = command == Cmd::slipLeft || command == Cmd::slipRight;
            const bool own = ! s.inGesture();
            if (own)
                s.beginGesture (slip ? "Slip Region" : "Region Length");
            bool ok = true;
            for (const auto& id : ids)
            {
                const auto* r = m.region (id);
                if (r == nullptr)
                    continue;
                const auto end = static_cast<Ticks> (std::llround (m.regionEndTick (*r)));
                const auto stepFrom = slip ? r->start : end;
                const auto delta = nudgeDelta (stepFrom, dir);
                // Ticks to samples at the region (audio lengths and offsets are samples).
                const auto samples = [&] (Ticks from, Ticks by) {
                    return static_cast<std::int64_t> (std::llround (m.tempo.tickToSample (static_cast<double> (from + by)) - m.tempo.tickToSample (static_cast<double> (from))));
                };
                commands::EditResult result;
                if (slip)
                    // Slip right: the content moves later, so it starts earlier in the source.
                    result = r->isMidi ? s.slipRegion (id, r->midi.sequenceOffset - delta)
                                       : s.slipRegion (id, r->audio.sourceOffset - samples (r->start, delta));
                else if (r->isMidi)
                    result = s.trimRegionLength (id, std::max<Ticks> (1, r->length + delta));
                else
                    result = s.trimRegionLength (id, std::max<std::int64_t> (model::minAudioRegionLength, r->length + samples (end, delta)));
                if (! result)
                {
                    ok = false;
                    break;
                }
            }
            if (own)
            {
                if (ok)
                    s.endGesture();
                else
                    s.cancelGesture();
            }
            if (! ok)
                setStatus (juce::String (s.lastError()));
            edited();
            return ok;
        }
        case Cmd::shuffleLeft:
        case Cmd::shuffleRight:
        {
            // D-W3-11: each selected region moves until it meets the region before it on its track (left; the project
            // start with none) or after it (right; with none it stays). One undo step "Shuffle".
            if (ids.empty())
            {
                setStatus ("Select regions first.");
                return false;
            }
            std::vector<const model::Region*> chosen;
            for (const auto& id : ids)
                if (const auto* r = m.region (id))
                    chosen.push_back (r);
            const bool left = command == Cmd::shuffleLeft;
            std::sort (chosen.begin(), chosen.end(), [left] (const auto* a, const auto* b) { return left ? a->start < b->start : a->start > b->start; });
            std::vector<std::pair<Id, Ticks>> moves;
            for (const auto* r : chosen)
                moves.emplace_back (r->id, 0);
            const bool own = ! s.inGesture();
            if (own)
                s.beginGesture ("Shuffle");
            bool any = false, ok = true;
            for (auto& [id, unused] : moves)
            {
                const auto& now = model();
                const auto* r = now.region (id);
                if (r == nullptr)
                    continue;
                const auto trackId = now.effectiveTrackId (*r);
                const auto lengthOf = [&] (const model::Region& x) { return static_cast<Ticks> (std::llround (now.regionEndTick (x))) - x.start; };
                const auto end = r->start + lengthOf (*r);
                std::optional<Ticks> to;
                for (const auto& other : now.regionsOnTrack (trackId))
                {
                    if (other->id == id)
                        continue;
                    const auto oEnd = other->start + lengthOf (*other);
                    if (left && oEnd <= r->start && (! to || oEnd > *to))
                        to = oEnd;
                    if (! left && other->start >= end && (! to || other->start - lengthOf (*r) < *to))
                        to = other->start - lengthOf (*r);
                }
                if (left && ! to)
                    to = 0;
                if (! to || *to == r->start)
                    continue;
                if (! s.moveRegion (id, *to))
                {
                    ok = false;
                    break;
                }
                any = true;
            }
            if (own)
            {
                if (ok && any)
                    s.endGesture();
                else
                    s.cancelGesture();
            }
            if (! ok)
                setStatus (juce::String (s.lastError()));
            else if (! any)
                setStatus ("Nothing to shuffle.");
            edited();
            return ok && any;
        }
        case Cmd::automationAllOff:
        case Cmd::automationAllRead:
        case Cmd::automationAllTouch:
        case Cmd::automationAllLatch:
        {
            const auto mode = command == Cmd::automationAllOff ? "off" : command == Cmd::automationAllRead ? "read" : command == Cmd::automationAllTouch ? "touch" : "latch";
            return setAllAutomationModes (mode);
        }
        case Cmd::automationOffRead:
        case Cmd::automationLatchRead:
        {
            const auto* t = m.track (local.selectedTrack);
            if (t == nullptr)
            {
                setStatus ("Select a track first.");
                return false;
            }
            const std::string other = command == Cmd::automationOffRead ? "off" : "latch";
            return check (s.setTrackAutomationMode (t->id, t->automationMode == "read" ? other : "read"));
        }
        case Cmd::cycleAutomationParameter: return cycleAutomationParameter (local.selectedTrack);
        case Cmd::octaveUp:
        case Cmd::octaveDown:
            if (! pointSelection.points.empty())
                return nudgeAutomationPoints (command == Cmd::octaveUp ? 0.1 : -0.1);
            return false;
        case Cmd::transposeUp: // Up and Down step through the tracks in the arrange view (D-W3-12)
        case Cmd::selectPreviousTrack:
        case Cmd::transposeDown:
        case Cmd::selectNextTrack:
        {
            // With automation points selected, Up/Down nudge their value by 1% instead (Shift: 10%, D-W8-7).
            if ((command == Cmd::transposeUp || command == Cmd::transposeDown) && ! pointSelection.points.empty())
                return nudgeAutomationPoints (command == Cmd::transposeUp ? 0.01 : -0.01);
            const auto& rows = m.rows;
            if (rows.empty())
                return false;
            const bool up = command == Cmd::transposeUp || command == Cmd::selectPreviousTrack;
            const auto at = std::find (rows.begin(), rows.end(), local.selectedTrack);
            std::size_t next = 0;
            if (at == rows.end())
                next = up ? rows.size() - 1 : 0;
            else
            {
                const auto i = static_cast<std::size_t> (at - rows.begin());
                if (up ? i == 0 : i + 1 >= rows.size())
                    return false;
                next = up ? i - 1 : i + 1;
            }
            selectTrack (rows[next]);
            return true;
        }
        case Cmd::notesLeft:
        case Cmd::notesRight:
            return false; // these arrows do nothing in the arrange view
        case Cmd::applyRegionParameters:
        case Cmd::applyRegionParametersExceptDelay:
        {
            std::vector<Id> midi;
            for (const auto& id : ids)
                if (const auto* r = m.region (id); r != nullptr && r->isMidi)
                    midi.push_back (id);
            if (midi.empty())
            {
                setStatus ("Select MIDI regions.");
                return false;
            }
            return check (s.applyRegionParameters (midi, command == Cmd::applyRegionParameters));
        }
        case Cmd::applyQuantize:
        {
            bool any = false;
            for (const auto& id : ids)
                if (const auto* r = m.region (id); r != nullptr && r->isMidi && r->midi.quantize)
                {
                    if (! check (s.applyRegionQuantize (id)))
                        return false;
                    any = true;
                }
            if (! any)
                setStatus ("Select a MIDI region with a quantize setting.");
            return any;
        }
        case Cmd::marker1:
        case Cmd::marker2:
        case Cmd::marker3:
        case Cmd::marker4:
        case Cmd::marker5:
        case Cmd::marker6:
        case Cmd::marker7:
        case Cmd::marker8:
        case Cmd::marker9:
        case Cmd::marker10:
        case Cmd::marker11:
        case Cmd::marker12:
        case Cmd::marker13:
        case Cmd::marker14:
        case Cmd::marker15:
        case Cmd::marker16:
        case Cmd::marker17:
        case Cmd::marker18:
        case Cmd::marker19:
        {
            const auto number = command >= Cmd::marker10 ? static_cast<int> (command) - static_cast<int> (Cmd::marker10) + 10
                                                         : static_cast<int> (command) - static_cast<int> (Cmd::marker1) + 1;
            const auto to = arrange::markerNumber (m, number);
            if (! to)
                return false;
            locate (*to);
            break;
        }
    }
    sendChangeMessage();
    return true;
}

bool Studio::goToMarkerNumber (int number)
{
    const auto to = number > 0 ? arrange::markerNumber (model(), number) : std::nullopt;
    if (! to)
    {
        setStatus ("There's no marker " + juce::String (number) + ".");
        return false;
    }
    locate (*to);
    sendChangeMessage();
    return true;
}

bool Studio::keyboardMidiKey (const juce::KeyPress& key, bool down)
{
    if (! keys.enabled() || key.getModifiers().isCommandDown() || key.getModifiers().isAltDown())
        return false;
    const auto c = static_cast<char> (juce::CharacterFunctions::toLowerCase (key.getTextCharacter()));
    std::vector<engine::KeyboardMidi::Message> out;
    const bool used = down ? keys.keyDown (c, false, out) : keys.keyUp (c, out);
    for (const auto& msg : out)
        midiRouter->deliver ({}, msg.status, msg.data1, msg.data2);
    return used;
}

// Files ------------------------------------------------------------------------------

engine::ImportOutcome Studio::importFiles (const std::vector<fs::path>& files, engine::DropTarget target)
{
    auto outcome = engine::importFiles (*sess, pool, mediaDir(), decoders, files, target);
    if (! outcome.regions.empty())
        selection.set (outcome.regions);
    status = juce::String (outcome.message);
    edited();
    return outcome;
}

engine::ImportOutcome Studio::importAtPlayhead (const std::vector<fs::path>& files)
{
    engine::DropTarget target;
    target.at = playhead();
    if (const auto* t = model().track (local.selectedTrack); t != nullptr && t->kind == model::TrackKind::audio)
        target.trackId = t->id;
    return importFiles (files, target);
}

bool Studio::previewFile (const fs::path& file)
{
    std::string error;
    auto audio = decoders.decode (file, &error);
    if (! audio)
    {
        setStatus (juce::String (file.filename().string()) + ": " + juce::String (error));
        return false;
    }
    eng->preview (std::make_shared<const media::AudioData> (std::move (*audio)));
    return true;
}

Ticks Studio::nudgeDelta (Ticks from, int direction) const
{
    // D-W3-11: the nudge value at `from`; Grid is the arrange grid as before (a sixteenth with snap off).
    const auto& m = model();
    const auto& sig = m.signatures.signatureAtBar (m.signatures.barAt (from).bar);
    const Ticks beat = 4 * ticksPerQuarter / sig.denominator;
    switch (nudgeValue)
    {
        case NudgeValue::bar: return direction * sig.barLength();
        case NudgeValue::beat: return direction * beat;
        case NudgeValue::division: return direction * std::max<Ticks> (1, beat / 4);
        case NudgeValue::tick: return direction;
        case NudgeValue::grid: break;
    }
    const auto grid = activeSnap().kind == arrange::Snap::Kind::off ? arrange::Snap::ticks (240) : activeSnap();
    return arrange::gridStep (from, direction, grid, m.signatures, view.pixelsPerTick) - from;
}

std::optional<Ticks> Studio::selectionEdge (bool end) const
{
    const auto& m = model();
    std::optional<Ticks> edge;
    const auto take = [&] (Ticks from, Ticks to) {
        const auto t = end ? to : from;
        if (! edge || (end ? t > *edge : t < *edge))
            edge = t;
    };
    if (notesFocused())
        for (const auto& ref : piano.selection)
            if (const auto* n = commands::findNote (m, ref))
                if (const auto* r = m.region (ref.regionId))
                {
                    const auto at = commands::noteTimelineTick (*r, n->pos);
                    take (at, at + n->length);
                }
    if (! edge)
        for (const auto& id : selection.ids())
            if (const auto* r = m.region (id))
                take (r->start, r->start + r->length);
    return edge;
}

std::optional<Id> Studio::newTrackNextInput()
{
    // D-UI-34: the selected track's kind, channels and output, one track below it, on the input after its own.
    const auto* t = model().track (local.selectedTrack);
    if (t == nullptr || (t->kind != model::TrackKind::audio && t->kind != model::TrackKind::instrument))
    {
        setStatus ("Select an audio or instrument track first.");
        return std::nullopt;
    }
    const auto kind = t->kind;
    const auto channels = t->channels;
    const auto input = t->input;
    const auto output = t->output.busId;
    const auto monitor = t->monitor;
    // The same instrument, fresh: its kind, not its settings.
    std::optional<model::PluginInstance> instrument;
    if (t->strip.instrument)
    {
        instrument.emplace();
        instrument->format = t->strip.instrument->format;
        instrument->pluginUid = t->strip.instrument->pluginUid;
        instrument->name = t->strip.instrument->name;
        instrument->vendor = t->strip.instrument->vendor;
        instrument->version = t->strip.instrument->version;
    }
    auto& s = *sess;
    s.beginGesture ("New Track");
    const auto made = s.createTracks (kind, 1, channels, local.selectedTrack);
    if (made.empty())
    {
        s.cancelGesture();
        setStatus (juce::String (s.lastError()));
        edited();
        return std::nullopt;
    }
    const auto id = made.front();
    if (kind == model::TrackKind::audio)
    {
        model::InputAssignment next;
        if (input.kind == model::InputAssignment::Kind::audio && input.firstChannel + 2 * channels <= device().config().numInputChannels)
        {
            next.kind = model::InputAssignment::Kind::audio;
            next.firstChannel = input.firstChannel + channels;
        }
        s.setTrackInput (id, next);
        s.setTrackMonitor (id, monitor);
    }
    if (kind == model::TrackKind::instrument && instrument)
        s.setInstrument (id, *instrument);
    if (! output.empty())
        s.setTrackOutput (id, output);
    s.endGesture();
    selectTrack (id);
    edited();
    return id;
}

// Projects ------------------------------------------------------------------------------

bool Studio::save() { return writeProject (true); }

bool Studio::writeProject (bool announce)
{
    flushPluginStates (true);
    std::string error;
    if (! folder->save (sess->document(), {}, &error))
    {
        setStatus ("Couldn't save: " + juce::String (error));
        return false;
    }
    project::writeMediaIndex (model(), folder->mediaDir());
    saveViewState();
    unsaved = false;
    scratch = false; // the user saved it: it stays when replaced
    lastSharedSave = juce::Time::getMillisecondCounterHiRes();
    if (announce)
    {
        recent.add (folder->root());
        setStatus ("Saved.");
    }
    else
        sendChangeMessage(); // the title loses its "*"
    return true;
}

// Shared folder (D-COL-45) ----------------------------------------------------------------

bool Studio::inSharedFolder() const { return folder != nullptr && folder->shared(); }

bool Studio::shareInFolder (const fs::path& parent, std::string* error)
{
    auto fail = [error] (const std::string& m) {
        if (error != nullptr)
            *error = m;
        return false;
    };
    if (inSharedFolder())
        return fail ("This project is already in a shared folder.");
    std::error_code ec;
    if (parent.empty() || ! fs::is_directory (parent, ec))
        return fail ("Choose a folder that your sync app copies to your other computers.");
    if (! fs::equivalent (folder->root().parent_path(), parent, ec))
    {
        auto target = project::ProjectFolder::folderFor (parent, model().info.name);
        if (fs::exists (target, ec))
            return fail (project::utf8 (target.filename()) + " is already in that folder.");
        if (! saveAs (target, true, error))
            return false;
    }
    if (! folder->makeShared (sess->document(), error))
        return false;
    unsaved = false;
    scratch = false;
    recent.add (folder->root());
    setStatus ("Shared. Open " + juce::String (project::utf8 (folder->root())) + " on your other computers.");
    return true;
}

void Studio::pollSharedFolder (double now)
{
    if (now - lastPeerPoll < sharedFolderPollMs)
        return;
    lastPeerPoll = now;
    const auto headsBefore = sess->document().heads();
    for (const auto& bytes : folder->pollPeers())
        sess->applyChanges (bytes);
    if (sess->document().heads() != headsBefore)
    {
        // Same as a relay change: orphaned regions land on the one Recovered track (D-ARCH-11).
        if (! sess->model().syntheticTracks.empty() && sess->access() == commands::Session::Access::full)
            sess->materializeRecoveredTracks();
        remoteChanged();
    }
    // A collaborator's audio can arrive after the change that uses it: pick it up when it lands.
    bool loaded = false;
    const auto folders = project::mediaFolders (folder->root());
    for (const auto& [hash, ref] : model().media)
        if (! pool.available (hash))
            if (const auto file = project::findMediaFile (folders, hash); file && pool.load (file->parent_path(), hash) != nullptr)
                loaded = true;
    if (loaded)
    {
        peakCache.clear();
        waveCache.clear();
        edited();
    }
}

juce::String Studio::windowTitle() const
{
    return juce::String (juce::CharPointer_UTF8 (model().info.name.c_str())) + (unsaved ? "*" : "") + " - Weft Studio";
}

bool Studio::newProject (const fs::path& parent, const commands::NewProjectOptions& options, std::string* error, bool scratchProject)
{
    if (! model::isValidSampleRate (options.sampleRate))
    {
        if (error != nullptr)
            *error = "The sample rate must be 44.1, 48, 88.2 or 96 kHz.";
        return false;
    }
    // An untouched new project gives way, so its name is free again.
    if (sess != nullptr && untouched())
    {
        std::error_code ec;
        fs::remove_all (folder->root(), ec);
        scratch = false;
    }
    fs::path dir;
    auto opts = options;
    for (int n = 1;; ++n)
    {
        opts.name = n == 1 ? options.name : options.name + " " + std::to_string (n);
        dir = project::ProjectFolder::folderFor (parent, opts.name);
        if (! fs::exists (dir))
            break;
    }
    auto projectFolder = std::make_unique<project::ProjectFolder> (dir, deviceIdText);
    if (! projectFolder->create (error))
        return false;
    auto created = commands::Session::createNew (opts, "local", error);
    if (! created || ! projectFolder->save (created->document(), {}, error))
        return false;
    adopt (std::move (projectFolder), std::make_unique<commands::Session> (std::move (*created)), scratchProject);
    return true;
}

std::vector<fs::path> Studio::exportSelectedRegions (const fs::path& destination)
{
    const auto ids = selection.ids();
    const auto result = engine::exportRegions (model(), { ids.begin(), ids.end() }, pool, destination, model().info.recordBitDepth);
    setStatus (juce::String::fromUTF8 (result.message.c_str()));
    return result.files;
}

bool Studio::deleteProject (const fs::path& root, std::string* error)
{
    auto fail = [error] (std::string why) {
        if (error != nullptr)
            *error = std::move (why);
        return false;
    };
    std::error_code ec;
    if (fs::equivalent (root, projectRoot(), ec))
        return fail ("That project is open. Open another project first.");
    if (! project::isProjectFolder (root))
        return fail ("That isn't a Weft project folder.");
    if (! moveToTrash (root))
        return fail ("Couldn't move the project to the Trash.");
    recent.remove (root);
    setStatus ("Moved " + juce::String::fromUTF8 (project::utf8 (root.filename()).c_str()) + " to the Trash.");
    return true;
}

Studio::OpenResult Studio::openProject (const fs::path& picked, Recovery recovery)
{
    OpenResult result;
    const auto root = project::projectRootFor (picked);
    result.root = root;
    if (! project::isProjectFolder (root))
    {
        result.error = project::utf8 (root.filename()) + " isn't a Weft project.";
        return result;
    }
    auto projectFolder = std::make_unique<project::ProjectFolder> (root, deviceIdText);
    auto opened = projectFolder->open();
    if (! opened.document)
    {
        result.error = opened.error;
        return result;
    }
    if (opened.recoveryAvailable())
    {
        const auto entries = projectFolder->log().read();
        result.recoveryTimeMs = entries.empty() ? 0 : entries.back().timeMs;
        if (recovery == Recovery::ask)
        {
            result.needsRecoveryChoice = true;
            return result;
        }
        if (recovery == Recovery::recover ? ! projectFolder->recover (opened, &result.error)
                                          : ! projectFolder->discard (opened, commands::systemClockMs()))
        {
            if (result.error.empty())
                result.error = "Couldn't archive the recovery log.";
            return result;
        }
    }
    auto session = std::make_unique<commands::Session> (std::move (*opened.document), "local");
    adopt (std::move (projectFolder), std::move (session), false);
    recent.add (root);
    result.ok = true;
    result.missingMedia = missingMedia();
    result.missingPlugins = missingPlugins();
    return result;
}

bool Studio::saveAs (const fs::path& newRoot, bool copyMedia, std::string* error)
{
    flushPluginStates (true);
    if (! project::saveProjectAs (sess->document(), model(), folder->root(), newRoot, { copyMedia }, error))
        return false;
    // The session (and its undo history) carries on in the new folder.
    folder = std::make_unique<project::ProjectFolder> (newRoot, deviceIdText);
    eng->setProjectFolder (folder->root());
    auto name = project::utf8 (newRoot.filename());
    if (name.ends_with (".weft"))
        name.resize (name.size() - 5);
    if (! name.empty() && name != model().info.name)
        sess->renameProject (name);
    scratch = false;
    if (! save())
        return false;
    edited();
    return true;
}

void Studio::discardUnsavedChanges()
{
    if (unsaved)
        folder->log().archive (commands::systemClockMs());
    unsaved = false;
    changesDiscarded = true;
    if (scratch)
        everEdited = false; // nothing worth keeping: the folder goes when the project is replaced
    sendChangeMessage();
}

std::vector<std::string> Studio::missingMedia() const { return project::missingMedia (model(), project::mediaFolders (folder->root())); }

int Studio::locateMedia (const fs::path& searchFolder)
{
    std::map<std::string, std::int64_t> wanted;
    for (const auto& hash : missingMedia())
    {
        const auto* ref = model().mediaRef (hash);
        wanted[hash] = ref != nullptr ? ref->sizeBytes : 0;
    }
    const auto found = project::locateMedia (searchFolder, wanted, folder->mediaDir());
    for (const auto& hash : found)
        pool.load (folder->mediaDir(), hash);
    if (! found.empty())
    {
        peakCache.clear();
        waveCache.clear();
        edited();
    }
    return static_cast<int> (found.size());
}

std::vector<std::string> Studio::missingPlugins() const
{
    std::vector<std::string> names;
    auto check = [&] (const model::PluginInstance& p) {
        const auto proc = eng->processor (p.id);
        if (proc != nullptr && proc->isPlaceholder() && std::find (names.begin(), names.end(), p.name) == names.end())
            names.push_back (p.name.empty() ? p.pluginUid : p.name);
    };
    for (const auto& id : model().trackOrder)
        if (const auto* t = model().track (id))
        {
            if (t->strip.instrument)
                check (*t->strip.instrument);
            for (const auto& p : t->strip.inserts)
                check (p);
        }
    if (const auto* master = model().track (model().info.masterTrackId))
        for (const auto& p : master->strip.inserts)
            check (p);
    return names;
}

void Studio::adopt (std::unique_ptr<project::ProjectFolder> projectFolder, std::unique_ptr<commands::Session> session, bool isScratch)
{
    releaseProject();
    folder = std::move (projectFolder);
    sess = std::move (session);
    scratch = isScratch;
    everEdited = false;
    unsaved = false;
    changesDiscarded = false;
    if (signedIn())
        sess->setUser (relayAccount.userId);
    sess->onLocalChange = [this] (const doc::Bytes& change, std::int64_t time) {
        folder->log().append (change, time);
        unsaved = true;
        everEdited = true;
    };
    resetViewState();
    local.selectedTrack = model().trackOrder.empty() ? Id {} : model().trackOrder.front();
    eng->setProjectFolder (folder->root());
    // The project's metronome defaults; this user's other transport options carry over.
    auto settings = engine::TransportSettings::fromDefaults (model().info.metronomeDefaults);
    const auto before = eng->settings();
    settings.returnToStartOnStop = before.returnToStartOnStop;
    settings.midiMergeLinear = before.midiMergeLinear;
    settings.midiMergeCycle = before.midiMergeCycle;
    settings.midiReplace = before.midiReplace;
    settings.clickBars = before.clickBars;
    settings.clickBeats = before.clickBeats;
    settings.clickDivisions = before.clickDivisions;
    settings.clickOnlyCountIn = before.clickOnlyCountIn;
    settings.clickTone = before.clickTone;
    eng->setSettings (settings);
    loadProjectMedia();
    syncEngine();
    loadViewState();
    startSync();
    ++editCount;
    sendChangeMessage();
}

void Studio::releaseProject()
{
    if (sess == nullptr)
        return;
    endSessionVersion();
    previewModel.reset();
    previewId.clear();
    mediaSyncer.reset();
    syncClient.reset();
    missingSeen.clear();
    pointerAt.reset();
    localPresence = {};
    if (bounceJob != nullptr)
        cancelBounce();
    eng->stop();
    eng->stopPreview();
    if (! scratch || everEdited)
        saveViewState();
    // A new project nobody touched leaves nothing behind.
    if (scratch && ! everEdited && folder != nullptr)
    {
        std::error_code ec;
        fs::remove_all (folder->root(), ec);
    }
    sess->onLocalChange = nullptr;
}

void Studio::resetViewState()
{
    selection = {};
    tool = Tool::pointer;
    view = {};
    editorRegion.clear();
    clipboard = {};
    piano = {};
    pointSelection = {};
    local = {};
    heights.clear();
    lanesShown.clear();
    pendingState.clear();
    peakCache.clear();
    waveCache.clear();
    locatedAt.reset();
    locatedFrame = -1;
    pool.clear();
    openThread.clear();
    mentionsShown.clear();
    notices.clear();
    commentsSeenAt = mentionsSeenAt = 0;
    chatReadId.clear();
    lastInputMs = 0;
}

void Studio::loadProjectMedia()
{
    const auto folders = project::mediaFolders (folder->root());
    for (const auto& [hash, ref] : model().media)
        if (const auto file = project::findMediaFile (folders, hash))
            pool.load (file->parent_path(), hash);
}

fs::path Studio::viewStateFile() const
{
    // Each computer keeps its own view in a shared folder, so a collaborator's scrolling doesn't move it.
    return folder->root() / "Cache" / (inSharedFolder() ? "view-" + deviceIdText + ".txt" : std::string ("view.txt"));
}

void Studio::saveViewState() const
{
    if (folder == nullptr || ! fs::exists (folder->root()))
        return;
    const auto text = "playhead=" + std::to_string (playhead()) + "\norigin=" + std::to_string (view.originTick)
                      + "\nzoom=" + std::to_string (view.pixelsPerTick) + "\ncommentsSeen=" + std::to_string (commentsSeenAt)
                      + "\nmentionsSeen=" + std::to_string (mentionsSeenAt) + "\nchatRead=" + chatReadId + "\n";
    std::error_code ec;
    fs::create_directories (folder->root() / "Cache", ec);
    const auto* p = reinterpret_cast<const std::uint8_t*> (text.data());
    project::writeFileAtomic (viewStateFile(), { p, text.size() });
}

void Studio::loadViewState()
{
    const auto bytes = project::readFile (viewStateFile());
    if (! bytes)
        return;
    const auto lines = juce::StringArray::fromLines (juce::String (reinterpret_cast<const char*> (bytes->data()), bytes->size()));
    for (const auto& line : lines)
    {
        const auto key = line.upToFirstOccurrenceOf ("=", false, false);
        const auto value = line.fromFirstOccurrenceOf ("=", false, false);
        if (key == "playhead")
            locate (std::max<Ticks> (0, value.getLargeIntValue()));
        else if (key == "origin")
            view.originTick = std::max (0.0, value.getDoubleValue());
        else if (key == "zoom")
            view.pixelsPerTick = std::clamp (value.getDoubleValue(), arrange::TimelineView::minPixelsPerTick, arrange::TimelineView::maxPixelsPerTick);
        else if (key == "commentsSeen")
            commentsSeenAt = value.getLargeIntValue();
        else if (key == "mentionsSeen")
            mentionsSeenAt = value.getLargeIntValue();
        else if (key == "chatRead")
            chatReadId = value.toStdString();
    }
}

// Audio device ---------------------------------------------------------------------------

bool Studio::setAudioDevice (std::unique_ptr<engine::AudioDevice> device, const engine::DeviceConfig& config)
{
    eng->stop();
    if (audioDevice != nullptr)
    {
        audioDevice->stop();
        audioDevice->close();
    }
    if (silentClock != nullptr)
    {
        silentClock->stop();
        silentClock.reset();
    }
    audioDevice = std::move (device);
    deviceConfig = config;
    lostDevice.clear();
    audioOk = audioDevice != nullptr && audioDevice->open (config) && audioDevice->start (*eng);
    if (! audioOk)
        startSilentClock();
    setStatus (audioOk ? "Audio: " + juce::String (audioDevice->name()) : "No audio device: Weft is running without sound.");
    return audioOk;
}

void Studio::checkDevice()
{
    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (audioOk && audioDevice != nullptr && audioDevice->failed())
    {
        // Unplugged or failed mid-session: stop, keep any recording (the silent clock finishes it),
        // carry on in No-audio mode, and reopen the same device when it's back.
        lostDevice = audioDevice->name();
        eng->stop();
        audioDevice->stop();
        audioDevice->close();
        audioOk = false;
        startSilentClock();
        lastReconnectMs = now;
        setStatus ("The audio device \"" + juce::String (lostDevice) + "\" stopped. Weft is running without sound until it's back.");
        return;
    }
    if (lostDevice.empty() || audioDevice == nullptr || now - lastReconnectMs < reconnectIntervalMs)
        return;
    lastReconnectMs = now;
    if (! audioDevice->open (deviceConfig))
        return;
    silentClock->stop();
    silentClock.reset();
    audioOk = audioDevice->start (*eng);
    if (! audioOk)
    {
        audioDevice->close();
        startSilentClock();
        return;
    }
    lostDevice.clear();
    setStatus ("Audio: " + juce::String (audioDevice->name()) + " is back.");
}

void Studio::startSilentClock()
{
    silentClock = std::make_unique<engine::NullDevice>();
    engine::DeviceConfig c;
    c.sampleRate = model().info.sampleRate;
    c.maxBlockSize = 512;
    c.numInputChannels = 0;
    silentClock->open (c);
    silentClock->start (*eng);
    silentClockMs = juce::Time::getMillisecondCounterHiRes();
}

void Studio::playTestTone()
{
    const auto rate = audioDevice != nullptr && audioOk ? audioDevice->config().sampleRate : 48000.0;
    auto tone = std::make_shared<media::AudioData>();
    tone->sampleRate = rate;
    const auto frames = static_cast<std::size_t> (std::lround (rate));
    const auto gain = std::pow (10.0, -18.0 / 20.0);
    tone->channels.assign (2, std::vector<float> (frames));
    for (std::size_t i = 0; i < frames; ++i)
        tone->channels[0][i] = tone->channels[1][i] = static_cast<float> (gain * std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * static_cast<double> (i) / rate));
    eng->preview (std::move (tone));
}

const WaveSummary& Studio::waveform (const std::string& hash)
{
    if (const auto it = waveCache.find (hash); it != waveCache.end())
        return it->second;
    auto& out = waveCache[hash];
    media::WavReader reader;
    const auto file = mediaDir() / media::mediaFileName (hash);
    if (fs::exists (file) && reader.open (file))
    {
        const auto& info = reader.info();
        const auto frames = info.frames;
        std::vector<std::vector<float>> buf (static_cast<std::size_t> (info.channels), std::vector<float> (65536));
        std::vector<float*> ptrs;
        for (auto& b : buf)
            ptrs.push_back (b.data());
        out.begin (frames);
        for (std::int64_t start = 0; start < frames; start += 65536)
        {
            const auto n = reader.read (start, std::min<std::int64_t> (65536, frames - start), ptrs.data());
            out.add (ptrs.data(), static_cast<int> (ptrs.size()), start, n);
        }
        out.finish();
        return out;
    }
    // Media held only in memory (tests, generated audio).
    if (const auto source = pool.find (hash); source && source->numBlocks() > 0)
    {
        out.begin (source->numFrames());
        for (std::int64_t b = 0; b < source->numBlocks(); ++b)
        {
            std::vector<const float*> channels;
            for (int ch = 0; ch < source->numChannels(); ++ch)
                channels.push_back (source->block (ch, b));
            const auto first = b * engine::MediaSource::blockFrames;
            out.add (channels.data(), static_cast<int> (channels.size()), first, std::min (engine::MediaSource::blockFrames, source->numFrames() - first));
        }
        out.finish();
    }
    return out;
}

const std::vector<float>& Studio::peaks (const std::string& hash)
{
    if (const auto it = peakCache.find (hash); it != peakCache.end())
        return it->second;
    auto& out = peakCache[hash];
    const auto& wave = waveform (hash);
    if (wave.empty())
        return out;
    out.resize (static_cast<std::size_t> ((wave.frames() + peakFrames - 1) / peakFrames));
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        const auto b = wave.range (static_cast<double> (i) * peakFrames, static_cast<double> (i + 1) * peakFrames);
        out[i] = std::max (std::abs (b.lo), std::abs (b.hi));
    }
    return out;
}
// Plug-ins -------------------------------------------------------------------------------

fs::path& Studio::settingsFolder()
{
    static fs::path folder = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Weft Studio").getFullPathName().toStdString();
    return folder;
}

engine::PluginCatalog::ScanResult Studio::rescanPlugins (bool retryFailed)
{
    engine::PluginCatalog::ScanOptions options;
    options.folders = pluginFolders;
    options.scanner = scannerPath;
    options.retryFailed = retryFailed;
    const auto result = catalog->scan (options);
    setStatus ("Plug-ins: " + juce::String (result.found) + " found, " + juce::String (result.failed) + " failed.");
    return result;
}

std::vector<Studio::PluginChoice> Studio::pluginChoices (bool instruments) const
{
    std::vector<PluginChoice> out;
    for (const auto& b : engine::builtinPlugins())
        if (b.instrument == instruments)
        {
            PluginChoice c;
            c.plugin.format = "builtin";
            c.plugin.pluginUid = b.uid;
            c.plugin.name = b.name;
            c.plugin.vendor = "Weft Studio";
            c.plugin.version = JUCE_APPLICATION_VERSION_STRING;
            c.label = b.name;
            c.group = "Weft";
            out.push_back (std::move (c));
        }
    for (const auto& p : catalog->menu (instruments))
    {
        PluginChoice c;
        c.plugin.format = p.format;
        c.plugin.pluginUid = p.uid;
        c.plugin.name = p.name;
        c.plugin.vendor = p.vendor;
        c.plugin.version = p.version;
        c.label = p.name;
        c.group = p.vendor.empty() ? juce::String ("Other") : juce::String (p.vendor);
        out.push_back (std::move (c));
    }
    return out;
}

std::optional<Id> Studio::addPlugin (const Id& trackId, const PluginChoice& choice, std::optional<std::size_t> index)
{
    endPluginGesture();
    const auto id = sess->addInsert (trackId, withDefaultSetting (choice.plugin), index);
    if (! id)
        setStatus (juce::String (sess->lastError()));
    edited();
    if (id && openPluginEditor)
        openPluginEditor (*id);
    return id;
}

bool Studio::replacePlugin (const Id& trackId, const Id& pluginId, const PluginChoice& choice)
{
    endPluginGesture();
    const auto id = sess->replaceInsert (trackId, pluginId, withDefaultSetting (choice.plugin));
    if (! id)
        setStatus (juce::String (sess->lastError()));
    edited();
    if (id && openPluginEditor)
        openPluginEditor (*id);
    return id.has_value();
}

bool Studio::movePlugin (Id fromTrackId, Id pluginId, Id toTrackId, std::size_t index, bool copy)
{
    // The ids are copies: saving the state below rebuilds the model a caller's references point into.
    endPluginGesture();
    writePluginState (pluginId);
    bool done = false;
    if (copy)
        done = sess->copyInsertToTrack (fromTrackId, pluginId, toTrackId, index).has_value();
    else
        done = static_cast<bool> (sess->moveInsertToTrack (fromTrackId, pluginId, toTrackId, index));
    if (! done)
        setStatus (juce::String (sess->lastError()));
    edited();
    return done;
}

bool Studio::setInstrument (const Id& trackId, const PluginChoice& choice)
{
    endPluginGesture();
    const auto id = sess->setInstrument (trackId, choice.plugin);
    if (! id)
        setStatus (juce::String (sess->lastError()));
    edited();
    if (id && openPluginEditor)
        openPluginEditor (*id);
    return id.has_value();
}

const model::PluginInstance* Studio::pluginInstance (const Id& pluginId, Id* trackId) const
{
    for (const auto& [id, t] : model().tracks)
    {
        if (t->strip.instrument && t->strip.instrument->id == pluginId)
        {
            if (trackId != nullptr)
                *trackId = id;
            return &*t->strip.instrument;
        }
        for (const auto& p : t->strip.inserts)
            if (p.id == pluginId)
            {
                if (trackId != nullptr)
                    *trackId = id;
                return &p;
            }
    }
    return nullptr;
}

void Studio::pluginParamsChanged (const Id& pluginId, const std::vector<std::pair<std::string, double>>& changes)
{
    Id trackId;
    if (pluginInstance (pluginId, &trackId) == nullptr || changes.empty())
        return;
    if (! sess->inGesture())
        sess->beginGesture ("Plug-in Parameter");
    for (const auto& [paramId, value] : changes)
        sess->setPluginParam (trackId, pluginId, paramId, value);
    eng->adoptPluginParams (pluginId, changes); // the processor already has these values
    lastParamChange = juce::Time::getMillisecondCounterHiRes();
    pendingState[pluginId] = lastParamChange;
    edited();
}

void Studio::endPluginGesture()
{
    if (lastParamChange > 0 && sess->inGesture())
        sess->endGesture();
    lastParamChange = 0;
}

void Studio::writePluginState (const Id& pluginId)
{
    pendingState.erase (pluginId);
    Id trackId;
    const auto* plugin = pluginInstance (pluginId, &trackId);
    const auto p = eng->processor (pluginId);
    if (plugin == nullptr || p == nullptr || p->isPlaceholder())
        return;
    const auto bytes = p->state();
    std::string error;
    const auto value = media::pluginStateValue (bytes, projectRoot() / "Blobs", &error);
    if (! value)
    {
        setStatus ("Couldn't save the plug-in's settings: " + juce::String (error));
        return;
    }
    if (*value == plugin->state)
        return;
    endPluginGesture();
    eng->adoptPluginState (pluginId, bytes); // no need to load the state back into the plug-in
    check (sess->setPluginState (trackId, pluginId, *value));
}

model::PluginInstance Studio::withDefaultSetting (model::PluginInstance plugin) const
{
    const auto setting = plugin_settings::loadDefault (pluginSettingsRoot(), plugin);
    if (! setting)
        return plugin;
    for (const auto& [id, v] : setting->params)
        plugin.params[id] = v;
    if (! setting->state.empty())
        if (const auto value = media::pluginStateValue (setting->state, projectRoot() / "Blobs"))
            plugin.state = *value;
    return plugin;
}

std::optional<PluginSetting> Studio::currentSetting (const Id& pluginId)
{
    writePluginState (pluginId); // the editor's last changes first
    const auto* plugin = pluginInstance (pluginId);
    if (plugin == nullptr)
        return std::nullopt;
    PluginSetting s;
    s.params = plugin->params;
    const auto p = eng->processor (pluginId);
    if (p != nullptr && ! p->isPlaceholder())
    {
        const auto infos = p->parameters();
        for (std::size_t i = 0; i < infos.size(); ++i)
            s.params[infos[i].id] = p->parameter (static_cast<int> (i));
        s.state = p->state();
    }
    else if (const auto bytes = media::pluginStateBytes (plugin->state, projectRoot() / "Blobs"))
        s.state = *bytes;
    return s;
}

bool Studio::applySetting (const Id& pluginId, const PluginSetting& setting, const juce::String& undoName)
{
    Id trackId;
    if (pluginInstance (pluginId, &trackId) == nullptr || previewing())
        return false;
    endPluginGesture();
    pendingState.erase (pluginId);
    std::optional<doc::Value> state;
    if (! setting.state.empty())
    {
        std::string error;
        state = media::pluginStateValue (setting.state, projectRoot() / "Blobs", &error);
        if (! state)
        {
            setStatus ("Couldn't load the setting: " + juce::String (error));
            return false;
        }
    }
    sess->beginGesture (undoName.toStdString());
    if (state)
        sess->setPluginState (trackId, pluginId, *state); // the chunk first: parameters then land on top of it
    for (const auto& [id, v] : setting.params)
        sess->setPluginParam (trackId, pluginId, id, v);
    sess->endGesture();
    edited();
    return true;
}

bool Studio::resetPluginSetting (const Id& pluginId)
{
    const auto p = eng->processor (pluginId);
    if (p == nullptr || p->isPlaceholder())
        return false;
    PluginSetting s;
    for (const auto& info : p->parameters())
        s.params[info.id] = info.defaultValue;
    return applySetting (pluginId, s, "Reset Setting");
}

bool Studio::copyPluginSetting (const Id& pluginId)
{
    auto s = currentSetting (pluginId);
    const auto* plugin = pluginInstance (pluginId);
    if (! s || plugin == nullptr)
        return false;
    settingClipboard = std::move (s);
    settingClipboardPlugin = plugin->format + ":" + plugin->pluginUid;
    return true;
}

bool Studio::canPastePluginSetting (const Id& pluginId) const
{
    const auto* plugin = pluginInstance (pluginId);
    return settingClipboard.has_value() && plugin != nullptr && settingClipboardPlugin == plugin->format + ":" + plugin->pluginUid;
}

bool Studio::pastePluginSetting (const Id& pluginId)
{
    if (! canPastePluginSetting (pluginId))
        return false;
    const auto s = *settingClipboard; // a copy: applying rebuilds the model
    return applySetting (pluginId, s, "Paste Setting");
}

void Studio::flushPluginStates (bool all)
{
    const auto now = juce::Time::getMillisecondCounterHiRes();
    std::vector<Id> due;
    for (const auto& [id, at] : pendingState)
        if (all || now - at >= 2000.0)
            due.push_back (id);
    for (const auto& id : due)
        writePluginState (id);
}

juce::String Studio::deleteTrackQuestion (const Id& trackId) const
{
    const auto* t = model().track (trackId);
    if (t == nullptr || t->kind != model::TrackKind::bus)
        return {};
    const auto n = sess->sendsTo (trackId);
    return "Remove " + juce::String (t->name) + " and " + juce::String (n) + (n == 1 ? " send?" : " sends?");
}

bool Studio::deleteTrack (const Id& trackId)
{
    endPluginGesture();
    if (local.selectedTrack == trackId)
        local.selectedTrack.clear();
    return check (sess->deleteTrack (trackId));
}
// Collaboration (M10) ---------------------------------------------------------------------------

namespace
{
bool sameRelay (const std::string& a, const std::string& b) { return collab::normalizeRelayUrl (a) == collab::normalizeRelayUrl (b); }
} // namespace

void Studio::loadAccount()
{
    std::ifstream f (accountFile(), std::ios::binary);
    const doc::Bytes bytes ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    if (bytes.empty())
        return;
    if (const auto v = doc::decode (bytes))
        relayAccount = collab::Account::fromValue (*v);
}

void Studio::saveAccount()
{
    std::error_code ec;
    fs::create_directories (accountFile().parent_path(), ec);
    if (! relayAccount.signedIn())
    {
        fs::remove (accountFile(), ec);
        return;
    }
    const auto bytes = doc::encode (relayAccount.toValue());
    const auto tmp = accountFile().string() + ".tmp";
    {
        std::ofstream f (tmp, std::ios::binary | std::ios::trunc);
        f.write (reinterpret_cast<const char*> (bytes.data()), static_cast<std::streamsize> (bytes.size()));
    }
    fs::rename (tmp, accountFile(), ec);
}

void Studio::signedInAs (const collab::Account& account)
{
    relayAccount = account;
    saveAccount();
    userDisplayName = account.displayName;
    sess->setUser (account.userId);
    startSync();
    sendChangeMessage();
}

void Studio::signOut()
{
    mediaSyncer.reset();
    syncClient.reset();
    relayAccount = {};
    saveAccount();
    sess->setUser ("local");
    sess->setAccess (commands::Session::Access::full);
    status = "Signed out. The project keeps working on this computer.";
    sendChangeMessage();
}

bool Studio::shared() const { return ! sess->relayUrl().empty(); }

void Studio::startSync()
{
    mediaSyncer.reset();
    syncClient.reset();
    if (! shared() || ! signedIn() || ! sameRelay (sess->relayUrl(), relayAccount.relayUrl))
        return;
    collab::SyncOptions o;
    o.account = relayAccount;
    o.projectId = model().info.id;
    o.clientId = "c" + juce::Uuid().toDashedString().removeCharacters ("-").toStdString();
    o.pendingFile = folder->root() / "Cache" / "sync-pending.txt";
    o.startOnline = wantOnline;
    if (syncClock)
    {
        o.clock = syncClock;
        sess->setClock (syncClock);
    }
    syncClient = std::make_unique<collab::SyncClient> (*sess, std::move (o));
    syncClient->onRemoteChange = [this] { remoteChanged(); };
    syncClient->onMessage = [this] (const std::string& m) { setStatus (m); };
    syncClient->onStatusChange = [this] {
        // A role change (made Editor, say) can start uploads.
        if (mediaSyncer != nullptr && ! syncClient->role().empty())
            mediaSyncer->setRole (syncClient->role());
        sendChangeMessage();
    };
    syncClient->onAccountChange = [this] (const collab::Account& a) {
        if (a.userId != relayAccount.userId)
            return;
        relayAccount = a;
        saveAccount();
        if (mediaSyncer != nullptr)
            mediaSyncer->setAccount (a);
    };
    syncClient->onJoined = [this] {
        if (mediaSyncer != nullptr)
            mediaSyncer->setRole (syncClient->role());
        collabNotices();
        sendChangeMessage();
    };
    syncClient->onBlobAvailable = [this] (const std::string& hash, std::int64_t) {
        if (mediaSyncer != nullptr)
            mediaSyncer->blobAvailable (hash);
    };
    syncClient->onPresenceChange = [this] { sendChangeMessage(); };
    startMediaSync();
}

void Studio::startMediaSync()
{
    collab::MediaSyncOptions o;
    o.account = relayAccount;
    o.projectId = model().info.id;
    o.projectRoot = folder->root();
    o.transfer = transferOptions;
    mediaSyncer = std::make_unique<collab::MediaSync> (*sess, std::move (o));
    mediaSyncer->onDownloadProgress = [this] (const std::string& hash, const collab::MediaSync::Download& d) {
        // Play what has arrived (D-COL-11): the engine picks up a new source; an existing one grows.
        const auto* ref = model().mediaRef (hash);
        if (ref == nullptr || d.partFile.empty())
            return;
        const bool fresh = pool.find (hash) == nullptr;
        if (pool.loadPartial (d.partFile, hash, ref->lengthSamples) && fresh)
            syncEngine();
        peakCache.erase (hash);
        waveCache.erase (hash);
        sendChangeMessage();
    };
    mediaSyncer->onDownloaded = [this] (const std::string& hash, const fs::path&) { mediaDownloaded (hash); };
    mediaSyncer->onMessage = [this] (const std::string& m) { setStatus (juce::String::fromUTF8 (m.c_str())); };
    mediaSyncer->onUploaded = [this] (const std::string&) { sendChangeMessage(); };
    mediaSyncer->onAccountChange = [this] (const collab::Account& a) {
        if (a.userId != relayAccount.userId)
            return;
        relayAccount = a;
        saveAccount();
    };
    if (! syncClient->role().empty())
        mediaSyncer->setRole (syncClient->role());
}

void Studio::mediaDownloaded (const std::string& hash)
{
    if (model().media.contains (hash))
    {
        std::string error;
        if (! pool.completeDownload (mediaDir(), hash, &error))
            setStatus ("Couldn't open downloaded audio: " + juce::String::fromUTF8 (error.c_str()));
    }
    peakCache.erase (hash);
    waveCache.erase (hash);
    syncEngine();
    sendChangeMessage();
}

std::optional<Studio::DownloadState> Studio::downloadState (const std::string& hash) const
{
    if (mediaSyncer == nullptr)
        return std::nullopt;
    const auto& downloads = mediaSyncer->downloads();
    const auto it = downloads.find (hash);
    if (it == downloads.end())
        return std::nullopt;
    DownloadState d;
    d.fraction = it->second.total > 0 ? std::clamp (static_cast<double> (it->second.done) / static_cast<double> (it->second.total), 0.0, 1.0) : 0.0;
    d.waiting = it->second.waiting;
    d.failed = ! it->second.error.empty();
    return d;
}

void Studio::setPointer (std::optional<collab::Presence::Pointer> pointer)
{
    pointerAt = std::move (pointer);
    noteInput();
}

void Studio::noteInput()
{
    lastInputMs = juce::Time::getMillisecondCounterHiRes();
    if (syncClient != nullptr)
        syncClient->noteInput();
}

std::vector<const collab::RemotePresence*> Studio::collaborators() const
{
    std::vector<const collab::RemotePresence*> out;
    if (syncClient == nullptr)
        return out;
    for (const auto& [id, who] : syncClient->collaborators())
        out.push_back (&who);
    // Stable order: by name, then client.
    std::sort (out.begin(), out.end(), [] (auto* a, auto* b) { return std::tie (a->displayName, a->clientId) < std::tie (b->displayName, b->clientId); });
    return out;
}

Ticks Studio::collaboratorPlayhead (const collab::RemotePresence& who) const
{
    const auto now = syncClient != nullptr ? syncClient->now() : 0;
    return collab::extrapolatePlayhead (who.state.playhead, model().tempo, now, who.receivedAt);
}

void Studio::updatePresence()
{
    if (syncClient == nullptr)
        return;
    collab::Presence p;
    p.status = recording() ? collab::Presence::Status::recording : collab::Presence::Status::active;
    switch (layout.bottom)
    {
        case BottomTab::pianoRoll: p.view = collab::Presence::View::pianoRoll; break;
        case BottomTab::audioEditor: p.view = collab::Presence::View::audioEditor; break;
        case BottomTab::mixer: p.view = collab::Presence::View::mixer; break;
        case BottomTab::none: p.view = collab::Presence::View::arrange; break;
    }
    p.focusTrack = local.selectedTrack;
    // While recording, the badge goes on the track being recorded.
    if (recording() && ! local.armed.empty() && ! local.armed.contains (p.focusTrack))
        p.focusTrack = *local.armed.begin();
    if (selection.regions().size() == 1)
        p.focusRegion = *selection.regions().begin();
    else if (layout.bottom == BottomTab::audioEditor)
        p.focusRegion = editorRegion;
    // The playhead's `at` only changes when it jumps or the transport starts or stops, so a
    // steady play doesn't count as a change (the 2 s refresh sends it, 05 §5).
    p.playhead.playing = playing();
    p.playhead.pos = playhead();
    p.playhead.at = syncClient->now();
    const auto& last = localPresence.playhead;
    if (last.playing == p.playhead.playing)
    {
        const auto expected = last.playing ? collab::extrapolatePlayhead (last, model().tempo, p.playhead.at, p.playhead.at) : last.pos;
        if (std::abs (expected - p.playhead.pos) < 480)
            p.playhead = last;
    }
    p.pointer = pointerAt;
    const auto take = [] (auto begin, auto end, std::vector<Id>& out, std::int64_t& count, auto id) {
        for (auto it = begin; it != end; ++it, ++count)
            if (out.size() < collab::Presence::maxSelectionIds)
                out.push_back (id (*it));
    };
    std::int64_t count = 0;
    take (selection.regions().begin(), selection.regions().end(), p.regions, count, [] (const Id& id) { return id; });
    take (piano.selection.begin(), piano.selection.end(), p.notes, count, [] (const commands::NoteRef& n) { return n.noteId; });
    take (pointSelection.points.begin(), pointSelection.points.end(), p.points, count, [] (const Id& id) { return id; });
    p.selectionCount = count;
    localPresence = p;
    syncClient->setPresence (p);
}

std::vector<Studio::MissingPlugin> Studio::missingPluginList() const
{
    std::vector<MissingPlugin> out;
    auto check = [&] (const model::PluginInstance& p) {
        const auto proc = eng->processor (p.id);
        if (proc == nullptr || ! proc->isPlaceholder())
            return;
        MissingPlugin m { p.name.empty() ? p.pluginUid : p.name, {} };
        if (! p.addedBy.empty())
            m.addedBy = syncClient != nullptr ? syncClient->displayName (p.addedBy) : p.addedBy;
        if (std::none_of (out.begin(), out.end(), [&] (const MissingPlugin& o) { return o.name == m.name && o.addedBy == m.addedBy; }))
            out.push_back (std::move (m));
    };
    for (const auto& [id, t] : model().tracks)
    {
        if (t->strip.instrument)
            check (*t->strip.instrument);
        for (const auto& p : t->strip.inserts)
            check (p);
    }
    std::sort (out.begin(), out.end(), [] (const MissingPlugin& a, const MissingPlugin& b) { return std::tie (a.name, a.addedBy) < std::tie (b.name, b.addedBy); });
    return out;
}

void Studio::noticeMissingPlugins()
{
    // Plug-ins a collaborator just added that aren't installed here: say so once each (F-COL-20).
    std::vector<MissingPlugin> fresh;
    auto check = [&] (const model::PluginInstance& p) {
        const auto proc = eng->processor (p.id);
        if (proc == nullptr || ! proc->isPlaceholder() || ! missingSeen.insert (p.id).second)
            return;
        fresh.push_back ({ p.name.empty() ? p.pluginUid : p.name, p.addedBy.empty() || syncClient == nullptr ? p.addedBy : syncClient->displayName (p.addedBy) });
    };
    for (const auto& [id, t] : model().tracks)
    {
        if (t->strip.instrument)
            check (*t->strip.instrument);
        for (const auto& p : t->strip.inserts)
            check (p);
    }
    if (fresh.empty())
        return;
    const auto& first = fresh.front();
    status = juce::String::fromUTF8 (first.name.c_str()) + (first.addedBy.empty() ? "" : " (added by " + juce::String::fromUTF8 (first.addedBy.c_str()) + ")")
             + (fresh.size() > 1 ? " and " + juce::String (fresh.size() - 1) + " more aren't installed here." : juce::String (" isn't installed here."))
             + " The track plays without it.";
    if (onMissingPlugins)
        onMissingPlugins (fresh);
}

void Studio::remoteChanged()
{
    // Collaborators' changes: they're in the document now, so it needs saving, but they
    // aren't this user's to undo or to recover (the relay keeps them).
    selection.prune (model());
    if (! editorRegion.empty() && model().region (editorRegion) == nullptr)
        editorRegion.clear();
    unsaved = true;
    everEdited = true;
    if (previewModel != nullptr && sess->model().version (previewId) == nullptr)
        returnToCurrent();
    edited();
    noticeMissingPlugins();
    collabNotices();
}

bool Studio::shareProject (std::string* error)
{
    auto fail = [error] (const std::string& m) {
        if (error != nullptr)
            *error = m;
        return false;
    };
    if (! signedIn())
        return fail ("Sign in to share this project.");
    if (shared() && ! sameRelay (sess->relayUrl(), relayAccount.relayUrl))
        return fail ("This project is shared on " + sess->relayUrl() + ". Sign in to that relay to sync it.");
    const auto info = withRelay ([&] (collab::RelayApi& api) {
        auto r = api.shareProject (model().info.id, model().info.name);
        if (! r && error != nullptr)
            *error = api.error();
        return r;
    });
    if (! info)
        return false;
    if (! shared() && ! check (sess->setRelayUrl (relayAccount.relayUrl)))
        return fail (sess->lastError());
    save();
    startSync();
    status = "Shared on " + juce::String (relayAccount.relayUrl) + ".";
    sendChangeMessage();
    return true;
}

bool Studio::joinProject (const std::string& inviteLink, std::string* error, fs::path parent)
{
    auto fail = [error] (const std::string& m) {
        if (error != nullptr)
            *error = m;
        return false;
    };
    const auto parsed = collab::parseInviteLink (inviteLink);
    if (! parsed)
        return fail ("That isn't a Weft invite link.");
    if (! signedIn())
        return fail ("Sign in to join a project.");
    if (! sameRelay (parsed->first, relayAccount.relayUrl))
        return fail ("This invite is for " + parsed->first + ". Sign in to that relay first.");
    std::string problem;
    const auto bytes = withRelay ([&] (collab::RelayApi& api) -> std::optional<doc::Bytes> {
        const auto joined = api.acceptInvite (parsed->second);
        if (! joined)
        {
            problem = api.error();
            return std::nullopt;
        }
        auto b = api.fetchDocument (joined->projectId);
        if (! b)
            problem = api.error();
        return b;
    });
    if (! bytes)
        return fail (problem.empty() ? "Couldn't download the project." : problem);
    auto document = doc::Document::load (*bytes);
    if (! document)
        return fail ("The project from the relay couldn't be read.");
    if (const auto newer = project::checkSchemaVersion (*document))
        return fail (*newer);
    // The Owner's copy reaches the relay just after sharing; until then there's nothing to open.
    if (const auto url = document->get (doc::path ({ "collab", "relayUrl" })); ! url || ! url->isString())
        return fail ("The project is still being uploaded. Try again in a moment.");
    if (parent.empty())
        parent = projectParent;
    auto name = model::Projector::project (*document).info.name;
    if (name.empty())
        name = "Shared Project";
    const auto projectId = model::Projector::project (*document).info.id;
    fs::path dir;
    for (int n = 1;; ++n)
    {
        dir = project::ProjectFolder::folderFor (parent, n == 1 ? name : name + " " + std::to_string (n));
        if (! fs::exists (dir))
            break;
        // Joined before: open that copy; the sync brings it up to date.
        project::ProjectFolder existing (dir);
        if (auto opened = existing.open(); opened.document && model::Projector::project (*opened.document).info.id == projectId)
        {
            const auto result = openProject (dir, Recovery::recover);
            return result.ok || fail (result.error);
        }
    }
    project::ProjectFolder created (dir);
    if (! created.create (error) || ! created.save (*document, {}, error))
        return false;
    const auto result = openProject (dir, Recovery::discard);
    if (! result.ok)
        return fail (result.error);
    status = "Joined \"" + juce::String (name) + "\".";
    sendChangeMessage();
    return true;
}

juce::String Studio::syncStatus() const
{
    if (! shared())
        return inSharedFolder() ? "Shared folder" : "Local only";
    if (! signedIn())
        return juce::String::fromUTF8 ("Signed out \xe2\x80\x93 sign in to sync");
    if (syncClient == nullptr)
        return "On another relay";
    auto text = juce::String::fromUTF8 (syncClient->statusText().c_str());
    if (mediaSyncer != nullptr)
        if (const auto transfers = mediaSyncer->statusText(); ! transfers.empty())
            text << juce::String::fromUTF8 (" \xc2\xb7 ") << juce::String::fromUTF8 (transfers.c_str());
    return text;
}

juce::String Studio::syncDetails() const
{
    if (! shared())
        return inSharedFolder() ? "This project is in a shared folder: " + juce::String (project::utf8 (folder->root()))
                                      + "\nYour sync app copies it to your collaborators, and Weft merges their changes every few seconds."
                                : juce::String ("This project is only on this computer. Share it to work on it with others.");
    juce::String text = "Relay: " + juce::String (sess->relayUrl());
    if (syncClient != nullptr)
    {
        const auto at = syncClient->lastSyncedAt();
        text << "\nLast synced: " << (at > 0 ? juce::Time (at).toString (true, true) : juce::String ("not yet"));
        text << "\nChanges pending: " << static_cast<int> (syncClient->pendingCount());
        if (! syncClient->role().empty())
            text << "\nYour role: " << collab::roleLabel (syncClient->role());
    }
    if (signedIn())
        text << "\nSigned in as " << relayAccount.displayName << " (" << relayAccount.email << ")";
    return text;
}

void Studio::setOnline (bool online)
{
    wantOnline = online;
    if (syncClient != nullptr)
        syncClient->setOnline (online);
    sendChangeMessage();
}

bool Studio::tapTempo (double nowMs)
{
    // L-TMP-9, D-W11-4: taps more than 2 s apart start again; the tempo in effect at the playhead
    // follows the average of the last four gaps, and one run of taps is one undo step.
    if (! taps.empty() && nowMs - taps.back() > 2000.0)
        taps.clear();
    if (! taps.empty() && nowMs <= taps.back())
        return false;
    taps.push_back (nowMs);
    if (taps.size() > 5)
        taps.erase (taps.begin());
    if (taps.size() < 2)
    {
        tapCommitted = false;
        setStatus ("Tap again to set the tempo.");
        return true;
    }
    const auto bpm = 60000.0 * static_cast<double> (taps.size() - 1) / (taps.back() - taps.front());
    auto& s = session();
    if (tapCommitted && s.undoName() == "Tap Tempo")
        s.undo();
    s.beginGesture ("Tap Tempo");
    const auto result = s.setTempoAt (playhead(), bpm);
    s.endGesture();
    tapCommitted = static_cast<bool> (result);
    if (! result)
    {
        setStatus (juce::String (result.error));
        return false;
    }
    edited();
    setStatus ("Tempo " + juce::String (model().tempo.bpmAt (playhead()), 2) + " BPM from " + juce::String (static_cast<int> (taps.size())) + " taps.");
    return true;
}

bool Studio::changesDocument (Cmd command)
{
    static constexpr Cmd editing[] = {
        Cmd::record, Cmd::newTrack, Cmd::undo, Cmd::redo, Cmd::cut, Cmd::paste, Cmd::deleteSelection, Cmd::split,
        Cmd::join, Cmd::muteRegions, Cmd::nudgeLeft, Cmd::nudgeRight, Cmd::repeat, Cmd::crossfade, Cmd::loopRegion,
        Cmd::addMarker, Cmd::muteNotes, Cmd::quantize, Cmd::transposeUp, Cmd::transposeDown, Cmd::octaveUp,
        Cmd::octaveDown, Cmd::notesLeft, Cmd::notesRight, Cmd::applyQuantize, Cmd::deleteTrack, Cmd::newBus,
        Cmd::addComment, Cmd::saveVersion, Cmd::lockTrack, Cmd::importAudio, Cmd::duplicateTrack,
        Cmd::duplicateTrackWithRegions, Cmd::moveTrackUp, Cmd::moveTrackDown, Cmd::discardRecording,
        Cmd::reverseRegions, Cmd::tapTempo, Cmd::showHiddenTracks, Cmd::muteTrack, Cmd::newAudioTrack, Cmd::newInstrumentTrack,
        Cmd::renameMarker, Cmd::deleteMarker, Cmd::searchPlugins, Cmd::deleteUnusedTracks, Cmd::newTrackNextInput,
        Cmd::legato, Cmd::removeOverlaps, Cmd::noteStartToPlayhead, Cmd::noteEndToPlayhead, Cmd::lengthenNotes, Cmd::shortenNotes, Cmd::scaleQuantize,
        Cmd::cycleForward, Cmd::cycleBack, Cmd::doubleCycle, Cmd::halveCycle, Cmd::slipLeft, Cmd::slipRight, Cmd::shuffleLeft, Cmd::shuffleRight,
        Cmd::toggleAllTracksOff, Cmd::unmuteAll, Cmd::monitorTrack, Cmd::pasteChannelStrip, Cmd::moveToPlayhead, Cmd::moveToPlayheadSelectNext,
        Cmd::regionStartToPlayhead, Cmd::regionEndToPlayhead, Cmd::splitAtCycle, Cmd::automationAllOff, Cmd::automationAllRead,
        Cmd::automationAllTouch, Cmd::automationAllLatch, Cmd::automationOffRead, Cmd::automationLatchRead, Cmd::markerForSelection,
        Cmd::cycleToMarker, Cmd::cycleToPreviousMarker, Cmd::cycleToNextMarker, Cmd::previousMarkerSetLocators, Cmd::nextMarkerSetLocators,
        Cmd::applyRegionParameters, Cmd::applyRegionParametersExceptDelay,
    };
    return std::find (std::begin (editing), std::end (editing), command) != std::end (editing);
}

juce::String Studio::readOnlyReason() const
{
    if (sess->access() == commands::Session::Access::full)
        return {};
    return sess->accessMessage().empty() ? juce::String ("This project is read-only here.") : juce::String (sess->accessMessage());
}
} // namespace weft::ui
