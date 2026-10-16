#pragma once

// The project dialogs of M9 (spec 04 §12, W-10): New Project, Save As, Project Settings,
// Missing Media and the Welcome screen. Each one is a plain component whose controls are
// public, so the [UI] tests drive them the way the user would.

#include "Studio.h"
#include "Widgets.h"

#include <functional>
#include <memory>

namespace weft::ui
{
/** Puts a dialog in a window centred on `parent`; the dialog's onClose closes it. */
template <typename Dialog>
Dialog* launchDialog (std::unique_ptr<Dialog> dialog, const juce::String& title, juce::Component* parent)
{
    auto* raw = dialog.release();
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (raw);
    options.dialogTitle = title;
    options.dialogBackgroundColour = colour (Token::surfaceRaised);
    options.componentToCentreAround = parent;
    options.useNativeTitleBar = true;
    options.resizable = false;
    auto* window = options.launchAsync();
    raw->onClose = [window] {
        if (window != nullptr)
            window->exitModalState (0);
    };
    return raw;
}

/** File › New (F-PRJ-1): name and sample rate; the project goes in the projects folder. */
class NewProjectDialog final : public juce::Component
{
public:
    explicit NewProjectDialog (Studio& studio);
    void resized() override;
    void paint (juce::Graphics&) override;

    juce::TextEditor name;
    juce::ComboBox sampleRate; // ids: the rate in Hz
    juce::TextButton create { "Create" }, cancel { "Cancel" };
    juce::Label error;
    std::function<void()> onClose;
    /** Called after the project was created. */
    std::function<void()> onCreated;

    commands::NewProjectOptions options() const;
    bool createProject();

private:
    Studio& studio;
};

/** File › Save As (F-PRJ-4): a name, a folder and "Copy media". */
class SaveAsDialog final : public juce::Component
{
public:
    explicit SaveAsDialog (Studio& studio);
    void resized() override;
    void paint (juce::Graphics&) override;

    juce::TextEditor name, folder;
    juce::TextButton choose { "Choose..." };
    juce::ToggleButton copyMedia { "Copy media into the new project" };
    juce::TextButton save { "Save" }, cancel { "Cancel" };
    juce::Label error;
    std::function<void()> onClose;

    /** `<folder>/<name>.weft` */
    std::filesystem::path target() const;
    bool saveProject();

private:
    Studio& studio;
    std::unique_ptr<juce::FileChooser> chooser;
};

/** File › Project Settings (F-PRJ-9): name, recording bit depth, sample rate (until the
    project has media), and the metronome defaults. Changes apply on OK, one undo step each. */
class ProjectSettingsDialog final : public juce::Component
{
public:
    explicit ProjectSettingsDialog (Studio& studio);
    void resized() override;
    void paint (juce::Graphics&) override;

    juce::TextEditor name;
    juce::ComboBox bitDepth;   // ids: bits
    juce::ComboBox sampleRate; // ids: Hz
    juce::Label rateNote;
    juce::ToggleButton metronome { "Metronome on" };
    juce::ComboBox metronomeDuring; // 1 record, 2 play, 3 both
    juce::ComboBox countIn;         // ids: bars + 1
    juce::Slider metronomeLevel;    // dB
    juce::TextButton ok { "OK" }, cancel { "Cancel" };
    juce::Label error;
    std::function<void()> onClose;

    bool apply();

private:
    Studio& studio;
};

/** W-10 Open step 4: "N audio files are missing" with Locate… and Continue. */
class MissingMediaDialog final : public juce::Component
{
public:
    explicit MissingMediaDialog (Studio& studio);
    void resized() override;
    void paint (juce::Graphics&) override;

    juce::Label message;
    juce::TextButton locate { "Locate..." }, proceed { "Continue" };
    std::function<void()> onClose;

    /** What Locate… does once a folder is picked. */
    void locateIn (const std::filesystem::path& folder);
    void refresh();

private:
    Studio& studio;
    std::unique_ptr<juce::FileChooser> chooser;
};

/** W-0 step 6: New project, Open, and the recent projects. */
class WelcomeDialog final : public juce::Component
{
public:
    WelcomeDialog (Studio& studio, std::function<void (Cmd)> runCommand, std::function<void (const std::filesystem::path&)> openRecent);
    void resized() override;
    void paint (juce::Graphics&) override;

    juce::TextButton newProject { "New Project..." }, open { "Open..." }, startEmpty { "Start with an Empty Project" };
    juce::OwnedArray<juce::TextButton> recent;
    std::function<void()> onClose;

private:
    Studio& studio;
};

juce::String pathText (const std::filesystem::path& p);
std::filesystem::path textPath (const juce::String& s);
} // namespace weft::ui
