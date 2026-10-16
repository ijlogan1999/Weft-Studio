#pragma once

// File > Bounce (W-9, Ctrl+B): range, tail, mode, formats, dither and destination; then a
// progress bar with Cancel, and a completion notice with Show in Folder and Add to Project.

#include "Studio.h"
#include "Widgets.h"

#include <memory>

namespace weft::ui
{
class BounceDialog final : public juce::Component, private juce::Timer
{
public:
    explicit BounceDialog (Studio& studio);
    ~BounceDialog() override;
    void resized() override;
    void paint (juce::Graphics&) override;

    enum class Stage
    {
        settings,
        rendering,
        done
    };
    Stage stage() const noexcept { return current; }

    /** The settings the controls describe. */
    engine::BounceSettings settings() const;
    /** Bounce: asks Replace / Keep both first when a file exists (W-9 step 6). */
    void bounce();
    /** The pending question ("Mix.wav already exists."), or empty. Headless tests answer it. */
    const juce::String& question() const noexcept { return asked; }
    enum Answer
    {
        cancelAnswer = 0,
        replaceAnswer = 1,
        keepBothAnswer = 2
    };
    void answer (int choice);

    // Settings
    juce::TextEditor start, end, folder, name, title, artist, album, year;
    juce::ToggleButton tail { "Include tail" }, wav { "WAV" }, mp3 { "MP3" };
    juce::ComboBox mode, bitDepth, wavRate, bitrate, mp3Rate, dither;
    juce::TextButton choose { "Choose..." }, bounceButton { "Bounce" }, cancel { "Cancel" };
    juce::Label warning;
    // Rendering and done
    juce::Label notice;
    juce::TextButton showInFolder { "Show in Folder" }, addToProject { "Add to Project" }, close { "Close" };
    std::function<void()> onClose;

    static void show (Studio& studio, juce::Component* parent);

private:
    void begin (engine::BounceSettings s);
    void timerCallback() override;
    void finished();
    void setStage (Stage s);
    void updateEnabled();
    void closeDialog();

    Studio& studio;
    Stage current = Stage::settings;
    double progress = 0;
    juce::ProgressBar bar { progress };
    juce::String asked;
    std::unique_ptr<juce::AlertWindow> questionWindow;
    std::unique_ptr<juce::FileChooser> chooser;
};
} // namespace weft::ui
