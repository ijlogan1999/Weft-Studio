#include "Theme.h"

#include <cmath>

namespace weft::ui
{
namespace
{
using C = juce::Colour;

Tokens makeDark()
{
    Tokens t;
    t.name = "Dark";
    auto set = [&] (Token k, juce::uint32 argb) { t.colours[static_cast<std::size_t> (k)] = C (argb); };
    set (Token::background, 0xff1b1530);    // plum
    set (Token::surface, 0xff221a3d);
    set (Token::surfaceRaised, 0xff2a2148);
    set (Token::lane, 0xff1f1838);
    set (Token::laneAlt, 0xff241c40);
    set (Token::border, 0xff3b3060);
    set (Token::text, 0xfff4efff);
    set (Token::textDim, 0xffb3a8d6);
    set (Token::accent, 0xff1dd1a1);        // mint
    set (Token::onAccent, 0xff1b1530);
    set (Token::record, 0xffff6b6b);        // coral
    set (Token::onRecord, 0xff1b1530);
    set (Token::playhead, 0xfffeca57);      // lemon
    set (Token::gridStrong, 0xff4a3f72);
    set (Token::gridWeak, 0xff2f2752);
    set (Token::regionText, 0xff1b1530);
    set (Token::warning, 0xffff9f43);
    set (Token::onWarning, 0xff1b1530);
    return t;
}

Tokens makeLight()
{
    Tokens t;
    t.name = "Light";
    auto set = [&] (Token k, juce::uint32 argb) { t.colours[static_cast<std::size_t> (k)] = C (argb); };
    set (Token::background, 0xfff7f3ff);
    set (Token::surface, 0xffefe8fb);
    set (Token::surfaceRaised, 0xffe4dbf5);
    set (Token::lane, 0xfffbf9ff);
    set (Token::laneAlt, 0xfff3eefc);
    set (Token::border, 0xffc9bde3);
    set (Token::text, 0xff1b1530);
    set (Token::textDim, 0xff574a7a);
    set (Token::accent, 0xff08694f);
    set (Token::onAccent, 0xffffffff);
    set (Token::record, 0xffc0392b);
    set (Token::onRecord, 0xffffffff);
    set (Token::playhead, 0xffb8740a);
    set (Token::gridStrong, 0xffb9acd8);
    set (Token::gridWeak, 0xffe2daf2);
    set (Token::regionText, 0xff1b1530);
    set (Token::warning, 0xff9a4d00);
    set (Token::onWarning, 0xffffffff);
    return t;
}

double luminance (C c)
{
    auto channel = [] (juce::uint8 v) {
        const double s = v / 255.0;
        return s <= 0.03928 ? s / 12.92 : std::pow ((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel (c.getRed()) + 0.7152 * channel (c.getGreen()) + 0.0722 * channel (c.getBlue());
}

// Candy swatches light enough for dark region text in both themes.
const juce::uint32 palette[24] = { 0xffff6b6b, 0xffff9f43, 0xfffeca57, 0xff1dd1a1, 0xff54a0ff, 0xffb98af2, 0xffff6fb5, 0xff48dbfb,
                                    0xffff8e72, 0xffffb86b, 0xffe6e66a, 0xff7bed9f, 0xff74b9ff, 0xffc8a2ff, 0xffffa3cf, 0xff81ecec,
                                    0xfff3a683, 0xfff7d794, 0xffc4e538, 0xff55efc4, 0xffa29bfe, 0xffe0a3f5, 0xfffab1a0, 0xff9ae6b4 };
} // namespace

const Tokens& darkTokens()
{
    static const Tokens t = makeDark();
    return t;
}

const Tokens& lightTokens()
{
    static const Tokens t = makeLight();
    return t;
}

juce::Colour paletteColour (int index) { return C (palette[((index % 24) + 24) % 24]); }

juce::Colour colourFor (int paletteIndex, const std::string& rgb)
{
    if (rgb.size() == 7 && rgb[0] == '#')
        return C (0xff000000u | static_cast<juce::uint32> (juce::String (rgb.substr (1)).getHexValue32()));
    return paletteColour (paletteIndex);
}

double contrastRatio (juce::Colour a, juce::Colour b)
{
    const auto la = luminance (a), lb = luminance (b);
    return (std::max (la, lb) + 0.05) / (std::min (la, lb) + 0.05);
}

const std::vector<TokenPair>& textPairs()
{
    static const std::vector<TokenPair> pairs {
        { Token::text, Token::background },    { Token::text, Token::surface },     { Token::text, Token::surfaceRaised },
        { Token::text, Token::lane },          { Token::text, Token::laneAlt },     { Token::textDim, Token::background },
        { Token::textDim, Token::surface },    { Token::textDim, Token::surfaceRaised }, { Token::textDim, Token::lane },
        { Token::onAccent, Token::accent },    { Token::onRecord, Token::record },  { Token::onWarning, Token::warning },
        { Token::accent, Token::background },  { Token::accent, Token::surface },
    };
    return pairs;
}

Theme& Theme::get()
{
    static Theme theme;
    return theme;
}

Theme::Theme() : lnf (std::make_unique<juce::LookAndFeel_V4>()) { apply(); }

juce::LookAndFeel& Theme::lookAndFeel() { return *lnf; }

void Theme::setDark (bool shouldBeDark)
{
    if (dark == shouldBeDark)
        return;
    dark = shouldBeDark;
    apply();
    for (int i = 0; i < juce::Desktop::getInstance().getNumComponents(); ++i)
        if (auto* c = juce::Desktop::getInstance().getComponent (i))
            c->sendLookAndFeelChange();
    sendSynchronousChangeMessage();
}

void Theme::apply()
{
    const auto& t = tokens();
    using CS = juce::LookAndFeel_V4::ColourScheme;
    lnf->setColourScheme (CS { t[Token::surface],       t[Token::background], t[Token::surfaceRaised], t[Token::border], t[Token::text],
                               t[Token::surfaceRaised], t[Token::text],       t[Token::accent],        t[Token::onAccent] });
    lnf->setColour (juce::TextButton::buttonOnColourId, t[Token::accent]);
    lnf->setColour (juce::TextButton::textColourOnId, t[Token::onAccent]);
    lnf->setColour (juce::TextButton::textColourOffId, t[Token::text]);
    lnf->setColour (juce::TextButton::buttonColourId, t[Token::surfaceRaised]);
    lnf->setColour (juce::Label::textColourId, t[Token::text]);
    lnf->setColour (juce::TooltipWindow::backgroundColourId, t[Token::surfaceRaised]);
    lnf->setColour (juce::TooltipWindow::textColourId, t[Token::text]);
    lnf->setColour (juce::TabbedButtonBar::tabTextColourId, t[Token::textDim]);
    lnf->setColour (juce::TabbedButtonBar::frontTextColourId, t[Token::text]);
    lnf->setColour (juce::ListBox::backgroundColourId, t[Token::surface]);
    lnf->setColour (juce::ListBox::textColourId, t[Token::text]);
    lnf->setColour (juce::DirectoryContentsDisplayComponent::textColourId, t[Token::text]);
    lnf->setColour (juce::DirectoryContentsDisplayComponent::highlightColourId, t[Token::accent]);
    lnf->setColour (juce::DirectoryContentsDisplayComponent::highlightedTextColourId, t[Token::onAccent]);
    lnf->setColour (juce::TextEditor::backgroundColourId, t[Token::lane]);
    lnf->setColour (juce::TextEditor::textColourId, t[Token::text]);
    lnf->setColour (juce::ComboBox::backgroundColourId, t[Token::surfaceRaised]);
    lnf->setColour (juce::ComboBox::textColourId, t[Token::text]);
    lnf->setColour (juce::PopupMenu::backgroundColourId, t[Token::surfaceRaised]);
    lnf->setColour (juce::PopupMenu::textColourId, t[Token::text]);
    lnf->setColour (juce::PopupMenu::highlightedBackgroundColourId, t[Token::accent]);
    lnf->setColour (juce::PopupMenu::highlightedTextColourId, t[Token::onAccent]);
    lnf->setColour (juce::Slider::textBoxTextColourId, t[Token::text]);
    lnf->setColour (juce::Slider::thumbColourId, t[Token::accent]);
    lnf->setColour (juce::ResizableWindow::backgroundColourId, t[Token::background]);
}
} // namespace weft::ui
