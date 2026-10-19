#pragma once

// Small shared pieces: command buttons that carry their name and shortcut (F-UI-10).

#include "Commands.h"
#include "Theme.h"

namespace weft::ui
{
/** False in the headless UI tests: plug-in windows and questions are built but never put on
    the desktop (a virtual X server without a window manager rejects them). */
inline bool& desktopWindows()
{
    static bool on = true;
    return on;
}

/** Names the control for accessibility and gives it "Name (Shortcut)" as its tooltip. */
inline void describe (juce::Component& c, const juce::String& name, const juce::String& tooltip = {})
{
    c.setTitle (name);
    c.setDescription (tooltip.isEmpty() ? name : tooltip);
    if (auto* t = dynamic_cast<juce::SettableTooltipClient*> (&c))
        t->setTooltip (tooltip.isEmpty() ? name : tooltip);
    // Every named control is reachable from the keyboard (D-UI-13).
    if (dynamic_cast<juce::Button*> (&c) != nullptr || dynamic_cast<juce::Slider*> (&c) != nullptr || dynamic_cast<juce::ComboBox*> (&c) != nullptr)
        makeFocusable (c);
}

inline void describe (juce::Component& c, Cmd command) { describe (c, commandInfo (command).name, tooltipFor (command)); }

/** A text or icon button for a command; toggles show their state. */
class CommandButton final : public juce::TextButton
{
public:
    CommandButton (const juce::String& label, Cmd cmd, bool toggle = false) : juce::TextButton (label), command (cmd)
    {
        describe (*this, cmd);
        setClickingTogglesState (false);
        if (toggle)
            setToggleable (true);
    }

    /** Draws `path` (16 x 16 units) instead of the label; the label stays as the accessible name. */
    void setIcon (juce::Path path)
    {
        icon = std::move (path);
        repaint();
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        if (icon.isEmpty())
        {
            juce::TextButton::paintButton (g, over, down);
            return;
        }
        getLookAndFeel().drawButtonBackground (g, *this, findColour (buttonColourId), over, down);
        const auto box = getLocalBounds().toFloat().withSizeKeepingCentre (static_cast<float> (size::icon), static_cast<float> (size::icon));
        g.setColour (buttonTextColour (*this, over, down));
        g.fillPath (icon, juce::AffineTransform::translation (box.getX(), box.getY()));
    }

    const Cmd command;

private:
    juce::Path icon;
};

} // namespace weft::ui
