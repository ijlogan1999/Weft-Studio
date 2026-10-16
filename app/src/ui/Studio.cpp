#include "Studio.h"

#include "Theme.h"

#include "weft/engine/dsp/BuiltinEffects.h"
#include "weft/media/Blobs.h"
#include "weft/media/Decoder.h"

#include <cmath>

namespace weft::ui
{
namespace fs = std::filesystem;

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
    }
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

Studio::Studio (std::unique_ptr<engine::AudioDevice> device, const fs::path& projectParent, engine::DeviceConfig config)
    : audioDevice (std::move (device))
{
    eng = std::make_unique<engine::Engine> (pool, rack);
    live = std::make_unique<engine::LiveAutomation> (*eng);
    catalog = std::make_unique<engine::PluginCatalog> (settingsFolder() / "plugins.json");
    catalog->load();
    host = std::make_unique<engine::Vst3Host> (*catalog);
    pluginFolders = engine::PluginCatalog::defaultFolders();
    eng->setPluginFactory ([this] (const model::PluginInstance& p, bool instrument) { return host->create (p, instrument); });
    scannerPath = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                      .getSiblingFile (juce::String ("weft-plugin-scanner") + (juce::SystemStats::getOperatingSystemType() & juce::SystemStats::Windows ? ".exe" : ""))
                      .getFullPathName()
                      .toStdString();
    fs::path dir;
    for (int n = 1;; ++n)
    {
        dir = project::ProjectFolder::folderFor (projectParent, n == 1 ? "Untitled" : "Untitled " + std::to_string (n));
        if (! fs::exists (dir))
            break;
    }
    folder = std::make_unique<project::ProjectFolder> (dir);
    std::string error;
    folder->create (&error);
    auto created = commands::Session::createNew ({}, "local", &error);
    jassert (created.has_value());
    sess = std::make_unique<commands::Session> (std::move (*created));
    sess->onLocalChange = [this] (const doc::Bytes& change, std::int64_t time) {
        folder->log().append (change, time);
        unsaved = true;
    };
    folder->save (sess->document());
    local.selectedTrack = model().trackOrder.empty() ? Id {} : model().trackOrder.front();
    eng->setProjectFolder (folder->root());
    auto settings = engine::TransportSettings::fromDefaults (model().info.metronomeDefaults);
    eng->setSettings (settings);
    audioOk = audioDevice != nullptr && audioDevice->open (config) && audioDevice->start (*eng);
    if (! audioOk)
        status = "No audio device: Weft is running without sound.";
    syncEngine();
}

Studio::~Studio()
{
    if (audioDevice != nullptr)
    {
        audioDevice->stop();
        audioDevice->close();
    }
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

void Studio::selectTrack (const Id& trackId)
{
    local.selectedTrack = trackId;
    eng->setSelectedTrack (trackId);
    sendChangeMessage();
}

void Studio::toggleArm (const Id& trackId)
{
    local.toggleArm (model(), trackId);
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
    locatedFrame = eng->deviceFrames();
    sendChangeMessage();
}

bool Studio::playing() const { return eng->state() != engine::TransportState::stopped; }
bool Studio::recording() const { return eng->state() == engine::TransportState::recording || eng->state() == engine::TransportState::countIn; }

void Studio::tick()
{
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
            edited();
        }
}

// Commands ---------------------------------------------------------------------------

namespace
{
void toggleBottom (PanelLayout& l, BottomTab tab) { l.bottom = l.bottom == tab ? BottomTab::none : tab; }
} // namespace

void Studio::zoomToFit()
{
    auto span = selection.span (model());
    if (! span)
        if (const auto all = arrange::projectSpan (model()))
            span = std::pair { 0.0, all->second }; // the whole project, from its start (F-ARR-21)
    if (! span)
        span = std::pair { 0.0, 8.0 * 3840.0 };
    view.fit (span->first, span->second, timelineWidth, 0.9);
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
        {
            if (! needNotes())
                return false;
            const auto step = piano.grid > 0 ? piano.grid : Ticks (240);
            return check (s.moveNotes (notes, command == Cmd::notesRight ? step : -step, 0));
        }
        case Cmd::escape:
            if (piano.selection.empty())
                return std::nullopt;
            piano.selection.clear();
            sendChangeMessage();
            return true;
        case Cmd::playStop: case Cmd::record: case Cmd::cycle: case Cmd::metronome: case Cmd::goToStart: case Cmd::goToEnd:
        case Cmd::previousBar: case Cmd::nextBar: case Cmd::autopunch: case Cmd::followPlayhead: case Cmd::save: case Cmd::newTrack:
        case Cmd::undo: case Cmd::redo: case Cmd::split: case Cmd::join: case Cmd::muteRegions: case Cmd::nudgeLeft: case Cmd::nudgeRight:
        case Cmd::repeat: case Cmd::crossfade: case Cmd::loopRegion: case Cmd::setCycleToSelection: case Cmd::zoomToFit: case Cmd::zoomIn:
        case Cmd::zoomOut: case Cmd::toolMenu: case Cmd::toggleInspector: case Cmd::toggleSidePanel: case Cmd::toggleMixer:
        case Cmd::togglePianoRoll: case Cmd::toggleAudioEditor: case Cmd::toggleGlobalTracks: case Cmd::addMarker: case Cmd::previousMarker:
        case Cmd::nextMarker: case Cmd::marker1: case Cmd::marker2: case Cmd::marker3: case Cmd::marker4: case Cmd::marker5: case Cmd::marker6:
        case Cmd::marker7: case Cmd::marker8: case Cmd::marker9: case Cmd::snapToggle: case Cmd::keyboardMidi: case Cmd::toggleTheme:
        case Cmd::about: case Cmd::applyQuantize: case Cmd::deleteTrack: case Cmd::newBus: case Cmd::pluginManager:
        case Cmd::toggleAutomation:
            break;
    }
    return std::nullopt;
}

bool Studio::perform (Cmd command)
{
    endPluginGesture();
    if (notesFocused())
        if (const auto handled = performNotes (command))
            return *handled;
    auto& s = *sess;
    const auto& m = model();
    const auto ids = selection.ids();
    const auto grid = activeSnap().kind == arrange::Snap::Kind::off ? arrange::Snap::ticks (240) : activeSnap();
    switch (command)
    {
        case Cmd::playStop:
            if (playing())
                eng->stop();
            else
                eng->play (playhead());
            locatedAt.reset();
            break;
        case Cmd::record:
        {
            if (recording())
            {
                eng->stopRecording();
                break;
            }
            const auto result = eng->record (playhead());
            locatedAt.reset();
            if (! result.message.empty())
                setStatus (juce::String (result.message));
            break;
        }
        case Cmd::cycle: return check (s.setCycle (! m.info.cycle.enabled, m.info.cycle.start, m.info.cycle.end));
        case Cmd::autopunch: return check (s.setPunch (! m.info.punch.enabled, m.info.punch.start, m.info.punch.end));
        case Cmd::metronome:
        {
            auto settings = eng->settings();
            settings.metronomeOn = ! settings.metronomeOn;
            eng->setSettings (settings);
            break;
        }
        case Cmd::goToStart: locate (0); break;
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
        case Cmd::muteRegions: return ! ids.empty() && check (s.toggleRegionsMuted (ids));
        case Cmd::nudgeLeft:
        case Cmd::nudgeRight:
        {
            if (ids.empty())
                return false;
            const auto anchor = selection.span (m)->first;
            const auto from = static_cast<Ticks> (anchor);
            const auto delta = arrange::gridStep (from, command == Cmd::nudgeRight ? 1 : -1, grid, m.signatures, view.pixelsPerTick) - from;
            return check (s.nudgeRegions (ids, delta));
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
                    const auto end = static_cast<Ticks> (std::ceil (m.regionEndTick (*r)));
                    if (! check (s.setRegionLoop (id, r->loop ? std::nullopt : std::optional<Ticks> (end + (end - r->start) * 3))))
                        return false;
                }
            break;
        case Cmd::setCycleToSelection: return check (s.setCycleToRegions (ids));
        case Cmd::zoomToFit: zoomToFit(); break;
        case Cmd::zoomIn: view.zoomAround (1.5, view.tickToX (static_cast<double> (playhead()))); break;
        case Cmd::zoomOut: view.zoomAround (1.0 / 1.5, view.tickToX (static_cast<double> (playhead()))); break;
        case Cmd::escape: tool = Tool::pointer; break;
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
        case Cmd::previousMarker:
        case Cmd::nextMarker:
        {
            const auto to = command == Cmd::nextMarker ? arrange::nextMarker (m, playhead()) : arrange::previousMarker (m, playhead());
            if (! to)
                return false;
            locate (*to);
            break;
        }
        case Cmd::snapToggle: snapEnabled = ! snapEnabled; break;
        case Cmd::keyboardMidi:
        {
            std::vector<engine::KeyboardMidi::Message> out;
            keys.toggle (out);
            for (const auto& msg : out)
                eng->sendMidi (msg.status, msg.data1, msg.data2);
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
        case Cmd::pluginManager:
        case Cmd::deleteTrack:
            if (uiCommand)
                uiCommand (command);
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
        case Cmd::muteNotes:
        case Cmd::quantize:
            setStatus ("Click in the piano roll to edit notes.");
            return false;
        case Cmd::transposeUp:
        case Cmd::transposeDown:
        case Cmd::octaveUp:
        case Cmd::octaveDown:
        case Cmd::notesLeft:
        case Cmd::notesRight:
            return false; // arrows do nothing in the arrange view
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
        {
            const auto to = arrange::markerNumber (m, static_cast<int> (command) - static_cast<int> (Cmd::marker1) + 1);
            if (! to)
                return false;
            locate (*to);
            break;
        }
    }
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
        eng->sendMidi (msg.status, msg.data1, msg.data2);
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

bool Studio::save()
{
    std::string error;
    if (! folder->save (sess->document(), {}, &error))
    {
        setStatus ("Couldn't save: " + juce::String (error));
        return false;
    }
    unsaved = false;
    setStatus ("Saved.");
    return true;
}

const std::vector<float>& Studio::peaks (const std::string& hash)
{
    if (const auto it = peakCache.find (hash); it != peakCache.end())
        return it->second;
    auto& out = peakCache[hash];
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
        out.assign (static_cast<std::size_t> ((frames + peakFrames - 1) / peakFrames), 0.0f);
        for (std::int64_t start = 0; start < frames; start += 65536)
        {
            const auto n = reader.read (start, std::min<std::int64_t> (65536, frames - start), ptrs.data());
            for (const auto& b : buf)
                for (std::int64_t i = 0; i < n; ++i)
                {
                    auto& p = out[static_cast<std::size_t> ((start + i) / peakFrames)];
                    p = std::max (p, std::abs (b[static_cast<std::size_t> (i)]));
                }
        }
        return out;
    }
    // Media held only in memory (tests, generated audio).
    if (const auto source = pool.find (hash); source && source->numBlocks() > 0)
    {
        out.assign (static_cast<std::size_t> ((source->numFrames() + peakFrames - 1) / peakFrames), 0.0f);
        for (std::int64_t b = 0; b < source->numBlocks(); ++b)
            for (int ch = 0; ch < source->numChannels(); ++ch)
                if (const float* data = source->block (ch, b))
                {
                    const auto first = b * engine::MediaSource::blockFrames;
                    const auto n = std::min (engine::MediaSource::blockFrames, source->numFrames() - first);
                    for (std::int64_t i = 0; i < n; ++i)
                    {
                        auto& p = out[static_cast<std::size_t> ((first + i) / peakFrames)];
                        p = std::max (p, std::abs (data[i]));
                    }
                }
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
    const auto id = sess->addInsert (trackId, choice.plugin, index);
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
    const auto id = sess->replaceInsert (trackId, pluginId, choice.plugin);
    if (! id)
        setStatus (juce::String (sess->lastError()));
    edited();
    if (id && openPluginEditor)
        openPluginEditor (*id);
    return id.has_value();
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
} // namespace weft::ui
