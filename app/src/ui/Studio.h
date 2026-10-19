#pragma once

// Everything the arrange window works on: the project (session, folder, media, engine)
// and this user's view state (selection, snap, zoom, tool, panels). Components read it
// and call perform(); it broadcasts a change after every edit. Message thread only.

#include "Commands.h"
#include "Waveform.h"

#include "weft/arrange/Grid.h"
#include "weft/arrange/Selection.h"
#include "weft/collab/MediaSync.h"
#include "weft/collab/Relay.h"
#include "weft/collab/SyncClient.h"
#include "weft/commands/Collab.h"
#include "weft/commands/LocalView.h"
#include "weft/commands/Session.h"
#include "weft/engine/arrange/Import.h"
#include "weft/engine/arrange/OfflineRender.h"
#include "weft/engine/devices/AudioDevice.h"
#include "weft/engine/devices/NullDevice.h"
#include "weft/engine/midi/KeyboardMidi.h"
#include "weft/engine/midi/MidiInputs.h"
#include "weft/engine/plugins/PluginCatalog.h"
#include "weft/engine/plugins/Vst3Host.h"
#include "weft/engine/render/Bounce.h"
#include "weft/engine/transport/Engine.h"
#include "weft/engine/transport/LiveAutomation.h"
#include "weft/model/Automation.h"
#include "weft/project/ProjectFiles.h"
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
    int inspectorWidth = 240;
    int sidePanelWidth = 304;
    int bottomHeight = 288;
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
inline constexpr int minTrackHeight = 24, maxTrackHeight = 400, defaultTrackHeight = 56;

class Studio : public juce::ChangeBroadcaster
{
public:
    /** A new, saved project named "Untitled" (or "Untitled 2"...) under projectParent. */
    Studio (std::unique_ptr<engine::AudioDevice> device, const std::filesystem::path& projectParent, engine::DeviceConfig config = {});
    ~Studio() override;

    commands::Session& session() noexcept { return *sess; }
    /** The project as shown: the current state, or the version being previewed (read-only, 04 §9.5). */
    const model::ProjectModel& model() const noexcept { return previewModel != nullptr ? *previewModel : sess->model(); }
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
    /** Counts edited() calls: plug-in windows tell their own parameter changes from the session's. */
    std::uint64_t edits() const noexcept { return editCount; }
    void setStatus (const juce::String& message);
    /** Applies a command result: on failure shows the session's message. */
    bool check (const commands::EditResult& result);

    // Projects (M9, W-10) -------------------------------------------------------------
    /** Ctrl+S: writes the document atomically, then the media index and this user's view. */
    bool save();
    bool dirty() const noexcept { return unsaved; }
    /** The window title: the project name, with "*" while there are unsaved changes (AT-PRJ-5). */
    juce::String windowTitle() const;

    /** Where new projects go by default (Documents/Weft Studio). */
    const std::filesystem::path& projectsFolder() const noexcept { return projectParent; }
    /** File › New (F-PRJ-1): a saved project `<parent>/<name>.weft` ("<name> 2"... when taken).
        A scratch project (the one Weft starts with) is deleted when it's replaced untouched. */
    bool newProject (const std::filesystem::path& parent, const commands::NewProjectOptions& options, std::string* error = nullptr,
                     bool scratchProject = false);

    enum class Recovery
    {
        ask,     // stop and report it; the caller asks the user and opens again
        recover, // apply the recovery log
        discard  // keep the saved state and archive the log
    };
    struct OpenResult
    {
        bool ok = false;
        std::string error;
        std::filesystem::path root; // the project folder
        /** The project has unsaved changes from a crash and `Recovery::ask` was given: nothing was opened. */
        bool needsRecoveryChoice = false;
        std::int64_t recoveryTimeMs = 0; // when the newest unsaved change was made
        std::vector<std::string> missingMedia;   // hashes no media folder has
        std::vector<std::string> missingPlugins; // names of plug-ins that became placeholders
    };
    /** File › Open (F-PRJ-5): a project folder or its project.automerge. Replaces the current project. */
    OpenResult openProject (const std::filesystem::path& picked, Recovery recovery = Recovery::ask);
    /** Save As (F-PRJ-4): the project moves to `newRoot` (`<name>.weft`) and takes its name. */
    bool saveAs (const std::filesystem::path& newRoot, bool copyMedia, std::string* error = nullptr);
    /** Media regions reference that isn't on disk (W-10 Open step 4). */
    std::vector<std::string> missingMedia() const;
    /** Locate…: copies files under `folder` whose hash matches missing media into Media/. Returns how many it found. */
    int locateMedia (const std::filesystem::path& folder);
    /** Names of the plug-ins in the project that aren't installed (placeholders, F-PLG-15). */
    std::vector<std::string> missingPlugins() const;
    /** File › Open Recent (F-PRJ-6). */
    project::RecentProjects& recentProjects() noexcept { return recent; }
    /** Don't Save: archives the recovery log, so the next open doesn't offer to recover these changes. */
    void discardUnsavedChanges();
    /** A new project nobody has edited or saved: it's deleted when replaced or on quit. */
    bool untouched() const noexcept { return scratch && ! everEdited; }

    // Audio device (W-0) -----------------------------------------------------------------
    /** Replaces the audio device (Audio Setup). False when it can't be opened: Weft then runs in
        No-audio mode, where the transport plays silently against the system clock and recording is off (W-0 step 7). */
    bool setAudioDevice (std::unique_ptr<engine::AudioDevice> device, const engine::DeviceConfig& config);
    /** Test (W-0 step 5): a 1 kHz sine at -18 dBFS for 1 s on outputs 1-2. */
    void playTestTone();
    /** Live MIDI from devices and the computer keyboard; disabled inputs are dropped (F-UI-8). */
    engine::MidiInputRouter& midiInputs() noexcept { return *midiRouter; }

    /** Peak overview of a media file: max |sample| per `peakFrames` frames, all channels (cached). */
    static constexpr int peakFrames = 256;
    /** The min/max/RMS overview a waveform is drawn from (cached). */
    const WaveSummary& waveform (const std::string& hash);
    const std::vector<float>& peaks (const std::string& hash);

    /** Commands that need the window (tool menu, New Track dialog, About). */
    std::function<void (Cmd)> uiCommand;
    /** Width of the visible timeline in pixels, for zoom to fit. */
    double timelineWidth = 800.0;

    /** Polls the engine (recordings, garbage) and the playhead; the window calls it on a timer. */
    void tick();

    // Plug-ins (M6) --------------------------------------------------------------------
    /** The per-user settings folder (the plug-in list lives here). Set before making a Studio to move it. */
    static std::filesystem::path& settingsFolder();
    engine::PluginCatalog& plugins() noexcept { return *catalog; }
    /** The out-of-process scanner (F-PLG-2); next to the app by default. */
    std::filesystem::path scannerPath;
    /** Where Rescan looks (the platform's VST3 folders by default). */
    std::vector<std::filesystem::path> pluginFolders;
    engine::PluginCatalog::ScanResult rescanPlugins (bool retryFailed = false);

    /** An entry of an insert or instrument menu: built-ins first, then VST3 by vendor (W-6). */
    struct PluginChoice
    {
        model::PluginInstance plugin;
        juce::String label, group;
    };
    std::vector<PluginChoice> pluginChoices (bool instruments) const;
    /** Adds an insert and opens its editor (W-6 step 2). */
    std::optional<Id> addPlugin (const Id& trackId, const PluginChoice& choice, std::optional<std::size_t> index = {});
    bool replacePlugin (const Id& trackId, const Id& pluginId, const PluginChoice& choice);
    bool setInstrument (const Id& trackId, const PluginChoice& choice);
    std::shared_ptr<engine::Processor> processor (const Id& pluginId) const { return eng->processor (pluginId); }
    /** The model's instance of a plug-in, and the track it's on. */
    const model::PluginInstance* pluginInstance (const Id& pluginId, Id* trackId = nullptr) const;
    /** Parameters changed in a plug-in window: written to `params` now (one undo step per gesture),
        the state chunk after 2 s without changes or when the window closes (D-DM-5). */
    void pluginParamsChanged (const Id& pluginId, const std::vector<std::pair<std::string, double>>& changes);
    void writePluginState (const Id& pluginId);
    /** Writes state for plug-ins idle for 2 s (all of them when `all`). tick() calls it. */
    void flushPluginStates (bool all = false);
    bool pluginStatePending (const Id& pluginId) const { return pendingState.contains (pluginId); }
    std::function<void (const Id& pluginId)> openPluginEditor;

    // Automation (M7) --------------------------------------------------------------------
    /** Automation view (A): lanes under each track (local view state, F-AUT-1). */
    bool automationView = false;
    static constexpr int automationLaneHeight = 56;
    /** The lanes a track shows in automation view, top to bottom (first: Volume). */
    std::vector<model::AutomationTarget> shownLanes (const Id& trackId) const;
    /** The parameter selector of a shown lane. */
    void setShownLane (const Id& trackId, std::size_t index, const model::AutomationTarget& target);
    /** "+": another lane, showing the next parameter not yet shown. */
    void addShownLane (const Id& trackId);
    /** A track row: the track lane plus its automation lanes in automation view. */
    int rowHeight (const Id& trackId) const;
    /** What a track can automate (AT-AUT-2); plug-in parameters come from the engine's processors. */
    std::vector<automation::TargetChoice> automationTargets (const Id& trackId) const;
    juce::String automationTargetName (const Id& trackId, const model::AutomationTarget& target) const;
    /** Selected automation points of one lane; Delete removes them (F-AUT-3). */
    struct PointSelection
    {
        Id laneId;
        std::set<Id> points;
    };
    PointSelection pointSelection;
    /** Mixer controls (F-AUT-6): while a target is live (W-8) a gesture overrides or writes its
        automation; otherwise it changes the static value. Values are normalized. */
    bool automationLive (const Id& trackId, const model::AutomationTarget& target) const;
    void touchControl (const Id& trackId, const model::AutomationTarget& target, double normalized);
    void releaseControl (const Id& trackId, const model::AutomationTarget& target);
    /** What the engine plays for a target while automation drives it (controls follow it). */
    std::optional<double> automationValue (const Id& trackId, const model::AutomationTarget& target) const;
    engine::LiveAutomation& liveAutomation() noexcept { return *live; }

    // Bounce (M8) ------------------------------------------------------------------------
    /** The Bounce dialog's starting values (W-9 step 2): default range, 24-bit WAV at the project
        rate, `<project folder>/Bounces/<project name>`, MP3 tags from the project and user. */
    engine::BounceSettings defaultBounceSettings() const;
    /** MP3 settings when MP3 is ticked: 320 kbps CBR, Title = project name, Artist = the user. */
    engine::Mp3Format defaultMp3Format() const;
    /** Stops the transport and starts a bounce; tick() renders it. False when one is running. */
    bool startBounce (const engine::BounceSettings& settings);
    void cancelBounce();
    bool bouncing() const noexcept { return bounceJob != nullptr; }
    double bounceProgress() const noexcept { return bounceJob != nullptr ? bounceJob->progress() : 0.0; }
    /** Why the running bounce plays in real time, or empty. */
    juce::String bounceNotice() const { return bounceJob != nullptr ? juce::String (bounceJob->notice()) : juce::String(); }
    /** The last finished (or cancelled) bounce. */
    std::optional<engine::BounceResult> lastBounce;
    /** Called on the message thread when a bounce finishes or is cancelled. */
    std::function<void()> onBounceDone;
    /** "Add to project": imports a bounced file as a new muted audio track at the range start. */
    bool addBounceToProject (const std::filesystem::path& file);
    /** The user's name for MP3 tags until accounts arrive (M10). */
    std::string userDisplayName;

    // Collaboration (M10, 05 §3) ---------------------------------------------------------------
    /** The relay account, kept in the settings folder; empty while signed out. */
    const collab::Account& account() const noexcept { return relayAccount; }
    bool signedIn() const noexcept { return relayAccount.signedIn(); }
    /** After Sign In / Create Account: keeps the account; new edits are attributed to it and a shared project starts syncing. */
    void signedInAs (const collab::Account& account);
    /** Share › Sign Out: forgets the tokens and stops syncing (the project keeps working locally). */
    void signOut();
    /** The project is on a relay (collab.relayUrl is set). */
    bool shared() const;
    /** Share Project (04 §9.2): registers the project on the relay with this user as Owner and starts syncing. */
    bool shareProject (std::string* error = nullptr);
    /** Opens a project from an invite link: accepts the invite, downloads the project into
        `parent` (the projects folder by default) as `<name>.weft` and opens it, syncing. */
    bool joinProject (const std::string& inviteLink, std::string* error = nullptr, std::filesystem::path parent = {});
    /** The live connection to the relay, or null (local project, signed out). */
    collab::SyncClient* sync() noexcept { return syncClient.get(); }
    const collab::SyncClient* sync() const noexcept { return syncClient.get(); }
    /** Status bar sync state (04 §10): "Local only", "Synced", "Offline – 3 changes pending"... */
    juce::String syncStatus() const;
    /** Clicking the sync state (04 §10): relay URL, last sync time and pending change count. */
    juce::String syncDetails() const;
    /** Share › Go Offline: edits keep working and are sent when going back online. */
    void setOnline (bool online);
    bool online() const noexcept { return wantOnline; }
    /** Why the project is read-only here (a Viewer, removed from the project), or empty. */
    juce::String readOnlyReason() const;
    /** The commands that edit the project, refused while previewing a version. */
    static bool changesDocument (Cmd command);
    /** Calls the relay as this account; refreshed tokens are saved afterwards. */
    template <typename Fn>
    auto withRelay (Fn&& fn)
    {
        collab::RelayApi api (relayAccount);
        auto result = fn (api);
        saveAccount();
        return result;
    }
    /** Where the account is kept (the settings folder). */
    std::filesystem::path accountFile() const { return settingsFolder() / "relay-account.bin"; }

    // Audio file sync and presence (M11, 05 §5 and §8) ----------------------------------------
    /** Uploads and downloads of the project's audio, or null while not syncing. */
    collab::MediaSync* mediaSync() noexcept { return mediaSyncer.get(); }
    /** Transfer limits and test hooks; set before the project starts syncing. */
    collab::TransferOptions transferOptions;
    /** The sync client's clock (Unix ms) for presence timing; tests set it before syncing starts. */
    std::function<std::int64_t()> syncClock;
    /** How far a region's audio has downloaded; nullopt when it's here (or not being fetched). */
    struct DownloadState
    {
        double fraction = 0.0;
        bool waiting = false; // the relay doesn't have it yet: still uploading from its author
        bool failed = false;
    };
    std::optional<DownloadState> downloadState (const std::string& hash) const;
    /** Where this user's pointer is in musical coordinates (the timeline and piano roll set it); nullopt when outside. */
    void setPointer (std::optional<collab::Presence::Pointer> pointer);
    /** Any keyboard or mouse input: 2 minutes without any shows this user as idle. */
    void noteInput();
    /** Other clients connected to the project now. */
    std::vector<const collab::RemotePresence*> collaborators() const;
    /** Where a collaborator's playhead is now (moved on while they play). */
    Ticks collaboratorPlayhead (const collab::RemotePresence& who) const;
    /** This client's presence as last sent to the relay (tests). */
    const collab::Presence& presence() const noexcept { return localPresence; }
    /** A plug-in this computer doesn't have: its name and who added it. */
    struct MissingPlugin
    {
        std::string name, addedBy;
    };
    std::vector<MissingPlugin> missingPluginList() const;
    /** Plug-ins a collaborator's change brought in that aren't installed here (F-COL-20). */
    std::function<void (const std::vector<MissingPlugin>&)> onMissingPlugins;

    // Comments, chat, locks and versions (M12, 05 §7 and §9) ------------------------------------
    /** Members' display names by user id (this user included), for mentions and labels. */
    std::map<UserId, std::string> memberNames() const;
    juce::String nameOf (const UserId& user) const;
    /** A comment; "@Name" in the text becomes a mention of that member (05 §9.1). */
    std::optional<Id> addComment (const commands::CommentAnchor& anchor, const juce::String& text);
    /** Shift+C (D-UI-2): where a new comment goes: the playhead, on the selected track. */
    commands::CommentAnchor commentAnchorHere() const;
    std::optional<Id> replyToComment (const Id& commentId, const juce::String& text);
    bool editComment (const Id& commentId, const juce::String& text);
    bool deleteComment (const Id& commentId);
    bool resolveComment (const Id& commentId, bool resolved);
    /** A comment's text as shown, mentions as "@Name". */
    juce::String commentText (const model::Comment& comment) const;
    /** The Comments tab's filter, and the thread it (or a pin's popover) shows. */
    commands::CommentFilter commentFilter = commands::CommentFilter::open;
    Id openThread;
    /** A comment being written (Shift+C, Add Comment): where it will be pinned. */
    std::optional<commands::CommentAnchor> draftComment;
    /** Add Comment at a place: opens the Comments tab's composer there. */
    void startComment (const commands::CommentAnchor& anchor);
    /** Clicking a pin: the thread opens (a popover at screenArea, and the Comments tab). */
    void showThread (const Id& threadId, juce::Rectangle<int> screenArea = {});
    std::function<void (const Id& threadId, juce::Rectangle<int> screenArea)> onShowThread;
    /** Break Lock asks the Owner first (04 §9.6); the window sets this. */
    std::function<void (const Id& trackId)> confirmBreakLock;
    /** The Comments tab's dot: comments from others since this user last looked. */
    bool commentsUnread() const;
    void markCommentsSeen();
    /** Mentions of this user that arrived since they were last told (shown once on connect). */
    std::vector<Id> mentionsShown;
    bool sendChat (const juce::String& text);
    bool deleteChat (const Id& messageId);
    std::size_t unreadChat() const;
    void markChatRead();
    /** Lock or unlock a track for this user (F-COL-11). */
    bool toggleLock (const Id& trackId);
    /** The Owner breaks someone's lock (after the confirmation, 04 §9.6). */
    bool breakLock (const Id& trackId);
    /** Locked by someone else: the track's controls are disabled here. */
    bool lockedByOther (const Id& trackId) const { return sess->lockedByOther (trackId); }
    /** "Locked by Ana" (or "Locked by you"), empty when not locked. */
    juce::String lockTooltip (const Id& trackId) const { return juce::String (sess->lockText (trackId)); }
    /** Save Version… (Ctrl+Alt+S). */
    std::optional<Id> saveVersion (const juce::String& name);
    /** Selecting a version in History: the arrange view shows it read-only (04 §9.5). */
    bool previewVersion (const Id& versionId);
    void returnToCurrent();
    /** Restore in the preview banner: a new change, undoable (D-COL-12). */
    bool restorePreviewed();
    bool previewing() const noexcept { return previewModel != nullptr; }
    const Id& previewedVersion() const noexcept { return previewId; }
    /** Collaborators' actions this user is told about (a lock broken, offline edits merged). */
    std::vector<commands::Notice> notices;
    std::function<void (const commands::Notice&)> onNotice;
    /** 30 minutes without input ends a session for automatic versions (05 §9.3). */
    static constexpr double idleSessionMs = 30.0 * 60 * 1000;

    /** "Remove bus and N sends?" (W-7 step 6) for a bus; empty when no question is needed. */
    juce::String deleteTrackQuestion (const Id& trackId) const;
    bool deleteTrack (const Id& trackId);

private:
    void startSync();
    void remoteChanged();
    void saveAccount();
    void loadAccount();
    collab::Account relayAccount;
    std::unique_ptr<collab::SyncClient> syncClient;
    std::unique_ptr<collab::MediaSync> mediaSyncer;
    bool mediaDirty = false;
    double lastMediaRefresh = 0;
    collab::Presence localPresence;
    std::optional<collab::Presence::Pointer> pointerAt;
    std::set<std::string> missingSeen; // plug-in ids already announced as missing
    void startMediaSync();
    void mediaDownloaded (const std::string& hash);
    void updatePresence();
    void noticeMissingPlugins();
    void collabNotices();
    void endSessionVersion();
    std::unique_ptr<model::ProjectModel> previewModel;
    Id previewId;
    commands::Session::Access accessBeforePreview = commands::Session::Access::full;
    std::string accessReasonBeforePreview;
    std::int64_t commentsSeenAt = 0, mentionsSeenAt = 0;
    Id chatReadId;
    double lastInputMs = 0;
    bool wantOnline = true;

    void syncEngine();
    void adopt (std::unique_ptr<project::ProjectFolder> projectFolder, std::unique_ptr<commands::Session> session, bool isScratch);
    void releaseProject();
    void resetViewState();
    void loadProjectMedia();
    void saveViewState() const;
    void loadViewState();
    std::optional<bool> performNotes (Cmd command);
    void zoomToFit();

    std::unique_ptr<engine::AudioDevice> audioDevice;
    engine::MediaPool pool;
    engine::InstrumentRack rack;
    std::unique_ptr<engine::Engine> eng;
    std::unique_ptr<engine::PluginCatalog> catalog;
    std::unique_ptr<engine::Vst3Host> host;
    std::map<Id, double> pendingState; // plug-in id -> when its parameters last changed (ms)
    double lastParamChange = 0;
    void endPluginGesture();
    std::unique_ptr<project::ProjectFolder> folder;
    std::unique_ptr<commands::Session> sess;
    commands::LocalView local;
    engine::KeyboardMidi keys;
    std::map<Id, int> heights;
    std::map<Id, std::vector<model::AutomationTarget>> lanesShown;
    std::unique_ptr<engine::LiveAutomation> live;
    std::unique_ptr<engine::MidiInputRouter> midiRouter;
    std::unique_ptr<engine::BounceJob> bounceJob;
    Ticks bounceFrom = 0;
    void stepBounce();
    std::map<std::string, std::vector<float>> peakCache;
    std::map<std::string, WaveSummary> waveCache;
    std::optional<Ticks> locatedAt; // shown until the audio thread has taken the locate
    std::int64_t locatedFrame = -1;
    bool audioOk = false;
    void startSilentClock();
    std::unique_ptr<engine::NullDevice> silentClock; // drives the engine in No-audio mode
    double silentClockMs = 0;
    bool unsaved = false;
    bool changesDiscarded = false; // Don't Save was chosen: no automatic version on close
    bool scratch = false, everEdited = false;
    std::filesystem::path projectParent;
    project::RecentProjects recent;
    std::uint64_t editCount = 0;
};
} // namespace weft::ui
