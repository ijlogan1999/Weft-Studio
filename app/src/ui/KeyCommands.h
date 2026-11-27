#pragma once

// Key Commands (D-UI-50): every command with its keys; search, show assigned or unassigned ones, and
// give a command new keys by pressing them. This computer's, not the project's.

#include "Studio.h"
#include "Widgets.h"

namespace weft::ui
{
class KeyCommandsPanel final : public juce::Component, public juce::TableListBoxModel
{
public:
    explicit KeyCommandsPanel (Studio& studio);
    ~KeyCommandsPanel() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;

    enum class Show
    {
        all,
        assigned,
        unassigned
    };
    void setSearch (const juce::String& text);
    void setShow (Show show);
    /** The listed commands, in menu-table order. */
    const std::vector<Cmd>& listed() const noexcept { return rows; }
    juce::String keysText (Cmd command) const;
    bool selectCommand (Cmd command);
    std::optional<Cmd> selectedCommand() const;

    /** Learn: the next key pressed (Esc cancels) becomes the selected command's only key. */
    void startLearning();
    bool learning() const noexcept { return isLearning; }
    /** Gives the selected command this key; the message says which command lost it. */
    bool assign (const juce::KeyPress& key);
    /** Adds the key to the selected command's keys. */
    bool addKey (const juce::KeyPress& key);
    void clearSelected();
    void defaultSelected();
    void resetAll();

    juce::TextEditor search;
    juce::ComboBox show;
    juce::TableListBox table;
    juce::TextButton learn { "Learn Key" }, add { "Add Key" }, clear { "Clear" }, defaults { "Default" }, resetEverything { "Reset All" };
    juce::Label message;

    int getNumRows() override { return static_cast<int> (rows.size()); }
    void paintRowBackground (juce::Graphics&, int row, int width, int height, bool selected) override;
    void paintCell (juce::Graphics&, int row, int column, int width, int height, bool selected) override;
    void cellDoubleClicked (int row, int column, const juce::MouseEvent&) override;
    void selectedRowsChanged (int) override { stopLearning(); }

private:
    void refresh();
    void stopLearning();
    bool give (const juce::KeyPress& key, bool replace);
    Studio& studio;
    std::vector<Cmd> rows;
    juce::String filter;
    Show showing = Show::all;
    bool isLearning = false;
    bool addLearning = false;
};
} // namespace weft::ui
