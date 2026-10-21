#include "ui/AudioSetup.h"
#include "ui/MainComponent.h"
#include "ui/ProjectDialogs.h"

#include "weft/engine/media/JuceDecoders.h"
#include "weft/engine/midi/JuceMidiInputs.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace
{
juce::PropertiesFile::Options settingsOptions()
{
    juce::PropertiesFile::Options o;
    o.applicationName = "Weft Studio";
    o.filenameSuffix = ".settings";
    o.folderName = "Weft Studio";
    o.osxLibrarySubFolder = "Application Support";
    return o;
}
} // namespace

class WeftStudioApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String& commandLine) override
    {
        settings = std::make_unique<juce::PropertiesFile> (settingsOptions());
        const auto layout = weft::ui::PanelLayout::fromString (settings->getValue ("layout"));
        weft::ui::Theme::get().setDark (layout.darkTheme);
        juce::LookAndFeel::setDefaultLookAndFeel (&weft::ui::Theme::get().lookAndFeel());

        // W-0: the saved device, or on first launch the defaults while Audio Setup is shown.
        devices = weft::ui::juceDeviceProvider();
        const auto stored = weft::ui::loadAudioSettings();
        const auto audio = stored.value_or (weft::ui::defaultAudioSettings (*devices));

        const auto documents = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
        const auto projects = documents.getChildFile ("Weft Studio");
        projects.createDirectory();
        studio = std::make_unique<weft::ui::Studio> (devices->make (audio), weft::ui::textPath (projects.getFullPathName()), audio.config());
        studio->decoders = weft::engine::importDecoders();
        studio->layout = layout;
        studio->midiInputs().setDisabled (audio.disabledMidiInputs);
        midi = std::make_unique<weft::engine::JuceMidiInputs> (studio->midiInputs());

        const auto music = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
        content = new weft::ui::MainComponent (*studio, music.isDirectory() ? music : documents);
        content->devices = devices.get();
        content->onLayoutChanged = [this] (const weft::ui::PanelLayout& l) {
            settings->setValue ("layout", l.toString());
            settings->saveIfNeeded();
        };

        const auto picked = commandLine.unquoted().trim();
        auto start = [this, picked] {
            if (mainWindow != nullptr)
                return;
            mainWindow = std::make_unique<MainWindow> (getApplicationName(), content);
            if (picked.isNotEmpty())
                content->openProject (weft::ui::textPath (picked));
            else
                content->showWelcome();
        };
        if (stored)
        {
            start();
            return;
        }
        // First launch: Audio Setup comes before the main window (W-0 step 1).
        auto* setup = weft::ui::launchDialog (std::make_unique<weft::ui::AudioSetupDialog> (*studio, *devices, true), "Audio Setup", nullptr);
        setup->onSaved = [start] { juce::MessageManager::callAsync (start); };
        setup->onDismissed = [start] { juce::MessageManager::callAsync (start); };
    }

    void shutdown() override
    {
        if (mainWindow != nullptr)
            mainWindow = nullptr; // deletes the content
        else
            delete content;
        content = nullptr;
        midi = nullptr;
        studio = nullptr;
        devices = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        settings = nullptr;
    }

    void systemRequestedQuit() override
    {
        if (content == nullptr)
        {
            quit();
            return;
        }
        content->whenSaved ([] { juce::JUCEApplication::getInstance()->quit(); });
    }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        MainWindow (const juce::String& name, weft::ui::MainComponent* c)
            : DocumentWindow (name, weft::ui::colour (weft::ui::Token::background), allButtons)
        {
            setUsingNativeTitleBar (true);
            setMenuBar (c);
            setContentOwned (c, true);
            setName (c->getName());
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
            c->grabKeyboardFocus();
        }

        ~MainWindow() override { setMenuBar (nullptr); }

        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
        /** When the window itself is given focus (say, as a dialog in front of it closes), the keys
            belong to the main view, or shortcuts like Space do nothing until something is clicked. */
        void focusGained (FocusChangeType) override
        {
            if (auto* c = getContentComponent(); c != nullptr && c->isShowing())
                c->grabKeyboardFocus();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<weft::ui::DeviceProvider> devices;
    std::unique_ptr<weft::ui::Studio> studio;
    std::unique_ptr<weft::engine::JuceMidiInputs> midi;
    weft::ui::MainComponent* content = nullptr; // owned by the main window once it exists
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (WeftStudioApplication)
