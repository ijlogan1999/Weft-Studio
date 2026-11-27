#include "UndoHistory.h"

namespace weft::ui
{
UndoHistory::UndoHistory (Studio& s) : studio (s)
{
    list.setModel (this);
    list.setRowHeight (size::control);
    describe (list, "Undo History", "Click a step to go back or forward to it");
    addAndMakeVisible (list);
    studio.addChangeListener (this);
    setSize (300, 400);
    refresh();
}

UndoHistory::~UndoHistory()
{
    studio.removeChangeListener (this);
    list.setModel (nullptr);
}

void UndoHistory::refresh()
{
    auto nowDone = studio.session().undoNames();
    auto nowUndone = studio.session().redoNames();
    if (nowDone == done && nowUndone == undone)
        return;
    done = std::move (nowDone);
    undone = std::move (nowUndone);
    list.updateContent();
    list.scrollToEnsureRowIsOnscreen (currentRow());
    list.repaint();
}

int UndoHistory::getNumRows() { return 1 + static_cast<int> (done.size() + undone.size()); }

int UndoHistory::currentRow() const { return static_cast<int> (done.size()); }

juce::String UndoHistory::rowText (int row) const
{
    if (row <= 0)
        return "Start";
    const auto i = static_cast<std::size_t> (row - 1);
    if (i < done.size())
        return juce::String::fromUTF8 (done[i].c_str());
    // Redo steps are listed in the order they'd be redone: the most recently undone first.
    const auto j = i - done.size();
    return j < undone.size() ? juce::String::fromUTF8 (undone[undone.size() - 1 - j].c_str()) : juce::String();
}

void UndoHistory::goTo (int row)
{
    row = std::clamp (row, 0, getNumRows() - 1);
    auto& s = studio.session();
    while (currentRow() > row && s.canUndo())
    {
        if (! s.undo().done)
            break;
        done.pop_back();
    }
    while (currentRow() < row && s.canRedo())
    {
        if (! s.redo().done)
            break;
        done.push_back ({});
    }
    done.clear();
    undone.clear();
    studio.edited();
    refresh();
}

void UndoHistory::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool)
{
    const bool current = row == currentRow();
    if (current)
    {
        g.setColour (colour (Token::hover));
        g.fillRect (0, 0, width, height);
    }
    g.setColour (row > currentRow() ? colour (Token::textDim) : colour (Token::text)); // steps that can be redone are dimmed
    g.setFont (font (current ? Type::strong : Type::body));
    g.drawText (rowText (row), space::s1, 0, width - space::s2, height, juce::Justification::centredLeft);
}

void UndoHistory::resized() { list.setBounds (getLocalBounds()); }

void UndoHistory::paint (juce::Graphics& g) { g.fillAll (colour (Token::surface)); }
} // namespace weft::ui
