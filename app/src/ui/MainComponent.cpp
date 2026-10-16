#include "MainComponent.h"

#include "AboutPanel.h"
#include "AudioSetup.h"
#include "BounceDialog.h"
#include "NewTrackDialog.h"
#include "ProjectDialogs.h"
#include "ShareDialogs.h"

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
    const auto sync = syncArea();
    g.setColour (studio.shared() && studio.sync() != nullptr && studio.sync()->state() == collab::SyncState::synced ? colour (Token::text)
                                                                                                                  : colour (Token::textDim));
    g.drawText (studio.syncStatus(), sync, juce::Justification::centred, true);
    r.setRight (std::min (r.getRight(), sync.getX() - 8));
    g.setColour (colour (Token::text));
    g.drawText (studio.status, r, juce::Justification::centredLeft, true);
}

juce::Rectangle<int> StatusBar::syncArea() const { return { getWidth() / 2 - 140, 0, 280, getHeight() }; }

void StatusBar::mouseUp (const juce::MouseEvent& e)
{
    if (syncArea().contains (e.getPosition()) && onSyncClicked)
        onSyncClicked();
}

// Read-only banner ------------------------------------------------------------------------------

ReadOnlyBanner::ReadOnlyBanner()
{
    setTitle ("Read-only");
    addAndMakeVisible (message);
}

void ReadOnlyBanner::paint (juce::Graphics& g) { g.fillAll (colour (Token::accent).withAlpha (0.22f)); }

void ReadOnlyBanner::resized() { message.setBounds (getLocalBounds().reduced (10, 2)); }

// No-audio banner -------------------------------------------------------------------------------

NoAudioBanner::NoAudioBanner()
{
    setTitle ("No audio device");
    message.setText ("No audio device: playback is silent and recording is off.", juce::dontSendNotification);
    describe (setup, "Audio Setup...", "Choose an audio device");
    addAndMakeVisible (message);
    addAndMakeVisible (setup);
}

void NoAudioBanner::paint (juce::Graphics& g) { g.fillAll (colour (Token::warning).withAlpha (0.25f)); }

void NoAudioBanner::resized()
{
    auto r = getLocalBounds().reduced (10, 3);
    setup.setBounds (r.removeFromRight (120));
    message.setBounds (r);
}

// Main component ------------------------------------------------------------------------------

MainComponent::MainComponent (Studio& s, const juce::File& browserRoot)
    : studio (s), control (s), inspectorPanel (s), tracksArea (s), bottomArea (s), sidePanel (s, browserRoot), status (s), plugins (s)
{
    setTitle ("Weft Studio");
    for (auto* c : std::initializer_list<juce::Component*> { &control, &inspectorPanel, &tracksArea, &bottomArea, &sidePanel, &status, &banner, &readOnly, &preview })
        addChildComponent (c);
    preview.restore.onClick = [this] { studio.restorePreviewed(); };
    preview.back.onClick = [this] { studio.returnToCurrent(); };
    studio.confirmBreakLock = [this] (const Id& trackId) {
        const auto* t = studio.model().track (trackId);
        const auto lock = studio.session().lockOf (trackId);
        if (t == nullptr || ! lock)
            return;
        ask ("Break Lock", studio.nameOf (lock->by) + " has locked \"" + juce::String (t->name) + "\". Break the lock? They'll be told.", "Break Lock", {},
             [this, trackId] (int choice) {
                 if (choice == 1)
                     studio.breakLock (trackId);
             });
    };
    studio.onShowThread = [this] (const Id& threadId, juce::Rectangle<int> area) {
        if (! desktopWindows() || area.isEmpty())
            return;
        auto view = std::make_unique<ThreadView> (studio, threadId);
        view->setSize (300, std::min (440, view->preferredHeight (300)));
        auto& box = juce::CallOutBox::launchAsynchronously (std::move (view), area, nullptr);
        juce::Component::SafePointer<juce::CallOutBox> safe (&box);
        if (auto* v = dynamic_cast<ThreadView*> (box.getChildComponent (0)))
            v->onClose = [safe] {
                if (safe != nullptr)
                    safe->dismiss();
            };
    };
    studio.onNotice = [this] (const commands::Notice& n) {
        if (n.kind == commands::Notice::Kind::lockBroken)
            alert ("Lock Broken", juce::String (n.text));
    };
    banner.setup.onClick = [this] { run (Cmd::audioSettings); };
    status.onSyncClicked = [this] { alert ("Sync", studio.syncDetails()); };
    studio.onMissingPlugins = [this] (const std::vector<Studio::MissingPlugin>& missing) {
        juce::StringArray lines;
        for (const auto& m : missing)
            lines.add (juce::String::fromUTF8 (m.name.c_str()) + (m.addedBy.empty() ? "" : ", added by " + juce::String::fromUTF8 (m.addedBy.c_str())));
        alert ("Missing Plug-ins", "A collaborator used plug-ins that aren't installed here. Their slots keep their settings and pass audio through:\n\n"
                                       + lines.joinIntoString ("\n"));
    };
    // Any input anywhere in the window keeps this user from showing as idle (05 §5).
    addMouseListener (this, true);
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
    studio.onMissingPlugins = nullptr;
    studio.confirmBreakLock = nullptr;
    studio.onShowThread = nullptr;
    studio.onNotice = nullptr;
    studio.removeChangeListener (this);
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    const auto title = studio.windowTitle();
    if (getName() != title)
    {
        setName (title);
        if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
            window->setName (title);
    }
    if (banner.isVisible() == studio.audioRunning())
    {
        banner.setVisible (! studio.audioRunning());
        resized();
    }
    const auto readOnlyText = studio.previewing() ? juce::String() : studio.readOnlyReason(); // the preview banner says it
    if (readOnly.message.getText() != readOnlyText)
        readOnly.message.setText (readOnlyText, juce::dontSendNotification);
    if (readOnly.isVisible() == readOnlyText.isEmpty())
    {
        readOnly.setVisible (readOnlyText.isNotEmpty());
        resized();
    }
    if (preview.isVisible() != studio.previewing())
    {
        preview.setVisible (studio.previewing());
        resized();
    }
    if (const auto* v = studio.session().model().version (studio.previewedVersion()))
        preview.message.setText (juce::String::fromUTF8 ("Viewing version '") + juce::String (v->name) + juce::String::fromUTF8 ("' \xe2\x80\x93 read-only."), juce::dontSendNotification);
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

void MainComponent::mouseMove (const juce::MouseEvent&) { studio.noteInput(); }
void MainComponent::mouseDown (const juce::MouseEvent&) { studio.noteInput(); }
void MainComponent::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) { studio.noteInput(); }

void MainComponent::paint (juce::Graphics& g) { g.fillAll (colour (Token::background)); }

void MainComponent::resized()
{
    auto r = getLocalBounds();
    control.setBounds (r.removeFromTop (52));
    status.setBounds (r.removeFromBottom (StatusBar::height));
    if (banner.isVisible())
        banner.setBounds (r.removeFromTop (NoAudioBanner::height));
    if (readOnly.isVisible())
        readOnly.setBounds (r.removeFromTop (ReadOnlyBanner::height));
    const auto& l = studio.layout;
    if (l.inspector)
        inspectorPanel.setBounds (r.removeFromLeft (std::clamp (l.inspectorWidth, 180, 420)));
    if (l.sidePanel)
        sidePanel.setBounds (r.removeFromRight (std::clamp (l.sidePanelWidth, 220, 520)));
    // The tracks area keeps at least 160 px (F-UI-1: it is always visible).
    if (l.bottom != BottomTab::none)
        bottomArea.setBounds (r.removeFromBottom (std::clamp (l.bottomHeight, 120, std::max (120, r.getHeight() - 160))));
    if (preview.isVisible())
        preview.setBounds (r.removeFromTop (PreviewBanner::height));
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
    studio.noteInput();
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
    else if (command == Cmd::bounce)
        BounceDialog::show (studio, this);
    else if (command == Cmd::newProject)
        whenSaved ([this] {
            if (desktopWindows())
                launchDialog (std::make_unique<NewProjectDialog> (studio), "New Project", this);
        });
    else if (command == Cmd::openProject)
        whenSaved ([this] { chooseProjectToOpen(); });
    else if (command == Cmd::saveAs)
    {
        if (desktopWindows())
            launchDialog (std::make_unique<SaveAsDialog> (studio), "Save As", this);
    }
    else if (command == Cmd::projectSettings)
    {
        if (desktopWindows())
            launchDialog (std::make_unique<ProjectSettingsDialog> (studio), "Project Settings", this);
    }
    else if (command == Cmd::signIn)
        whenSignedIn ([] {});
    else if (command == Cmd::shareProject)
        whenSignedIn ([this] {
            if (desktopWindows())
                launchDialog (std::make_unique<ShareDialog> (studio), studio.shared() ? "Invite" : "Share Project", this);
        });
    else if (command == Cmd::joinProject)
        whenSignedIn ([this] {
            whenSaved ([this] {
                if (desktopWindows())
                    launchDialog (std::make_unique<JoinDialog> (studio), "Join Project", this);
            });
        });
    else if (command == Cmd::addComment || command == Cmd::saveVersion)
    {
        // Shift+C writes a comment at the playhead on the selected track; Ctrl+Alt+S names a version (04 §9.3, §9.5).
        if (command == Cmd::addComment && ! studio.draftComment)
            studio.draftComment = studio.commentAnchorHere();
        studio.layout.sidePanel = true;
        sidePanel.showTab (command == Cmd::addComment ? SidePanel::commentsTab : SidePanel::historyTab);
        studio.sendChangeMessage();
        changeListenerCallback (nullptr);
        if (command == Cmd::addComment)
            sidePanel.comments().focusDraft();
        else if (sidePanel.history().name.isShowing())
            sidePanel.history().name.grabKeyboardFocus();
    }
    else if (command == Cmd::audioSettings && devices != nullptr && desktopWindows())
        launchDialog (std::make_unique<AudioSetupDialog> (studio, *devices, false), "Audio & MIDI Settings", this);
}

void MainComponent::whenSignedIn (std::function<void()> next)
{
    if (studio.signedIn())
    {
        next();
        return;
    }
    if (! desktopWindows())
        return;
    auto* dialog = launchDialog (std::make_unique<SignInDialog> (studio), "Sign In", this);
    // The next dialog opens once this window has closed.
    dialog->onSignedIn = [next = std::move (next)] { juce::MessageManager::callAsync (next); };
}

// Projects ------------------------------------------------------------------------------------

void MainComponent::ask (const juce::String& title, const juce::String& text, const juce::String& first, const juce::String& second,
                         std::function<void (int)> answer)
{
    projectAsked = text;
    projectAnswer = std::move (answer);
    if (! desktopWindows())
        return; // headless: answerProject() stands in for the buttons
    projectAlert = std::make_unique<juce::AlertWindow> (title, text, juce::MessageBoxIconType::QuestionIcon, this);
    projectAlert->addButton (first, 1, juce::KeyPress (juce::KeyPress::returnKey));
    if (second.isNotEmpty())
        projectAlert->addButton (second, 2);
    projectAlert->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    projectAlert->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<MainComponent> (this)] (int result) {
                                       if (safe != nullptr)
                                           safe->answerProject (result);
                                   }),
                                   false);
}

void MainComponent::answerProject (int choice)
{
    auto answer = std::move (projectAnswer);
    projectAnswer = nullptr;
    projectAsked.clear();
    if (projectAlert != nullptr)
    {
        projectAlert->exitModalState (0);
        projectAlert.reset();
    }
    if (answer)
        answer (choice);
}

void MainComponent::alert (const juce::String& title, const juce::String& text)
{
    alertText = text;
    if (desktopWindows())
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, title, text, "OK", this);
}

void MainComponent::whenSaved (std::function<void()> next)
{
    if (! studio.dirty() || studio.untouched())
    {
        next();
        return;
    }
    const auto name = juce::String::fromUTF8 (studio.model().info.name.c_str());
    ask ("Unsaved Changes", "Save changes to \"" + name + "\"?", "Save", "Don't Save", [this, next] (int choice) {
        if (choice == 1 && ! studio.save())
            return;
        if (choice == 2)
            studio.discardUnsavedChanges();
        if (choice != 0)
            next();
    });
}

void MainComponent::chooseProjectToOpen()
{
    if (! desktopWindows())
        return;
    openChooser = std::make_unique<juce::FileChooser> ("Open a Weft project", juce::File (pathText (studio.projectsFolder())), "project.automerge");
    openChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectDirectories,
                              [safe = juce::Component::SafePointer<MainComponent> (this)] (const juce::FileChooser& fc) {
                                  if (safe != nullptr && fc.getResult() != juce::File())
                                  {
                                      // The question about unsaved changes was already answered.
                                      safe->opened (safe->studio.openProject (textPath (fc.getResult().getFullPathName())));
                                  }
                              });
}

void MainComponent::openProject (const std::filesystem::path& picked)
{
    whenSaved ([this, picked] { opened (studio.openProject (picked)); });
}

void MainComponent::opened (const Studio::OpenResult& result)
{
    if (result.needsRecoveryChoice)
    {
        const auto when = juce::Time (result.recoveryTimeMs).toString (true, true, false);
        ask ("Recover Unsaved Changes", "Recover unsaved changes from " + when + "?", "Recover", "Discard",
             [this, path = result.root] (int choice) {
                 if (choice != 0)
                     opened (studio.openProject (path, choice == 1 ? Studio::Recovery::recover : Studio::Recovery::discard));
             });
        return;
    }
    if (! result.ok)
    {
        alert ("Couldn't Open the Project", juce::String::fromUTF8 (result.error.c_str()));
        return;
    }
    if (! result.missingPlugins.empty())
    {
        juce::StringArray names;
        for (const auto& m : studio.missingPluginList())
            names.add (juce::String::fromUTF8 (m.name.c_str()) + (m.addedBy.empty() ? "" : ", added by " + juce::String::fromUTF8 (m.addedBy.c_str())));
        alert ("Missing Plug-ins", "These plug-ins aren't installed. Their slots keep their settings and pass audio through:\n\n"
                                       + names.joinIntoString ("\n"));
    }
    if (! result.missingMedia.empty() && desktopWindows())
        launchDialog (std::make_unique<MissingMediaDialog> (studio), "Missing Media", this);
}

void MainComponent::openRecent (int index)
{
    const auto& items = studio.recentProjects().items();
    if (index < 0 || index >= static_cast<int> (items.size()))
        return;
    const auto root = items[static_cast<std::size_t> (index)];
    if (! project::RecentProjects::available (root))
    {
        studio.recentProjects().remove (root);
        studio.setStatus (pathText (root.filename()) + " isn't there any more; removed it from Open Recent.");
        return;
    }
    openProject (root);
}

void MainComponent::showWelcome()
{
    if (! desktopWindows())
        return;
    launchDialog (std::make_unique<WelcomeDialog> (
                      studio, [this] (Cmd c) { run (c); }, [this] (const std::filesystem::path& p) { openProject (p); }),
                  "Welcome", this);
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

juce::StringArray MainComponent::getMenuBarNames() { return { "File", "Edit", "Track", "Mix", "Navigate", "View", "Share", "Help" }; }

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
        case 0:
        {
            add (Cmd::newProject);
            add (Cmd::openProject);
            juce::PopupMenu recentMenu;
            const auto& items = studio.recentProjects().items();
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                auto label = pathText (items[i].filename()).upToLastOccurrenceOf (".weft", false, false);
                if (! project::RecentProjects::available (items[i]))
                    label += " (not found)";
                recentMenu.addItem (recentMenuBase + static_cast<int> (i) + 1, label);
            }
            menu.addSubMenu ("Open Recent", recentMenu, ! items.empty());
            menu.addSeparator();
            add (Cmd::save);
            add (Cmd::saveAs);
            add (Cmd::saveVersion);
            menu.addSeparator();
            add (Cmd::bounce);
            menu.addSeparator();
            add (Cmd::projectSettings);
            add (Cmd::audioSettings);
            break;
        }
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
            menu.addSeparator();
            add (Cmd::addComment);
            break;
        case 2:
            add (Cmd::newTrack);
            add (Cmd::deleteTrack);
            add (Cmd::lockTrack);
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
        case 6:
        {
            juce::PopupMenu::Item share (studio.shared() ? "Invite..." : commandInfo (Cmd::shareProject).name);
            share.itemID = static_cast<int> (Cmd::shareProject) + 1;
            menu.addItem (share);
            add (Cmd::joinProject);
            menu.addSeparator();
            if (studio.signedIn())
            {
                juce::PopupMenu::Item out ("Sign Out (" + juce::String (studio.account().displayName) + ")");
                out.itemID = static_cast<int> (Cmd::signOut) + 1;
                menu.addItem (out);
            }
            else
                add (Cmd::signIn);
            menu.addSeparator();
            juce::PopupMenu::Item offline (commandInfo (Cmd::goOffline).name);
            offline.itemID = static_cast<int> (Cmd::goOffline) + 1;
            offline.isTicked = ! studio.online();
            offline.isEnabled = studio.shared();
            menu.addItem (offline);
            break;
        }
        case 7: add (Cmd::about); break;
        default: break;
    }
    return menu;
}

void MainComponent::menuItemSelected (int itemId, int)
{
    if (itemId > recentMenuBase)
        openRecent (itemId - recentMenuBase - 1);
    else if (itemId > 0)
        run (static_cast<Cmd> (itemId - 1));
}
} // namespace weft::ui
