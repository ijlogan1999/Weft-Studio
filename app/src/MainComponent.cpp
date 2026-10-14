#include "MainComponent.h"

#include "AboutPanel.h"
#include "Theme.h"

#include "weft/core/Version.h"

namespace
{
const juce::String donateUrl { WEFT_DONATE_URL };

void stylePill (juce::TextButton& b, juce::Colour colour)
{
    b.setColour (juce::TextButton::buttonColourId, colour);
    b.setColour (juce::TextButton::textColourOffId, weft::theme::plum);
    b.setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
}
} // namespace

MainComponent::MainComponent()
{
    stylePill (aboutButton, weft::theme::grape);
    stylePill (donateButton, weft::theme::bubblegum);
    aboutButton.onClick = [this] { AboutPanel::show (this); };
    donateButton.onClick = [] { juce::URL (donateUrl).launchInDefaultBrowser(); };
    addAndMakeVisible (aboutButton);
    addChildComponent (donateButton);
    donateButton.setVisible (donateUrl.isNotEmpty());
    setSize (960, 600);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (weft::theme::plum);

    // A row of candy "threads" across the window: the weft.
    const juce::Colour threads[] { weft::theme::coral, weft::theme::tangerine, weft::theme::lemon,
                                   weft::theme::mint, weft::theme::sky, weft::theme::grape, weft::theme::bubblegum };
    const auto centre = getLocalBounds().getCentre().toFloat();
    float y = centre.y - 110.0f;
    for (auto c : threads)
    {
        g.setColour (c);
        g.fillRoundedRectangle (centre.x - 180.0f, y, 360.0f, 8.0f, 4.0f);
        y += 14.0f;
    }

    g.setColour (weft::theme::text);
    g.setFont (juce::FontOptions (44.0f, juce::Font::bold));
    g.drawText (juce::String (weft::productName.data(), weft::productName.size()),
                getLocalBounds().withY (static_cast<int> (y) + 8).withHeight (56), juce::Justification::centredTop);
    g.setColour (weft::theme::textDim);
    g.setFont (juce::FontOptions (16.0f));
    g.drawText ("Version " + juce::String (weft::versionString().data(), weft::versionString().size())
                    + "  ·  Milestone 0 skeleton",
                getLocalBounds().withY (static_cast<int> (y) + 66).withHeight (24), juce::Justification::centredTop);
}

void MainComponent::resized()
{
    auto row = getLocalBounds().removeFromBottom (90).withSizeKeepingCentre (donateButton.isVisible() ? 340 : 160, 40);
    aboutButton.setBounds (row.removeFromLeft (160));
    if (donateButton.isVisible())
        donateButton.setBounds (row.removeFromRight (160));
}
