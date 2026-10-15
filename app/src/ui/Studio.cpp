#include "Studio.h"

#include "Theme.h"

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
    syncEngine();
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
    eng->collectGarbage();
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

bool Studio::perform (Cmd command)
{
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
            if (uiCommand)
                uiCommand (command);
            break;
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
} // namespace weft::ui
