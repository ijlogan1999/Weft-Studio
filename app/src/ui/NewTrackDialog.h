#pragma once

// New Track dialog (W-5, F-TRK-6): kind, count and channels; the tracks go below the
// selected track as one undo step.

#include "Studio.h"
#include "Widgets.h"

namespace weft::ui
{
class NewTrackDialog final : public juce::Component
{
public:
    explicit NewTrackDialog (Studio& studio);
    void resized() override;
    void paint (juce::Graphics&) override;

    /** Creates the tracks the dialog describes; returns their ids. */
    std::vector<Id> create();
    /** Opens the dialog in a modal window over parent. */
    static void show (Studio& studio, juce::Component* parent);

    juce::ComboBox kind, channels;
    juce::Slider count;
    juce::TextButton ok { "Create" }, cancel { "Cancel" };
    std::function<void()> onClose;

private:
    Studio& studio;
};
} // namespace weft::ui
