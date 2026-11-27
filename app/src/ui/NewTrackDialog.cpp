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
    kind.onChange = [this] { updateForKind(); };
    channels.onChange = [this] { updateForKind(); };
    describe (input, "Input", "Audio input for the new tracks");
    describe (ascending, "Ascending inputs", "Several tracks take the next inputs in turn instead of the same one");
    describe (output, "Output", "Where the new tracks play: Master or a bus");
    output.addItem ("Master", 1);
    for (const auto& id : studio.model().trackOrder)
        if (const auto* t = studio.model().track (id); t != nullptr && t->kind == model::TrackKind::bus)
        {
            outputBuses.push_back (id);
            output.addItem (juce::String (t->name), static_cast<int> (outputBuses.size()) + 1);
        }
    output.setSelectedId (1, juce::dontSendNotification);
    describe (instrument, "Instrument", "Instrument for the new tracks");
    instrument.addItem ("Weft Keys (default)", 1);
    {
        const auto choices = studio.pluginChoices (true);
        for (std::size_t i = 0; i < choices.size(); ++i)
            if (choices[i].plugin.format != "builtin" || choices[i].label != "Weft Keys")
                instrument.addItem (choices[i].label, static_cast<int> (i) + 2);
    }
    instrument.setSelectedId (1, juce::dontSendNotification);
    describe (monitoring, "Input monitoring", "Turn input monitoring on for the new tracks");
    describe (recordEnable, "Record enable", "Record-enable the new tracks");
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
    for (auto* c : std::initializer_list<juce::Component*> { &kind, &channels, &count, &input, &ascending, &output, &instrument, &monitoring,
                                                             &recordEnable, &ok, &cancel })
        addAndMakeVisible (c);
    updateForKind();
    setSize (380, 330);
}

void NewTrackDialog::updateForKind()
{
    const auto k = kind.getSelectedItemIndex();
    const bool audio = k == 0, instrumentKind = k == 1;
    channels.setEnabled (audio);
    for (juce::Component* c : { static_cast<juce::Component*> (&input), static_cast<juce::Component*> (&ascending), static_cast<juce::Component*> (&monitoring) })
        c->setEnabled (audio);
    instrument.setEnabled (instrumentKind);
    recordEnable.setEnabled (audio || instrumentKind);
    // Inputs in the chosen format: 1, 2, ... for mono, 1-2, 3-4, ... for stereo.
    const auto previous = input.getSelectedId();
    input.clear (juce::dontSendNotification);
    input.addItem ("No input", noInput);
    const auto inputs = studio.device().config().numInputChannels;
    const auto pair = channels.getSelectedItemIndex() == 0 ? 1 : 2;
    for (int ch = 0; ch + pair <= inputs; ch += pair)
        input.addItem (pair == 1 ? "Input " + juce::String (ch + 1) : "Input " + juce::String (ch + 1) + "-" + juce::String (ch + 2), firstInput + ch);
    if (previous >= firstInput && input.indexOfItemId (previous) >= 0)
        input.setSelectedId (previous, juce::dontSendNotification);
    else
        input.setSelectedItemIndex (input.getNumItems() > 1 ? 1 : 0, juce::dontSendNotification);
}

std::vector<Id> NewTrackDialog::create()
{
    static const model::TrackKind kinds[] = { model::TrackKind::audio, model::TrackKind::instrument, model::TrackKind::bus };
    const auto k = kinds[std::clamp (kind.getSelectedItemIndex(), 0, 2)];
    const auto ch = k == model::TrackKind::audio && channels.getSelectedItemIndex() == 0 ? 1 : 2;
    const int n = static_cast<int> (count.getValue());
    auto& s = studio.session();
    const bool own = ! s.inGesture();
    if (own)
        s.beginGesture (n == 1 ? "New Track" : "New Tracks");
    auto ids = s.createTracks (k, n, ch, studio.selectedTrack());
    if (ids.empty())
    {
        if (own)
            s.cancelGesture();
        studio.setStatus (juce::String (s.lastError()));
        studio.edited();
        return ids;
    }
    // The choices beyond the defaults (D-UI-32), in the same undo step.
    const auto inputs = studio.device().config().numInputChannels;
    const auto outputIndex = output.getSelectedId() - 2;
    const auto choices = studio.pluginChoices (true);
    const auto instrumentIndex = instrument.getSelectedId() - 2;
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        const auto id = ids[i]; // by value: each edit rebuilds the model
        if (k == model::TrackKind::audio)
        {
            model::InputAssignment in;
            if (input.getSelectedId() >= firstInput)
            {
                const auto first = input.getSelectedId() - firstInput + (ascending.getToggleState() ? static_cast<int> (i) * ch : 0);
                if (first + ch <= inputs)
                {
                    in.kind = model::InputAssignment::Kind::audio;
                    in.firstChannel = first;
                }
            }
            s.setTrackInput (id, in);
            if (monitoring.getToggleState())
                s.setTrackMonitor (id, "on");
        }
        if (outputIndex >= 0 && static_cast<std::size_t> (outputIndex) < outputBuses.size())
            s.setTrackOutput (id, outputBuses[static_cast<std::size_t> (outputIndex)]);
        if (k == model::TrackKind::instrument && instrumentIndex >= 0 && static_cast<std::size_t> (instrumentIndex) < choices.size())
            s.setInstrument (id, choices[static_cast<std::size_t> (instrumentIndex)].plugin);
    }
    if (own)
        s.endGesture();
    if (recordEnable.getToggleState())
        for (const auto& id : ids)
            studio.setTrackSwitch (TrackSwitch::arm, id, true);
    studio.selectTrack (ids.back());
    studio.edited();
    return ids;
}

void NewTrackDialog::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::surfaceRaised));
    g.setColour (colour (Token::text));
    g.setFont (font (Type::body));
    int y = 20;
    for (const auto* label : { "Type", "Count", "Channels", "Input", "Output", "Instrument" })
    {
        g.drawText (label, 20, y, 90, 26, juce::Justification::centredLeft);
        y += 36;
    }
}

void NewTrackDialog::resized()
{
    const auto w = getWidth() - 130;
    kind.setBounds (110, 20, w, 26);
    count.setBounds (110, 56, 130, 26);
    channels.setBounds (110, 92, w, 26);
    input.setBounds (110, 128, w - 110, 26);
    ascending.setBounds (110 + w - 100, 128, 100, 26);
    output.setBounds (110, 164, w, 26);
    instrument.setBounds (110, 200, w, 26);
    monitoring.setBounds (110, 236, 140, 26);
    recordEnable.setBounds (260, 236, 120, 26);
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
