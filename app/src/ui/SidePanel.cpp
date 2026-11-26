#include "SidePanel.h"

#include "BottomArea.h"

namespace weft::ui
{
bool SupportedFileFilter::isFileSuitable (const juce::File& file) const
{
    const std::filesystem::path p (file.getFullPathName().toStdString());
    if (! (engine::isMidiFile (p) || decoders.canDecode (p)))
        return false;
    const std::scoped_lock hold (lock);
    const auto name = file.getFileName();
    for (const auto& w : words)
        if (! name.containsIgnoreCase (w))
            return false;
    return true;
}

void SupportedFileFilter::setSearch (const juce::String& text)
{
    auto split = juce::StringArray::fromTokens (text, " ", "");
    split.removeEmptyStrings();
    const std::scoped_lock hold (lock);
    words = split;
}

juce::String SupportedFileFilter::search() const
{
    const std::scoped_lock hold (lock);
    return words.joinIntoString (" ");
}

FileBrowser::FileBrowser (Studio& s, const juce::File& root) : studio (s), filter (s.decoders)
{
    setTitle ("File browser");
    thread.startThread (juce::Thread::Priority::low);
    fileTree.setTitle ("Files");
    fileTree.setDragAndDropDescription ("weft-files");
    fileTree.setMultiSelectEnabled (true);
    fileTree.addListener (this);
    fileTree.setItemHeight (size::control);
    describe (back, "Back", "Go back to the previous folder");
    describe (forward, "Forward", "Go forward again");
    describe (up, "Up", "Go to the parent folder");
    describe (home, "Home", "Go to your home folder");
    describe (project, "Project", "Go to this project's folder");
    back.onClick = [this] { goBack(); };
    forward.onClick = [this] { goForward(); };
    up.onClick = [this] { setRoot (list.getDirectory().getParentDirectory()); };
    home.onClick = [this] { goHome(); };
    project.onClick = [this] { goToProject(); };
    searchBox.setTextToShowWhenEmpty ("Search", colour (Token::textDim));
    searchBox.setTitle ("Search files");
    searchBox.setTooltip ("Show only files whose name has every word typed here");
    searchBox.onTextChange = [this] { setSearch (searchBox.getText()); };
    searchBox.onEscapeKey = [this] { searchBox.setText ({}); };
    describe (play, "Play", "Play the selected file, or stop the preview");
    play.onClick = [this] { playOrStop(); };
    volume.setRange (-60.0, 0.0, 0.1);
    volume.setValue (0.0, juce::dontSendNotification);
    volume.setDoubleClickReturnValue (true, 0.0);
    volume.setTextValueSuffix (" dB");
    describe (volume, "Preview volume");
    volume.onValueChange = [this] { setPreviewVolumeDb (volume.getValue()); };
    path.setFont (font (Type::strong));
    path.setMinimumHorizontalScale (1.0f);
    describe (path, "Folder");
    for (juce::Component* c : { static_cast<juce::Component*> (&back), static_cast<juce::Component*> (&forward), static_cast<juce::Component*> (&up),
                                static_cast<juce::Component*> (&home), static_cast<juce::Component*> (&project), static_cast<juce::Component*> (&path),
                                static_cast<juce::Component*> (&searchBox), static_cast<juce::Component*> (&fileTree), static_cast<juce::Component*> (&play),
                                static_cast<juce::Component*> (&volume) })
        addAndMakeVisible (c);
    setRoot (root);
}

FileBrowser::~FileBrowser()
{
    stopTimer();
    fileTree.removeListener (this);
    thread.stopThread (2000);
}

void FileBrowser::setRoot (const juce::File& folder)
{
    if (historyPos < 0 || history[static_cast<std::size_t> (historyPos)] != folder)
    {
        history.resize (static_cast<std::size_t> (historyPos + 1)); // a new place drops the forward history
        history.push_back (folder);
        constexpr std::size_t keep = 50;
        if (history.size() > keep)
            history.erase (history.begin(), history.end() - static_cast<std::ptrdiff_t> (keep));
        historyPos = static_cast<int> (history.size()) - 1;
    }
    show (folder);
}

void FileBrowser::show (const juce::File& folder)
{
    if (list.getDirectory() == folder)
        list.refresh();
    else
        list.setDirectory (folder, true, true);
    // The folder's name; the whole path is in the tooltip.
    path.setText (folder.getFileName().isEmpty() ? folder.getFullPathName() : folder.getFileName(), juce::dontSendNotification);
    path.setTooltip (folder.getFullPathName());
    updateButtons();
}

void FileBrowser::goBack()
{
    if (! canGoBack())
        return;
    --historyPos;
    show (history[static_cast<std::size_t> (historyPos)]);
}

void FileBrowser::goForward()
{
    if (! canGoForward())
        return;
    ++historyPos;
    show (history[static_cast<std::size_t> (historyPos)]);
}

void FileBrowser::goHome() { setRoot (juce::File::getSpecialLocation (juce::File::userHomeDirectory)); }

void FileBrowser::goToProject()
{
    const juce::File folder (juce::String (studio.projectRoot().string()));
    if (studio.projectRoot().empty() || ! folder.isDirectory())
        return;
    setRoot (folder);
}

void FileBrowser::setSearch (const juce::String& text)
{
    if (searchBox.getText() != text)
        searchBox.setText (text, juce::dontSendNotification);
    filter.setSearch (text);
    list.refresh();
    // Subfolders the tree has opened keep their own lists; reopening the root rebuilds them.
    fileTree.refresh();
}

void FileBrowser::playOrStop()
{
    if (studio.engine().previewing())
    {
        studio.engine().stopPreview();
        updateButtons();
        return;
    }
    const auto file = fileTree.getSelectedFile();
    if (file == juce::File() || file.isDirectory())
    {
        studio.setStatus ("Select an audio file to play.");
        return;
    }
    const std::filesystem::path p (file.getFullPathName().toStdString());
    if (engine::isMidiFile (p))
    {
        studio.setStatus ("MIDI files don't preview yet.");
        return;
    }
    if (studio.previewFile (p))
    {
        play.setButtonText ("Stop");
        startTimer (100);
    }
}

void FileBrowser::setPreviewVolumeDb (double db)
{
    if (std::abs (volume.getValue() - db) > 1e-9)
        volume.setValue (db, juce::dontSendNotification);
    studio.engine().setPreviewGain (db <= -60.0 ? 0.0f : engine::dbToGain (db));
}

void FileBrowser::updateButtons()
{
    back.setEnabled (canGoBack());
    forward.setEnabled (canGoForward());
    project.setEnabled (! studio.projectRoot().empty());
}

void FileBrowser::timerCallback()
{
    // The preview ends by itself; the button goes back to Play when it does.
    if (! studio.engine().previewing())
    {
        play.setButtonText ("Play");
        stopTimer();
    }
}

void FileBrowser::fileClicked (const juce::File& file, const juce::MouseEvent&)
{
    if (file.isDirectory())
        return;
    const std::filesystem::path p (file.getFullPathName().toStdString());
    if (! engine::isMidiFile (p) && studio.previewFile (p)) // click previews (F-LIB-1)
    {
        play.setButtonText ("Stop");
        startTimer (100);
    }
}

void FileBrowser::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (size::panelHeader).reduced (space::s1, space::half);
    const int small = std::max (24, top.getHeight());
    back.setBounds (top.removeFromLeft (small));
    forward.setBounds (top.removeFromLeft (small).withTrimmedLeft (2));
    up.setBounds (top.removeFromLeft (40).withTrimmedLeft (space::half));
    home.setBounds (top.removeFromLeft (52).withTrimmedLeft (space::half));
    project.setBounds (top.removeFromLeft (64).withTrimmedLeft (space::half));
    auto second = r.removeFromTop (size::panelHeader).reduced (space::s1, space::half);
    path.setBounds (second.removeFromLeft (second.getWidth() / 2));
    searchBox.setBounds (second.withTrimmedLeft (space::half));
    auto bottom = r.removeFromBottom (size::panelHeader).reduced (space::s1, space::half);
    play.setBounds (bottom.removeFromLeft (56));
    volume.setBounds (bottom.withTrimmedLeft (space::s1));
    fileTree.setBounds (r);
}

MediaList::MediaList (Studio& s) : studio (s)
{
    setTitle ("Project media");
    box.setModel (this);
    box.setRowHeight (space::s6 - space::s1);
    box.setTitle ("Project media");
    addAndMakeVisible (box);
    studio.addChangeListener (this);
    changeListenerCallback (nullptr);
}

MediaList::~MediaList() { studio.removeChangeListener (this); }

void MediaList::changeListenerCallback (juce::ChangeBroadcaster*)
{
    auto now = arrange::mediaUsage (studio.model());
    const bool same = now.size() == items.size()
                      && std::equal (now.begin(), now.end(), items.begin(), [] (const auto& a, const auto& b) {
                             return a.hash == b.hash && a.regions == b.regions && a.name == b.name;
                         });
    if (same)
        return;
    items = std::move (now);
    box.updateContent();
    box.repaint();
}

void MediaList::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= getNumRows())
        return;
    const auto& item = items[static_cast<std::size_t> (row)];
    if (selected)
    {
        g.fillAll (colour (Token::hover));
        g.setColour (colour (Token::accent));
        g.fillRect (0, 0, 2, height);
    }
    g.setColour (colour (Token::lineSubtle));
    g.fillRect (space::s2, height - 1, width - space::s2, 1);
    g.setColour (colour (Token::text));
    g.setFont (font (Type::body));
    g.drawText (juce::String (item.name), space::s2, 2, width - space::s3, height / 2, juce::Justification::bottomLeft, true);
    g.setColour (colour (Token::textDim));
    g.setFont (font (Type::monoSmall));
    const auto used = item.regions == 1 ? juce::String ("1 region") : juce::String (item.regions) + " regions";
    g.drawText (juce::String (item.seconds, 1) + " s  ·  " + used, space::s2, height / 2, width - space::s3, height / 2 - 2, juce::Justification::topLeft);
}

SidePanel::SidePanel (Studio& s, const juce::File& browserRoot) : studio (s)
{
    setTitle ("Side panel");
    fileBrowser = new FileBrowser (studio, browserRoot);
    mediaList = new MediaList (studio);
    commentsPanel = new CommentsPanel (studio);
    chatPanel = new ChatPanel (studio);
    historyPanel = new HistoryPanel (studio);
    const auto tab = colour (Token::surface);
    tabbed.addTab ("Browser", tab, fileBrowser, true);
    tabbed.addTab ("Media", tab, mediaList, true);
    tabbed.addTab ("Comments", tab, commentsPanel, true);
    tabbed.addTab ("Chat", tab, chatPanel, true);
    tabbed.addTab ("History", tab, historyPanel, true);
    tabbed.setTabBarDepth (size::panelHeader);
    tabbed.setOutline (0);
    addAndMakeVisible (tabbed);
    studio.addChangeListener (this);
    refreshTabs();
}

SidePanel::~SidePanel() { studio.removeChangeListener (this); }

void SidePanel::showTab (Tab tab) { tabbed.setCurrentTabIndex (static_cast<int> (tab)); }

void SidePanel::refreshTabs()
{
    // Looking at a tab reads it.
    const bool visible = studio.layout.sidePanel;
    const auto current = tabbed.getCurrentTabIndex();
    if (visible && current == commentsTab)
        studio.markCommentsSeen();
    if (visible && current == chatTab)
        studio.markChatRead();
    const auto unread = studio.unreadChat();
    const auto comments = studio.commentsUnread() ? juce::String::fromUTF8 ("Comments \xe2\x80\xa2") : juce::String ("Comments");
    const juce::String chat = unread > 0 ? "Chat (" + juce::String (unread) + ")" : juce::String ("Chat");
    if (tabbed.getNumTabs() <= historyTab)
        return;
    if (tabbed.getTabNames()[commentsTab] != comments)
        tabbed.setTabName (commentsTab, comments);
    if (tabbed.getTabNames()[chatTab] != chat)
        tabbed.setTabName (chatTab, chat);
}

void SidePanel::paint (juce::Graphics& g) { g.fillAll (colour (Token::surface)); }
void SidePanel::resized() { tabbed.setBounds (getLocalBounds().withTrimmedLeft (1)); }
} // namespace weft::ui
