#pragma once

// B · Control bar (spec 04 §3): panel toggles, transport, the position/tempo/signature
// display, mode buttons and the CPU meter.

#include "Displays.h"
#include "PresenceUi.h"
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
    juce::Label& keyDisplay() noexcept { return key; }
    juce::ComboBox& snapChoice() noexcept { return snapBox; }
    Avatars& collaboratorAvatars() noexcept { return avatars; }
    /** The "More" button, shown while the window is too narrow for the count-in menu or the CPU
        meter, and the menu it opens: whatever was dropped (AT-UI-11). */
    juce::Button& moreButton() noexcept { return more; }
    juce::PopupMenu overflowMenu() const;
    void pickOverflow (int item);
    static constexpr int overflowCountIn = 1, overflowCpu = 100;

    /** Customize Control Bar (D-UI-43): right-click the bar or its display. */
    juce::PopupMenu customizeMenu() const;
    void pickCustomize (int item);
    static constexpr int itemShowBase = 200, itemMasterBase = 300, itemTimeBase = 310, itemTempoBase = 320, itemBigBeats = 330, itemBigTime = 331,
                         itemPerformance = 332, itemApplyDefaults = 340, itemSaveDefault = 341, itemRevert = 342;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    juce::Slider& masterVolumeSlider() noexcept { return masterVolume; }
    OutputMeter& outputMeterView() noexcept { return outputMeter; }
    juce::Label& timeDisplay() noexcept { return time; }
    juce::Label& cpuDisplay() noexcept { return cpu; }
    /** Double-click on the CPU reading: the performance meter window, bigger with Ctrl (1 to 3). */
    std::function<void (int scale)> onPerformanceMeter;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    std::vector<std::vector<juce::Button*>> groups();
    juce::Rectangle<int> displayBounds() const;
    void timerCallback() override;

    Studio& studio;
    CommandButton inspector { "Inspector", Cmd::toggleInspector, true }, side { "Side", Cmd::toggleSidePanel, true },
        mixer { "Mixer", Cmd::toggleMixer, true }, editor { "Editor", Cmd::toggleAudioEditor, true }, global { "Global", Cmd::toggleGlobalTracks, true },
        toolbarToggle { "Toolbar", Cmd::toggleToolbar, true };
    CommandButton start { "Go to Start", Cmd::goToStart }, rewind { "Previous Bar", Cmd::previousBar }, forward { "Next Bar", Cmd::nextBar }, stop { "Stop", Cmd::playStop },
        play { "Play", Cmd::playStop, true }, record { "Record", Cmd::record, true }, cycle { "Cycle", Cmd::cycle, true };
    CommandButton metronome { "Click", Cmd::metronome, true }, punch { "Punch", Cmd::autopunch, true }, replace { "Replace", Cmd::toggleReplace, true },
        snap { "Snap", Cmd::snapToggle, true },
        follow { "Follow", Cmd::followPlayhead, true }, keyboard { "Keys", Cmd::keyboardMidi, true };
    juce::Label position, time, tempo, signature, key, cpu;
    juce::ComboBox countIn, snapBox;
    juce::TextButton more { juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa6")) };
    juce::Slider masterVolume { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    OutputMeter outputMeter { studio };
    ControlBarLayout beforeLastChange, shownBar;
    bool canRevert = false;
    void changeLayout (const ControlBarLayout& next);
    bool allowed (const juce::Component* c) const;
    Avatars avatars { studio };
    int avatarsWidth = 0;
};
} // namespace weft::ui
