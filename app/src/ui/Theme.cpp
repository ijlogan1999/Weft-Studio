#include "Theme.h"

#include <BinaryData.h>

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
    // Neutral, very slightly cool grays: no tint to bend colour judgement over a long session.
    set (Token::background, 0xff16171a);
    set (Token::surface, 0xff232529);
    set (Token::surfaceRaised, 0xff2c2e33);
    set (Token::raisedTop, 0xff313338);
    set (Token::raisedBottom, 0xff2a2c30);
    set (Token::hover, 0xff36393e);
    set (Token::pressed, 0xff1f2023);
    set (Token::well, 0xff121315);
    set (Token::lane, 0xff1c1d20);
    set (Token::laneAlt, 0xff202125);
    set (Token::lineSubtle, 0xff2e3035);
    set (Token::border, 0xff3d4046);
    set (Token::gridStrong, 0xff35383d);
    set (Token::gridWeak, 0xff2a2c30);
    set (Token::gridSub, 0xff232427);
    set (Token::text, 0xffd9dbdf);
    set (Token::textDim, 0xff9ea2a9);
    set (Token::textDisabled, 0xff666a71);
    set (Token::accent, 0xffe8823a); // vermilion
    set (Token::onAccent, 0xff16171a);
    set (Token::record, 0xffe8823a);
    set (Token::onRecord, 0xff16171a);
    set (Token::solo, 0xffcdb65a);
    set (Token::mute, 0xff8e939b);
    set (Token::playhead, 0xffecedef);
    set (Token::loop, 0xff8a8f98);
    set (Token::meterSafe, 0xff6daf7b);
    set (Token::meterHot, 0xffc8b45c);
    set (Token::meterClip, 0xffd9503f);
    set (Token::capTop, 0xffc6cacf);
    set (Token::capBottom, 0xffa9adb3);
    set (Token::regionText, 0xff16171a);
    set (Token::warning, 0xffd9a441);
    set (Token::onWarning, 0xff16171a);
    return t;
}

Tokens makeLight()
{
    Tokens t;
    t.name = "Light";
    auto set = [&] (Token k, juce::uint32 argb) { t.colours[static_cast<std::size_t> (k)] = C (argb); };
    set (Token::background, 0xffd0d3d7);
    set (Token::surface, 0xffe3e5e8);
    set (Token::surfaceRaised, 0xffeff0f2);
    set (Token::raisedTop, 0xfff7f8f9);
    set (Token::raisedBottom, 0xffe9ebed);
    set (Token::hover, 0xffd6d9dd);
    set (Token::pressed, 0xffd2d5d9);
    set (Token::well, 0xffc9ccd1);
    set (Token::lane, 0xfff0f1f3);
    set (Token::laneAlt, 0xffe8e9ec);
    set (Token::lineSubtle, 0xffd3d6da);
    set (Token::border, 0xffaeb2b8);
    set (Token::gridStrong, 0xffb9bdc3);
    set (Token::gridWeak, 0xffd4d7db);
    set (Token::gridSub, 0xffe2e4e7);
    set (Token::text, 0xff1c1d20);
    set (Token::textDim, 0xff4a4e56);
    set (Token::textDisabled, 0xff8c9098);
    set (Token::accent, 0xff943f14);
    set (Token::onAccent, 0xffffffff);
    set (Token::record, 0xff943f14);
    set (Token::onRecord, 0xffffffff);
    set (Token::solo, 0xffcdb65a);
    set (Token::mute, 0xff8e939b);
    set (Token::playhead, 0xff1c1d20);
    set (Token::loop, 0xff6b7079);
    set (Token::meterSafe, 0xff4f9a5f);
    set (Token::meterHot, 0xffb59c34);
    set (Token::meterClip, 0xffc8402f);
    set (Token::capTop, 0xfffafbfc);
    set (Token::capBottom, 0xffd9dce0);
    set (Token::regionText, 0xff16171a);
    set (Token::warning, 0xff8a5200);
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

// 12 hues at two muted tones (light: L 64 % S 40 %, deep: L 58.5 % S 27 %); all keep dark region
// text at 4.5:1 or better.
const juce::uint32 palette[24] = { 0xffc8837e, 0xffc8977e, 0xffc8a87e, 0xffc8b77e, 0xffc5c87e, 0xff9dc87e, 0xff7ec8a3, 0xff7ec8c5,
                                    0xff7eafc8, 0xff7e99c8, 0xff917ec8, 0xffc87ec8, 0xffb27c79, 0xffb28c79, 0xffb29979, 0xffb2a479,
                                    0xffb0b279, 0xff90b279, 0xff79b295, 0xff79b2b0, 0xff799fb2, 0xff798eb2, 0xff8779b2, 0xffb279b2 };

// Typefaces --------------------------------------------------------------------------------

struct Typefaces
{
    juce::Typeface::Ptr regular = load (BinaryData::IBMPlexSansRegular_ttf, BinaryData::IBMPlexSansRegular_ttfSize);
    juce::Typeface::Ptr medium = load (BinaryData::IBMPlexSansMedium_ttf, BinaryData::IBMPlexSansMedium_ttfSize);
    juce::Typeface::Ptr semiBold = load (BinaryData::IBMPlexSansSemiBold_ttf, BinaryData::IBMPlexSansSemiBold_ttfSize);
    juce::Typeface::Ptr mono = load (BinaryData::IBMPlexMonoRegular_ttf, BinaryData::IBMPlexMonoRegular_ttfSize);
    juce::Typeface::Ptr monoMedium = load (BinaryData::IBMPlexMonoMedium_ttf, BinaryData::IBMPlexMonoMedium_ttfSize);

    static juce::Typeface::Ptr load (const char* data, int bytes)
    {
        return juce::Typeface::createSystemTypefaceFor (data, static_cast<std::size_t> (bytes));
    }
    static Typefaces& get()
    {
        static Typefaces t;
        return t;
    }
};

// Type sizes are em sizes in px, as a designer gives them; JUCE heights are ascent + descent.
juce::Font plex (juce::Typeface::Ptr face, float px) { return juce::Font (juce::FontOptions (face).withPointHeight (px)); }

// Look and feel --------------------------------------------------------------------------------

class WeftLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    juce::Typeface::Ptr getTypefaceForFont (const juce::Font& f) override
    {
        auto& t = Typefaces::get();
        if (f.getTypefaceName() == juce::Font::getDefaultMonospacedFontName() || f.getTypefaceName() == t.mono->getName())
            return f.isBold() ? t.monoMedium : t.mono;
        if (f.isBold())
            return t.semiBold;
        if (f.getTypefaceStyle() == "Medium")
            return t.medium;
        return t.regular;
    }

    // Buttons ------------------------------------------------------------------------------
    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down) override
    {
        const auto role = roleOf (b);
        auto r = b.getLocalBounds().toFloat().reduced (0.5f, 1.0f);
        const auto rad = r.getHeight() <= 20.0f ? radius::r1 : radius::r2;
        const bool on = b.getToggleState();
        const auto enabled = b.isEnabled();
        if (role == ButtonRole::tab)
        {
            if (over && ! on)
            {
                g.setColour (colour (Token::hover));
                g.fillRect (r);
            }
            if (on)
            {
                g.setColour (colour (Token::accent));
                g.fillRect (r.removeFromBottom (2.0f));
            }
            return;
        }
        const bool segment = b.isConnectedOnLeft() || b.isConnectedOnRight() || b.isConnectedOnTop() || b.isConnectedOnBottom();
        if (segment && ! on && ! down)
        {
            if (over && b.isEnabled())
            {
                g.setColour (colour (Token::hover));
                g.fillRect (b.getLocalBounds().toFloat().reduced (0.0f, 1.0f));
            }
            return;
        }
        if (segment)
        {
            r = b.getLocalBounds().toFloat().reduced (0.0f, 1.0f);
            if (role == ButtonRole::record || role == ButtonRole::solo || role == ButtonRole::mute)
            {
                const auto c = colour (role == ButtonRole::record ? Token::record : role == ButtonRole::solo ? Token::solo : Token::mute);
                g.setGradientFill (juce::ColourGradient::vertical (c.brighter (0.08f), r.getY(), c.darker (0.08f), r.getBottom()));
                g.fillRect (r);
            }
            else
                depth::pressed (g, r, 0.0f);
            return;
        }
        if (role == ButtonRole::flat && ! on)
        {
            if (over)
            {
                g.setColour (colour (Token::hover));
                g.fillRoundedRectangle (r, rad);
            }
            return;
        }
        if (on || down)
        {
            switch (role)
            {
                case ButtonRole::record: depth::tinted (g, r, rad, colour (Token::record)); return;
                case ButtonRole::solo: depth::tinted (g, r, rad, colour (Token::solo)); return;
                case ButtonRole::mute: depth::tinted (g, r, rad, colour (Token::mute)); return;
                case ButtonRole::primary: depth::tinted (g, r, rad, colour (Token::accent).brighter (down ? 0.0f : 0.1f)); return;
                case ButtonRole::warning: depth::tinted (g, r, rad, colour (Token::warning)); return;
                case ButtonRole::normal:
                case ButtonRole::tab:
                case ButtonRole::flat: depth::pressed (g, r, rad); return;
            }
        }
        if (role == ButtonRole::primary || role == ButtonRole::warning)
        {
            depth::tinted (g, r, rad, colour (role == ButtonRole::primary ? Token::accent : Token::warning).withMultipliedBrightness (over ? 1.08f : 1.0f));
            return;
        }
        depth::raised (g, r, rad, over && enabled);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int) override { return font (Type::body); }

    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool over, bool down) override
    {
        g.setColour (buttonTextColour (b, over, down));
        g.setFont (b.getHeight() <= 20 ? font (Type::caption) : font (Type::body));
        const auto pad = b.getWidth() < 40 ? 0 : space::s1;
        g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (pad, 0), juce::Justification::centred, 1, 0.8f);
    }

    void drawTickBox (juce::Graphics& g, juce::Component& c, float x, float y, float w, float h, bool ticked, bool enabled, bool, bool) override
    {
        const auto box = juce::Rectangle<float> (x, y, w, h).withSizeKeepingCentre (14.0f, 14.0f);
        if (ticked)
            depth::tinted (g, box, radius::r1, colour (Token::accent));
        else
            depth::well (g, box, radius::r1);
        if (ticked)
        {
            juce::Path tick;
            tick.startNewSubPath (box.getX() + 3.5f, box.getCentreY());
            tick.lineTo (box.getX() + 6.0f, box.getBottom() - 4.0f);
            tick.lineTo (box.getRight() - 3.5f, box.getY() + 4.0f);
            g.setColour (colour (Token::onAccent).withAlpha (enabled ? 1.0f : 0.5f));
            g.strokePath (tick, juce::PathStrokeType (1.75f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        juce::ignoreUnused (c);
    }

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool down) override
    {
        drawTickBox (g, b, 0.0f, 0.0f, 16.0f, static_cast<float> (b.getHeight()), b.getToggleState(), b.isEnabled(), over, down);
        g.setColour (b.isEnabled() ? (over ? colour (Token::text) : colour (Token::textDim)) : colour (Token::textDisabled));
        g.setFont (font (Type::body));
        g.drawFittedText (b.getButtonText(), b.getLocalBounds().withTrimmedLeft (16 + space::s1), juce::Justification::centredLeft, 1);
    }

    // Combo boxes and menus ----------------------------------------------------------------
    void drawComboBox (juce::Graphics& g, int w, int h, bool down, int, int, int, int, juce::ComboBox& box) override
    {
        const auto r = juce::Rectangle<float> (0, 0, static_cast<float> (w), static_cast<float> (h)).reduced (0.5f, 1.0f);
        if (down)
            depth::pressed (g, r, radius::r2);
        else
            depth::raised (g, r, radius::r2, box.isMouseOver (true) && box.isEnabled());
        const auto arrow = juce::Rectangle<float> (static_cast<float> (w) - 20.0f, 0.0f, 12.0f, static_cast<float> (h)).withSizeKeepingCentre (8.0f, 4.0f);
        juce::Path p;
        p.startNewSubPath (arrow.getX(), arrow.getY());
        p.lineTo (arrow.getCentreX(), arrow.getBottom());
        p.lineTo (arrow.getRight(), arrow.getY());
        g.setColour (box.isEnabled() ? colour (Token::textDim) : colour (Token::textDisabled));
        g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    juce::Font getComboBoxFont (juce::ComboBox&) override { return font (Type::body); }
    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (space::half, 1, box.getWidth() - 24, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }

    void drawPopupMenuBackground (juce::Graphics& g, int w, int h) override
    {
        g.fillAll (colour (Token::surfaceRaised));
        g.setColour (colour (Token::border));
        g.drawRect (0, 0, w, h);
        depth::highlight (g, juce::Rectangle<float> (1.0f, 1.0f, static_cast<float> (w) - 2.0f, static_cast<float> (h) - 2.0f), 0.0f);
    }
    int getPopupMenuBorderSize() override { return space::half; }
    juce::Font getPopupMenuFont() override { return font (Type::body); }
    void getIdealPopupMenuItemSize (const juce::String& text, bool separator, int, int& w, int& h) override
    {
        if (separator)
        {
            w = 50;
            h = space::s1 + 1;
            return;
        }
        h = size::control;
        w = juce::GlyphArrangement::getStringWidthInt (getPopupMenuFont(), text) + space::s6 + space::s2;
    }
    void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool separator, bool active, bool highlighted, bool ticked,
                            bool subMenu, const juce::String& text, const juce::String& shortcut, const juce::Drawable*, const juce::Colour* textColour) override
    {
        if (separator)
        {
            g.setColour (colour (Token::lineSubtle));
            g.fillRect (area.withSizeKeepingCentre (area.getWidth() - space::s2, 1));
            return;
        }
        auto r = area.reduced (space::half, 0);
        if (highlighted && active)
        {
            g.setColour (colour (Token::hover));
            g.fillRoundedRectangle (r.toFloat(), radius::r1);
        }
        const auto check = r.removeFromLeft (space::s3);
        if (ticked)
        {
            g.setColour (colour (Token::accent));
            g.fillEllipse (check.toFloat().withSizeKeepingCentre (6.0f, 6.0f));
        }
        g.setColour (! active ? colour (Token::textDisabled) : textColour != nullptr ? *textColour : highlighted ? colour (Token::text) : colour (Token::text).withAlpha (0.9f));
        g.setFont (font (Type::body));
        if (subMenu)
        {
            const auto a = r.removeFromRight (space::s2).toFloat().withSizeKeepingCentre (4.0f, 8.0f);
            juce::Path p;
            p.startNewSubPath (a.getX(), a.getY());
            p.lineTo (a.getRight(), a.getCentreY());
            p.lineTo (a.getX(), a.getBottom());
            g.strokePath (p, juce::PathStrokeType (1.5f));
        }
        g.drawFittedText (text, r.withTrimmedRight (space::s1), juce::Justification::centredLeft, 1);
        if (shortcut.isNotEmpty())
        {
            g.setColour (colour (Token::textDim));
            g.setFont (font (Type::monoSmall));
            g.drawText (shortcut, r.withTrimmedRight (space::s1), juce::Justification::centredRight);
        }
    }

    // Sliders ------------------------------------------------------------------------------
    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float, juce::Slider::SliderStyle style, juce::Slider& s) override
    {
        const auto r = juce::Rectangle<int> (x, y, w, h).toFloat();
        const bool vertical = style == juce::Slider::LinearVertical;
        const bool hot = s.isMouseOverOrDragging() && s.isEnabled();
        if (vertical)
        {
            const auto track = r.withSizeKeepingCentre (4.0f, r.getHeight());
            depth::well (g, track, radius::r1);
            const auto cap = juce::Rectangle<float> (0, 0, std::min (28.0f, r.getWidth()), static_cast<float> (size::faderCap)).withCentre ({ r.getCentreX(), pos });
            drawCap (g, cap, hot, s.isMouseButtonDown(), s.isMouseButtonDown() && juce::ModifierKeys::currentModifiers.isShiftDown());
            return;
        }
        const auto track = r.withSizeKeepingCentre (r.getWidth(), 4.0f);
        depth::well (g, track, radius::r1);
        g.setColour (hot ? colour (Token::text) : colour (Token::textDim));
        g.fillRoundedRectangle (track.withRight (pos), radius::r1);
        const auto thumb = juce::Rectangle<float> (0, 0, 8.0f, 14.0f).withCentre ({ pos, r.getCentreY() });
        drawCap (g, thumb, hot, s.isMouseButtonDown(), false);
    }

    /** Linear sliders travel between the cap's centres, so a scale drawn beside a fader can use
        the same inset (size::faderCap / 2) and line up with it. */
    int getSliderThumbRadius (juce::Slider& s) override
    {
        return s.getSliderStyle() == juce::Slider::LinearVertical ? size::faderCap / 2 : 4;
    }

    /** `fine`: Shift is held during the drag, so the cap's line turns accent to say so. */
    static void drawCap (juce::Graphics& g, juce::Rectangle<float> cap, bool hot, bool down, bool fine)
    {
        depth::shadow (g, cap, radius::r1, depth::Level::panel);
        auto top = colour (Token::capTop), bottom = colour (Token::capBottom);
        if (hot || down)
        {
            top = top.brighter (down ? 0.25f : 0.12f);
            bottom = bottom.brighter (down ? 0.25f : 0.12f);
        }
        g.setGradientFill (juce::ColourGradient::vertical (top, cap.getY(), bottom, cap.getBottom()));
        g.fillRoundedRectangle (cap, radius::r1);
        g.setColour (juce::Colours::white.withAlpha (0.45f));
        g.drawHorizontalLine (juce::roundToInt (cap.getY()), cap.getX() + 1.0f, cap.getRight() - 1.0f);
        if (cap.getWidth() > 12.0f)
        {
            g.setColour (fine ? colour (Token::accent) : colour (Token::regionText));
            g.fillRect (cap.withSizeKeepingCentre (cap.getWidth() - 8.0f, 2.0f));
        }
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float start, float end, juce::Slider& s) override
    {
        const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (1.0f);
        const auto outer = std::min (bounds.getWidth(), bounds.getHeight()) / 2.0f;
        const auto c = bounds.getCentre();
        const auto line = outer >= 14.0f ? 3.0f : 2.0f;
        const auto arcR = outer - line / 2.0f;
        const auto hot = s.isMouseOverOrDragging() && s.isEnabled();
        juce::Path track;
        track.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, start, end, true);
        g.setColour (colour (Token::well));
        g.strokePath (track, juce::PathStrokeType (line, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        // The arc grows from the default value (pan from the centre).
        const auto def = s.getDoubleClickReturnValue();
        const auto from = s.isDoubleClickReturnEnabled() ? static_cast<float> (s.valueToProportionOfLength (def)) : 0.0f;
        const auto a0 = start + from * (end - start), a1 = start + pos * (end - start);
        juce::Path value;
        value.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, std::min (a0, a1), std::max (a0, a1), true);
        g.setColour (s.isMouseButtonDown() ? colour (Token::text) : hot ? colour (Token::text).withAlpha (0.85f) : colour (Token::textDim));
        g.strokePath (value, juce::PathStrokeType (line, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        // The body: a small raised disc with a pointer.
        const auto bodyR = arcR - line - 1.5f;
        const auto body = juce::Rectangle<float> (bodyR * 2.0f, bodyR * 2.0f).withCentre (c);
        depth::shadow (g, body, bodyR, depth::Level::raised);
        g.setGradientFill (juce::ColourGradient::vertical (hot ? colour (Token::hover).brighter (0.1f) : colour (Token::raisedTop).brighter (0.05f), body.getY(),
                                                           hot ? colour (Token::hover) : colour (Token::raisedBottom), body.getBottom()));
        g.fillEllipse (body);
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.drawEllipse (body.reduced (0.5f), 1.0f);
        const auto tip = c.getPointOnCircumference (bodyR - 1.0f, a1);
        const auto inner = c.getPointOnCircumference (bodyR * 0.35f, a1);
        g.setColour (colour (Token::text));
        g.drawLine (juce::Line<float> (inner, tip), 2.0f);
    }

    // Text editors -------------------------------------------------------------------------
    void fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor& e) override
    {
        const auto r = juce::Rectangle<float> (0, 0, static_cast<float> (w), static_cast<float> (h));
        g.setColour (e.findColour (juce::TextEditor::backgroundColourId));
        g.fillRoundedRectangle (r, radius::r2);
        juce::ColourGradient shade (juce::Colours::black.withAlpha (0.35f), 0.0f, 0.0f, juce::Colours::transparentBlack, 0.0f, 3.0f, false);
        g.setGradientFill (shade);
        g.fillRoundedRectangle (r.withHeight (std::min (4.0f, r.getHeight())), radius::r2);
    }
    void drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& e) override
    {
        if (! e.isEnabled())
            return;
        const auto r = juce::Rectangle<float> (0, 0, static_cast<float> (w), static_cast<float> (h)).reduced (0.5f);
        if (e.hasKeyboardFocus (true) && ! e.isReadOnly())
        {
            g.setColour (colour (Token::accent));
            g.drawRoundedRectangle (r.reduced (0.5f), radius::r2, 1.5f);
        }
        else if (e.findColour (juce::TextEditor::outlineColourId).getAlpha() > 0)
        {
            g.setColour (colour (Token::lineSubtle));
            g.drawRoundedRectangle (r, radius::r2, 1.0f);
        }
    }

    // Tabs, scroll bars, tooltips -------------------------------------------------------------
    int getTabButtonBestWidth (juce::TabBarButton& b, int) override
    {
        return juce::GlyphArrangement::getStringWidthInt (font (Type::body), b.getButtonText()) + space::s2;
    }
    void drawTabButton (juce::TabBarButton& b, juce::Graphics& g, bool over, bool) override
    {
        const auto r = b.getActiveArea().toFloat();
        const bool front = b.isFrontTab();
        if (over && ! front)
        {
            g.setColour (colour (Token::hover));
            g.fillRect (r);
        }
        if (front)
        {
            g.setColour (colour (Token::accent));
            g.fillRect (r.withTop (r.getBottom() - 2.0f));
        }
        g.setColour (front ? colour (Token::text) : over ? colour (Token::text) : colour (Token::textDim));
        g.setFont (front ? font (Type::strong) : font (Type::body));
        g.drawFittedText (b.getButtonText(), r.toNearestInt(), juce::Justification::centred, 1);
    }
    void drawTabbedButtonBarBackground (juce::TabbedButtonBar& bar, juce::Graphics& g) override
    {
        g.setColour (colour (Token::surface));
        g.fillRect (bar.getLocalBounds());
        g.setColour (colour (Token::lineSubtle));
        g.fillRect (bar.getLocalBounds().removeFromBottom (1));
    }
    void drawTabAreaBehindFrontButton (juce::TabbedButtonBar&, juce::Graphics&, int, int) override {}

    int getDefaultScrollbarWidth() override { return space::s1 + 2; }
    void drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int w, int h, bool vertical, int thumbStart, int thumbSize, bool over, bool down) override
    {
        juce::Rectangle<float> thumb = vertical ? juce::Rectangle<int> (x, thumbStart, w, thumbSize).toFloat() : juce::Rectangle<int> (thumbStart, y, thumbSize, h).toFloat();
        thumb = vertical ? thumb.reduced (3.0f, 1.0f) : thumb.reduced (1.0f, 3.0f);
        g.setColour (down ? colour (Token::textDim) : over ? colour (Token::textDim).withAlpha (0.7f) : colour (Token::border));
        g.fillRoundedRectangle (thumb, std::min (thumb.getWidth(), thumb.getHeight()) / 2.0f);
    }

    juce::Rectangle<int> getTooltipBounds (const juce::String& text, juce::Point<int> pos, juce::Rectangle<int> parent) override
    {
        const auto w = std::min (360, juce::GlyphArrangement::getStringWidthInt (font (Type::body), text) + space::s2);
        const auto lines = juce::jmax (1, juce::GlyphArrangement::getStringWidthInt (font (Type::body), text) / std::max (1, w - space::s2) + 1);
        const auto h = lines * 16 + space::s1;
        return juce::Rectangle<int> (pos.x > parent.getCentreX() ? pos.x - (w + 12) : pos.x + 12, pos.y > parent.getCentreY() ? pos.y - (h + 6) : pos.y + 6, w, h)
            .constrainedWithin (parent);
    }
    void drawTooltip (juce::Graphics& g, const juce::String& text, int w, int h) override
    {
        const auto r = juce::Rectangle<float> (0, 0, static_cast<float> (w), static_cast<float> (h));
        g.setColour (colour (Token::surfaceRaised));
        g.fillRoundedRectangle (r, radius::r2);
        g.setColour (colour (Token::border));
        g.drawRoundedRectangle (r.reduced (0.5f), radius::r2, 1.0f);
        g.setColour (colour (Token::text));
        g.setFont (font (Type::body));
        g.drawFittedText (text, r.toNearestInt().reduced (space::s1, space::half), juce::Justification::centredLeft, 4);
    }

    // Keyboard focus ------------------------------------------------------------------------
    std::unique_ptr<juce::FocusOutline> createFocusOutlineForComponent (juce::Component&) override
    {
        struct Ring final : juce::FocusOutline::OutlineWindowProperties
        {
            juce::Rectangle<int> getOutlineBounds (juce::Component& c) override { return c.getScreenBounds().expanded (3); }
            void drawOutline (juce::Graphics& g, int w, int h) override
            {
                depth::focusRing (g, juce::Rectangle<float> (0, 0, static_cast<float> (w), static_cast<float> (h)).reduced (3.0f), radius::r2);
            }
        };
        return std::make_unique<juce::FocusOutline> (std::make_unique<Ring>());
    }

    // Dialogs --------------------------------------------------------------------------------
    juce::Font getAlertWindowTitleFont() override { return font (Type::title); }
    juce::Font getAlertWindowMessageFont() override { return font (Type::body); }
    juce::Font getAlertWindowFont() override { return font (Type::body); }
    juce::Font getLabelFont (juce::Label& l) override { return l.getFont(); }
};
} // namespace

// Fonts ----------------------------------------------------------------------------------------

juce::Font font (Type type)
{
    auto& t = Typefaces::get();
    switch (type)
    {
        case Type::caption: return plex (t.semiBold, 11.0f).withExtraKerningFactor (0.06f);
        case Type::small: return plex (t.regular, 11.0f);
        case Type::body: return plex (t.regular, 12.0f);
        case Type::strong: return plex (t.medium, 13.0f);
        case Type::title: return plex (t.semiBold, 15.0f);
        case Type::display: return plex (t.semiBold, 24.0f);
        case Type::monoSmall: return plex (t.mono, 11.0f);
        case Type::monoValue: return plex (t.monoMedium, 12.0f);
        case Type::monoCounter: return plex (t.monoMedium, 20.0f);
    }
    return plex (t.regular, 12.0f);
}

juce::String captionText (const juce::String& s) { return s.toUpperCase(); }

// Depth --------------------------------------------------------------------------------------

namespace depth
{
void shadow (juce::Graphics& g, juce::Rectangle<float> r, float rad, Level level)
{
    if (level == Level::flat)
        return;
    // A few offset translucent fills read as a soft shadow without a blur pass.
    const auto dark = Theme::get().isDark();
    struct Layer
    {
        float dy, grow, alpha;
    };
    static constexpr Layer raisedLayers[] = { { 1.0f, 0.5f, 0.45f }, { 0.0f, 1.0f, 0.18f } };
    static constexpr Layer panelLayers[] = { { 1.0f, 0.5f, 0.35f }, { 2.0f, 1.5f, 0.18f }, { 3.0f, 3.0f, 0.08f } };
    static constexpr Layer floatLayers[] = { { 2.0f, 1.0f, 0.3f }, { 4.0f, 4.0f, 0.16f }, { 8.0f, 10.0f, 0.08f } };
    const auto layers = level == Level::raised ? juce::Span<const Layer> (raisedLayers) : level == Level::panel ? juce::Span<const Layer> (panelLayers) : juce::Span<const Layer> (floatLayers);
    for (const auto& l : layers)
    {
        g.setColour (juce::Colours::black.withAlpha (l.alpha * (dark ? 1.0f : 0.45f)));
        g.fillRoundedRectangle (r.translated (0.0f, l.dy).expanded (l.grow), rad + l.grow);
    }
}

void highlight (juce::Graphics& g, juce::Rectangle<float> r, float rad)
{
    g.setColour (juce::Colours::white.withAlpha (Theme::get().isDark() ? 0.07f : 0.7f));
    g.fillRect (r.getX() + rad, r.getY(), r.getWidth() - 2.0f * rad, 1.0f);
}

void raised (juce::Graphics& g, juce::Rectangle<float> r, float rad, bool hover, Level level)
{
    shadow (g, r, rad, level);
    auto top = colour (Token::raisedTop), bottom = colour (Token::raisedBottom);
    if (hover)
    {
        top = colour (Token::hover).brighter (0.06f);
        bottom = colour (Token::hover);
    }
    g.setGradientFill (juce::ColourGradient::vertical (top, r.getY(), bottom, r.getBottom()));
    g.fillRoundedRectangle (r, rad);
    highlight (g, r, rad);
}

void pressed (juce::Graphics& g, juce::Rectangle<float> r, float rad)
{
    g.setColour (colour (Token::pressed));
    g.fillRoundedRectangle (r, rad);
    juce::ColourGradient shade (juce::Colours::black.withAlpha (Theme::get().isDark() ? 0.45f : 0.15f), 0.0f, r.getY(), juce::Colours::transparentBlack, 0.0f, r.getY() + 3.0f, false);
    g.setGradientFill (shade);
    g.fillRoundedRectangle (r, rad);
    g.setColour (juce::Colours::white.withAlpha (Theme::get().isDark() ? 0.04f : 0.5f));
    g.fillRect (r.getX() + rad, r.getBottom() - 1.0f, r.getWidth() - 2.0f * rad, 1.0f);
}

void well (juce::Graphics& g, juce::Rectangle<float> r, float rad)
{
    g.setColour (colour (Token::well));
    g.fillRoundedRectangle (r, rad);
    juce::ColourGradient shade (juce::Colours::black.withAlpha (Theme::get().isDark() ? 0.55f : 0.18f), 0.0f, r.getY(), juce::Colours::transparentBlack, 0.0f,
                                r.getY() + std::min (4.0f, r.getHeight()), false);
    g.setGradientFill (shade);
    g.fillRoundedRectangle (r, rad);
}

void tinted (juce::Graphics& g, juce::Rectangle<float> r, float rad, juce::Colour c)
{
    shadow (g, r, rad, Level::raised);
    g.setGradientFill (juce::ColourGradient::vertical (c.brighter (0.08f), r.getY(), c.darker (0.08f), r.getBottom()));
    g.fillRoundedRectangle (r, rad);
    g.setColour (juce::Colours::white.withAlpha (0.25f));
    g.fillRect (r.getX() + rad, r.getY(), r.getWidth() - 2.0f * rad, 1.0f);
}

void focusRing (juce::Graphics& g, juce::Rectangle<float> r, float rad)
{
    g.setColour (colour (Token::accent));
    g.drawRoundedRectangle (r.expanded (2.0f), rad + 2.0f, 2.0f);
}
juce::Colour shade (float darkAlpha, float lightAlpha) { return juce::Colours::black.withAlpha (Theme::get().isDark() ? darkAlpha : lightAlpha); }

juce::Colour wash (float darkAlpha, float lightAlpha)
{
    return Theme::get().isDark() ? juce::Colours::white.withAlpha (darkAlpha) : juce::Colours::black.withAlpha (lightAlpha);
}
} // namespace depth

// Roles ----

juce::Colour buttonTextColour (const juce::Button& b, bool over, bool down)
{
    const auto role = roleOf (b);
    const bool on = b.getToggleState() || down;
    if (! b.isEnabled())
        return colour (Token::textDisabled);
    if (role == ButtonRole::primary)
        return colour (Token::onAccent);
    if (on && (role == ButtonRole::solo || role == ButtonRole::mute))
        return colour (Token::regionText);
    if (on && role == ButtonRole::record)
        return colour (Token::onRecord);
    if (role == ButtonRole::warning)
        return colour (Token::onWarning);
    if (role == ButtonRole::record && static_cast<bool> (b.getProperties()["weftTintedWhenOff"]))
        return colour (Token::record);
    if (on && role == ButtonRole::tab)
        return colour (Token::text);
    return on || over ? colour (Token::text) : colour (Token::textDim);
}

void paintGroup (juce::Graphics& g, juce::Rectangle<int> bounds)
{
    const auto r = bounds.toFloat().reduced (0.5f, 1.0f);
    depth::raised (g, r, radius::r2);
    g.setColour (depth::shade (0.35f, 0.12f));
    g.drawRoundedRectangle (r, radius::r2, 1.0f);
}

juce::Colour collaboratorColour (int index)
{
    static const juce::uint32 colours[12] = { 0xff3b82f6, 0xff14b8a6, 0xfff59e0b, 0xff22c55e, 0xffa855f7, 0xffec4899,
                                              0xfff97316, 0xff06b6d4, 0xff84cc16, 0xff6366f1, 0xffeab308, 0xfff43f5e };
    return C (colours[((index % 12) + 12) % 12]);
}

juce::Colour textOn (juce::Colour c)
{
    const C darkText (0xff16171a), lightText (0xfff4f5f6);
    return contrastRatio (darkText, c) >= contrastRatio (lightText, c) ? darkText : lightText;
}

namespace icon
{
juce::Path goToStart()
{
    juce::Path p;
    p.addRectangle (3.0f, 3.0f, 2.0f, 10.0f);
    p.addTriangle (13.0f, 3.0f, 13.0f, 13.0f, 6.0f, 8.0f);
    return p;
}
juce::Path rewind()
{
    juce::Path p;
    p.addTriangle (8.0f, 3.0f, 8.0f, 13.0f, 2.0f, 8.0f);
    p.addTriangle (14.0f, 3.0f, 14.0f, 13.0f, 8.0f, 8.0f);
    return p;
}
juce::Path forward()
{
    juce::Path p;
    p.addTriangle (2.0f, 3.0f, 2.0f, 13.0f, 8.0f, 8.0f);
    p.addTriangle (8.0f, 3.0f, 8.0f, 13.0f, 14.0f, 8.0f);
    return p;
}
juce::Path stop()
{
    juce::Path p;
    p.addRoundedRectangle (4.0f, 4.0f, 8.0f, 8.0f, 1.0f);
    return p;
}
juce::Path play()
{
    juce::Path p;
    p.addTriangle (4.0f, 3.0f, 4.0f, 13.0f, 13.0f, 8.0f);
    return p;
}
juce::Path record()
{
    juce::Path p;
    p.addEllipse (3.5f, 3.5f, 9.0f, 9.0f);
    return p;
}
juce::Path cycle()
{
    juce::Path p;
    p.startNewSubPath (3.0f, 9.0f);
    p.lineTo (3.0f, 7.0f);
    p.quadraticTo (3.0f, 5.0f, 5.0f, 5.0f);
    p.lineTo (11.0f, 5.0f);
    p.startNewSubPath (13.0f, 7.0f);
    p.lineTo (13.0f, 9.0f);
    p.quadraticTo (13.0f, 11.0f, 11.0f, 11.0f);
    p.lineTo (5.0f, 11.0f);
    juce::Path stroked;
    juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath (stroked, p);
    stroked.addTriangle (10.0f, 2.5f, 10.0f, 7.5f, 13.5f, 5.0f);
    stroked.addTriangle (6.0f, 8.5f, 6.0f, 13.5f, 2.5f, 11.0f);
    return stroked;
}
juce::Path metronome()
{
    juce::Path p;
    p.addTriangle (8.0f, 2.0f, 3.5f, 14.0f, 12.5f, 14.0f);
    juce::Path arm;
    arm.addLineSegment ({ 8.0f, 11.0f, 12.0f, 4.0f }, 1.5f);
    p.addPath (arm);
    p.setUsingNonZeroWinding (false);
    return p;
}
juce::Path lock()
{
    juce::Path shackle;
    shackle.addCentredArc (8.0f, 7.0f, 3.0f, 3.5f, 0.0f, -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, true);
    juce::Path p;
    juce::PathStrokeType (1.5f).createStrokedPath (p, shackle);
    p.addRoundedRectangle (3.0f, 7.0f, 10.0f, 7.0f, 1.0f);
    return p;
}
} // namespace icon----------------------------------------------------------------------------------

void setRole (juce::Button& b, ButtonRole role, bool tintedWhenOff)
{
    b.getProperties().set ("weftRole", static_cast<int> (role));
    b.getProperties().set ("weftTintedWhenOff", tintedWhenOff);
}

ButtonRole roleOf (const juce::Component& c)
{
    const auto v = c.getProperties()["weftRole"];
    return v.isVoid() ? ButtonRole::normal : static_cast<ButtonRole> (static_cast<int> (v));
}

// Tokens -------------------------------------------------------------------------------------

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

juce::Colour regionBody (juce::Colour c) { return colour (Token::lane).interpolatedWith (c, Theme::get().isDark() ? 0.28f : 0.45f); }

double contrastRatio (juce::Colour a, juce::Colour b)
{
    const auto la = luminance (a), lb = luminance (b);
    return (std::max (la, lb) + 0.05) / (std::min (la, lb) + 0.05);
}

const std::vector<TokenPair>& textPairs()
{
    static const std::vector<TokenPair> pairs {
        { Token::text, Token::background },       { Token::text, Token::surface },         { Token::text, Token::surfaceRaised },
        { Token::text, Token::lane },             { Token::text, Token::laneAlt },         { Token::text, Token::hover },
        { Token::text, Token::raisedBottom },     { Token::text, Token::pressed },         { Token::text, Token::well },
        { Token::textDim, Token::background },    { Token::textDim, Token::surface },      { Token::textDim, Token::surfaceRaised },
        { Token::textDim, Token::lane },          { Token::textDim, Token::hover },        { Token::textDim, Token::raisedTop },
        { Token::onAccent, Token::accent },       { Token::onRecord, Token::record },      { Token::onWarning, Token::warning },
        { Token::regionText, Token::solo },       { Token::regionText, Token::mute },      { Token::accent, Token::background },
        { Token::accent, Token::surface },        { Token::accent, Token::lane },          { Token::accent, Token::raisedBottom },
    };
    return pairs;
}

// Theme --------------------------------------------------------------------------------------

Theme& Theme::get()
{
    static Theme theme;
    return theme;
}

Theme::Theme() : lnf (std::make_unique<WeftLookAndFeel>()) { apply(); }

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
    lnf->setColour (juce::TextButton::buttonOnColourId, t[Token::pressed]);
    lnf->setColour (juce::TextButton::textColourOnId, t[Token::text]);
    lnf->setColour (juce::TextButton::textColourOffId, t[Token::textDim]);
    lnf->setColour (juce::TextButton::buttonColourId, t[Token::surfaceRaised]);
    lnf->setColour (juce::Label::textColourId, t[Token::text]);
    lnf->setColour (juce::TooltipWindow::backgroundColourId, t[Token::surfaceRaised]);
    lnf->setColour (juce::TooltipWindow::textColourId, t[Token::text]);
    lnf->setColour (juce::TabbedButtonBar::tabTextColourId, t[Token::textDim]);
    lnf->setColour (juce::TabbedButtonBar::frontTextColourId, t[Token::text]);
    lnf->setColour (juce::TabbedComponent::backgroundColourId, t[Token::surface]);
    lnf->setColour (juce::TabbedComponent::outlineColourId, juce::Colours::transparentBlack);
    lnf->setColour (juce::ListBox::backgroundColourId, t[Token::surface]);
    lnf->setColour (juce::ListBox::textColourId, t[Token::text]);
    lnf->setColour (juce::DirectoryContentsDisplayComponent::textColourId, t[Token::text]);
    lnf->setColour (juce::DirectoryContentsDisplayComponent::highlightColourId, t[Token::hover]);
    lnf->setColour (juce::DirectoryContentsDisplayComponent::highlightedTextColourId, t[Token::text]);
    lnf->setColour (juce::TextEditor::backgroundColourId, t[Token::well]);
    lnf->setColour (juce::TextEditor::textColourId, t[Token::text]);
    lnf->setColour (juce::TextEditor::outlineColourId, t[Token::lineSubtle]);
    lnf->setColour (juce::TextEditor::focusedOutlineColourId, t[Token::accent]);
    lnf->setColour (juce::TextEditor::highlightColourId, t[Token::accent].withAlpha (0.35f));
    lnf->setColour (juce::CaretComponent::caretColourId, t[Token::accent]);
    lnf->setColour (juce::ComboBox::backgroundColourId, t[Token::surfaceRaised]);
    lnf->setColour (juce::ComboBox::textColourId, t[Token::text]);
    lnf->setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
    lnf->setColour (juce::ComboBox::focusedOutlineColourId, t[Token::accent]);
    lnf->setColour (juce::PopupMenu::backgroundColourId, t[Token::surfaceRaised]);
    lnf->setColour (juce::PopupMenu::textColourId, t[Token::text]);
    lnf->setColour (juce::PopupMenu::highlightedBackgroundColourId, t[Token::hover]);
    lnf->setColour (juce::PopupMenu::highlightedTextColourId, t[Token::text]);
    lnf->setColour (juce::Slider::textBoxTextColourId, t[Token::text]);
    lnf->setColour (juce::Slider::textBoxBackgroundColourId, t[Token::well]);
    lnf->setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    lnf->setColour (juce::Slider::thumbColourId, t[Token::capTop]);
    lnf->setColour (juce::ScrollBar::thumbColourId, t[Token::border]);
    lnf->setColour (juce::ResizableWindow::backgroundColourId, t[Token::surface]);
    lnf->setColour (juce::AlertWindow::backgroundColourId, t[Token::surface]);
    lnf->setColour (juce::AlertWindow::textColourId, t[Token::text]);
    lnf->setColour (juce::AlertWindow::outlineColourId, t[Token::border]);
    lnf->setColour (juce::ProgressBar::foregroundColourId, t[Token::accent]);
    lnf->setColour (juce::ProgressBar::backgroundColourId, t[Token::well]);
    lnf->setColour (juce::DocumentWindow::textColourId, t[Token::text]);
    lnf->setColour (juce::CaretComponent::caretColourId, t[Token::accent]);
    lnf->setDefaultSansSerifTypeface (Typefaces::get().regular);
}
} // namespace weft::ui
