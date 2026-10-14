#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/** Milestone 0 placeholder window: product name, version, About and donate buttons.
    The arrange view replaces it in Milestone 4. */
class MainComponent final : public juce::Component
{
public:
    MainComponent();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::TextButton aboutButton { "About Weft" };
    juce::TextButton donateButton { "Support Weft" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
