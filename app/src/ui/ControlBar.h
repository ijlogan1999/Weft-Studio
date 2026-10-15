#pragma once

// B · Control bar (spec 04 §3): panel toggles, transport, the position/tempo/signature
// display, mode buttons and the CPU meter.

#include "Studio.h"
#include "Widgets.h"

namespace weft::ui
{
class ControlBar final : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    explicit ControlBar (Studio& studio);
    ~ControlBar() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Refreshes toggle states and displays from the studio. */
    void refresh();

    std::vector<CommandButton*> commandButtons();
    juce::Label& positionDisplay() noexcept { return position; }
    juce::Label& tempoDisplay() noexcept { return tempo; }
    juce::ComboBox& snapChoice() noexcept { return snapBox; }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void timerCallback() override;

    Studio& studio;
    CommandButton inspector { "Inspector", Cmd::toggleInspector, true }, side { "Side", Cmd::toggleSidePanel, true },
        mixer { "Mixer", Cmd::toggleMixer, true }, editor { "Editor", Cmd::toggleAudioEditor, true }, global { "Global", Cmd::toggleGlobalTracks, true };
    CommandButton start { "|<", Cmd::goToStart }, rewind { "<<", Cmd::previousBar }, forward { ">>", Cmd::nextBar }, stop { "Stop", Cmd::playStop },
        play { "Play", Cmd::playStop, true }, record { "Rec", Cmd::record, true }, cycle { "Cycle", Cmd::cycle, true };
    CommandButton metronome { "Click", Cmd::metronome, true }, punch { "Punch", Cmd::autopunch, true }, snap { "Snap", Cmd::snapToggle, true },
        follow { "Follow", Cmd::followPlayhead, true }, keyboard { "Keys", Cmd::keyboardMidi, true }, theme { "Theme", Cmd::toggleTheme };
    juce::Label position, time, tempo, signature, cpu;
    juce::ComboBox countIn, snapBox;
};
} // namespace weft::ui
