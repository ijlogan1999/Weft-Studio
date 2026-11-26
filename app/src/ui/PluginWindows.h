#pragma once

// Plug-in editor windows (spec 04 §10: floating, on top, one per instance, position kept)
// with a generic slider editor for processors without their own (F-PLG-4), and the
// plug-in manager dialog (F-PLG-3).

#include "Studio.h"
#include "Widgets.h"

namespace weft::ui
{
/** A slider per parameter, for built-ins and plug-ins without an editor (AT-PLG-5). A
    missing plug-in shows "Not installed on this computer" instead (F-PLG-15). */
class GenericPluginEditor final : public juce::Component, private juce::Timer
{
public:
    GenericPluginEditor (std::shared_ptr<engine::Processor> processor, const juce::String& name);
    void paint (juce::Graphics&) override;
    void resized() override;

    struct Row
    {
        std::unique_ptr<juce::Label> label;
        std::unique_ptr<juce::Slider> slider;
    };
    std::vector<Row> rows;
    juce::Label message;
    static constexpr int rowHeight = 30;

private:
    void timerCallback() override;
    std::shared_ptr<engine::Processor> proc;
    std::vector<engine::ParameterInfo> infos;
};

/** What a plug-in window shows (D-UI-24): a header strip with the Bypass button (insert effects only), and the
    editor under it. The frame follows the editor's size when the plug-in resizes itself. */
class PluginFrame final : public juce::Component, private juce::ComponentListener
{
public:
    PluginFrame (Studio& studio, const Id& pluginId, juce::Component* editorToOwn);
    ~PluginFrame() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    /** Shows the insert's bypass state (the window's timer calls it). */
    void refresh();

    juce::TextButton bypass { "Bypass" };
    juce::Component& editor() noexcept { return *content; }
    static constexpr int headerHeight = 36;

private:
    void componentMovedOrResized (juce::Component&, bool moved, bool resized) override;
    /** The track and insert this window edits, or nothing for an instrument or a removed plug-in. */
    const model::PluginInstance* insert (Id* trackId = nullptr) const;
    Studio& studio;
    const Id pluginId;
    std::unique_ptr<juce::Component> content;
};

class PluginWindows;

class PluginWindow final : public juce::DocumentWindow, private juce::Timer
{
public:
    PluginWindow (Studio& studio, PluginWindows& owner, const Id& pluginId);
    ~PluginWindow() override;
    void closeButtonPressed() override;
    void moved() override;
    const Id pluginId;
    /** The generic editor, when the plug-in has no editor of its own. */
    GenericPluginEditor* generic() const;
    /** The header strip and editor (D-UI-24). */
    PluginFrame* frame() const;
    /** Reports parameters the editor changed to the session (the window's timer calls it). */
    void pollParameters();

private:
    void timerCallback() override;
    Studio& studio;
    PluginWindows& windows;
    std::shared_ptr<engine::Processor> proc; // keeps the plug-in alive while its editor is open
    std::vector<engine::ParameterInfo> infos;
    std::vector<double> lastSeen;
    std::uint64_t seenEdits = 0;
};

/** Owns the open plug-in windows: Studio::openPluginEditor opens or raises one. */
class PluginWindows final : private juce::ChangeListener
{
public:
    explicit PluginWindows (Studio& studio);
    ~PluginWindows() override;
    void open (const Id& pluginId);
    /** Writes the plug-in's state (D-DM-5) and closes its window. */
    void close (const Id& pluginId);
    void closeAll();
    PluginWindow* find (const Id& pluginId) const;
    std::size_t count() const noexcept { return windows.size(); }
    std::map<Id, juce::Point<int>> positions; // remembered while the app runs

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    Studio& studio;
    std::map<Id, std::unique_ptr<PluginWindow>> windows;
};

/** Plug-in manager (F-PLG-3): name, vendor, format and status; enable or disable; Rescan; Retry failed. */
class PluginManager final : public juce::Component, private juce::TableListBoxModel
{
public:
    explicit PluginManager (Studio& studio);
    void resized() override;
    void paint (juce::Graphics&) override;
    void refresh();

    enum Column
    {
        colName = 1,
        colVendor,
        colFormat,
        colStatus,
        colEnabled
    };
    int rows() const;
    juce::String cellText (int row, int column) const;
    void setEnabled (int row, bool enabled);
    juce::TableListBox table;
    juce::TextButton rescan { "Rescan" }, retry { "Retry Failed" }, close { "Close" };
    juce::Label summary;
    std::function<void()> onClose;

    static void show (Studio& studio, juce::Component* parent);

private:
    int getNumRows() override { return rows(); }
    void paintRowBackground (juce::Graphics&, int row, int width, int height, bool selected) override;
    void paintCell (juce::Graphics&, int row, int column, int width, int height, bool selected) override;
    juce::Component* refreshComponentForCell (int row, int column, bool selected, juce::Component* existing) override;
    void scan (bool retryFailed);
    Studio& studio;
};
} // namespace weft::ui
