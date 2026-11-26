#pragma once

// New Track dialog (W-5, F-TRK-6, W-1a step 1): kind, count, channels, input, output and, per
// kind, input monitoring, record enable and the instrument (D-UI-32). The tracks go below the
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

    juce::ComboBox kind, channels, input, output, instrument;
    juce::ToggleButton ascending { "Ascending" }, monitoring { "Input monitoring" }, recordEnable { "Record enable" };
    juce::Slider count;
    /** Item ids of the input menu: noInput, or firstInput + the first channel. */
    static constexpr int noInput = 1, firstInput = 100;
    juce::TextButton ok { "Create" }, cancel { "Cancel" };
    std::function<void()> onClose;

private:
    /** Shows the controls the chosen kind has and refills the input menu for the channel format. */
    void updateForKind();
    std::vector<Id> outputBuses;
    Studio& studio;
};
} // namespace weft::ui
