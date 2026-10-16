#include "MainComponent.h"

#include "AboutPanel.h"
#include "NewTrackDialog.h"

namespace weft::ui
{
// Status bar --------------------------------------------------------------------------------

StatusBar::StatusBar (Studio& s) : studio (s)
{
    setTitle ("Status bar");
    studio.addChangeListener (this);
}

StatusBar::~StatusBar() { studio.removeChangeListener (this); }

void StatusBar::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::surface));
    g.setColour (colour (Token::border));
    g.drawHorizontalLine (0, 0.0f, static_cast<float> (getWidth()));
    g.setFont (juce::FontOptions (12.0f));
    auto r = getLocalBounds().reduced (10, 0);
    g.setColour (colour (Token::textDim));
    const auto right = juce::String (studio.model().info.name) + (studio.dirty() ? " (edited)" : "") + "   ·   Tool: " + toolName (studio.tool)
                       + "   ·   Snap: " + (studio.snapEnabled ? arrange::snapLabel (studio.snap) : juce::String ("Off"));
    g.drawText (right, r.removeFromRight (420), juce::Justification::centredRight);
    g.setColour (colour (Token::text));
    g.drawText (studio.status, r, juce::Justification::centredLeft, true);
}

// Main component ------------------------------------------------------------------------------

MainComponent::MainComponent (Studio& s, const juce::File& browserRoot)
    : studio (s), control (s), inspectorPanel (s), tracksArea (s), bottomArea (s), sidePanel (s, browserRoot), status (s), plugins (s)
{
    setTitle ("Weft Studio");
    for (auto* c : std::initializer_list<juce::Component*> { &control, &inspectorPanel, &tracksArea, &bottomArea, &sidePanel, &status })
        addChildComponent (c);
    control.setVisible (true);
    tracksArea.setVisible (true);
    status.setVisible (true);
    setWantsKeyboardFocus (true);
    studio.uiCommand = [this] (Cmd c) { handleUiCommand (c); };
    studio.addChangeListener (this);
    shownLayout = studio.layout;
    changeListenerCallback (nullptr);
    startTimerHz (30);
    setSize (1400, 860);
}

MainComponent::~MainComponent()
{
    studio.uiCommand = nullptr;
    studio.removeChangeListener (this);
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    inspectorPanel.setVisible (studio.layout.inspector);
    sidePanel.setVisible (studio.layout.sidePanel);
    bottomArea.setVisible (studio.layout.bottom != BottomTab::none);
    if (! (studio.layout == shownLayout))
    {
        shownLayout = studio.layout;
        resized();
        if (onLayoutChanged)
            onLayoutChanged (shownLayout);
    }
    repaint();
}

void MainComponent::timerCallback() { studio.tick(); }

void MainComponent::paint (juce::Graphics& g) { g.fillAll (colour (Token::background)); }

void MainComponent::resized()
{
    auto r = getLocalBounds();
    control.setBounds (r.removeFromTop (52));
    status.setBounds (r.removeFromBottom (StatusBar::height));
    const auto& l = studio.layout;
    if (l.inspector)
        inspectorPanel.setBounds (r.removeFromLeft (std::clamp (l.inspectorWidth, 180, 420)));
    if (l.sidePanel)
        sidePanel.setBounds (r.removeFromRight (std::clamp (l.sidePanelWidth, 220, 520)));
    // The tracks area keeps at least 160 px (F-UI-1: it is always visible).
    if (l.bottom != BottomTab::none)
        bottomArea.setBounds (r.removeFromBottom (std::clamp (l.bottomHeight, 120, std::max (120, r.getHeight() - 160))));
    tracksArea.setBounds (r);
}

bool MainComponent::run (Cmd command)
{
    if (command == Cmd::escape && tracksArea.timeline().cancelDrag())
        return true;
    return studio.perform (command);
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    // Keyboard MIDI gets letter keys first (AT-REC-20); key repeat is ignored by the mapper.
    if (studio.keyboardMidiKey (key, true))
    {
        if (std::find (heldKeys.begin(), heldKeys.end(), key.getKeyCode()) == heldKeys.end())
            heldKeys.push_back (key.getKeyCode());
        return true;
    }
    if (const auto command = commandForKey (key))
    {
        run (*command);
        return true;
    }
    return false;
}

bool MainComponent::keyStateChanged (bool isKeyDown)
{
    if (isKeyDown)
        return false;
    bool used = false;
    for (auto it = heldKeys.begin(); it != heldKeys.end();)
    {
        if (! juce::KeyPress::isKeyCurrentlyDown (*it))
        {
            used = studio.keyboardMidiKey (juce::KeyPress (*it, {}, static_cast<juce::juce_wchar> (*it)), false) || used;
            it = heldKeys.erase (it);
        }
        else
            ++it;
    }
    return used;
}

juce::PopupMenu MainComponent::toolMenu() const
{
    juce::PopupMenu menu;
    for (const auto t : { Tool::pointer, Tool::pencil, Tool::scissors, Tool::glue, Tool::eraser, Tool::mute })
        menu.addItem (static_cast<int> (t) + 1, toolName (t), true, studio.tool == t);
    return menu;
}

void MainComponent::handleUiCommand (Cmd command)
{
    if (command == Cmd::newTrack)
        NewTrackDialog::show (studio, this);
    else if (command == Cmd::about)
        AboutPanel::show (this);
    else if (command == Cmd::toolMenu)
        toolMenu().showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [this] (int chosen) { pickTool (chosen); });
    else if (command == Cmd::pluginManager)
        PluginManager::show (studio, this);
    else if (command == Cmd::deleteTrack)
        deleteSelectedTrack();
}

void MainComponent::deleteSelectedTrack()
{
    const auto id = studio.selectedTrack();
    if (id.empty())
    {
        studio.setStatus ("Select a track to delete.");
        return;
    }
    const auto text = studio.deleteTrackQuestion (id);
    if (text.isEmpty())
    {
        studio.deleteTrack (id);
        return;
    }
    questionTrack = id;
    asked = text;
    if (! desktopWindows())
        return; // headless: answer() stands in for the buttons
    question = std::make_unique<juce::AlertWindow> ("Delete Track", text, juce::MessageBoxIconType::QuestionIcon, this);
    question->addButton ("Remove", 1, juce::KeyPress (juce::KeyPress::returnKey));
    question->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    question->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<MainComponent> (this)] (int result) {
                                   if (safe != nullptr)
                                       safe->answer (result == 1);
                               }),
                               false);
}

void MainComponent::answer (bool confirmed)
{
    const auto id = questionTrack;
    questionTrack.clear();
    if (question != nullptr)
    {
        question->exitModalState (0);
        question.reset();
    }
    if (confirmed && ! id.empty())
        studio.deleteTrack (id);
}

void MainComponent::pickTool (int menuItemId)
{
    if (menuItemId < 1 || menuItemId > static_cast<int> (Tool::mute) + 1)
        return;
    studio.tool = static_cast<Tool> (menuItemId - 1);
    studio.sendChangeMessage();
}

juce::StringArray MainComponent::getMenuBarNames() { return { "File", "Edit", "Track", "Mix", "Navigate", "View", "Help" }; }

juce::PopupMenu MainComponent::getMenuForIndex (int index, const juce::String&)
{
    juce::PopupMenu menu;
    auto add = [&menu] (Cmd c) {
        const auto& info = commandInfo (c);
        juce::PopupMenu::Item item (info.name);
        item.itemID = static_cast<int> (c) + 1;
        item.shortcutKeyDescription = info.shortcut;
        menu.addItem (item);
    };
    switch (index)
    {
        case 0: add (Cmd::save); break;
        case 1:
            for (auto c : { Cmd::undo, Cmd::redo })
                add (c);
            menu.addSeparator();
            for (auto c : { Cmd::cut, Cmd::copy, Cmd::paste, Cmd::deleteSelection, Cmd::selectAll })
                add (c);
            menu.addSeparator();
            for (auto c : { Cmd::split, Cmd::join, Cmd::muteRegions, Cmd::repeat, Cmd::crossfade, Cmd::loopRegion, Cmd::nudgeLeft, Cmd::nudgeRight,
                            Cmd::applyQuantize, Cmd::quantize, Cmd::muteNotes })
                add (c);
            menu.addSeparator();
            add (Cmd::toolMenu);
            break;
        case 2:
            add (Cmd::newTrack);
            add (Cmd::deleteTrack);
            break;
        case 3:
            add (Cmd::newBus);
            add (Cmd::toggleMixer);
            menu.addSeparator();
            add (Cmd::pluginManager);
            break;
        case 4:
            for (auto c : { Cmd::playStop, Cmd::record, Cmd::cycle, Cmd::setCycleToSelection, Cmd::goToStart, Cmd::goToEnd, Cmd::previousBar,
                            Cmd::nextBar, Cmd::addMarker, Cmd::previousMarker, Cmd::nextMarker, Cmd::metronome, Cmd::autopunch })
                add (c);
            break;
        case 5:
            for (auto c : { Cmd::toggleInspector, Cmd::toggleSidePanel, Cmd::toggleMixer, Cmd::togglePianoRoll, Cmd::toggleAudioEditor,
                            Cmd::toggleGlobalTracks, Cmd::toggleAutomation })
                add (c);
            menu.addSeparator();
            for (auto c : { Cmd::zoomToFit, Cmd::zoomIn, Cmd::zoomOut, Cmd::followPlayhead, Cmd::snapToggle, Cmd::keyboardMidi, Cmd::toggleTheme })
                add (c);
            break;
        case 6: add (Cmd::about); break;
        default: break;
    }
    return menu;
}

void MainComponent::menuItemSelected (int itemId, int)
{
    if (itemId > 0)
        run (static_cast<Cmd> (itemId - 1));
}
} // namespace weft::ui
