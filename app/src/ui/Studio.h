#pragma once

// Everything the arrange window works on: the project (session, folder, media, engine)
// and this user's view state (selection, snap, zoom, tool, panels). Components read it
// and call perform(); it broadcasts a change after every edit. Message thread only.

#include "Commands.h"

#include "weft/arrange/Grid.h"
#include "weft/arrange/Selection.h"
#include "weft/commands/LocalView.h"
#include "weft/commands/Session.h"
#include "weft/engine/arrange/Import.h"
#include "weft/engine/arrange/OfflineRender.h"
#include "weft/engine/devices/AudioDevice.h"
#include "weft/engine/midi/KeyboardMidi.h"
#include "weft/engine/transport/Engine.h"
#include "weft/project/ProjectFolder.h"

#include <juce_events/juce_events.h>

#include <filesystem>
#include <map>
#include <set>
#include <memory>

namespace weft::ui
{
enum class Tool
{
    pointer,
    pencil,
    scissors,
    glue,
    eraser,
    mute
};
juce::String toolName (Tool t);

enum class BottomTab
{
    none,
    mixer,
    pianoRoll,
    audioEditor
};

/** Panel visibility and sizes, kept per user across restarts (F-UI-1). */
struct PanelLayout
{
    bool inspector = true;
    bool sidePanel = false;
    bool globalTracks = true;
    BottomTab bottom = BottomTab::none;
    int inspectorWidth = 250;
    int sidePanelWidth = 300;
    int bottomHeight = 280;
    bool darkTheme = true;

    juce::String toString() const;
    static PanelLayout fromString (const juce::String& text);
    friend bool operator== (const PanelLayout&, const PanelLayout&) = default;
};

/** The piano roll's view state (local, F-PRL): its selection, clipboard, grid and quantize settings. */
struct PianoRollState
{
    std::set<commands::NoteRef> selection;
    std::optional<std::pair<Id, Id>> point; // a selected controller point (region, event): Delete removes it
    commands::NoteClipboard clipboard;
    bool focus = false;        // note commands go to the piano roll (it was clicked last)
    Ticks grid = 240;          // snap; 0 = off
    Ticks quantizeGrid = 240;
    double strength = 100.0;   // percent
    double swing = 0.0;        // percent
    bool audition = true;
    bool follow = true;
    Ticks lastLength = 240;    // the pencil draws notes like the last one made or resized
    int lastVelocity = 100;
};

/** Track lane heights: local view state (F-TRK-12). */
inline constexpr int minTrackHeight = 24, maxTrackHeight = 400, defaultTrackHeight = 72;

class Studio : public juce::ChangeBroadcaster
{
public:
    /** A new, saved project named "Untitled" (or "Untitled 2"...) under projectParent. */
    Studio (std::unique_ptr<engine::AudioDevice> device, const std::filesystem::path& projectParent, engine::DeviceConfig config = {});
    ~Studio() override;

    commands::Session& session() noexcept { return *sess; }
    const model::ProjectModel& model() const noexcept { return sess->model(); }
    engine::Engine& engine() noexcept { return *eng; }
    engine::MediaPool& media() noexcept { return pool; }
    engine::AudioDevice& device() noexcept { return *audioDevice; }
    const std::filesystem::path& projectRoot() const noexcept { return folder->root(); }
    std::filesystem::path mediaDir() const { return folder->mediaDir(); }
    bool audioRunning() const noexcept { return audioOk; }
    media::DecoderSet decoders;

    // View state ---------------------------------------------------------------
    arrange::Selection selection;
    arrange::Snap snap = arrange::Snap::smart();
    bool snapEnabled = true;
    arrange::TimelineView view;
    Tool tool = Tool::pointer;
    PanelLayout layout;
    bool follow = true;
    Id editorRegion; // the region the audio editor shows
    commands::RegionClipboard clipboard;
    juce::String status;
    PianoRollState piano;

    /** The MIDI regions the piano roll shows: the opened one plus the selected MIDI regions on its track (F-PRL-1). */
    std::vector<Id> pianoRollRegions() const;
    bool notesFocused() const noexcept { return piano.focus && layout.bottom == BottomTab::pianoRoll; }
    /** Plays a note on its track's instrument when audition is on (F-PRL-11): its length, at most 1 s. */
    void auditionNote (const Id& regionId, int pitch, int velocity, int channel = 1, Ticks length = 960);

    /** The grid in effect (snap off when disabled). */
    arrange::Snap activeSnap() const { return snapEnabled ? snap : arrange::Snap::off(); }
    Ticks snapped (Ticks t, bool bypass = false) const { return arrange::snapTick (t, activeSnap(), model().signatures, view.pixelsPerTick, bypass); }

    const Id& selectedTrack() const noexcept { return local.selectedTrack; }
    void selectTrack (const Id& trackId);
    const commands::LocalView& localView() const noexcept { return local; }
    void toggleArm (const Id& trackId);
    void toggleSolo (const Id& trackId, bool exclusive = false);

    int trackHeight (const Id& trackId) const;
    void setTrackHeight (const Id& trackId, int height);
    void setAllTrackHeights (int height);

    // Transport ------------------------------------------------------------------
    Ticks playhead() const;
    void locate (Ticks t);
    bool playing() const;
    bool recording() const;

    // Commands ---------------------------------------------------------------------
    /** Runs a command; false when it didn't apply (the status line says why). */
    bool perform (Cmd command);
    /** Keyboard MIDI mode gets the key first (AT-REC-20). True when it used the key. */
    bool keyboardMidiKey (const juce::KeyPress& key, bool down);
    bool keyboardMidiOn() const noexcept { return keys.enabled(); }

    /** Drop of files on the arrange view (W-3, F-ARR-26/27). */
    engine::ImportOutcome importFiles (const std::vector<std::filesystem::path>& files, engine::DropTarget target);
    /** Plays a file in the browser (F-LIB-1). False when it can't be read. */
    bool previewFile (const std::filesystem::path& file);

    /** Call after any session change made directly: syncs the engine and tells the UI. */
    void edited();
    void setStatus (const juce::String& message);
    /** Applies a command result: on failure shows the session's message. */
    bool check (const commands::EditResult& result);

    bool save();
    bool dirty() const noexcept { return unsaved; }

    /** Peak overview of a media file: max |sample| per `peakFrames` frames, all channels (cached). */
    static constexpr int peakFrames = 256;
    const std::vector<float>& peaks (const std::string& hash);

    /** Commands that need the window (tool menu, New Track dialog, About). */
    std::function<void (Cmd)> uiCommand;
    /** Width of the visible timeline in pixels, for zoom to fit. */
    double timelineWidth = 800.0;

    /** Polls the engine (recordings, garbage) and the playhead; the window calls it on a timer. */
    void tick();

private:
    void syncEngine();
    std::optional<bool> performNotes (Cmd command);
    void zoomToFit();

    std::unique_ptr<engine::AudioDevice> audioDevice;
    engine::MediaPool pool;
    engine::InstrumentRack rack;
    std::unique_ptr<engine::Engine> eng;
    std::unique_ptr<project::ProjectFolder> folder;
    std::unique_ptr<commands::Session> sess;
    commands::LocalView local;
    engine::KeyboardMidi keys;
    std::map<Id, int> heights;
    std::map<std::string, std::vector<float>> peakCache;
    std::optional<Ticks> locatedAt; // shown until the audio thread has taken the locate
    std::int64_t locatedFrame = -1;
    bool audioOk = false;
    bool unsaved = false;
};
} // namespace weft::ui
