#pragma once

// The command table: every command's name and default shortcut (spec 04 §11). Menus,
// tooltips and the keyboard all read it, so a shortcut is defined in one place.

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>
#include <vector>

namespace weft::ui
{
enum class Cmd
{
    playStop, record, cycle, metronome, goToStart, goToEnd, previousBar, nextBar, autopunch, followPlayhead,
    save, newTrack, undo, redo, cut, copy, paste, deleteSelection, selectAll, split, join, muteRegions,
    nudgeLeft, nudgeRight, repeat, crossfade, loopRegion, setCycleToSelection,
    zoomToFit, zoomIn, zoomOut, toolMenu, escape,
    toggleInspector, toggleSidePanel, toggleMixer, togglePianoRoll, toggleAudioEditor, toggleGlobalTracks,
    addMarker, previousMarker, nextMarker, marker1, marker2, marker3, marker4, marker5, marker6, marker7, marker8, marker9,
    snapToggle, keyboardMidi, toggleTheme, about,
    // Piano roll (they act on notes while the piano roll has focus).
    muteNotes, quantize, transposeUp, transposeDown, octaveUp, octaveDown, notesLeft, notesRight, applyQuantize,
    // Mixer and plug-ins (M6).
    deleteTrack, newBus, pluginManager,
    // Automation (M7).
    toggleAutomation,
    // Bounce (M8).
    bounce,
    // Projects and preferences (M9).
    newProject, openProject, saveAs, projectSettings, audioSettings,
    // Collaboration (M10).
    shareProject, shareFolder, joinProject, signIn, signOut, goOffline,
    // M12
    addComment, saveVersion, lockTrack,
    // File and Track menus (04 §2), track header menu and reordering (F-TRK-6, F-TRK-7)
    importAudio, quit, duplicateTrack, duplicateTrackWithRegions, moveTrackUp, moveTrackDown,
    // Pause (F-TRN-3)
    pause,
    // Export Regions as Files (L-EXP-2, L-EXP-3)
    exportRegions,
    // Discard Recording (L-REC-7, F-REC-18)
    discardRecording,
    // Reverse audio regions (L-RGN-12)
    reverseRegions,
    // Audio editor (L-ATE-3)
    snapZeroCrossings,
    // Tap Tempo (L-TMP-9)
    tapTempo,
    // Hide tracks (L-TRH-10)
    showHiddenTracks,
    // Second-pass small wins (D-UI-17 to D-UI-19): Stop on its own, count-in, selection, the selected track's buttons, one-step new tracks
    stop, countIn, deselectAll, invertSelection, muteTrack, soloTrack, armTrack, newAudioTrack, newInstrumentTrack,
    // Remaining small wins (D-UI-26 to D-UI-28): marker commands, stepping through regions and notes
    renameMarker, deleteMarker, selectPrevious, selectNext,
    marker10, marker11, marker12, marker13, marker14, marker15, marker16, marker17, marker18, marker19,
    // Search and Add Plug-in (D-W6-11)
    searchPlugins,
    // Delete Unused Tracks (D-UI-32)
    deleteUnusedTracks,
    // D-UI-34: a track like the selected one on the next input; find a track by name
    newTrackNextInput, searchTracks,
    // Piano roll note functions (D-W4-14, D-W4-15)
    selectAllFollowing, legato, removeOverlaps, noteStartToPlayhead, noteEndToPlayhead, lengthenNotes, shortenNotes, scaleQuantize,
    // Transport go-to and play-from commands (D-UI-35)
    goToCycleStart, goToCycleEnd, goToSelectionStart, goToSelectionEnd, goToLastLocate, goToPosition, playFromStart, playFromCycleStart, playFromSelection, playFromWindowEdge, playFromPreviousBar, playFromLastLocate, stopGoToStart, stopGoToCycleStart, stopGoToLastLocate, cycleForward, cycleBack, doubleCycle, halveCycle,
    // D-UI-36
    nextTool, previousTool,
    // D-W3-11: nudge value, slip, shuffle, grid lines
    nudgeByGrid, nudgeByBar, nudgeByBeat, nudgeByDivision, nudgeByTick, slipLeft, slipRight, shuffleLeft, shuffleRight, gridLines,
    // D-UI-37: track header options, all tracks on/off
    configureTrackHeader, toggleAllTracksOff,
    // D-W6-12
    togglePluginWindows,
    // D-W6-13: mixer commands
    unmuteAll, clearRecallSolo, soloSelectedRegions, monitorTrack, copyChannelStrip, pasteChannelStrip,
    // D-W6-14
    cycleMixerMode,
    // D-UI-38: zoom and navigation
    zoomVerticalIn, zoomVerticalOut, saveZoom1, saveZoom2, saveZoom3, recallZoom1, recallZoom2, recallZoom3, navigateBack, navigateForward,
    pageUp, pageDown, pageLeft, pageRight, zoomTrack,
    // D-W3-12: region selection and edit commands
    selectPreviousTrack, selectNextTrack, selectInsideCycle, selectMutedRegions, selectSameColor, selectFirst, selectLast, togglePreviousRegion, toggleNextRegion,
    scrollToSelection, moveToPlayhead, moveToPlayheadSelectNext, regionStartToPlayhead, regionEndToPlayhead, splitAtCycle,
    // D-W8-7: automation mode commands and lanes
    automationAllOff, automationAllRead, automationAllTouch, automationAllLatch, automationOffRead, automationLatchRead, cycleAutomationParameter,
    // D-UI-39: marker commands
    markerForSelection, cycleToMarker, cycleToPreviousMarker, cycleToNextMarker, previousMarkerSetLocators, nextMarkerSetLocators, goToMarkerNumber,
    applyRegionParameters, applyRegionParametersExceptDelay, settings,
    // D-UI-43: control bar, toolbar and big displays
    toggleControlBar, toggleToolbar, customizeControlBar, bigBeatsDisplay, bigTimeDisplay, performanceMeter,
    // D-UI-44, D-UI-45: global tracks
    configureGlobalTracks, showSingleGlobalTrack, toggleArrangementTrack, toggleMarkerTrack, toggleTempoTrack, toggleSignatureTrack, toggleKeyTrack,
    markerTrackOnly, createSignatureChange, createKeyChange, addSection,
    // D-W9-9, D-W9-10: exports
    exportSelectionMidi, exportTrackAudio, exportAllTracksAudio,
    // D-W1-12, D-W1-13: recording
    recordToggle, toggleReplace, captureRecording,
    // D-W6-15, D-W6-16: groups and VCA faders
    toggleGroups, groupsWindow, createGroup, hideGroup1, hideGroup2, hideGroup3, hideGroup4, hideGroup5, hideGroup6, hideGroup7, hideGroup8,
    hideGroup9, newVca,
    // D-W6-17: track stacks
    createFolderStack, createSummingStack, flattenStack, toggleStack, followStacks,
    // D-UI-46: windows and screensets
    screenset1, screenset2, screenset3, screenset4, screenset5, screenset6, screenset7, screenset8, screenset9,
    screensetTens1, screensetTens2, screensetTens3, screensetTens4, screensetTens5, screensetTens6, screensetTens7, screensetTens8, screensetTens9,
    undoHistory, openMainWindow, mixerWindow, pianoRollWindow, audioEditorWindow, projectAudioWindow, regionInspectorWindow, closeWindow,
    cycleWindows, cycleWindowsBack, minimizeWindow, minimizeAllWindows, zoomWindow, fullScreen, closeProject, cycleWindowViews,
    cycleWindowViewsBack,
    // D-UI-47: notes
    toggleNotes, mixerNotes, listEditors, openEventList, openMarkerList, openTempoList, openSignatureList, openMarkerText,
    toggleAudioFileEditor, keyCommands, toggleSmartControls, smartControlsWindow,
    toggleLibrary, loopBrowser, searchLibrary
};

struct CommandInfo
{
    Cmd id;
    juce::String name;
    juce::String shortcut; // as shown in menus and tooltips; empty when none
    std::vector<juce::KeyPress> keys;
};

const std::vector<CommandInfo>& commandTable();
const CommandInfo& commandInfo (Cmd id);
/** "Play/Stop (Space)", or just the name when there's no shortcut (F-UI-10). */
juce::String tooltipFor (Cmd id);
std::optional<Cmd> commandForKey (const juce::KeyPress& key);

// Key Commands (D-UI-50): this computer's own keys, in place of a command's defaults. Menus and tooltips follow.
/** Gives a command these keys (none clears it); a key another command had is taken from it. */
void setCommandKeys (Cmd id, std::vector<juce::KeyPress> keys);
const std::vector<juce::KeyPress>& defaultKeys (Cmd id);
/** Every command back to its default keys. */
void resetAllKeys();
/** The command a key runs now, other than `except`. */
std::optional<Cmd> commandUsingKey (const juce::KeyPress& key, std::optional<Cmd> except = std::nullopt);
/** "Ctrl+Alt+K", as menus show a key; and back. */
juce::String keyDescription (const juce::KeyPress& key);
std::optional<juce::KeyPress> parseKeyDescription (const juce::String& text);
/** The assignments that differ from the defaults, as JSON ({"Metronome": ["Ctrl+K"]}); and back (unknown names are skipped). */
juce::String keyAssignmentsToString();
void keyAssignmentsFromString (const juce::String& json);
/** Called after any change, so the app can save the assignments. */
std::function<void()>& onKeyAssignmentsChanged();
} // namespace weft::ui
