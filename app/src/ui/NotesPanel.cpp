#include "NotesPanel.h"

namespace weft::ui
{
namespace
{
constexpr float textSizes[] = { 12.0f, 14.0f, 18.0f };

void styleNotes (juce::TextEditor& e)
{
    e.setMultiLine (true, true);
    e.setReturnKeyStartsNewLine (true);
    e.setScrollbarsShown (true);
    e.setReadOnly (true);
    e.setCaretVisible (false);
}
} // namespace

NotesPanel::NotesPanel (Studio& s) : studio (s)
{
    projectTitle.setText ("Project", juce::dontSendNotification);
    projectTitle.setFont (font (Type::strong));
    addAndMakeVisible (projectTitle);
    trackTitle.setFont (font (Type::strong));
    addAndMakeVisible (trackTitle);
    for (auto* e : { &projectNotes, &trackNotes })
    {
        styleNotes (*e);
        addAndMakeVisible (e);
    }
    describe (projectNotes, "Project notes", "Notes about the project (Edit to change them)");
    describe (trackNotes, "Track notes", "Notes about the selected track (Edit to change them)");
    describe (edit, "Edit", "Edit the notes; Done saves them");
    edit.onClick = [this] { setEditing (! isEditing); };
    addAndMakeVisible (edit);
    describe (textSize, "Text Size", "Small, medium or large text");
    textSize.onClick = [this] {
        juce::PopupMenu menu;
        menu.addItem (1, "Small", true, sizeIndex == 0);
        menu.addItem (2, "Medium", true, sizeIndex == 1);
        menu.addItem (3, "Large", true, sizeIndex == 2);
        juce::Component::SafePointer<NotesPanel> self (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (textSize), [self] (int item) {
            if (self != nullptr && item > 0)
                self->setTextSize (item - 1);
        });
    };
    addAndMakeVisible (textSize);
    studio.addChangeListener (this);
    setTextSize (1);
    refresh();
}

NotesPanel::~NotesPanel() { studio.removeChangeListener (this); }

void NotesPanel::setTextSize (int index)
{
    sizeIndex = std::clamp (index, 0, 2);
    for (auto* e : { &projectNotes, &trackNotes })
    {
        const auto text = e->getText();
        e->setFont (juce::FontOptions (textSizes[sizeIndex]));
        e->setText (text, false); // the new size applies to text already there
    }
}

void NotesPanel::setEditing (bool on)
{
    if (isEditing && ! on)
        save();
    isEditing = on;
    edit.setButtonText (on ? "Done" : "Edit");
    refresh();
}

void NotesPanel::save()
{
    auto& s = studio.session();
    const auto project = projectNotes.getText().toStdString();
    if (project != studio.model().info.notes)
        studio.check (s.setProjectNotes (project));
    if (const auto* t = studio.model().track (shownTrack))
    {
        const auto text = trackNotes.getText().toStdString();
        if (text != t->notes)
            studio.check (s.setTrackNotes (shownTrack, text));
    }
    studio.edited();
}

void NotesPanel::refresh()
{
    const auto& m = studio.model();
    const auto* t = m.track (studio.selectedTrack());
    // While editing, a change of track saves what was typed for the last one.
    if (isEditing && t != nullptr && t->id != shownTrack && m.track (shownTrack) != nullptr)
    {
        const auto text = trackNotes.getText().toStdString();
        if (text != m.track (shownTrack)->notes)
            studio.check (studio.session().setTrackNotes (shownTrack, text));
    }
    const bool trackChanged = t == nullptr ? ! shownTrack.empty() : t->id != shownTrack;
    shownTrack = t != nullptr ? t->id : Id();
    trackTitle.setText (t != nullptr ? juce::String::fromUTF8 (t->name.c_str()) : juce::String ("No track selected"), juce::dontSendNotification);
    if (! isEditing || ! projectNotes.hasKeyboardFocus (true))
        if (projectNotes.getText().toStdString() != m.info.notes && ! isEditing)
            projectNotes.setText (juce::String::fromUTF8 (m.info.notes.c_str()), false);
    if (! isEditing || trackChanged)
        trackNotes.setText (t != nullptr ? juce::String::fromUTF8 (t->notes.c_str()) : juce::String(), false);
    trackNotes.setEnabled (t != nullptr);
    projectNotes.setReadOnly (! isEditing);
    trackNotes.setReadOnly (! isEditing || t == nullptr || studio.lockedByOther (t->id));
    for (auto* e : { &projectNotes, &trackNotes })
        e->setCaretVisible (isEditing && ! e->isReadOnly());
}

void NotesPanel::paint (juce::Graphics& g) { g.fillAll (colour (Token::surface)); }

void NotesPanel::resized()
{
    auto r = getLocalBounds().reduced (space::s1);
    auto top = r.removeFromTop (size::control);
    edit.setBounds (top.removeFromRight (64));
    top.removeFromRight (space::half);
    textSize.setBounds (top.removeFromRight (80));
    r.removeFromTop (space::s1);
    const auto half = (r.getHeight() - 2 * size::control - space::s1) / 2;
    projectTitle.setBounds (r.removeFromTop (size::control));
    projectNotes.setBounds (r.removeFromTop (half));
    r.removeFromTop (space::s1);
    trackTitle.setBounds (r.removeFromTop (size::control));
    trackNotes.setBounds (r);
}
} // namespace weft::ui
