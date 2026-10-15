// One JUCE GUI session for the whole run: JUCE's singletons don't survive being torn
// down and brought back between test cases.

#include "ui/Theme.h"

#include <catch2/catch_session.hpp>

int main (int argc, char* argv[])
{
    int result = 0;
    {
        juce::ScopedJuceInitialiser_GUI gui;
        juce::LookAndFeel::setDefaultLookAndFeel (&weft::ui::Theme::get().lookAndFeel());
        result = Catch::Session().run (argc, argv);
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }
    return result;
}
