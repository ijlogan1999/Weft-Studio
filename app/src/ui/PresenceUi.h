#pragma once

// How collaborators are drawn (spec 04 §3.6, §6, §9; 05 §5): avatars in the control bar,
// badges on track headers, pointers, playheads and selection outlines in their colour.

#include "Studio.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace weft::ui
{
/** The 12 collaborator colours; the relay gives each member an index per project. */
juce::Colour collaboratorColour (int index);
juce::Colour colourOf (const collab::RemotePresence& who);
/** "Ada Lovelace" -> "AL"; one name -> its first two letters. */
juce::String initials (const std::string& displayName);
/** A pointer is fully shown for 4 s after it last moved, then fades out by 5 s (05 §5). */
float pointerAlpha (const Studio& studio, const collab::RemotePresence& who);
/** Pointer arrow with a name label, tip at `tip`. */
void drawPointer (juce::Graphics& g, juce::Point<float> tip, juce::Colour c, const juce::String& name, float alpha);
/** A round badge with initials; recording ones are red with a ring. */
void drawBadge (juce::Graphics& g, juce::Rectangle<float> box, juce::Colour c, const juce::String& text, bool recording, bool dim = false);
/** A member's collaborator colour (the accent for this user or someone unknown). */
juce::Colour userColour (const Studio& studio, const UserId& user);

/** A small padlock (locked tracks, 04 §9.6). */
void drawLock (juce::Graphics& g, juce::Rectangle<float> box, juce::Colour c);
/** A comment pin: a rounded speech mark, hollow once resolved (04 §9.3). */
void drawCommentPin (juce::Graphics& g, juce::Rectangle<float> box, juce::Colour c, bool resolved);

/** Avatar hover text: name, status and where they are (04 §9). */
juce::String describe (const Studio& studio, const collab::RemotePresence& who);

/** Control bar avatars: up to 5, then "+N". Clicking one scrolls to their selection or playhead. */
class Avatars final : public juce::Component, public juce::TooltipClient
{
public:
    explicit Avatars (Studio& studio);
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    juce::String getTooltip() override;
    /** Width for the avatars shown now; 0 with nobody else here. */
    int preferredWidth() const;
    /** Index into Studio::collaborators() of the avatar at x, or -1 (the "+N" is -1). */
    int avatarAt (int x) const;
    /** Scrolls the arrange view to collaborator `index` (what a click does). */
    void goTo (int index);
    static constexpr int maxShown = 5, size = 26;

private:
    Studio& studio;
};
} // namespace weft::ui
