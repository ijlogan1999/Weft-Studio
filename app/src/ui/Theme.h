#pragma once

// Weft's design tokens (spec 04, F-UI-3, D-UI-13). Every colour, size, radius, typeface and depth
// the UI draws comes from this file, so the dark (default) and light themes switch without a
// restart and every view shares one 8 px grid. tools/ui_token_lint.py fails the build if a UI
// source outside Theme.* uses a colour literal or a numeric font size.

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <string>
#include <vector>

namespace weft::ui
{
enum class Token
{
    background,    // window ground and the 4 px gutters between zones
    surface,       // panels: track headers, inspector, mixer, side panel
    surfaceRaised, // transport bar, panel headers, fields
    raisedTop,     // raised controls: gradient top
    raisedBottom,  //                  gradient bottom
    hover,         // hover on raised controls, selected track header
    pressed,       // toggles that are on
    well,          // meters, fader slots, the counter, text fields
    lane,          // the timeline canvas
    laneAlt,       // the selected track's lane
    lineSubtle,    // dividers inside a zone
    border,        // zone edges, control outlines
    gridStrong,    // bar lines
    gridWeak,      // beat lines
    gridSub,       // subdivisions
    text,
    textDim,
    textDisabled,
    accent,        // the one accent: selection, record, keyboard focus
    onAccent,      // text on accent
    record,        // the same hue as accent (one accent for selection and record)
    onRecord,
    solo,
    mute,
    playhead,
    loop,          // the cycle band
    meterSafe,     // below -18 dBFS
    meterHot,      // -18 to -6 dBFS
    meterClip,     // above -6 dBFS and the clip light
    capTop,        // fader caps
    capBottom,
    regionText,    // text on region colours, solo and mute
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

/** The 8 px grid. `half` is only for padding inside a control. */
namespace space
{
inline constexpr int half = 4, s1 = 8, s2 = 16, s3 = 24, s4 = 32, s6 = 48;
}

/** Fixed sizes, all on the grid. */
namespace size
{
inline constexpr int control = 24;     // buttons, fields, rows (spec 04: 24 px minimum target)
inline constexpr int transport = 48;   // the control bar
inline constexpr int panelHeader = 32; // headers above panels and lists
inline constexpr int ruler = 32;
inline constexpr int trackRow = 56;
inline constexpr int lane = 56;        // automation lanes
inline constexpr int strip = 72;       // mixer channel strips
inline constexpr int gutter = 4;       // between zones; also the resize handles
inline constexpr int icon = 16;
inline constexpr int faderCap = 16;   // fader cap height; the fader's travel is inset by half of it
inline constexpr int status = 24;      // the status bar
} // namespace size

namespace radius
{
inline constexpr float r0 = 0.0f, r1 = 2.0f, r2 = 4.0f;
}

/** The type scale: IBM Plex Sans for the UI, IBM Plex Mono for time, tempo and levels. */
enum class Type
{
    caption,    // 11, SemiBold, caps: section labels
    small,      // 11: secondary lines, note and key labels
    body,       // 12: default UI, buttons
    strong,     // 13, Medium: track names, inspector values
    title,      // 15, SemiBold: dialog and panel titles
    display,    // 24, SemiBold: the product name on the About and start pages
    monoSmall,  // 11: ruler, meter scale
    monoValue,  // 12, Medium: dB, pan, BPM readouts
    monoCounter // 20, Medium: the transport counter
};
juce::Font font (Type);
/** Upper-cases and letter-spaces a caption the way `Type::caption` is meant to read. */
juce::String captionText (const juce::String&);

/** The 24-swatch track palette (F-TRK-9), Weft's own: 12 muted hues in two tones. */
inline constexpr int paletteSize = 24;
juce::Colour paletteColour (int index);
/** The palette as a menu of swatches, item ids base + 1 .. base + paletteSize; `current` (-1: none) is ticked. */
juce::PopupMenu colourMenu (int base, int current);
/** A model colour (palette index or "#RRGGBB"). */
juce::Colour colourFor (int palette, const std::string& rgb);
/** A region's body: its colour laid over the canvas, so content reads on it without glare. */
juce::Colour regionBody (juce::Colour c);

/** Collaborator colours (05 §5): saturated, so they never read as the muted track colours. */
juce::Colour collaboratorColour (int index);
/** Dark or light text, whichever reads better on `c`. */
juce::Colour textOn (juce::Colour c);

/** 16 x 16 icons, drawn as filled paths in the button's text colour. */
namespace icon
{
juce::Path goToStart();
juce::Path rewind();
juce::Path forward();
juce::Path stop();
juce::Path play();
juce::Path record();
juce::Path cycle();
juce::Path metronome();
juce::Path lock();
} // namespace icon

/** WCAG 2 contrast ratio of two opaque colours (1..21). */
double contrastRatio (juce::Colour a, juce::Colour b);

/** Every text-on-background pair the UI draws, checked for 4.5:1 (AT-UI-2). */
struct TokenPair
{
    Token text, background;
};
const std::vector<TokenPair>& textPairs();

/** Depth (D-UI-13): light comes from above. Raised things get a top highlight and a short soft
    shadow, wells and pressed toggles sink in. All of it is a few translucent fills, so it is
    cheap enough to draw on every repaint. */
namespace depth
{
enum class Level
{
    flat,
    raised,  // buttons, slots, regions, ruler
    panel,   // transport bar, track headers, mixer strips, fader caps
    floating // menus, popovers, value tags
};
void shadow (juce::Graphics&, juce::Rectangle<float>, float cornerRadius, Level);
void highlight (juce::Graphics&, juce::Rectangle<float>, float cornerRadius);
/** A raised control: shadow, gradient body and top highlight. */
void raised (juce::Graphics&, juce::Rectangle<float>, float cornerRadius, bool hover = false, Level = Level::raised);
/** A toggle that is on, pushed into the surface. */
void pressed (juce::Graphics&, juce::Rectangle<float>, float cornerRadius);
/** A well: meter, fader slot, field. */
void well (juce::Graphics&, juce::Rectangle<float>, float cornerRadius);
/** A coloured fill with raised treatment (solo, mute, record, accent buttons). */
void tinted (juce::Graphics&, juce::Rectangle<float>, float cornerRadius, juce::Colour);
/** The keyboard focus ring: 2 px accent with a 1 px gap. */
void focusRing (juce::Graphics&, juce::Rectangle<float>, float cornerRadius);
/** Shadow ink for edges drawn by hand (column and bar shadows): black at the theme's alpha. */
juce::Colour shade (float darkAlpha, float lightAlpha);
/** A faint wash that lifts a band off the canvas: light on the dark theme, dark on the light one. */
juce::Colour wash (float darkAlpha, float lightAlpha);
} // namespace depth

/** Roles a button can take, set with `setRole`; the look and feel draws each one. */
enum class ButtonRole
{
    normal,
    record, // accent when on
    solo,
    mute,
    primary, // the default action in a dialog: accent fill
    tab,     // panel tabs: underline when on
    flat,    // borderless, for dense rows
    warning  // something needs attention: a missing plug-in, a disabled route
};
/** Buttons whose edges are connected (`setConnectedEdges`) are segments of a group: the group
    paints one raised frame behind them (`paintGroup`) and each segment draws only its state. */
void paintGroup (juce::Graphics&, juce::Rectangle<int> bounds);
/** `tintedWhenOff` keeps the role's colour on the label while off (the transport's record
    button, so it is found at a glance); otherwise an off button reads like any other. */
void setRole (juce::Button&, ButtonRole, bool tintedWhenOff = false);
/** Reachable with Tab and drawn with the focus ring, without a click taking focus from the
    arrange window (so shortcuts keep working after using the mouse). */
void makeFocusable (juce::Component&);
/** Draws a label as an editable field: a well with mono text (values typed by hand). */
void setField (juce::Label&);
/** The role set with `setRole`; otherwise primary for a button Return presses, else normal. */
ButtonRole roleOf (const juce::Component&);
/** The colour a button's label or icon is drawn in. */
juce::Colour buttonTextColour (const juce::Button&, bool over, bool down);

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
