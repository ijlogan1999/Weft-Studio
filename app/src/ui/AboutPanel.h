#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/** Help › About (F-SYS-1): version plus every third-party component and its license text. */
class AboutPanel final : public juce::Component
{
public:
    AboutPanel();

    void paint (juce::Graphics&) override;
    void resized() override;

    static void show (juce::Component* parent);

private:
    juce::TextEditor notices;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AboutPanel)
};
