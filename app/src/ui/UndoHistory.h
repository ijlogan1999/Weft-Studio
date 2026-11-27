#pragma once

// Undo History (D-UI-46, K-WIN-3): the project's undo steps, oldest first, then the steps that can be
// redone. Clicking a step undoes or redoes until it is the last one done; clicking "Start" undoes all.

#include "Studio.h"
#include "Widgets.h"

namespace weft::ui
{
class UndoHistory final : public juce::Component, private juce::ChangeListener, private juce::ListBoxModel
{
public:
    explicit UndoHistory (Studio& studio);
    ~UndoHistory() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    void refresh();

    /** Row 0 is the project as opened ("Start"); rows 1..done are undo steps, the rest can be redone. */
    int getNumRows() override;
    juce::String rowText (int row) const;
    /** The row of the last step done. */
    int currentRow() const;
    /** Undoes or redoes until `row` is the last step done. */
    void goTo (int row);
    juce::ListBox list;

private:
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override { goTo (row); }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    Studio& studio;
    std::vector<std::string> done, undone;
};
} // namespace weft::ui
