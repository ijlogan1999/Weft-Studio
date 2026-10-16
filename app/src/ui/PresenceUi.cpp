#include "PresenceUi.h"

#include "Theme.h"

namespace weft::ui
{
juce::Colour collaboratorColour (int index)
{
    static const juce::uint32 palette[12] = { 0xff3b82f6, 0xff14b8a6, 0xfff59e0b, 0xff22c55e, 0xffa855f7, 0xffec4899,
                                              0xfff97316, 0xff06b6d4, 0xff84cc16, 0xff6366f1, 0xffeab308, 0xfff43f5e };
    return juce::Colour (palette[((index % 12) + 12) % 12]);
}

juce::Colour colourOf (const collab::RemotePresence& who) { return collaboratorColour (who.color); }

juce::String initials (const std::string& displayName)
{
    const auto words = juce::StringArray::fromTokens (juce::String::fromUTF8 (displayName.c_str()), true);
    if (words.isEmpty())
        return "?";
    if (words.size() == 1)
        return words[0].substring (0, 2).toUpperCase();
    return (words[0].substring (0, 1) + words[words.size() - 1].substring (0, 1)).toUpperCase();
}

float pointerAlpha (const Studio& studio, const collab::RemotePresence& who)
{
    if (studio.sync() == nullptr || ! who.state.pointer)
        return 0.0f;
    const auto age = static_cast<float> (studio.sync()->now() - who.pointerMovedAt);
    return std::clamp (1.0f - (age - 4000.0f) / 1000.0f, 0.0f, 1.0f);
}

void drawPointer (juce::Graphics& g, juce::Point<float> tip, juce::Colour c, const juce::String& name, float alpha)
{
    if (alpha <= 0.0f)
        return;
    juce::Path arrow;
    arrow.startNewSubPath (tip);
    arrow.lineTo (tip.translated (0.0f, 14.0f));
    arrow.lineTo (tip.translated (4.0f, 10.5f));
    arrow.lineTo (tip.translated (10.0f, 10.0f));
    arrow.closeSubPath();
    g.setColour (c.withMultipliedAlpha (alpha));
    g.fillPath (arrow);
    g.setColour (juce::Colours::white.withAlpha (0.9f * alpha));
    g.strokePath (arrow, juce::PathStrokeType (1.0f));
    const juce::Font font (juce::FontOptions (11.0f, juce::Font::bold));
    g.setFont (font);
    const auto w = juce::GlyphArrangement::getStringWidth (font, name) + 10.0f;
    const auto label = juce::Rectangle<float> (tip.x + 10.0f, tip.y + 12.0f, w, 16.0f);
    g.setColour (c.withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (label, 4.0f);
    g.setColour ((c.getPerceivedBrightness() > 0.6f ? juce::Colours::black : juce::Colours::white).withAlpha (alpha));
    g.drawText (name, label, juce::Justification::centred, false);
}

void drawBadge (juce::Graphics& g, juce::Rectangle<float> box, juce::Colour c, const juce::String& text, bool recording, bool dim)
{
    auto fill = recording ? colour (Token::record) : c;
    if (dim)
        fill = fill.withMultipliedAlpha (0.45f);
    g.setColour (fill);
    g.fillEllipse (box);
    if (recording)
    {
        g.setColour (colour (Token::record).withAlpha (0.5f));
        g.drawEllipse (box.expanded (2.0f), 1.5f);
    }
    g.setColour (recording ? colour (Token::onRecord) : (c.getPerceivedBrightness() > 0.6f ? juce::Colours::black : juce::Colours::white));
    g.setFont (juce::FontOptions (box.getHeight() * 0.42f, juce::Font::bold));
    g.drawText (text, box, juce::Justification::centred, false);
}

juce::String describe (const Studio& studio, const collab::RemotePresence& who)
{
    using S = collab::Presence::Status;
    auto text = juce::String::fromUTF8 (who.displayName.c_str());
    if (! who.hasState)
        return text + "\nJoining";
    switch (who.state.status)
    {
        case S::active: text << "\nEditing"; break;
        case S::idle: text << "\nIdle"; break;
        case S::recording: text << "\nRecording"; break;
        case S::offlineEditing: text << "\nEditing offline"; break;
    }
    if (const auto* t = studio.model().track (who.state.focusTrack))
        text << " on " << juce::String::fromUTF8 (t->name.c_str());
    switch (who.state.view)
    {
        case collab::Presence::View::pianoRoll: text << ", in the piano roll"; break;
        case collab::Presence::View::audioEditor: text << ", in the audio editor"; break;
        case collab::Presence::View::mixer: text << ", in the mixer"; break;
        case collab::Presence::View::arrange: break;
    }
    return text;
}

Avatars::Avatars (Studio& s) : studio (s) { setTitle ("Collaborators"); }

int Avatars::preferredWidth() const
{
    const auto n = static_cast<int> (studio.collaborators().size());
    return std::min (n, maxShown) * (size + 2) + (n > maxShown ? 32 : 0);
}

int Avatars::avatarAt (int x) const
{
    const auto n = static_cast<int> (studio.collaborators().size());
    const auto i = x / (size + 2);
    return x >= 0 && i < std::min (n, maxShown) ? i : -1;
}

void Avatars::paint (juce::Graphics& g)
{
    const auto who = studio.collaborators();
    const auto y = (getHeight() - size) / 2.0f;
    for (int i = 0; i < std::min (static_cast<int> (who.size()), maxShown); ++i)
    {
        const auto* p = who[static_cast<std::size_t> (i)];
        const auto box = juce::Rectangle<float> (static_cast<float> (i * (size + 2)) + 1.0f, y, size - 2.0f, size - 2.0f);
        drawBadge (g, box, colourOf (*p), initials (p->displayName), p->state.status == collab::Presence::Status::recording,
                   p->state.status == collab::Presence::Status::idle);
    }
    if (who.size() > maxShown)
    {
        g.setColour (colour (Token::textDim));
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText ("+" + juce::String (static_cast<int> (who.size()) - maxShown), maxShown * (size + 2), 0, 32, getHeight(), juce::Justification::centred);
    }
}

juce::String Avatars::getTooltip()
{
    const auto who = studio.collaborators();
    const auto i = avatarAt (getMouseXYRelative().x);
    if (i >= 0)
        return describe (studio, *who[static_cast<std::size_t> (i)]);
    juce::StringArray rest;
    for (std::size_t k = maxShown; k < who.size(); ++k)
        rest.add (juce::String::fromUTF8 (who[k]->displayName.c_str()));
    return rest.joinIntoString ("\n");
}

void Avatars::mouseUp (const juce::MouseEvent& e)
{
    if (const auto i = avatarAt (e.x); i >= 0)
        goTo (i);
}

void Avatars::goTo (int index)
{
    const auto who = studio.collaborators();
    if (index < 0 || index >= static_cast<int> (who.size()))
        return;
    const auto& p = *who[static_cast<std::size_t> (index)];
    auto target = static_cast<double> (studio.collaboratorPlayhead (p));
    for (const auto& id : p.state.regions)
        if (const auto* r = studio.model().region (id))
        {
            target = static_cast<double> (r->start);
            break;
        }
    studio.view.originTick = std::max (0.0, target - studio.timelineWidth * 0.1 / studio.view.pixelsPerTick);
    studio.sendChangeMessage();
}
} // namespace weft::ui
