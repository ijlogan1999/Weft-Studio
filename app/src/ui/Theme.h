#pragma once

// Weft's design tokens (spec 04, F-UI-3): every colour the UI draws comes from a token,
// so the dark (default) and light themes switch without a restart.

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <string>
#include <vector>

namespace weft::ui
{
enum class Token
{
    background,    // window ground
    surface,       // panels
    surfaceRaised, // headers, buttons
    lane,          // track lanes
    laneAlt,       // every other lane
    border,
    text,
    textDim,
    accent,        // selection, toggles that are on
    onAccent,      // text on accent
    record,
    onRecord,
    playhead,
    gridStrong,
    gridWeak,
    regionText,    // text on region colours
    warning,
    onWarning,
    count
};

struct Tokens
{
    std::string name;
    std::array<juce::Colour, static_cast<std::size_t> (Token::count)> colours;
    juce::Colour operator[] (Token t) const noexcept { return colours[static_cast<std::size_t> (t)]; }
};

const Tokens& darkTokens();
const Tokens& lightTokens();

/** The 24-swatch track palette (F-TRK-9), Weft's own. */
inline constexpr int paletteSize = 24;
juce::Colour paletteColour (int index);
/** A model colour (palette index or "#RRGGBB"). */
juce::Colour colourFor (int palette, const std::string& rgb);

/** WCAG 2 contrast ratio of two opaque colours (1..21). */
double contrastRatio (juce::Colour a, juce::Colour b);

/** Every text-on-background pair the UI draws, checked for 4.5:1 (AT-UI-2). */
struct TokenPair
{
    Token text, background;
};
const std::vector<TokenPair>& textPairs();

/** The current theme: tokens, the look and feel that applies them, and change notifications. */
class Theme : public juce::ChangeBroadcaster
{
public:
    static Theme& get();
    const Tokens& tokens() const noexcept { return dark ? darkTokens() : lightTokens(); }
    juce::Colour operator[] (Token t) const noexcept { return tokens()[t]; }
    bool isDark() const noexcept { return dark; }
    /** Switches theme and repaints everything (no restart). */
    void setDark (bool shouldBeDark);
    juce::LookAndFeel& lookAndFeel();

private:
    Theme();
    void apply();
    bool dark = true;
    std::unique_ptr<juce::LookAndFeel_V4> lnf;
};

inline juce::Colour colour (Token t) { return Theme::get()[t]; }
} // namespace weft::ui
