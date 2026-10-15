#pragma once

// Small shared pieces: command buttons that carry their name and shortcut (F-UI-10).

#include "Commands.h"
#include "Theme.h"

namespace weft::ui
{
/** Names the control for accessibility and gives it "Name (Shortcut)" as its tooltip. */
inline void describe (juce::Component& c, const juce::String& name, const juce::String& tooltip = {})
{
    c.setTitle (name);
    c.setDescription (tooltip.isEmpty() ? name : tooltip);
    if (auto* t = dynamic_cast<juce::SettableTooltipClient*> (&c))
        t->setTooltip (tooltip.isEmpty() ? name : tooltip);
}

inline void describe (juce::Component& c, Cmd command) { describe (c, commandInfo (command).name, tooltipFor (command)); }

/** A pill-shaped text button for a command; toggles show their state. */
class CommandButton final : public juce::TextButton
{
public:
    CommandButton (const juce::String& label, Cmd cmd, bool toggle = false) : juce::TextButton (label), command (cmd)
    {
        describe (*this, cmd);
        setClickingTogglesState (false);
        if (toggle)
            setToggleable (true);
        setWantsKeyboardFocus (false);
    }
    const Cmd command;
};
} // namespace weft::ui
