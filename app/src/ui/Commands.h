#pragma once

// The command table: every command's name and default shortcut (spec 04 §11). Menus,
// tooltips and the keyboard all read it, so a shortcut is defined in one place.

#include <juce_gui_basics/juce_gui_basics.h>

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
    muteNotes, quantize, transposeUp, transposeDown, octaveUp, octaveDown, notesLeft, notesRight, applyQuantize
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
} // namespace weft::ui
