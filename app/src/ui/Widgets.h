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

/** A key for display: "F♯ minor", "B♭ major" (the model's # and b as sharp and flat signs). */
inline juce::String keyText (const model::KeySignature& key)
{
    auto name = juce::String (key.name());
    const auto space = name.indexOfChar (' ');
    auto tonic = name.substring (0, space).replace ("#", juce::String::charToString (0x266F)).replace ("b", juce::String::charToString (0x266D));
    return tonic + name.substring (space);
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
    /** Right-click opens this menu instead of running the command (D-UI-35); empty: right-click clicks. */
    std::function<juce::PopupMenu()> popupMenu;
    std::function<void (int)> popupPicked;
    void showPopup()
    {
        if (! popupMenu)
            return;
        juce::Component::SafePointer<CommandButton> self (this);
        popupMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [self] (int item) {
            if (self != nullptr && item != 0 && self->popupPicked)
                self->popupPicked (item);
        });
    }
    /** The last click came from the popup-menu button (right-click, or Ctrl+click on a Mac). */
    bool popupClick = false;

    using juce::TextButton::clicked;
    void clicked (const juce::ModifierKeys& mods) override
    {
        popupClick = mods.isPopupMenu();
        juce::TextButton::clicked (mods);
    }

private:
    juce::Path icon;
};

/** A track's Mute, Solo or Record button (D-UI-25). Pressing one and dragging over the same button of other tracks
    gives each the state the first one took; the click itself still goes through onClick. */
class SwipeButton final : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;

    /** Sets this button's track on or off (not a toggle). */
    std::function<void (bool on)> setTrackState;
    /** Around a swipe (one undo step for mutes). */
    std::function<void()> onSwipeStart, onSwipeEnd;

    /** The pointer is over `other` during a drag from this button. Tests call it directly. */
    void swipeOver (SwipeButton* other)
    {
        if (other == nullptr || other == this || ! setTrackState || roleOf (*other) != roleOf (*this) || other->getButtonText() != getButtonText() || ! other->isEnabled() || ! other->isVisible())
            return;
        for (const auto& done : swiped)
            if (done == other)
                return;
        if (! swiping)
        {
            swiping = true;
            target = ! getToggleState();
            if (onSwipeStart)
                onSwipeStart();
            if (setTrackState)
                setTrackState (target);
        }
        swiped.emplace_back (other);
        if (other->setTrackState)
            other->setTrackState (target);
    }
    /** Ends a swipe started by swipeOver(); false when there was none. */
    bool endSwipe()
    {
        swiped.clear();
        if (! std::exchange (swiping, false))
            return false;
        if (onSwipeEnd)
            onSwipeEnd();
        return true;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        swiped.clear();
        swiping = false;
        juce::TextButton::mouseDown (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        juce::TextButton::mouseDrag (e);
        if (e.mods.isAnyModifierKeyDown())
            return;
        auto* top = getTopLevelComponent();
        auto* under = top->getComponentAt (e.getEventRelativeTo (top).position.toInt());
        swipeOver (dynamic_cast<SwipeButton*> (under));
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! endSwipe())
            juce::TextButton::mouseUp (e);
        else
            setState (buttonNormal); // a swipe isn't also a click
    }

private:
    bool swiping = false, target = false;
    std::vector<juce::Component::SafePointer<SwipeButton>> swiped;
};

/** A Mute button whose track a solo elsewhere silences: drawn with a faint mute tint (D-UI-31). */
inline void setSilencedBySolo (juce::Button& b, bool silenced)
{
    if (static_cast<bool> (b.getProperties()["weftImplied"]) != silenced)
    {
        b.getProperties().set ("weftImplied", silenced);
        b.repaint();
    }
}
inline bool silencedBySolo (const juce::Button& b) { return static_cast<bool> (b.getProperties()["weftImplied"]); }
/** A small modal box with one text field; `done` gets the text when OK is pressed. */
inline void askForText (const juce::String& title, const juce::String& current, std::function<void (juce::String)> done)
{
    auto* w = new juce::AlertWindow (title, {}, juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("text", current);
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([w, done = std::move (done)] (int result) {
                            if (result == 1)
                                done (w->getTextEditorContents ("text"));
                        }),
                        true);
}
} // namespace weft::ui
