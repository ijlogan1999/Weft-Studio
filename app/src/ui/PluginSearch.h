#pragma once

// Search and Add Plug-in (D-W6-11): type part of a plug-in's name or maker, Enter adds the
// highlighted match to the selected track (an effect into its next free insert slot, an
// instrument as its instrument).

#include "Studio.h"
#include "Widgets.h"

namespace weft::ui
{
class PluginSearch final : public juce::Component, private juce::ListBoxModel
{
public:
    explicit PluginSearch (Studio& studio);
    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;

    /** The choices whose name or maker contains every word of `text` (any case); instruments only
        for an instrument track. An empty text lists them all. */
    static std::vector<Studio::PluginChoice> matches (const Studio& studio, const juce::String& text);
    /** Adds a choice to the selected track; false (with a status message) when it can't. */
    static bool add (Studio& studio, const Studio::PluginChoice& choice);

    /** Adds the highlighted match and closes. */
    bool addSelected();
    void setSearch (const juce::String& text);
    const std::vector<Studio::PluginChoice>& results() const noexcept { return shown; }

    /** Opens the search over parent. */
    static void show (Studio& studio, juce::Component* parent);

    juce::TextEditor search;
    juce::ListBox list;
    std::function<void()> onClose;

private:
    int getNumRows() override { return static_cast<int> (shown.size()); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    void returnKeyPressed (int) override { addSelected(); }
    void close();

    Studio& studio;
    std::vector<Studio::PluginChoice> shown;
};

/** Search and Select Track (D-UI-34): type part of a track's name, Enter selects the highlighted
    track and scrolls it into view. */
class TrackSearch final : public juce::Component, private juce::ListBoxModel
{
public:
    explicit TrackSearch (Studio& studio);
    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;

    /** Tracks (not the master) whose name holds every word of `text`, any case, in track order;
        hidden tracks too. An empty text lists them all. */
    static std::vector<Id> matches (const Studio& studio, const juce::String& text);
    void setSearch (const juce::String& text);
    /** Selects the highlighted track and closes. */
    bool selectHighlighted();
    const std::vector<Id>& results() const noexcept { return shown; }

    /** Opens the search over parent; null without desktop windows. */
    static TrackSearch* show (Studio& studio, juce::Component* parent);

    juce::TextEditor search;
    juce::ListBox list;
    std::function<void()> onClose;
    /** Called with the track chosen, after it's selected (the window scrolls to it). */
    std::function<void (const Id&)> onChosen;

private:
    int getNumRows() override { return static_cast<int> (shown.size()); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    void returnKeyPressed (int) override { selectHighlighted(); }

    Studio& studio;
    std::vector<Id> shown;
};
} // namespace weft::ui
