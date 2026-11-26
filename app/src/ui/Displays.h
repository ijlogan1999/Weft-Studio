#pragma once

// Control bar extras (D-UI-43): the master volume slider and output meter, the big beats and
// time displays, the performance meter window, and the toolbar under the control bar.

#include "Studio.h"
#include "Widgets.h"

#include <functional>
#include <memory>

namespace weft::ui
{
/** A small horizontal meter of the master's output peak, with a short decay. */
class OutputMeter final : public juce::Component
{
public:
    explicit OutputMeter (Studio& studio);
    void paint (juce::Graphics&) override;
    /** Reads the master's peak since the last call (the control bar's timer calls it). */
    void update();
    float levelDb() const noexcept { return shownDb; }

private:
    Studio& studio;
    float shownDb = -200.0f;
};

/** A large readout of the playhead in bars and beats, or in time (Open Big Beats or Big Time Display). */
class BigDisplay final : public juce::Component, private juce::Timer
{
public:
    enum class Mode { beats, time };
    BigDisplay (Studio& studio, Mode mode);
    ~BigDisplay() override;
    void paint (juce::Graphics&) override;
    /** What it shows now: "3 2 1 1" style bars and beats, or the time in the control bar's format. */
    juce::String text() const;
    const Mode mode;

private:
    void timerCallback() override;
    Studio& studio;
    juce::String shown;
};

/** The audio thread's load as a bar, with the dropout count (double-click the CPU reading). */
class PerformanceMeter final : public juce::Component, private juce::Timer
{
public:
    explicit PerformanceMeter (Studio& studio);
    ~PerformanceMeter() override;
    void paint (juce::Graphics&) override;
    juce::String text() const;

private:
    void timerCallback() override;
    Studio& studio;
};

/** A floating, resizable window for one of the displays; several can be open. Closing it asks its
    owner to delete it. */
class FloatingWindow final : public juce::DocumentWindow
{
public:
    FloatingWindow (const juce::String& title, std::unique_ptr<juce::Component> content, std::function<void (FloatingWindow*)> onClose);
    void closeButtonPressed() override;

private:
    std::function<void (FloatingWindow*)> closed;
};

/** View › Toolbar (D-UI-43): a row of command buttons under the control bar. Right-click it to
    choose the buttons, save them as the default or restore the default. */
class Toolbar final : public juce::Component, private juce::ChangeListener
{
public:
    explicit Toolbar (Studio& studio);
    ~Toolbar() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    /** Runs a button's command; the main window sets it so dialogs open there. */
    std::function<void (Cmd)> run;
    juce::PopupMenu customizeMenu() const;
    void pickCustomize (int item);
    std::vector<CommandButton*> buttons() const;
    static constexpr int height = 36;
    static constexpr int itemSaveDefault = 900, itemRestoreDefault = 901;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void rebuild();
    Studio& studio;
    std::vector<Cmd> shownCommands;
    std::vector<std::unique_ptr<CommandButton>> owned;
};
} // namespace weft::ui
