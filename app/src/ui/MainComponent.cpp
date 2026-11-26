#include "MainComponent.h"

#include "AboutPanel.h"
#include "AudioSetup.h"
#include "BounceDialog.h"
#include "NewTrackDialog.h"
#include "PluginSearch.h"
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
    depth::highlight (g, getLocalBounds().toFloat().withTrimmedTop (1.0f), 0.0f);
    g.setFont (font (Type::body));
    auto r = getLocalBounds().reduced (space::s1, 0);
    g.setColour (colour (Token::textDim));
    const auto right = juce::String (studio.model().info.name) + (studio.dirty() ? " (edited)" : "") + "   ·   Tool: " + toolName (studio.tool)
                       + "   ·   Snap: " + (studio.snapEnabled ? arrange::snapLabel (studio.snap) : juce::String ("Off"));
    g.drawText (right, r.removeFromRight (420), juce::Justification::centredRight);
    const auto sync = syncArea();
    g.setColour (studio.shared() && studio.sync() != nullptr && studio.sync()->state() == collab::SyncState::synced ? colour (Token::text)
                                                                                                                  : colour (Token::textDim));
    g.drawText (studio.syncStatus(), sync, juce::Justification::centred, true);
    r.setRight (std::min (r.getRight(), sync.getX() - space::s1));
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

void ReadOnlyBanner::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::surfaceRaised));
    g.setColour (colour (Token::accent));
    g.fillRect (getLocalBounds().removeFromLeft (space::half));
}

void ReadOnlyBanner::resized() { message.setBounds (getLocalBounds().reduced (space::s2, space::half)); }

// No-audio banner -------------------------------------------------------------------------------

NoAudioBanner::NoAudioBanner()
{
    setTitle ("No audio device");
    message.setText ("No audio device: playback is silent and recording is off.", juce::dontSendNotification);
    describe (setup, "Audio Setup...", "Choose an audio device");
    addAndMakeVisible (message);
    addAndMakeVisible (setup);
}

void NoAudioBanner::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::surfaceRaised));
    g.setColour (colour (Token::warning));
    g.fillRect (getLocalBounds().removeFromLeft (space::half));
}

void NoAudioBanner::resized()
{
    auto r = getLocalBounds().reduced (space::s2, space::half);
    setup.setBounds (r.removeFromRight (120));
    message.setBounds (r);
}

// Gutters --------------------------------------------------------------------------------------

Gutter::Gutter (bool isVertical) : vertical (isVertical)
{
    setMouseCursor (vertical ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::UpDownResizeCursor);
    setTitle (vertical ? "Panel divider" : "Bottom area divider");
    setRepaintsOnMouseActivity (true);
}

void Gutter::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::background));
    if (isMouseOverOrDragging())
    {
        g.setColour (colour (Token::border));
        g.fillRect (vertical ? getLocalBounds().withSizeKeepingCentre (2, getHeight()) : getLocalBounds().withSizeKeepingCentre (getWidth(), 2));
    }
}

void Gutter::mouseDown (const juce::MouseEvent&)
{
    if (onPress)
        onPress();
}

void Gutter::mouseDrag (const juce::MouseEvent& e)
{
    if (onDrag)
        onDrag (vertical ? e.getDistanceFromDragStartX() : e.getDistanceFromDragStartY());
}

void Gutter::mouseDoubleClick (const juce::MouseEvent&)
{
    if (onDoubleClick)
        onDoubleClick();
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
    for (auto* edge : { &inspectorEdge, &sideEdge, &bottomEdge })
        addChildComponent (edge);
    auto resize = [this] (int PanelLayout::*field, int value) {
        studio.layout.*field = value;
        resized();
        if (onLayoutChanged)
            onLayoutChanged (studio.layout);
    };
    inspectorEdge.onPress = [this] { dragFrom = inspectorPanel.getWidth(); };
    inspectorEdge.onDrag = [this, resize] (int d) { resize (&PanelLayout::inspectorWidth, std::clamp (dragFrom + d, minInspector, maxInspector)); };
    sideEdge.onPress = [this] { dragFrom = sidePanel.getWidth(); };
    sideEdge.onDrag = [this, resize] (int d) { resize (&PanelLayout::sidePanelWidth, std::clamp (dragFrom - d, minSide, maxSide)); };
    bottomEdge.onPress = [this] { dragFrom = bottomArea.getHeight(); };
    bottomEdge.onDrag = [this, resize] (int d) { resize (&PanelLayout::bottomHeight, std::clamp (dragFrom - d, minBottom, maxBottom())); };
    // Double-clicking the bottom area's divider toggles it maximised (spec 04 §G).
    bottomEdge.onDoubleClick = [this, resize] {
        if (bottomArea.getHeight() >= maxBottom() - 1 && restoreHeight > 0)
            resize (&PanelLayout::bottomHeight, restoreHeight);
        else
        {
            restoreHeight = bottomArea.getHeight();
            resize (&PanelLayout::bottomHeight, maxBottom());
        }
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
    const auto bannerText = studio.lostDeviceName().empty()
                                ? juce::String ("No audio device: playback is silent and recording is off.")
                                : "\"" + juce::String (studio.lostDeviceName()) + "\" stopped: playback is silent and recording is off until it's back.";
    if (banner.message.getText() != bannerText)
        banner.message.setText (bannerText, juce::dontSendNotification);
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

void MainComponent::paintOverChildren (juce::Graphics& g)
{
    // Zones cast short shadows onto the ones beneath them: the control bar onto everything, the
    // inspector and track headers onto the timeline. It is what makes the canvas read as recessed.
    auto shade = [&g] (juce::Rectangle<int> edge, bool down) {
        if (edge.isEmpty())
            return;
        const auto r = edge.toFloat();
        const auto a = depth::shade (0.35f, 0.12f);
        g.setGradientFill (down ? juce::ColourGradient::vertical (a, r.getY(), a.withAlpha (0.0f), r.getBottom())
                                : juce::ColourGradient::horizontal (a, r.getX(), a.withAlpha (0.0f), r.getRight()));
        g.fillRect (r);
    };
    shade ({ 0, control.getBottom(), getWidth(), 6 }, true);
    if (inspectorPanel.isVisible())
        shade ({ inspectorPanel.getRight(), inspectorPanel.getY(), 5, inspectorPanel.getHeight() }, false);
}

int MainComponent::maxBottom() const
{
    // The tracks area keeps at least 160 px (F-UI-1: it is always visible); the bottom area may
    // take 80 % of the window (spec 04 §G).
    const auto room = getHeight() - control.getHeight() - StatusBar::height - 160;
    return std::max (minBottom, std::min (room, getHeight() * 4 / 5));
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    control.setBounds (r.removeFromTop (size::transport));
    status.setBounds (r.removeFromBottom (StatusBar::height));
    if (banner.isVisible())
        banner.setBounds (r.removeFromTop (NoAudioBanner::height));
    if (readOnly.isVisible())
        readOnly.setBounds (r.removeFromTop (ReadOnlyBanner::height));
    const auto& l = studio.layout;
    inspectorEdge.setVisible (l.inspector);
    sideEdge.setVisible (l.sidePanel);
    bottomEdge.setVisible (l.bottom != BottomTab::none);
    if (l.inspector)
    {
        inspectorPanel.setBounds (r.removeFromLeft (std::clamp (l.inspectorWidth, minInspector, maxInspector)));
        inspectorEdge.setBounds (r.removeFromLeft (size::gutter));
    }
    if (l.sidePanel)
    {
        sidePanel.setBounds (r.removeFromRight (std::clamp (l.sidePanelWidth, minSide, maxSide)));
        sideEdge.setBounds (r.removeFromRight (size::gutter));
    }
    if (l.bottom != BottomTab::none)
    {
        bottomArea.setBounds (r.removeFromBottom (std::clamp (l.bottomHeight, minBottom, maxBottom())));
        bottomEdge.setBounds (r.removeFromBottom (size::gutter));
    }
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
    for (int i = 0; i <= static_cast<int> (lastTool); ++i)
    {
        const auto t = static_cast<Tool> (i);
        juce::PopupMenu::Item item (toolName (t));
        item.itemID = static_cast<int> (t) + 1;
        item.isTicked = studio.tool == t;
        item.shortcutKeyDescription = toolKey (t); // one key picks it while the menu is open (D-UI-29)
        menu.addItem (item);
    }
    return menu;
}

void MainComponent::handleUiCommand (Cmd command)
{
    if (command == Cmd::newTrack)
        NewTrackDialog::show (studio, this);
    else if (command == Cmd::about)
        AboutPanel::show (this);
    else if (command == Cmd::toolMenu)
    {
        toolMenu().showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [this] (int chosen) { pickTool (chosen); });
        // The menu ignores letters; the tool keys reach this listener on its window (D-UI-29).
        auto& desktop = juce::Desktop::getInstance();
        for (int i = desktop.getNumComponents(); --i >= 0;)
            if (auto* window = desktop.getComponent (i); window != nullptr && window->getName() == "menu")
            {
                window->removeKeyListener (&toolKeys);
                window->addKeyListener (&toolKeys);
                break;
            }
    }
    else if (command == Cmd::renameMarker)
    {
        if (const auto* marker = studio.markerAtPlayhead())
            tracks().ruler().renameMarker (marker->id);
    }
    else if (command == Cmd::searchPlugins)
        PluginSearch::show (studio, this);
    else if (command == Cmd::goToPosition)
        control.positionDisplay().showEditor();
    else if (command == Cmd::configureTrackHeader)
        tracks().showHeaderOptions();
    else if (command == Cmd::togglePluginWindows)
        plugins.toggleAll();
    else if (command == Cmd::searchTracks)
    {
        if (auto* search = TrackSearch::show (studio, this))
        {
            juce::Component::SafePointer<MainComponent> self (this);
            search->onChosen = [self] (const Id& id) {
                if (self != nullptr)
                    self->tracks().revealTrack (id);
            };
        }
    }
    else if (command == Cmd::pluginManager)
        PluginManager::show (studio, this);
    else if (command == Cmd::deleteTrack)
        deleteSelectedTrack();
    else if (command == Cmd::bounce)
        BounceDialog::show (studio, this);
    else if (command == Cmd::exportRegions)
    {
        if (! desktopWindows())
            return;
        // A folder, starting at the project's Bounces folder; the files are named after the regions.
        const auto start = studio.projectRoot() / "Bounces";
        std::error_code ec;
        std::filesystem::create_directories (start, ec);
        importChooser = std::make_unique<juce::FileChooser> ("Export Regions to Folder", juce::File (pathText (start)));
        importChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                    [safe = juce::Component::SafePointer<MainComponent> (this)] (const juce::FileChooser& fc) {
                                        if (safe != nullptr && fc.getResult() != juce::File())
                                            safe->studio.exportSelectedRegions (textPath (fc.getResult().getFullPathName()));
                                    });
    }
    else if (command == Cmd::newProject)
        whenSaved ([this] {
            if (desktopWindows())
                launchDialog (std::make_unique<NewProjectDialog> (studio), "New Project", this);
        });
    else if (command == Cmd::openProject)
        whenSaved ([this] { chooseProjectToOpen(); });
    else if (command == Cmd::importAudio)
        chooseAudioToImport();
    else if (command == Cmd::quit)
    {
        if (auto* app = juce::JUCEApplicationBase::getInstance())
            app->systemRequestedQuit(); // asks about unsaved changes first
    }
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
    else if (command == Cmd::shareFolder)
        chooseSharedFolder();
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

void MainComponent::chooseSharedFolder()
{
    if (! desktopWindows())
        return;
    // Google Drive for desktop's usual drive; otherwise start in the home folder.
    juce::File start (juce::File::getSpecialLocation (juce::File::userHomeDirectory));
    for (const auto* guess : { "G:\\My Drive", "G:\\" })
        if (juce::File::isAbsolutePath (guess) && juce::File (guess).isDirectory())
        {
            start = juce::File (guess);
            break;
        }
    openChooser = std::make_unique<juce::FileChooser> ("Choose a folder your sync app (Google Drive, Dropbox, OneDrive) shares", start);
    openChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe = juce::Component::SafePointer<MainComponent> (this)] (const juce::FileChooser& fc) {
                                  if (safe == nullptr || fc.getResult() == juce::File())
                                      return;
                                  std::string error;
                                  if (safe->studio.shareInFolder (textPath (fc.getResult().getFullPathName()), &error))
                                      safe->alert ("Shared in a synced folder",
                                                   "Your collaborators open this project from their copy of the folder with File > Open:\n"
                                                       + pathText (safe->studio.projectRoot())
                                                       + "\n\nWeft saves your changes there and merges theirs every few seconds. Use the "
                                                         "relay for live sessions with cursors and presence.");
                                  else
                                      safe->alert ("Couldn't share the project", juce::String::fromUTF8 (error.c_str()));
                              });
}

void MainComponent::chooseAudioToImport()
{
    if (! desktopWindows())
        return;
    importChooser = std::make_unique<juce::FileChooser> ("Import Audio", juce::File(), "*.wav;*.wave;*.bwf;*.aif;*.aiff;*.aifc;*.flac;*.mp3");
    importChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                    | juce::FileBrowserComponent::canSelectMultipleItems,
                                [safe = juce::Component::SafePointer<MainComponent> (this)] (const juce::FileChooser& fc) {
                                    std::vector<std::filesystem::path> files;
                                    for (const auto& f : fc.getResults())
                                        files.push_back (textPath (f.getFullPathName()));
                                    if (safe != nullptr && ! files.empty())
                                        safe->studio.importAtPlayhead (files);
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

void MainComponent::deleteRecentProject (int index)
{
    const auto& items = studio.recentProjects().items();
    if (index < 0 || index >= static_cast<int> (items.size()))
        return;
    const auto root = items[static_cast<std::size_t> (index)];
    const auto name = pathText (root.filename()).upToLastOccurrenceOf (".weft", false, false);
    ask ("Delete Project", "Move \"" + name + "\" and its media to the Trash? You can restore it from there.", "Move to Trash", {},
         [this, root] (int answer) {
             if (answer != 1)
                 return;
             std::string error;
             if (! studio.deleteProject (root, &error))
                 studio.setStatus (juce::String::fromUTF8 (error.c_str()));
             studio.sendChangeMessage();
         });
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

bool MainComponent::ToolKeys::keyPressed (const juce::KeyPress& key, juce::Component*)
{
    if (key.getModifiers().isAnyModifierKeyDown())
        return false;
    const auto tool = toolForKey (key.getTextCharacter() != 0 ? key.getTextCharacter() : static_cast<juce::juce_wchar> (key.getKeyCode()));
    if (! tool)
        return false;
    juce::PopupMenu::dismissAllActiveMenus();
    main.pickTool (static_cast<int> (*tool) + 1);
    return true;
}

void MainComponent::pickTool (int menuItemId)
{
    if (menuItemId < 1 || menuItemId > static_cast<int> (lastTool) + 1)
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
            // Delete Project: any recent project but the open one, moved to the Trash after asking.
            juce::PopupMenu deleteMenu;
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                std::error_code ec;
                if (project::RecentProjects::available (items[i]) && ! std::filesystem::equivalent (items[i], studio.projectRoot(), ec))
                    deleteMenu.addItem (deleteProjectMenuBase + static_cast<int> (i) + 1,
                                        pathText (items[i].filename()).upToLastOccurrenceOf (".weft", false, false));
            }
            menu.addSubMenu ("Delete Project", deleteMenu, deleteMenu.getNumItems() > 0);
            menu.addSeparator();
            add (Cmd::save);
            add (Cmd::saveAs);
            add (Cmd::saveVersion);
            menu.addSeparator();
            add (Cmd::bounce);
            add (Cmd::importAudio);
            add (Cmd::exportRegions);
            menu.addSeparator();
            add (Cmd::projectSettings);
            add (Cmd::audioSettings);
            menu.addSeparator();
            add (Cmd::quit);
            break;
        }
        case 1:
            for (auto c : { Cmd::undo, Cmd::redo })
                add (c);
            menu.addSeparator();
            for (auto c : { Cmd::cut, Cmd::copy, Cmd::paste, Cmd::deleteSelection, Cmd::selectAll, Cmd::deselectAll, Cmd::invertSelection, Cmd::selectPrevious,
                            Cmd::selectNext })
                add (c);
            menu.addSeparator();
            for (auto c : { Cmd::split, Cmd::join, Cmd::muteRegions, Cmd::repeat, Cmd::crossfade, Cmd::loopRegion, Cmd::reverseRegions, Cmd::nudgeLeft, Cmd::nudgeRight,
                            Cmd::lengthenNotes, Cmd::shortenNotes, Cmd::slipLeft, Cmd::slipRight, Cmd::shuffleLeft, Cmd::shuffleRight })
                add (c);
            {
                // D-W3-11: what Nudge, Lengthen/Shorten and Slip move regions by.
                juce::PopupMenu nudge;
                const std::pair<Cmd, Studio::NudgeValue> values[] = { { Cmd::nudgeByGrid, Studio::NudgeValue::grid }, { Cmd::nudgeByBar, Studio::NudgeValue::bar },
                                                                       { Cmd::nudgeByBeat, Studio::NudgeValue::beat }, { Cmd::nudgeByDivision, Studio::NudgeValue::division },
                                                                       { Cmd::nudgeByTick, Studio::NudgeValue::tick } };
                for (const auto& [c, v] : values)
                    nudge.addItem (static_cast<int> (c) + 1, commandInfo (c).name, true, studio.nudgeValue == v);
                menu.addSubMenu ("Nudge Value", nudge);
            }
            for (auto c : { Cmd::applyQuantize, Cmd::quantize, Cmd::muteNotes })
                add (c);
            {
                // Piano roll note functions (D-W4-14, D-W4-15).
                juce::PopupMenu noteMenu;
                for (auto c : { Cmd::selectAllFollowing, Cmd::legato, Cmd::removeOverlaps, Cmd::noteStartToPlayhead, Cmd::noteEndToPlayhead,
                                Cmd::scaleQuantize })
                {
                    const auto& info = commandInfo (c);
                    juce::PopupMenu::Item item (info.name);
                    item.itemID = static_cast<int> (c) + 1;
                    item.shortcutKeyDescription = info.shortcut;
                    noteMenu.addItem (item);
                }
                menu.addSubMenu ("Note Functions", noteMenu);
            }
            menu.addSeparator();
            add (Cmd::toolMenu);
            add (Cmd::nextTool);
            add (Cmd::previousTool);
            menu.addSeparator();
            add (Cmd::addComment);
            break;
        case 2:
        {
            add (Cmd::newTrack);
            add (Cmd::newAudioTrack);
            add (Cmd::newInstrumentTrack);
            add (Cmd::newTrackNextInput);
            add (Cmd::duplicateTrack);
            add (Cmd::duplicateTrackWithRegions);
            add (Cmd::deleteTrack);
            add (Cmd::deleteUnusedTracks);
            menu.addSeparator();
            const auto* t = studio.model().track (studio.selectedTrack());
            const bool colourable = t != nullptr && t->kind != model::TrackKind::master;
            menu.addSubMenu ("Color", colourMenu (colourMenuBase, colourable && t->color.rgb.empty() ? t->color.palette : -1), colourable);
            add (Cmd::moveTrackUp);
            add (Cmd::moveTrackDown);
            menu.addSeparator();
            for (auto c : { Cmd::muteTrack, Cmd::soloTrack, Cmd::armTrack, Cmd::monitorTrack, Cmd::unmuteAll, Cmd::clearRecallSolo, Cmd::soloSelectedRegions })
                add (c);
            menu.addSeparator();
            add (Cmd::searchTracks);
            add (Cmd::lockTrack);
            menu.addSeparator();
            add (Cmd::toggleAllTracksOff);
            add (Cmd::configureTrackHeader);
            break;
        }
        case 3:
            add (Cmd::newBus);
            add (Cmd::toggleMixer);
            add (Cmd::cycleMixerMode);
            menu.addSeparator();
            add (Cmd::searchPlugins);
            add (Cmd::pluginManager);
            add (Cmd::togglePluginWindows);
            menu.addSeparator();
            add (Cmd::copyChannelStrip);
            add (Cmd::pasteChannelStrip);
            break;
        case 4:
        {
            for (auto c : { Cmd::playStop, Cmd::stop, Cmd::pause, Cmd::record, Cmd::discardRecording, Cmd::cycle, Cmd::setCycleToSelection, Cmd::goToStart, Cmd::goToEnd, Cmd::previousBar,
                            Cmd::nextBar, Cmd::addMarker, Cmd::previousMarker, Cmd::nextMarker, Cmd::renameMarker, Cmd::deleteMarker, Cmd::metronome, Cmd::autopunch, Cmd::tapTempo })
                add (c);
            // D-UI-35: the go-to, play-from and stop commands, and cycle moves.
            const auto subMenu = [] (std::initializer_list<Cmd> cmds) {
                juce::PopupMenu sub;
                for (auto c : cmds)
                {
                    const auto& info = commandInfo (c);
                    juce::PopupMenu::Item item (info.name);
                    item.itemID = static_cast<int> (c) + 1;
                    item.shortcutKeyDescription = info.shortcut;
                    sub.addItem (item);
                }
                return sub;
            };
            menu.addSubMenu ("Go To", subMenu ({ Cmd::goToPosition, Cmd::goToCycleStart, Cmd::goToCycleEnd, Cmd::goToSelectionStart, Cmd::goToSelectionEnd,
                                                 Cmd::goToLastLocate }));
            menu.addSubMenu ("Play From", subMenu ({ Cmd::playFromStart, Cmd::playFromCycleStart, Cmd::playFromSelection, Cmd::playFromWindowEdge,
                                                     Cmd::playFromPreviousBar, Cmd::playFromLastLocate }));
            menu.addSubMenu ("Stop And", subMenu ({ Cmd::stopGoToStart, Cmd::stopGoToCycleStart, Cmd::stopGoToLastLocate }));
            menu.addSubMenu ("Cycle Range", subMenu ({ Cmd::cycleForward, Cmd::cycleBack, Cmd::doubleCycle, Cmd::halveCycle }));
            const auto settings = studio.engine().settings();
            menu.addSeparator();
            juce::PopupMenu during;
            during.addItem (clickDuringRecord, "Recording", true, settings.metronomeDuring == engine::ClickDuring::record);
            during.addItem (clickDuringPlay, "Playback", true, settings.metronomeDuring == engine::ClickDuring::play);
            during.addItem (clickDuringBoth, "Recording and Playback", true, settings.metronomeDuring == engine::ClickDuring::both);
            menu.addSubMenu ("Click During", during);
            juce::PopupMenu clicks;
            clicks.addItem (clickBar, "Bar", true, settings.clickBars);
            clicks.addItem (clickBeat, "Beat", true, settings.clickBeats);
            clicks.addItem (clickDivision, "Division", true, settings.clickDivisions);
            clicks.addSeparator();
            clicks.addItem (clickOnlyCountIn, "Only During Count-in", true, settings.clickOnlyCountIn);
            juce::PopupMenu tone;
            static const char* const toneNames[] = { "Low", "Lower", "Normal", "Higher", "High" };
            static_assert (std::size (toneNames) == engine::Metronome::numTones);
            for (int t = 0; t < engine::Metronome::numTones; ++t)
                tone.addItem (clickTone0 + t, toneNames[t], true, settings.clickTone == t);
            clicks.addSubMenu ("Tone", tone);
            menu.addSubMenu ("Metronome Clicks", clicks);
            // The count-in lives here as well as in the control bar, which drops its menu in a narrow window (AT-UI-11).
            juce::PopupMenu countIn;
            countIn.addItem (countInOff, "Off", true, settings.countInBars == 0);
            countIn.addItem (countInOne, "1 Bar", true, settings.countInBars == 1);
            countIn.addItem (countInTwo, "2 Bars", true, settings.countInBars == 2);
            countIn.addSeparator();
            {
                juce::PopupMenu::Item toggle ("Count-in On/Off");
                toggle.itemID = static_cast<int> (Cmd::countIn) + 1;
                toggle.shortcutKeyDescription = commandInfo (Cmd::countIn).shortcut;
                countIn.addItem (toggle);
            }
            menu.addSubMenu ("Count-in", countIn);
            menu.addItem (returnToStart, "Return to Start on Stop", true, settings.returnToStartOnStop);
            juce::PopupMenu midi;
            midi.addItem (mergeLinear, "Merge into Existing Region", true, settings.midiMergeLinear);
            midi.addItem (mergeCycle, "Merge Cycle Passes", true, settings.midiMergeCycle);
            midi.addSeparator();
            midi.addItem (replaceMidi, "Replace", true, settings.midiReplace);
            menu.addSubMenu ("MIDI Recording", midi);
            break;
        }
        case 5:
            for (auto c : { Cmd::toggleInspector, Cmd::toggleSidePanel, Cmd::toggleMixer, Cmd::togglePianoRoll, Cmd::toggleAudioEditor,
                            Cmd::toggleGlobalTracks, Cmd::toggleAutomation })
                add (c);
            menu.addSeparator();
            for (auto c : { Cmd::zoomToFit, Cmd::zoomIn, Cmd::zoomOut, Cmd::followPlayhead, Cmd::snapToggle, Cmd::snapZeroCrossings, Cmd::showHiddenTracks, Cmd::keyboardMidi,
                            Cmd::toggleTheme })
                add (c);
            menu.addItem (static_cast<int> (Cmd::gridLines) + 1, commandInfo (Cmd::gridLines).name, true, studio.gridLines);
            {
                // D-UI-38: vertical zoom, saved zooms, navigation and paging.
                juce::PopupMenu zoom;
                for (auto c : { Cmd::zoomVerticalIn, Cmd::zoomVerticalOut, Cmd::zoomTrack, Cmd::saveZoom1, Cmd::saveZoom2, Cmd::saveZoom3, Cmd::recallZoom1,
                                Cmd::recallZoom2, Cmd::recallZoom3, Cmd::navigateBack, Cmd::navigateForward, Cmd::pageUp, Cmd::pageDown, Cmd::pageLeft, Cmd::pageRight })
                {
                    const auto& info = commandInfo (c);
                    juce::PopupMenu::Item item (info.name);
                    item.itemID = static_cast<int> (c) + 1;
                    item.shortcutKeyDescription = info.shortcut;
                    if (c == Cmd::saveZoom1 || c == Cmd::recallZoom1 || c == Cmd::navigateBack || c == Cmd::pageUp)
                        zoom.addSeparator();
                    zoom.addItem (item);
                }
                menu.addSubMenu ("Zoom and Navigation", zoom);
            }
            break;
        case 6:
        {
            juce::PopupMenu::Item share (studio.shared() ? "Invite..." : commandInfo (Cmd::shareProject).name);
            share.itemID = static_cast<int> (Cmd::shareProject) + 1;
            menu.addItem (share);
            add (Cmd::joinProject);
            menu.addSeparator();
            {
                juce::PopupMenu::Item folderShare (commandInfo (Cmd::shareFolder).name);
                folderShare.itemID = static_cast<int> (Cmd::shareFolder) + 1;
                folderShare.isEnabled = ! studio.inSharedFolder();
                menu.addItem (folderShare);
            }
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
    if (itemId > clickDuringRecord - 1 && itemId <= clickToneLast)
    {
        auto settings = studio.engine().settings();
        switch (itemId)
        {
            case clickDuringRecord: settings.metronomeDuring = engine::ClickDuring::record; break;
            case clickDuringPlay: settings.metronomeDuring = engine::ClickDuring::play; break;
            case clickDuringBoth: settings.metronomeDuring = engine::ClickDuring::both; break;
            case returnToStart: settings.returnToStartOnStop = ! settings.returnToStartOnStop; break;
            case countInOff: settings.countInBars = 0; break;
            case countInOne: settings.countInBars = 1; break;
            case countInTwo: settings.countInBars = 2; break;
            case mergeLinear: settings.midiMergeLinear = ! settings.midiMergeLinear; break;
            case mergeCycle: settings.midiMergeCycle = ! settings.midiMergeCycle; break;
            case replaceMidi: settings.midiReplace = ! settings.midiReplace; break;
            case clickBar: settings.clickBars = ! settings.clickBars; break;
            case clickBeat: settings.clickBeats = ! settings.clickBeats; break;
            case clickDivision: settings.clickDivisions = ! settings.clickDivisions; break;
            case clickOnlyCountIn: settings.clickOnlyCountIn = ! settings.clickOnlyCountIn; break;
            default:
                if (itemId >= clickTone0 && itemId <= clickToneLast)
                    settings.clickTone = itemId - clickTone0;
                else
                    return;
        }
        studio.engine().setSettings (settings);
        studio.sendChangeMessage();
    }
    else if (itemId > deleteProjectMenuBase)
        deleteRecentProject (itemId - deleteProjectMenuBase - 1);
    else if (itemId > colourMenuBase)
        studio.check (studio.session().setTrackColor (studio.selectedTrack(), model::Color { itemId - colourMenuBase - 1, {} }));
    else if (itemId > recentMenuBase)
        openRecent (itemId - recentMenuBase - 1);
    else if (itemId > 0)
        run (static_cast<Cmd> (itemId - 1));
}
} // namespace weft::ui
