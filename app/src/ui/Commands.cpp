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
        { Cmd::notesLeft, "Move Notes Left", "Left", { key (K::leftKey) } },
        { Cmd::notesRight, "Move Notes Right", "Right", { key (K::rightKey) } },
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
        { Cmd::joinProject, "Join from Invite Link...", "", {} },
        { Cmd::signIn, "Sign In...", "", {} },
        { Cmd::signOut, "Sign Out", "", {} },
        { Cmd::goOffline, "Go Offline", "", {} },
        { Cmd::addComment, "Add Comment", "Shift+C", { key ('C', shift) } },
        { Cmd::saveVersion, "Save Version...", "Ctrl+Alt+S", { key ('S', ctrl | alt) } },
        { Cmd::lockTrack, "Lock/Unlock Track", "", {} },
    };
    for (int n = 1; n <= 9; ++n)
        t.push_back ({ static_cast<Cmd> (static_cast<int> (Cmd::marker1) + n - 1), "Go to Marker " + juce::String (n), juce::String (n),
                       { key ('0' + n) } });
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
