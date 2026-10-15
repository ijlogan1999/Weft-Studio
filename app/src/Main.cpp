#include "ui/MainComponent.h"

#include "weft/engine/devices/JuceAudioDevice.h"
#include "weft/engine/media/JuceDecoders.h"

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

    void initialise (const juce::String&) override
    {
        settings = std::make_unique<juce::PropertiesFile> (settingsOptions());
        const auto layout = weft::ui::PanelLayout::fromString (settings->getValue ("layout"));
        weft::ui::Theme::get().setDark (layout.darkTheme);
        juce::LookAndFeel::setDefaultLookAndFeel (&weft::ui::Theme::get().lookAndFeel());

        const auto documents = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
        const auto projects = documents.getChildFile ("Weft Studio");
        projects.createDirectory();
        studio = std::make_unique<weft::ui::Studio> (std::make_unique<weft::engine::JuceAudioDevice>(),
                                                     std::filesystem::path (projects.getFullPathName().toStdString()));
        studio->decoders = weft::engine::importDecoders();
        studio->layout = layout;

        auto music = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
        auto* content = new weft::ui::MainComponent (*studio, music.isDirectory() ? music : documents);
        content->onLayoutChanged = [this] (const weft::ui::PanelLayout& l) {
            settings->setValue ("layout", l.toString());
            settings->saveIfNeeded();
        };
        mainWindow = std::make_unique<MainWindow> (getApplicationName(), content);
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        if (studio != nullptr)
            studio->save();
        studio = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        settings = nullptr;
    }

    void systemRequestedQuit() override { quit(); }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        MainWindow (const juce::String& name, weft::ui::MainComponent* content)
            : DocumentWindow (name, weft::ui::colour (weft::ui::Token::background), allButtons)
        {
            setUsingNativeTitleBar (true);
            setMenuBar (content);
            setContentOwned (content, true);
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
            content->grabKeyboardFocus();
        }

        ~MainWindow() override { setMenuBar (nullptr); }

        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<weft::ui::Studio> studio;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (WeftStudioApplication)
