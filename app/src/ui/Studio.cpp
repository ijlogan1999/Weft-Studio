#include "Studio.h"

#include "Theme.h"

#include "weft/engine/dsp/BuiltinEffects.h"
#include "weft/media/Blobs.h"
#include "weft/media/Decoder.h"

#include <cmath>
#include <fstream>
#include <iterator>

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
        case Cmd::toggleAutomation: case Cmd::bounce: case Cmd::newProject: case Cmd::openProject: case Cmd::saveAs:
        case Cmd::projectSettings: case Cmd::audioSettings: case Cmd::shareProject: case Cmd::joinProject: case Cmd::signIn:
        case Cmd::signOut: case Cmd::goOffline: case Cmd::addComment: case Cmd::saveVersion: case Cmd::lockTrack:
        case Cmd::importAudio: case Cmd::quit: case Cmd::duplicateTrack: case Cmd::duplicateTrackWithRegions: case Cmd::moveTrackUp:
        case Cmd::moveTrackDown:
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
            if (! audioOk)
            {
                setStatus ("Recording needs an audio device: choose one in Audio & MIDI Settings.");
                return false;
            }
            const auto result = eng->record (playhead());
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
        case Cmd::pluginManager:
        case Cmd::deleteTrack:
        case Cmd::bounce:
        case Cmd::newProject:
        case Cmd::openProject:
        case Cmd::saveAs:
        case Cmd::projectSettings:
        case Cmd::audioSettings:
        case Cmd::shareProject:
        case Cmd::joinProject:
        case Cmd::signIn:
        case Cmd::addComment:
        case Cmd::saveVersion:
        case Cmd::importAudio:
        case Cmd::quit:
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

// Projects ------------------------------------------------------------------------------

bool Studio::save()
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
    recent.add (folder->root());
    setStatus ("Saved.");
    return true;
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
    auto projectFolder = std::make_unique<project::ProjectFolder> (dir);
    if (! projectFolder->create (error))
        return false;
    auto created = commands::Session::createNew (opts, "local", error);
    if (! created || ! projectFolder->save (created->document(), {}, error))
        return false;
    adopt (std::move (projectFolder), std::make_unique<commands::Session> (std::move (*created)), scratchProject);
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
    auto projectFolder = std::make_unique<project::ProjectFolder> (root);
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
    folder = std::make_unique<project::ProjectFolder> (newRoot);
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
    eng->setSettings (engine::TransportSettings::fromDefaults (model().info.metronomeDefaults));
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
    project::writeFileAtomic (folder->root() / "Cache" / "view.txt", { p, text.size() });
}

void Studio::loadViewState()
{
    const auto bytes = project::readFile (folder->root() / "Cache" / "view.txt");
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
        return "Local only";
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
        return "This project is only on this computer. Share it to work on it with others.";
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

bool Studio::changesDocument (Cmd command)
{
    switch (command)
    {
        case Cmd::record: case Cmd::newTrack: case Cmd::undo: case Cmd::redo: case Cmd::cut: case Cmd::paste:
        case Cmd::deleteSelection: case Cmd::split: case Cmd::join: case Cmd::muteRegions: case Cmd::nudgeLeft:
        case Cmd::nudgeRight: case Cmd::repeat: case Cmd::crossfade: case Cmd::loopRegion: case Cmd::addMarker:
        case Cmd::muteNotes: case Cmd::quantize: case Cmd::transposeUp: case Cmd::transposeDown: case Cmd::octaveUp:
        case Cmd::octaveDown: case Cmd::notesLeft: case Cmd::notesRight: case Cmd::applyQuantize: case Cmd::deleteTrack:
        case Cmd::newBus: case Cmd::addComment: case Cmd::saveVersion: case Cmd::lockTrack: case Cmd::importAudio:
        case Cmd::duplicateTrack: case Cmd::duplicateTrackWithRegions: case Cmd::moveTrackUp: case Cmd::moveTrackDown:
            return true;
        default:
            return false;
    }
}

juce::String Studio::readOnlyReason() const
{
    if (sess->access() == commands::Session::Access::full)
        return {};
    return sess->accessMessage().empty() ? juce::String ("This project is read-only here.") : juce::String (sess->accessMessage());
}
} // namespace weft::ui
