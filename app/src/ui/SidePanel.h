#pragma once

// G · Side panel (spec 04 §8): the file browser and the project's media list; comments,
// chat and history are placeholders until the collaboration milestones.

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

class SidePanel final : public juce::Component
{
public:
    SidePanel (Studio& studio, const juce::File& browserRoot);
    void resized() override;
    void paint (juce::Graphics&) override;
    FileBrowser& browser() noexcept { return *fileBrowser; }
    MediaList& media() noexcept { return *mediaList; }
    juce::TabbedComponent& tabs() noexcept { return tabbed; }

private:
    juce::TabbedComponent tabbed { juce::TabbedButtonBar::TabsAtTop };
    FileBrowser* fileBrowser = nullptr;
    MediaList* mediaList = nullptr;
};
} // namespace weft::ui
