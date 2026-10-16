// One JUCE GUI session for the whole run: JUCE's singletons don't survive being torn
// down and brought back between test cases.

#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <catch2/catch_session.hpp>

int main (int argc, char* argv[])
{
    int result = 0;
    // Keep the plug-in list away from the user's own settings.
    const auto settings = std::filesystem::temp_directory_path() / ("weft-ui-settings-" + std::to_string (juce::Random::getSystemRandom().nextInt64()));
    weft::ui::Studio::settingsFolder() = settings;
    weft::ui::desktopWindows() = false;
    {
        juce::ScopedJuceInitialiser_GUI gui;
        juce::LookAndFeel::setDefaultLookAndFeel (&weft::ui::Theme::get().lookAndFeel());
        result = Catch::Session().run (argc, argv);
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }
    std::error_code ec;
    std::filesystem::remove_all (settings, ec);
    return result;
}
