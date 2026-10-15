#include "NewTrackDialog.h"

namespace weft::ui
{
NewTrackDialog::NewTrackDialog (Studio& s) : studio (s)
{
    setTitle ("New Track");
    kind.addItemList ({ "Audio", "Software instrument", "Bus" }, 1);
    kind.setSelectedItemIndex (0, juce::dontSendNotification);
    describe (kind, "Track type");
    channels.addItemList ({ "Mono", "Stereo" }, 1);
    channels.setSelectedItemIndex (1, juce::dontSendNotification);
    describe (channels, "Channels", "Audio track channels");
    kind.onChange = [this] { channels.setEnabled (kind.getSelectedItemIndex() == 0); };
    count.setSliderStyle (juce::Slider::IncDecButtons);
    count.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 50, 24);
    count.setRange (1, 16, 1);
    count.setValue (1, juce::dontSendNotification);
    describe (count, "Count", "Number of tracks to create");
    describe (ok, "Create", "Create the tracks (Enter)");
    describe (cancel, "Cancel", "Close without creating tracks (Esc)");
    ok.addShortcut (juce::KeyPress (juce::KeyPress::returnKey));
    cancel.addShortcut (juce::KeyPress (juce::KeyPress::escapeKey));
    ok.onClick = [this] {
        create();
        if (onClose)
            onClose();
    };
    cancel.onClick = [this] {
        if (onClose)
            onClose();
    };
    for (auto* c : std::initializer_list<juce::Component*> { &kind, &channels, &count, &ok, &cancel })
        addAndMakeVisible (c);
    setSize (380, 200);
}

std::vector<Id> NewTrackDialog::create()
{
    static const model::TrackKind kinds[] = { model::TrackKind::audio, model::TrackKind::instrument, model::TrackKind::bus };
    const auto k = kinds[std::clamp (kind.getSelectedItemIndex(), 0, 2)];
    const auto ch = k == model::TrackKind::audio && channels.getSelectedItemIndex() == 0 ? 1 : 2;
    auto ids = studio.session().createTracks (k, static_cast<int> (count.getValue()), ch, studio.selectedTrack());
    if (ids.empty())
        studio.setStatus (juce::String (studio.session().lastError()));
    else
        studio.selectTrack (ids.back());
    studio.edited();
    return ids;
}

void NewTrackDialog::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::surfaceRaised));
    g.setColour (colour (Token::text));
    g.setFont (juce::FontOptions (13.0f));
    g.drawText ("Type", 20, 20, 90, 26, juce::Justification::centredLeft);
    g.drawText ("Count", 20, 56, 90, 26, juce::Justification::centredLeft);
    g.drawText ("Channels", 20, 92, 90, 26, juce::Justification::centredLeft);
}

void NewTrackDialog::resized()
{
    kind.setBounds (110, 20, getWidth() - 130, 26);
    count.setBounds (110, 56, 130, 26);
    channels.setBounds (110, 92, getWidth() - 130, 26);
    cancel.setBounds (getWidth() - 200, getHeight() - 46, 85, 28);
    ok.setBounds (getWidth() - 105, getHeight() - 46, 85, 28);
}

void NewTrackDialog::show (Studio& studio, juce::Component* parent)
{
    auto* dialog = new NewTrackDialog (studio);
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (dialog);
    options.dialogTitle = "New Track";
    options.dialogBackgroundColour = colour (Token::surfaceRaised);
    options.componentToCentreAround = parent;
    options.useNativeTitleBar = true;
    options.resizable = false;
    auto* window = options.launchAsync();
    dialog->onClose = [window] {
        if (window != nullptr)
            window->exitModalState (0);
    };
}
} // namespace weft::ui
