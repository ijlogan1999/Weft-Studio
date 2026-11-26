#include "PluginSearch.h"

namespace weft::ui
{
namespace
{
bool isInstrumentTrack (const Studio& studio)
{
    const auto* t = studio.model().track (studio.selectedTrack());
    return t != nullptr && t->kind == model::TrackKind::instrument;
}
} // namespace

PluginSearch::PluginSearch (Studio& s) : studio (s)
{
    setTitle ("Search and Add Plug-in");
    describe (search, "Search plug-ins", "Type part of a plug-in's name or maker; Enter adds the highlighted one, Up/Down move, Esc closes");
    search.setTextToShowWhenEmpty ("Search plug-ins", colour (Token::textDim));
    search.onTextChange = [this] { setSearch (search.getText()); };
    search.onReturnKey = [this] { addSelected(); };
    search.onEscapeKey = [this] { close(); };
    addAndMakeVisible (search);
    list.setModel (this);
    list.setRowHeight (size::control);
    describe (list, "Matching plug-ins");
    addAndMakeVisible (list);
    setSearch ({});
    setSize (360, 320);
}

std::vector<Studio::PluginChoice> PluginSearch::matches (const Studio& studio, const juce::String& text)
{
    auto words = juce::StringArray::fromTokens (text.trim(), true);
    words.removeEmptyStrings();
    std::vector<Studio::PluginChoice> out;
    const auto take = [&] (const std::vector<Studio::PluginChoice>& choices) {
        for (const auto& c : choices)
        {
            bool all = true;
            for (const auto& w : words)
                all = all && (c.label.containsIgnoreCase (w) || c.group.containsIgnoreCase (w));
            if (all)
                out.push_back (c);
        }
    };
    if (isInstrumentTrack (studio))
        take (studio.pluginChoices (true));
    take (studio.pluginChoices (false));
    return out;
}

bool PluginSearch::add (Studio& studio, const Studio::PluginChoice& choice)
{
    const auto trackId = studio.selectedTrack();
    const auto* t = studio.model().track (trackId);
    if (t == nullptr)
    {
        studio.setStatus ("Select a track first.");
        return false;
    }
    const auto instruments = studio.pluginChoices (true);
    const bool isInstrument = std::any_of (instruments.begin(), instruments.end(), [&] (const auto& c) {
        return c.plugin.format == choice.plugin.format && c.plugin.pluginUid == choice.plugin.pluginUid && c.label == choice.label;
    });
    if (isInstrument)
    {
        if (t->kind != model::TrackKind::instrument)
        {
            studio.setStatus ("An instrument needs an instrument track.");
            return false;
        }
        return studio.setInstrument (trackId, choice);
    }
    if (t->strip.inserts.size() >= static_cast<std::size_t> (model::maxInserts))
    {
        studio.setStatus ("This channel's insert slots are full.");
        return false;
    }
    return studio.addPlugin (trackId, choice).has_value();
}

void PluginSearch::setSearch (const juce::String& text)
{
    if (search.getText() != text)
        search.setText (text, juce::dontSendNotification);
    shown = matches (studio, text);
    list.updateContent();
    list.selectRow (shown.empty() ? -1 : 0);
    list.repaint();
}

bool PluginSearch::addSelected()
{
    const auto row = list.getSelectedRow();
    if (row < 0 || row >= static_cast<int> (shown.size()))
        return false;
    const auto ok = add (studio, shown[static_cast<std::size_t> (row)]);
    if (ok)
        close();
    return ok;
}

bool PluginSearch::keyPressed (const juce::KeyPress& key)
{
    // Up and Down move through the matches while the text field keeps the keys.
    if (key.isKeyCode (juce::KeyPress::downKey) || key.isKeyCode (juce::KeyPress::upKey))
    {
        const auto step = key.isKeyCode (juce::KeyPress::downKey) ? 1 : -1;
        list.selectRow (std::clamp (list.getSelectedRow() + step, 0, std::max (0, getNumRows() - 1)));
        return true;
    }
    return false;
}

void PluginSearch::close()
{
    if (onClose)
        onClose();
}

void PluginSearch::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= getNumRows())
        return;
    const auto& c = shown[static_cast<std::size_t> (row)];
    if (selected)
    {
        g.setColour (colour (Token::accent).withAlpha (0.35f));
        g.fillRect (0, 0, width, height);
    }
    g.setFont (font (Type::body));
    g.setColour (colour (Token::text));
    g.drawText (c.label, space::s1, 0, width - 2 * space::s1, height, juce::Justification::centredLeft, true);
    g.setFont (font (Type::small));
    g.setColour (colour (Token::textDim));
    g.drawText (c.plugin.format == "builtin" ? juce::String ("Weft") : c.group, space::s1, 0, width - 2 * space::s1, height,
                juce::Justification::centredRight, true);
}

void PluginSearch::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    list.selectRow (row);
    addSelected();
}

void PluginSearch::paint (juce::Graphics& g) { g.fillAll (colour (Token::surfaceRaised)); }

void PluginSearch::resized()
{
    auto r = getLocalBounds().reduced (space::s2);
    search.setBounds (r.removeFromTop (size::control));
    r.removeFromTop (space::s1);
    list.setBounds (r);
}

void PluginSearch::show (Studio& studio, juce::Component* parent)
{
    if (studio.model().track (studio.selectedTrack()) == nullptr)
    {
        studio.setStatus ("Select a track first.");
        return;
    }
    if (! desktopWindows())
        return;
    auto* dialog = new PluginSearch (studio);
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (dialog);
    options.dialogTitle = "Search and Add Plug-in";
    options.dialogBackgroundColour = colour (Token::surfaceRaised);
    options.componentToCentreAround = parent;
    options.useNativeTitleBar = true;
    options.resizable = false;
    auto* window = options.launchAsync();
    dialog->onClose = [window] {
        if (window != nullptr)
            window->exitModalState (0);
    };
    dialog->search.grabKeyboardFocus();
}

// Search and Select Track ------------------------------------------------------------------

TrackSearch::TrackSearch (Studio& s) : studio (s)
{
    setTitle ("Search and Select Track");
    describe (search, "Search tracks", "Type part of a track's name; Enter selects the highlighted one, Up/Down move, Esc closes");
    search.setTextToShowWhenEmpty ("Search tracks", colour (Token::textDim));
    search.onTextChange = [this] { setSearch (search.getText()); };
    search.onReturnKey = [this] { selectHighlighted(); };
    search.onEscapeKey = [this] {
        if (onClose)
            onClose();
    };
    addAndMakeVisible (search);
    list.setModel (this);
    list.setRowHeight (size::control);
    describe (list, "Matching tracks");
    addAndMakeVisible (list);
    setSearch ({});
    setSize (320, 300);
}

std::vector<Id> TrackSearch::matches (const Studio& studio, const juce::String& text)
{
    auto words = juce::StringArray::fromTokens (text.trim(), true);
    words.removeEmptyStrings();
    std::vector<Id> out;
    for (const auto& id : studio.model().trackOrder)
    {
        const auto* t = studio.model().track (id);
        if (t == nullptr || t->kind == model::TrackKind::master)
            continue;
        const juce::String name (t->name);
        bool all = true;
        for (const auto& w : words)
            all = all && name.containsIgnoreCase (w);
        if (all)
            out.push_back (id);
    }
    return out;
}

void TrackSearch::setSearch (const juce::String& text)
{
    if (search.getText() != text)
        search.setText (text, juce::dontSendNotification);
    shown = matches (studio, text);
    list.updateContent();
    list.selectRow (shown.empty() ? -1 : 0);
    list.repaint();
}

bool TrackSearch::selectHighlighted()
{
    const auto row = list.getSelectedRow();
    if (row < 0 || row >= static_cast<int> (shown.size()))
        return false;
    const auto id = shown[static_cast<std::size_t> (row)];
    studio.selectTrack (id);
    if (onChosen)
        onChosen (id);
    if (onClose)
        onClose();
    return true;
}

bool TrackSearch::keyPressed (const juce::KeyPress& key)
{
    if (key.isKeyCode (juce::KeyPress::downKey) || key.isKeyCode (juce::KeyPress::upKey))
    {
        const auto step = key.isKeyCode (juce::KeyPress::downKey) ? 1 : -1;
        list.selectRow (std::clamp (list.getSelectedRow() + step, 0, std::max (0, getNumRows() - 1)));
        return true;
    }
    return false;
}

void TrackSearch::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= getNumRows())
        return;
    const auto* t = studio.model().track (shown[static_cast<std::size_t> (row)]);
    if (t == nullptr)
        return;
    if (selected)
    {
        g.setColour (colour (Token::accent).withAlpha (0.35f));
        g.fillRect (0, 0, width, height);
    }
    g.setFont (font (Type::body));
    g.setColour (colour (Token::text));
    g.drawText (juce::String (t->name), space::s1, 0, width - 2 * space::s1, height, juce::Justification::centredLeft, true);
    g.setFont (font (Type::small));
    g.setColour (colour (Token::textDim));
    const char* kind = t->kind == model::TrackKind::audio ? "Audio" : t->kind == model::TrackKind::instrument ? "Instrument" : "Bus";
    g.drawText (juce::String (kind) + (t->hidden ? " (hidden)" : ""), space::s1, 0, width - 2 * space::s1, height, juce::Justification::centredRight, true);
}

void TrackSearch::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    list.selectRow (row);
    selectHighlighted();
}

TrackSearch* TrackSearch::show (Studio& studio, juce::Component* parent)
{
    if (! desktopWindows())
        return nullptr;
    auto* dialog = new TrackSearch (studio);
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (dialog);
    options.dialogTitle = "Search and Select Track";
    options.dialogBackgroundColour = colour (Token::surfaceRaised);
    options.componentToCentreAround = parent;
    options.useNativeTitleBar = true;
    options.resizable = false;
    auto* window = options.launchAsync();
    dialog->onClose = [window] {
        if (window != nullptr)
            window->exitModalState (0);
    };
    dialog->search.grabKeyboardFocus();
    return dialog;
}

void TrackSearch::paint (juce::Graphics& g) { g.fillAll (colour (Token::surfaceRaised)); }

void TrackSearch::resized()
{
    auto r = getLocalBounds().reduced (space::s2);
    search.setBounds (r.removeFromTop (size::control));
    r.removeFromTop (space::s1);
    list.setBounds (r);
}
} // namespace weft::ui
