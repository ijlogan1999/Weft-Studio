#pragma once

// File > Bounce (W-9, Ctrl+B): range, tail, mode, formats, dither and destination; then a
// progress bar with Cancel, and a completion notice with Show in Folder and Add to Project.
// Export Track as Audio File and Export All Tracks use it too, one file set per track (D-W9-9).

#include "Studio.h"
#include "Widgets.h"

#include <memory>

namespace weft::ui
{
class BounceDialog final : public juce::Component, private juce::Timer
{
public:
    /** `tracks` empty: the mix. Otherwise each track on its own, named after it. */
    explicit BounceDialog (Studio& studio, std::vector<Id> tracks = {});
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
    /** One per bounce: the mix, or each exported track. */
    std::vector<engine::BounceSettings> allSettings() const;
    const std::vector<Id> tracks;
    /** New Folder: asks for a name, then makes it inside the folder field's folder. Tests replace the question. */
    std::function<void (std::function<void (juce::String)>)> askFolderName;
    void newFolder();
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
    juce::ToggleButton tail { "Include tail" }, secondPass { "2nd cycle pass" }, wav { "WAV" }, aiff { "AIFF" }, flac { "FLAC" }, mp3 { "MP3" },
        tempoInfo { "Include tempo" };
    juce::ComboBox mode, bitDepth, wavRate, bitrate, mp3Rate, dither, normalize, channels;
    juce::TextButton choose { "Choose..." }, makeFolder { "New Folder..." }, bounceButton { "Bounce" }, cancel { "Cancel" };
    juce::Label warning;
    // Rendering and done
    juce::Label notice;
    juce::TextButton showInFolder { "Show in Folder" }, addToProject { "Add to Project" }, close { "Close" };
    std::function<void()> onClose;

    static void show (Studio& studio, juce::Component* parent, std::vector<Id> tracks = {});

private:
    void begin (bool replace);
    std::vector<juce::Component*> settingsControls();
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
