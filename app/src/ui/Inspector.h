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
    // MIDI region quantize (F-QNT-1).
    juce::ComboBox regionQuantize;
    juce::Slider quantizeStrength, quantizeSwing;
    juce::TextButton applyQuantize { "Apply Quantize" };
    // Note section (piano roll focus with notes selected).
    juce::Label notePitch, noteVelocity, noteLength, notePosition, noteChannel;
    // Track section.
    juce::Label trackName;
    juce::ComboBox trackColour, monitor;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    const model::Region* region() const;
    std::vector<commands::NoteRef> notes() const;
    /** Sets a field on every selected note, as one undo step. */
    void editNotes (std::string_view name, const std::function<void (model::Note&)>& change);
    void setRegionQuantize();

    Studio& studio;
    bool refreshing = false;
    int regionTop = 0, trackTop = 0, noteTop = 0;
};
} // namespace weft::ui
