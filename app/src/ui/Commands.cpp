#include "Commands.h"

namespace weft::ui
{
namespace
{
using K = juce::KeyPress;
using M = juce::ModifierKeys;
constexpr int ctrl = M::commandModifier, shift = M::shiftModifier, alt = M::altModifier;

K key (int code, int mods = 0) { return K (code, M (mods), 0); }

std::vector<CommandInfo> build()
{
    std::vector<CommandInfo> t {
        { Cmd::playStop, "Play/Stop", "Space", { key (K::spaceKey) } },
        { Cmd::pause, "Pause", "Ctrl+Space", { key (K::spaceKey, ctrl) } },
        { Cmd::record, "Record", "R", { key ('R') } },
        { Cmd::cycle, "Cycle", "C", { key ('C') } },
        { Cmd::metronome, "Metronome", "K", { key ('K') } },
        { Cmd::goToStart, "Go to Start", "Home", { key (K::homeKey) } },
        { Cmd::goToEnd, "Go to End", "End", { key (K::endKey) } },
        { Cmd::previousBar, "Previous Bar", ",", { key (',') } },
        { Cmd::nextBar, "Next Bar", ".", { key ('.') } },
        { Cmd::autopunch, "Autopunch", "Ctrl+Alt+P", { key ('P', ctrl | alt) } },
        { Cmd::followPlayhead, "Follow Playhead", "Ctrl+F", { key ('F', ctrl) } },
        { Cmd::save, "Save", "Ctrl+S", { key ('S', ctrl) } },
        { Cmd::newTrack, "New Track", "Ctrl+Alt+N", { key ('N', ctrl | alt) } },
        { Cmd::undo, "Undo", "Ctrl+Z", { key ('Z', ctrl) } },
        { Cmd::redo, "Redo", "Ctrl+Y", { key ('Y', ctrl), key ('Z', ctrl | shift) } },
        { Cmd::cut, "Cut", "Ctrl+X", { key ('X', ctrl) } },
        { Cmd::copy, "Copy", "Ctrl+C", { key ('C', ctrl) } },
        { Cmd::paste, "Paste", "Ctrl+V", { key ('V', ctrl) } },
        { Cmd::deleteSelection, "Delete", "Delete", { key (K::deleteKey), key (K::backspaceKey) } },
        { Cmd::selectAll, "Select All", "Ctrl+A", { key ('A', ctrl) } },
        { Cmd::split, "Split at Playhead", "Ctrl+T", { key ('T', ctrl) } },
        { Cmd::join, "Join", "Ctrl+J", { key ('J', ctrl) } },
        { Cmd::muteRegions, "Mute Regions", "M", { key ('M') } },
        { Cmd::nudgeLeft, "Nudge Left", "Alt+Left", { key (K::leftKey, alt) } },
        { Cmd::nudgeRight, "Nudge Right", "Alt+Right", { key (K::rightKey, alt) } },
        { Cmd::repeat, "Repeat Regions", "Ctrl+R", { key ('R', ctrl) } },
        { Cmd::crossfade, "Crossfade", "", {} },
        { Cmd::loopRegion, "Loop Region", "", {} },
        { Cmd::setCycleToSelection, "Set Cycle to Selection", "U", { key ('U') } },
        { Cmd::zoomToFit, "Zoom to Fit", "Z", { key ('Z') } },
        { Cmd::zoomIn, "Zoom In", "Ctrl+=", { key ('=', ctrl) } },
        { Cmd::zoomOut, "Zoom Out", "Ctrl+-", { key ('-', ctrl) } },
        { Cmd::toolMenu, "Tool Menu", "T", { key ('T') } },
        { Cmd::escape, "Cancel / Pointer Tool", "Esc", { key (K::escapeKey) } },
        { Cmd::toggleInspector, "Inspector", "I", { key ('I') } },
        { Cmd::toggleSidePanel, "Side Panel", "Ctrl+L", { key ('L', ctrl) } },
        { Cmd::toggleMixer, "Mixer", "X", { key ('X') } },
        { Cmd::togglePianoRoll, "Piano Roll", "P", { key ('P') } },
        { Cmd::toggleAudioEditor, "Audio Editor", "E", { key ('E') } },
        { Cmd::toggleGlobalTracks, "Global Tracks", "G", { key ('G') } },
        { Cmd::addMarker, "Add Marker", ";", { key (';') } },
        { Cmd::previousMarker, "Previous Marker", "Alt+,", { key (',', alt) } },
        { Cmd::nextMarker, "Next Marker", "Alt+.", { key ('.', alt) } },
        { Cmd::snapToggle, "Snap", "N", { key ('N') } },
        { Cmd::keyboardMidi, "Keyboard MIDI", "Ctrl+K", { key ('K', ctrl) } },
        { Cmd::toggleTheme, "Light/Dark Theme", "", {} },
        { Cmd::about, "About Weft Studio", "", {} },
        { Cmd::muteNotes, "Mute Notes", "Ctrl+M", { key ('M', ctrl) } },
        { Cmd::quantize, "Quantize", "Q", { key ('Q') } },
        { Cmd::transposeUp, "Transpose Up", "Up", { key (K::upKey) } },
        { Cmd::transposeDown, "Transpose Down", "Down", { key (K::downKey) } },
        { Cmd::octaveUp, "Transpose Octave Up", "Shift+Up", { key (K::upKey, shift) } },
        { Cmd::octaveDown, "Transpose Octave Down", "Shift+Down", { key (K::downKey, shift) } },
        // Alt+Left/Right reach these through Nudge, which moves notes while the piano roll has the keys (D-UI-28).
        { Cmd::notesLeft, "Move Notes Left", "Alt+Left", {} },
        { Cmd::notesRight, "Move Notes Right", "Alt+Right", {} },
        { Cmd::applyQuantize, "Apply Quantize", "", {} },
        { Cmd::deleteTrack, "Delete Track", "", {} },
        { Cmd::newBus, "New Bus", "", {} },
        { Cmd::pluginManager, "Plug-in Manager", "", {} },
        { Cmd::toggleAutomation, "Automation", "A", { key ('A') } },
        { Cmd::bounce, "Bounce", "Ctrl+B", { key ('B', ctrl) } },
        { Cmd::newProject, "New Project", "Ctrl+N", { key ('N', ctrl) } },
        { Cmd::openProject, "Open", "Ctrl+O", { key ('O', ctrl) } },
        { Cmd::saveAs, "Save As", "Ctrl+Shift+S", { key ('S', ctrl | shift) } },
        { Cmd::projectSettings, "Project Settings", "", {} },
        { Cmd::audioSettings, "Audio & MIDI Settings", "", {} },
        { Cmd::shareProject, "Share Project...", "", {} },
        { Cmd::shareFolder, "Share in a Synced Folder...", "", {} },
        { Cmd::joinProject, "Join from Invite Link...", "", {} },
        { Cmd::signIn, "Sign In...", "", {} },
        { Cmd::signOut, "Sign Out", "", {} },
        { Cmd::goOffline, "Go Offline", "", {} },
        { Cmd::addComment, "Add Comment", "Shift+C", { key ('C', shift) } },
        { Cmd::saveVersion, "Save Version...", "Ctrl+Alt+S", { key ('S', ctrl | alt) } },
        { Cmd::lockTrack, "Lock/Unlock Track", "", {} },
        { Cmd::importAudio, "Import Audio...", "Ctrl+I", { key ('I', ctrl) } },
        { Cmd::quit, "Exit", "Ctrl+Q", { key ('Q', ctrl) } },
        { Cmd::duplicateTrack, "Duplicate Track", "Ctrl+D", { key ('D', ctrl) } },
        { Cmd::duplicateTrackWithRegions, "Duplicate Track with Regions", "Ctrl+Shift+D", { key ('D', ctrl | shift) } },
        { Cmd::moveTrackUp, "Move Track Up", "", {} },
        { Cmd::moveTrackDown, "Move Track Down", "", {} },
        { Cmd::exportRegions, "Export Regions as Files...", "", {} },
        { Cmd::discardRecording, "Discard Recording", "Ctrl+.", { key ('.', ctrl) } },
        { Cmd::reverseRegions, "Reverse Regions", "", {} },
        { Cmd::snapZeroCrossings, "Snap Edits to Zero Crossings", "", {} },
        { Cmd::tapTempo, "Tap Tempo", "Shift+T", { key ('T', shift) } },
        { Cmd::showHiddenTracks, "Show Hidden Tracks", "Ctrl+Shift+H", { key ('H', ctrl | shift) } },
        { Cmd::stop, "Stop", "Numpad 0", { key (K::numberPad0) } },
        { Cmd::countIn, "Count-in", "Shift+K", { key ('K', shift) } },
        { Cmd::deselectAll, "Deselect All", "Ctrl+Shift+A", { key ('A', ctrl | shift) } },
        { Cmd::invertSelection, "Invert Selection", "Ctrl+Shift+I", { key ('I', ctrl | shift) } },
        { Cmd::muteTrack, "Mute Track", "Shift+M", { key ('M', shift) } },
        { Cmd::soloTrack, "Solo Track", "S", { key ('S') } },
        { Cmd::armTrack, "Record-Enable Track", "Shift+R", { key ('R', shift) } },
        { Cmd::newAudioTrack, "New Audio Track", "Ctrl+Alt+A", { key ('A', ctrl | alt) } },
        { Cmd::newInstrumentTrack, "New Instrument Track", "Ctrl+Alt+I", { key ('I', ctrl | alt) } },
        { Cmd::renameMarker, "Rename Marker...", "Shift+'", { key ('\'', shift), key ('"', shift) } },
        { Cmd::deleteMarker, "Delete Marker", "Alt+Delete", { key (K::deleteKey, alt), key (K::backspaceKey, alt) } },
        { Cmd::selectPrevious, "Select Previous Region or Note", "Left", { key (K::leftKey) } },
        { Cmd::selectNext, "Select Next Region or Note", "Right", { key (K::rightKey) } },
        { Cmd::searchPlugins, "Search and Add Plug-in...", "Ctrl+Shift+P", { key ('P', ctrl | shift) } },
        { Cmd::deleteUnusedTracks, "Delete Unused Tracks", "", {} },
        { Cmd::newTrackNextInput, "New Track with Next Input", "Ctrl+Shift+N", { key ('N', ctrl | shift) } },
        { Cmd::searchTracks, "Search and Select Track...", "Ctrl+Alt+F", { key ('F', ctrl | alt) } },
    };
    for (int n = 1; n <= 9; ++n)
        t.push_back ({ static_cast<Cmd> (static_cast<int> (Cmd::marker1) + n - 1), "Go to Marker " + juce::String (n), juce::String (n),
                       { key ('0' + n) } });
    // Markers 10 to 19: Ctrl with the numeric keypad's 0 to 9 (D-UI-26). Keypad codes aren't consecutive everywhere.
    const int pad[] = { K::numberPad0, K::numberPad1, K::numberPad2, K::numberPad3, K::numberPad4,
                        K::numberPad5, K::numberPad6, K::numberPad7, K::numberPad8, K::numberPad9 };
    for (int n = 10; n <= 19; ++n)
        t.push_back ({ static_cast<Cmd> (static_cast<int> (Cmd::marker10) + n - 10), "Go to Marker " + juce::String (n),
                       "Ctrl+Numpad " + juce::String (n - 10), { key (pad[n - 10], ctrl) } });
    return t;
}
} // namespace

const std::vector<CommandInfo>& commandTable()
{
    static const auto table = build();
    return table;
}

const CommandInfo& commandInfo (Cmd id)
{
    for (const auto& c : commandTable())
        if (c.id == id)
            return c;
    jassertfalse;
    return commandTable().front();
}

juce::String tooltipFor (Cmd id)
{
    const auto& c = commandInfo (id);
    return c.shortcut.isEmpty() ? c.name : c.name + " (" + c.shortcut + ")";
}

std::optional<Cmd> commandForKey (const juce::KeyPress& pressed)
{
    // Letters arrive as their upper or lower case character; compare on the key code, case-insensitively.
    auto normal = [] (const juce::KeyPress& k) {
        auto code = k.getKeyCode();
        if (code >= 'a' && code <= 'z')
            code -= 'a' - 'A';
        return std::pair { code, k.getModifiers().withoutMouseButtons().getRawFlags() & (ctrl | shift | alt) };
    };
    const auto p = normal (pressed);
    for (const auto& c : commandTable())
        for (const auto& k : c.keys)
            if (normal (k) == p)
                return c.id;
    return std::nullopt;
}
} // namespace weft::ui
