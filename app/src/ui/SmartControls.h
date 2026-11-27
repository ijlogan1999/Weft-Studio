#pragma once

// Smart Controls (D-UI-52, P-SC-1 to P-SC-3): a row of knobs for the selected track, each sweeping one or more of
// its parameters, in the bottom area or a window of its own. The Inspector beside them names a knob, maps it to the
// track's parameters with a range for each, and assigns it a hardware controller (Learn).

#include "Studio.h"
#include "Widgets.h"

namespace weft::ui
{
class SmartControlsPanel final : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    explicit SmartControlsPanel (Studio& studio);
    ~SmartControlsPanel() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    void refresh();

    const Id& track() const noexcept { return trackId; }
    int knobCount() const noexcept { return static_cast<int> (knobs.size()); }
    juce::Slider& knob (int index) { return *knobs[static_cast<std::size_t> (index)]; }
    /** The knob the Inspector edits (clicking a knob picks it). */
    void selectKnob (int index);
    int selectedKnob() const noexcept { return selected; }
    void setInspectorShown (bool shown);
    bool inspectorShown() const noexcept { return showInspector; }

    // Inspector edits, each one undo step "Smart Controls" -------------------------------------------------
    void setLayout (int knobsInLayout);
    void setKnobName (const juce::String& name);
    /** Adds a mapping to the selected knob: the track's target at `choice` (an index into Studio::automationTargets). */
    void addMapping (int choice);
    void setMappingTarget (int mapping, int choice);
    void setMappingRange (int mapping, double min, double max);
    void removeMapping (int mapping);
    void learnController();
    void clearController();
    /** Back to the automatic mapping. */
    void useAutomatic();

    juce::Label title;
    juce::ComboBox layoutBox;
    juce::TextButton automatic { "Automatic" }, inspectorButton { "Inspector" };
    // Inspector.
    juce::TextEditor nameField;
    struct MappingRow
    {
        juce::ComboBox target;
        juce::Slider min { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight }, max { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::TextButton remove { "Remove" };
    };
    std::vector<std::unique_ptr<MappingRow>> rows;
    juce::TextButton addButton { "Add Mapping" }, learn { "Learn" }, clear { "Clear" };
    juce::Label controllerLabel;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void timerCallback() override;
    void buildKnobs (int count);
    void buildRows();
    void updateKnobs();
    void apply (const std::function<void (model::SmartControls&)>& change);
    juce::Rectangle<int> knobArea() const;
    juce::Rectangle<int> inspectorArea() const;

    Studio& studio;
    Id trackId;
    std::vector<std::unique_ptr<juce::Slider>> knobs;
    std::vector<automation::TargetChoice> choices;
    int selected = 0;
    bool showInspector = false;
    bool refreshing = false;
    bool gesture = false;
};
} // namespace weft::ui
