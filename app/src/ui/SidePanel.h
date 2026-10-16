#pragma once

// G · Side panel (spec 04 §8, §9): the file browser, the project's media list, and the
// Comments, Chat and History tabs.

#include "CollabPanels.h"
#include "Studio.h"
#include "Widgets.h"

#include "weft/arrange/MediaBrowser.h"

namespace weft::ui
{
/** Folders plus the audio and MIDI files the importer can read (F-LIB-1). */
class SupportedFileFilter final : public juce::FileFilter
{
public:
    explicit SupportedFileFilter (const media::DecoderSet& d) : juce::FileFilter ("Audio and MIDI files"), decoders (d) {}
    bool isFileSuitable (const juce::File& file) const override;
    bool isDirectorySuitable (const juce::File&) const override { return true; }

private:
    const media::DecoderSet& decoders;
};

class FileBrowser final : public juce::Component, public juce::FileBrowserListener
{
public:
    FileBrowser (Studio& studio, const juce::File& root);
    ~FileBrowser() override;
    void resized() override;
    void setRoot (const juce::File& folder);
    juce::FileTreeComponent& tree() noexcept { return fileTree; }
    juce::DirectoryContentsList& contents() noexcept { return list; }

    void selectionChanged() override {}
    /** A click on an audio file previews it (F-LIB-1). */
    void fileClicked (const juce::File& file, const juce::MouseEvent&) override;
    void fileDoubleClicked (const juce::File&) override {}
    void browserRootChanged (const juce::File&) override {}

private:

    Studio& studio;
    SupportedFileFilter filter;
    juce::TimeSliceThread thread { "File browser" };
    juce::DirectoryContentsList list { &filter, thread };
    juce::FileTreeComponent fileTree { list };
    juce::Label path;
    juce::TextButton up { "Up" };
};

class MediaList final : public juce::Component, private juce::ListBoxModel, private juce::ChangeListener
{
public:
    explicit MediaList (Studio& studio);
    ~MediaList() override;
    void resized() override { box.setBounds (getLocalBounds()); }
    const std::vector<arrange::MediaUsage>& rows() const noexcept { return items; }

private:
    int getNumRows() override { return static_cast<int> (items.size()); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    Studio& studio;
    juce::ListBox box;
    std::vector<arrange::MediaUsage> items;
};

class SidePanel final : public juce::Component, private juce::ChangeListener
{
public:
    SidePanel (Studio& studio, const juce::File& browserRoot);
    ~SidePanel() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    FileBrowser& browser() noexcept { return *fileBrowser; }
    MediaList& media() noexcept { return *mediaList; }
    CommentsPanel& comments() noexcept { return *commentsPanel; }
    ChatPanel& chat() noexcept { return *chatPanel; }
    HistoryPanel& history() noexcept { return *historyPanel; }
    juce::TabbedComponent& tabs() noexcept { return tabbed; }
    enum Tab
    {
        browserTab,
        mediaTab,
        commentsTab,
        chatTab,
        historyTab
    };
    void showTab (Tab tab);
    /** Tab names with the unread markers: "Comments •", "Chat (2)" (04 §9.3, §9.4). */
    void refreshTabs();

private:
    class Tabs final : public juce::TabbedComponent
    {
    public:
        explicit Tabs (SidePanel& p) : juce::TabbedComponent (juce::TabbedButtonBar::TabsAtTop), panel (p) {}
        void currentTabChanged (int, const juce::String&) override { panel.refreshTabs(); }

    private:
        SidePanel& panel;
    };
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refreshTabs(); }
    Studio& studio;
    Tabs tabbed { *this };
    FileBrowser* fileBrowser = nullptr;
    MediaList* mediaList = nullptr;
    CommentsPanel* commentsPanel = nullptr;
    ChatPanel* chatPanel = nullptr;
    HistoryPanel* historyPanel = nullptr;
};
} // namespace weft::ui
