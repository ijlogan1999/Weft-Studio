#pragma once

// C · Inspector (spec 04 §4): the selected region's parameters above the selected
// track's. Edits go straight to the session; each control change is one undo step.

#include "Studio.h"
#include "Widgets.h"

namespace weft::ui
{
class Inspector final : public juce::Component, private juce::ChangeListener
{
public:
    explicit Inspector (Studio& studio);
    ~Inspector() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();

    // Region section.
    juce::Label regionName;
    juce::ToggleButton regionMute { "Mute" }, regionLoop { "Loop" };
    juce::Slider regionGain, fadeIn, fadeOut;
    juce::ComboBox fadeInCurve, fadeOutCurve;
    // Track section.
    juce::Label trackName;
    juce::ComboBox trackColour, monitor;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    const model::Region* region() const;

    Studio& studio;
    bool refreshing = false;
    int regionTop = 0, trackTop = 0;
};
} // namespace weft::ui
