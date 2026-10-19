#include "SidePanel.h"

#include "BottomArea.h"

namespace weft::ui
{
bool SupportedFileFilter::isFileSuitable (const juce::File& file) const
{
    const std::filesystem::path p (file.getFullPathName().toStdString());
    return engine::isMidiFile (p) || decoders.canDecode (p);
}

FileBrowser::FileBrowser (Studio& s, const juce::File& root) : studio (s), filter (s.decoders)
{
    setTitle ("File browser");
    thread.startThread (juce::Thread::Priority::low);
    fileTree.setTitle ("Files");
    fileTree.setDragAndDropDescription ("weft-files");
    fileTree.setMultiSelectEnabled (true);
    fileTree.addListener (this);
    fileTree.setColour (juce::TreeView::backgroundColourId, colour (Token::surface));
    describe (up, "Up", "Go to the parent folder");
    up.onClick = [this] { setRoot (list.getDirectory().getParentDirectory()); };
    path.setColour (juce::Label::textColourId, colour (Token::textDim));
    describe (path, "Folder");
    addAndMakeVisible (up);
    addAndMakeVisible (path);
    addAndMakeVisible (fileTree);
    setRoot (root);
}

FileBrowser::~FileBrowser()
{
    fileTree.removeListener (this);
    thread.stopThread (2000);
}

void FileBrowser::setRoot (const juce::File& folder)
{
    if (list.getDirectory() == folder)
        list.refresh();
    else
        list.setDirectory (folder, true, true);
    path.setText (folder.getFullPathName(), juce::dontSendNotification);
}

void FileBrowser::fileClicked (const juce::File& file, const juce::MouseEvent&)
{
    if (file.isDirectory())
        return;
    const std::filesystem::path p (file.getFullPathName().toStdString());
    if (! engine::isMidiFile (p))
        studio.previewFile (p); // click previews (F-LIB-1)
}

void FileBrowser::resized()
{
    auto r = getLocalBounds().reduced (4);
    auto top = r.removeFromTop (26);
    up.setBounds (top.removeFromLeft (44));
    path.setBounds (top.withTrimmedLeft (6));
    r.removeFromTop (4);
    fileTree.setBounds (r);
}

MediaList::MediaList (Studio& s) : studio (s)
{
    setTitle ("Project media");
    box.setModel (this);
    box.setRowHeight (36);
    box.setTitle ("Project media");
    box.setColour (juce::ListBox::backgroundColourId, colour (Token::surface));
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
        g.fillAll (colour (Token::surfaceRaised));
    g.setColour (colour (Token::text));
    g.setFont (font (Type::body));
    g.drawText (juce::String (item.name), 8, 2, width - 16, height / 2, juce::Justification::bottomLeft, true);
    g.setColour (colour (Token::textDim));
    g.setFont (font (Type::small));
    const auto used = item.regions == 1 ? juce::String ("1 region") : juce::String (item.regions) + " regions";
    g.drawText (juce::String (item.seconds, 1) + " s  ·  " + used, 8, height / 2, width - 16, height / 2 - 2, juce::Justification::topLeft);
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
    tabbed.setTabBarDepth (28);
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
