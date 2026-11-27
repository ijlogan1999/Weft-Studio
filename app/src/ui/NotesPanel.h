#pragma once

// Notes (D-UI-47, P-NTE-1 to P-NTE-5): the project's notes and the selected track's, in the side panel's
// Notes tab. Edit makes them editable and Done saves them; Text Size picks small, medium or large.

#include "Studio.h"
#include "Widgets.h"

namespace weft::ui
{
class NotesPanel final : public juce::Component, private juce::ChangeListener
{
public:
    explicit NotesPanel (Studio& studio);
    ~NotesPanel() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    void refresh();
    /** Edit / Done: Done saves whatever changed, each as one undo step. */
    void setEditing (bool on);
    bool editing() const noexcept { return isEditing; }
    void setTextSize (int index); // 0 small, 1 medium, 2 large (this machine's)
    int textSizeIndex() const noexcept { return sizeIndex; }

    juce::Label projectTitle, trackTitle;
    juce::TextEditor projectNotes, trackNotes;
    juce::TextButton edit { "Edit" }, textSize { "Text Size" };

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void save();
    Studio& studio;
    bool isEditing = false;
    Id shownTrack;
    int sizeIndex = 1;
};
} // namespace weft::ui
