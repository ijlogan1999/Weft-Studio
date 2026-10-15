#pragma once

// The editing session: owns the document and its projection, runs commands as
// CRDT changes, and keeps this user's undo/redo stacks (spec 05 §6).

#include "weft/commands/Arrange.h"
#include "weft/commands/Recording.h"
#include "weft/commands/Txn.h"
#include "weft/core/Id.h"
#include "weft/model/ProjectModel.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace weft::commands
{
using model::ProjectModel;

/** Result of a command: ok, or the reason it was rejected (nothing was written). */
struct EditResult
{
    bool ok = true;
    std::string error;

    static EditResult fail (std::string why) { return { false, std::move (why) }; }
    explicit operator bool() const noexcept { return ok; }
};

struct UndoResult
{
    bool done = false;      // something was undone/redone (a change was made)
    std::string message;    // status bar text when parts were skipped
};

struct NewProjectOptions
{
    std::string name = "Untitled";
    int sampleRate = 48000;
    int recordBitDepth = 24;
};

/** End of the project: the end marker, or the end of the last region (Go to End, F-TRN-4). */
Ticks projectEndTick (const ProjectModel& model);

/** Milliseconds since the Unix epoch. Injectable so tests are deterministic. */
using Clock = std::function<std::int64_t()>;
std::int64_t systemClockMs();

/** Commit message: "weft1|<userId>|<command name>" (attribution and history labels). */
std::string commitMessage (const UserId& user, std::string_view name);

class Session
{
public:
    Session (doc::Document document, UserId user, Clock clock = systemClockMs, std::uint64_t idSeed = 0);

    /** A new project (AT-PRJ-1). Not undoable: it is the document's first change. */
    static std::optional<Session> createNew (const NewProjectOptions& options, UserId user, std::string* error = nullptr,
                                             Clock clock = systemClockMs, std::uint64_t idSeed = 0);

    const ProjectModel& model() const noexcept { return projection; }
    doc::Document& document() noexcept { return doc; }
    const doc::Document& document() const noexcept { return doc; }
    const UserId& user() const noexcept { return userId; }
    Id newId() { return ids.next(); }

    /** Called with each local change's bytes after commit (the recovery log listens here). */
    std::function<void (const doc::Bytes& change, std::int64_t timeMs)> onLocalChange;

    // Remote/merged changes ---------------------------------------------------
    /** Applies changes from elsewhere and refreshes the projection. Not undoable. */
    doc::Status applyChanges (std::span<const std::uint8_t> changes);
    /** Re-reads the whole document (after a merge or load). */
    void reproject();

    // Undo ----------------------------------------------------------------------
    bool canUndo() const noexcept { return ! undoStack.empty() && ! gesture; }
    bool canRedo() const noexcept { return ! redoStack.empty() && ! gesture; }
    std::string undoName() const { return undoStack.empty() ? std::string() : undoStack.back().name; }
    std::string redoName() const { return redoStack.empty() ? std::string() : redoStack.back().name; }
    UndoResult undo();
    UndoResult redo();

    // Gestures: begin, any number of command calls, then end (one change, one undo step) or cancel.
    void beginGesture (std::string name);
    bool inGesture() const noexcept { return gesture.has_value(); }
    void endGesture();
    void cancelGesture();

    /** Runs fn as one undoable change. fn returns false to reject (its writes are rolled back). */
    EditResult edit (std::string_view name, const std::function<EditResult (TxnWriter&)>& fn);

    // Project -------------------------------------------------------------------
    EditResult renameProject (const std::string& name);
    EditResult setCycle (bool enabled, Ticks start, Ticks end);
    EditResult setPunch (bool enabled, Ticks start, Ticks end);
    EditResult setEndMarker (std::optional<Ticks> end);
    /** U: sets the cycle to the regions' span rounded outward to bars and turns it on. No regions: no change. */
    EditResult setCycleToRegions (const std::vector<Id>& regionIds);

    // Tracks ------------------------------------------------------------------
    std::optional<Id> createTrack (model::TrackKind kind, const std::string& name, int channels = 2, std::optional<std::size_t> orderIndex = {});
    /** New Track dialog (W-5): `count` tracks named in sequence, inserted below `belowTrackId` (or at the end), one undo step. */
    std::vector<Id> createTracks (model::TrackKind kind, int count, int channels = 2, const Id& belowTrackId = {});
    /** Duplicate (F-TRK-6): "<name> copy" below the original with the same strip and settings; regions only when asked. */
    std::optional<Id> duplicateTrack (const Id& trackId, bool includeRegions = false);
    EditResult setTrackColor (const Id& trackId, const model::Color& color);
    EditResult deleteTrack (const Id& trackId);
    EditResult renameTrack (const Id& trackId, const std::string& name);
    EditResult moveTrack (const Id& trackId, std::size_t newIndex);
    EditResult setTrackVolume (const Id& trackId, double volumeDb);
    EditResult setTrackPan (const Id& trackId, double pan);
    EditResult setTrackMute (const Id& trackId, bool mute);
    EditResult setTrackOutput (const Id& trackId, const Id& busIdOrEmptyForMaster);
    /** Input monitoring: "auto", "on" or "off" (F-REC-4). */
    EditResult setTrackMonitor (const Id& trackId, const std::string& mode);
    /** Audio input channel(s) or MIDI filter (F-TRK-17). */
    EditResult setTrackInput (const Id& trackId, const model::InputAssignment& input);
    /** Writes the Recovered tracks the projection synthesized for orphan regions (D-DM-17). */
    EditResult materializeRecoveredTracks();

    // Regions -----------------------------------------------------------------
    std::optional<Id> createAudioRegion (const Id& trackId, Ticks start, const std::string& mediaHash, Samples sourceOffset, Samples length);
    std::optional<Id> createMidiRegion (const Id& trackId, Ticks start, Ticks length);
    EditResult moveRegion (const Id& regionId, Ticks newStart, std::optional<Id> newTrackId = {});
    /** Moves the region's start edge, keeping the end fixed on the timeline. */
    EditResult trimRegionStart (const Id& regionId, Ticks newStart);
    /** Sets the region length (ticks for MIDI, samples for audio), keeping the start fixed. */
    EditResult trimRegionLength (const Id& regionId, std::int64_t newLength);
    std::optional<Id> splitRegion (const Id& regionId, Ticks at);
    /** Joins regions on one track into the earliest one (MIDI, or contiguous audio from one file). */
    EditResult joinRegions (const std::vector<Id>& regionIds);
    EditResult deleteRegions (const std::vector<Id>& regionIds);
    EditResult setRegionFades (const Id& regionId, std::optional<model::Fade> fadeIn, std::optional<model::Fade> fadeOut);
    EditResult setRegionGain (const Id& regionId, double gainDb);
    EditResult setRegionMuted (const Id& regionId, bool muted);
    EditResult setRegionName (const Id& regionId, const std::string& name);
    /** A region's own color, or nullopt to follow its track (F-TRK-9). */
    EditResult setRegionColor (const Id& regionId, std::optional<model::Color> color);
    EditResult setRegionQuantize (const Id& regionId, std::optional<model::RegionQuantize> q);

    // Several regions (arrange view) ---------------------------------------------
    /** Drag move (W-3b): one undo step; the plan is planMove's. Moved regions go on top (D-W3-2). */
    EditResult moveRegions (const std::vector<Id>& regionIds, Ticks deltaTicks, int deltaTracks);
    /** Ctrl+drag copy (W-3c): new ids, MIDI regions get their own sequence copy. Returns the copies. */
    std::vector<Id> copyRegions (const std::vector<Id>& regionIds, Ticks deltaTicks, int deltaTracks);
    /** Ctrl+V (W-3c): the clipboard's leftmost region lands at `at` on targetTrackId, the rest keep their offsets. */
    std::vector<Id> pasteRegions (const RegionClipboard& clip, Ticks at, const Id& targetTrackId);
    /** Ctrl+R (F-ARR-29): `times` copies of the selection, each after the previous, as a block. */
    std::vector<Id> repeatRegions (const std::vector<Id>& regionIds, int times = 1);
    /** Alt+Left/Right (F-ARR-23): moves by deltaTicks, clamped so nothing starts before 0. */
    EditResult nudgeRegions (const std::vector<Id>& regionIds, Ticks deltaTicks);
    /** Ctrl+T (W-3e): splits every listed region the position crosses, one undo step. Returns the new right-hand pieces. */
    std::vector<Id> splitRegionsAt (const std::vector<Id>& regionIds, Ticks at);
    /** Loop handle (W-3i): repeats the region up to loopEnd (absolute ticks); nullopt turns looping off. */
    EditResult setRegionLoop (const Id& regionId, std::optional<Ticks> loopEnd);
    /** M (F-ARR-14): mutes all listed regions, or unmutes them all when every one is already muted. */
    EditResult toggleRegionsMuted (const std::vector<Id>& regionIds);
    /** Crossfade command (W-3h step 3): equal-power fades over the overlap of each overlapping pair. */
    EditResult crossfadeRegions (const std::vector<Id>& regionIds);
    /**
        Join of audio that isn't one contiguous piece of a file (D-ARR-3): the regions become one
        region at the earliest start playing `rendered` (already rendered with their gain and
        inner fades, and in Media/), with the outer fades given. One undo step "Join Regions".
    */
    EditResult joinRegionsAs (const std::vector<Id>& regionIds, const model::MediaRef& rendered, model::Fade fadeIn, model::Fade fadeOut);

    // Media ---------------------------------------------------------------------
    EditResult addMedia (const model::MediaRef& media);

    // Recording -------------------------------------------------------------------
    /** Adds the recorded media, regions and take folders as one undo step "Record" (W-1a step 8). */
    EditResult commitRecording (const std::vector<RecordedAudio>& audio, const std::vector<RecordedMidi>& midi);
    /** Makes a take the one that plays (W-1c step 5). */
    EditResult setActiveTake (const Id& takeFolderId, const Id& regionId);
    /** Display name for the next recording on a track: "<TrackName>#<n>" (W-1a step 8). */
    std::string nextRecordingName (const std::string& trackName) const;

    // Notes (positions are sequence ticks) ------------------------------------------
    std::optional<Id> addNote (const Id& regionId, model::Note note);
    EditResult updateNote (const Id& regionId, const model::Note& note);
    EditResult deleteNotes (const Id& regionId, const std::vector<Id>& noteIds);
    /** Destructive quantize of notes (all notes in the region window if noteIds is empty). */
    EditResult quantizeNotes (const Id& regionId, const std::vector<Id>& noteIds, Ticks grid, double strength, double swingPercent);

    // Plugins and sends ---------------------------------------------------------
    std::optional<Id> addInsert (const Id& trackId, model::PluginInstance plugin, std::optional<std::size_t> index = {});
    EditResult removeInsert (const Id& trackId, const Id& pluginId);
    EditResult setInsertBypass (const Id& trackId, const Id& pluginId, bool bypass);
    EditResult setPluginParam (const Id& trackId, const Id& pluginId, const std::string& paramId, double normalized);
    std::optional<Id> addSend (const Id& trackId, const Id& busId, double levelDb = 0.0, bool pre = false);
    EditResult setSendLevel (const Id& trackId, const Id& sendId, double levelDb);
    EditResult setSendTarget (const Id& trackId, const Id& sendId, const Id& busId);
    EditResult removeSend (const Id& trackId, const Id& sendId);

    // Automation ----------------------------------------------------------------
    std::optional<Id> addAutomationPoint (const Id& trackId, const model::AutomationTarget& target, Ticks pos, double value, double curve = 0.0);
    EditResult moveAutomationPoint (const Id& laneId, const Id& pointId, Ticks pos, double value);
    EditResult deleteAutomationPoint (const Id& laneId, const Id& pointId);

    // Tempo, signature, markers --------------------------------------------------
    std::optional<Id> addTempoPoint (Ticks pos, double bpm);
    EditResult setTempoPoint (const Id& pointId, Ticks pos, double bpm);
    EditResult deleteTempoPoint (const Id& pointId);
    std::optional<Id> addSignaturePoint (int bar, int numerator, int denominator);
    EditResult setSignaturePoint (const Id& pointId, int bar, int numerator, int denominator);
    EditResult deleteSignaturePoint (const Id& pointId);
    /** Tempo display edit: sets the tempo in effect at `at`, clamped to 20-999 BPM (F-TRN-9). */
    EditResult setTempoAt (Ticks at, double bpm);
    /** Signature display edit: changes the signature in effect at the 0-based bar (F-TRN-10). */
    EditResult setSignatureAt (int bar, int numerator, int denominator);
    std::optional<Id> addMarker (Ticks pos, const std::string& name);
    EditResult moveMarker (const Id& markerId, Ticks pos);
    EditResult renameMarker (const Id& markerId, const std::string& name);
    EditResult deleteMarker (const Id& markerId);

    /** Message of the last rejected command (for the status bar). */
    const std::string& lastError() const noexcept { return error; }

private:
    struct Gesture
    {
        Txn txn;
        std::unique_ptr<TxnWriter> writer;
    };

    EditResult run (std::string_view name, const std::function<EditResult (TxnWriter&)>& fn);
    std::optional<Id> runCreate (std::string_view name, Id id, const std::function<EditResult (TxnWriter&)>& fn);
    void finish (Txn txn, bool isUndoRedo, std::vector<Txn>* pushTo);
    void refreshFor (const Txn& txn);
    UndoResult invert (std::vector<Txn>& from, std::vector<Txn>& to, std::string_view label);
    bool wouldCycle (const Id& fromTrack, const Id& toTrack, const std::string& ignoreEdgeKey) const;
    void stamp (TxnWriter& w, const Id& regionId);
    /** Writes a move or copy plan. nextZ tracks the next z per track across calls in one edit. */
    bool writePlacement (TxnWriter& w, const RegionClipboard& clip, const MovePlan& plan, bool copy, std::vector<Id>& newTrackIds,
                         std::map<Id, std::int64_t>& nextZ, std::vector<Id>* created);
    std::vector<Id> placeCopies (std::string_view name, const RegionClipboard& clip, const std::vector<std::pair<Ticks, int>>& shifts, int fallbackRow);
    EditResult reject (std::string why)
    {
        error = why;
        return EditResult::fail (std::move (why));
    }

    doc::Document doc;
    UserId userId;
    Clock clock;
    IdGenerator ids;
    ProjectModel projection;
    std::vector<Txn> undoStack, redoStack;
    /** Objects re-created by undo/redo get new op ids; older records still name the old ones. */
    std::map<std::string, std::string> opIdAliases;
    std::string resolveOpId (std::string opId) const;
    std::optional<Gesture> gesture;
    std::string error;
};
} // namespace weft::commands
