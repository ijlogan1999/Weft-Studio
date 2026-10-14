#include "AboutPanel.h"

#include "Theme.h"

#include <BinaryData.h>

#include "weft/core/Version.h"

AboutPanel::AboutPanel()
{
    notices.setMultiLine (true);
    notices.setReadOnly (true);
    notices.setScrollbarsShown (true);
    notices.setFont (juce::FontOptions (13.0f));
    notices.setColour (juce::TextEditor::backgroundColourId, weft::theme::plumRaised);
    notices.setColour (juce::TextEditor::textColourId, weft::theme::text);
    notices.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);

    juce::String text;
    text << juce::String::fromUTF8 (BinaryData::LICENSE, BinaryData::LICENSESize) << "\n\n"
         << juce::String::fromUTF8 (BinaryData::ThirdPartyNotices_txt, BinaryData::ThirdPartyNotices_txtSize);
    notices.setText (text, false);
    addAndMakeVisible (notices);
    setSize (720, 560);
}

void AboutPanel::paint (juce::Graphics& g)
{
    g.fillAll (weft::theme::plum);
    g.setColour (weft::theme::text);
    g.setFont (juce::FontOptions (26.0f, juce::Font::bold));
    const auto title = juce::String (weft::productName.data(), weft::productName.size());
    const auto version = juce::String (weft::versionString().data(), weft::versionString().size());
    g.drawText (title, 24, 16, getWidth() - 48, 34, juce::Justification::left);
    g.setColour (weft::theme::textDim);
    g.setFont (juce::FontOptions (14.0f));
    g.drawText ("Version " + version + "  ·  Free software under the GNU AGPL v3", 24, 50, getWidth() - 48, 20,
                juce::Justification::left);
}

void AboutPanel::resized() { notices.setBounds (getLocalBounds().withTrimmedTop (84).reduced (16)); }

void AboutPanel::show (juce::Component* parent)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (new AboutPanel());
    options.dialogTitle = "About Weft Studio";
    options.dialogBackgroundColour = weft::theme::plum;
    options.componentToCentreAround = parent;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.launchAsync();
}
