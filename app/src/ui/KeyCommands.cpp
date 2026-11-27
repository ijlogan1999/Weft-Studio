#include "KeyCommands.h"

namespace weft::ui
{
KeyCommandsPanel::KeyCommandsPanel (Studio& s) : studio (s)
{
    search.setTextToShowWhenEmpty ("Search commands or keys", colour (Token::textDim));
    describe (search, "Search", "Find a command by its name or its key");
    search.onTextChange = [this] { setSearch (search.getText()); };
    addAndMakeVisible (search);
    show.addItem ("All", 1);
    show.addItem ("Assigned", 2);
    show.addItem ("Unassigned", 3);
    show.setSelectedId (1, juce::dontSendNotification);
    describe (show, "Show", "All commands, only those with keys, or only those without");
    show.onChange = [this] { setShow (static_cast<Show> (show.getSelectedId() - 1)); };
    addAndMakeVisible (show);

    table.setModel (this);
    table.setRowHeight (size::control);
    table.getHeader().addColumn ("Command", 1, 260, 80, -1, juce::TableHeaderComponent::notSortable);
    table.getHeader().addColumn ("Key", 2, 160, 60, -1, juce::TableHeaderComponent::notSortable);
    table.setColour (juce::ListBox::backgroundColourId, colour (Token::surface));
    describe (table, "Key commands", "Select a command, then Learn Key and press the keys");
    addAndMakeVisible (table);

    describe (learn, "Learn Key", "Press the keys for the selected command (Esc cancels)");
    learn.onClick = [this] { startLearning(); };
    describe (add, "Add Key", "Press another key for the selected command");
    add.onClick = [this] {
        startLearning();
        addLearning = true;
    };
    describe (clear, "Clear", "The selected command has no key");
    clear.onClick = [this] { clearSelected(); };
    describe (defaults, "Default", "The selected command's default keys");
    defaults.onClick = [this] { defaultSelected(); };
    describe (resetEverything, "Reset All", "Every command's default keys");
    resetEverything.onClick = [this] { resetAll(); };
    for (auto* b : { &learn, &add, &clear, &defaults, &resetEverything })
        addAndMakeVisible (*b);
    message.setFont (font (Type::small));
    message.setColour (juce::Label::textColourId, colour (Token::textDim));
    addAndMakeVisible (message);
    setWantsKeyboardFocus (true);
    setSize (480, 520);
    refresh();
}

KeyCommandsPanel::~KeyCommandsPanel() { table.setModel (nullptr); }

juce::String KeyCommandsPanel::keysText (Cmd command) const
{
    juce::StringArray keys;
    for (const auto& k : commandInfo (command).keys)
        keys.add (keyDescription (k));
    return keys.joinIntoString (", ");
}

void KeyCommandsPanel::refresh()
{
    const auto keep = selectedCommand();
    rows.clear();
    const auto words = filter.trim().toLowerCase();
    for (const auto& c : commandTable())
    {
        if (c.name.isEmpty())
            continue;
        const bool hasKeys = ! c.keys.empty();
        if ((showing == Show::assigned && ! hasKeys) || (showing == Show::unassigned && hasKeys))
            continue;
        if (words.isNotEmpty() && ! c.name.toLowerCase().contains (words) && ! keysText (c.id).toLowerCase().contains (words))
            continue;
        rows.push_back (c.id);
    }
    table.updateContent();
    table.repaint();
    table.deselectAllRows();
    if (keep)
        selectCommand (*keep);
    const bool any = selectedCommand().has_value();
    for (auto* b : { &learn, &add, &clear, &defaults })
        b->setEnabled (any);
}

void KeyCommandsPanel::setSearch (const juce::String& text)
{
    filter = text;
    refresh();
}

void KeyCommandsPanel::setShow (Show s)
{
    showing = s;
    refresh();
}

bool KeyCommandsPanel::selectCommand (Cmd command)
{
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows[i] == command)
        {
            table.selectRow (static_cast<int> (i));
            for (auto* b : { &learn, &add, &clear, &defaults })
                b->setEnabled (true);
            return true;
        }
    return false;
}

std::optional<Cmd> KeyCommandsPanel::selectedCommand() const
{
    const auto row = table.getSelectedRow();
    if (row < 0 || row >= static_cast<int> (rows.size()))
        return std::nullopt;
    return rows[static_cast<std::size_t> (row)];
}

void KeyCommandsPanel::startLearning()
{
    if (! selectedCommand())
        return;
    isLearning = true;
    addLearning = false;
    message.setText ("Press the keys for " + commandInfo (*selectedCommand()).name + " (Esc cancels).", juce::dontSendNotification);
    learn.setToggleState (true, juce::dontSendNotification);
    if (isShowing())
        grabKeyboardFocus();
}

void KeyCommandsPanel::stopLearning()
{
    isLearning = addLearning = false;
    learn.setToggleState (false, juce::dontSendNotification);
}

bool KeyCommandsPanel::give (const juce::KeyPress& key, bool replace)
{
    const auto command = selectedCommand();
    if (! command)
        return false;
    // Only Ctrl, Alt and Shift count; a lone modifier isn't a key.
    const auto code = key.getKeyCode();
    if (code == 0)
        return false;
    const auto taken = commandUsingKey (key, *command);
    auto keys = replace ? std::vector<juce::KeyPress> {} : commandInfo (*command).keys;
    keys.push_back (key);
    setCommandKeys (*command, keys);
    message.setText (taken ? keyDescription (key) + " was " + commandInfo (*taken).name + "'s; it is now " + commandInfo (*command).name + "'s."
                           : keyDescription (key) + " is now " + commandInfo (*command).name + "'s.",
                     juce::dontSendNotification);
    stopLearning();
    refresh();
    studio.sendChangeMessage(); // menus and tooltips show the new key
    return true;
}

bool KeyCommandsPanel::assign (const juce::KeyPress& key) { return give (key, true); }

bool KeyCommandsPanel::addKey (const juce::KeyPress& key) { return give (key, false); }

void KeyCommandsPanel::clearSelected()
{
    if (const auto command = selectedCommand())
    {
        setCommandKeys (*command, {});
        message.setText (commandInfo (*command).name + " has no key.", juce::dontSendNotification);
        refresh();
        studio.sendChangeMessage();
    }
}

void KeyCommandsPanel::defaultSelected()
{
    if (const auto command = selectedCommand())
    {
        setCommandKeys (*command, defaultKeys (*command));
        message.setText (commandInfo (*command).name + " has its default keys.", juce::dontSendNotification);
        refresh();
        studio.sendChangeMessage();
    }
}

void KeyCommandsPanel::resetAll()
{
    resetAllKeys();
    message.setText ("Every command has its default keys.", juce::dontSendNotification);
    refresh();
    studio.sendChangeMessage();
}

bool KeyCommandsPanel::keyPressed (const juce::KeyPress& key)
{
    if (! isLearning)
        return false;
    if (key == juce::KeyPress::escapeKey)
    {
        stopLearning();
        message.setText ({}, juce::dontSendNotification);
        return true;
    }
    if (addLearning)
        addKey (key);
    else
        assign (key);
    return true;
}

void KeyCommandsPanel::paintRowBackground (juce::Graphics& g, int row, int width, int height, bool selected)
{
    if (selected)
        g.fillAll (colour (Token::accent).withAlpha (0.35f));
    else if (row % 2 == 1)
        g.fillAll (colour (Token::surfaceRaised).withAlpha (0.5f));
    g.setColour (colour (Token::lineSubtle));
    g.drawHorizontalLine (height - 1, 0.0f, static_cast<float> (width));
}

void KeyCommandsPanel::paintCell (juce::Graphics& g, int row, int column, int width, int height, bool)
{
    if (row < 0 || row >= getNumRows())
        return;
    const auto command = rows[static_cast<std::size_t> (row)];
    g.setColour (colour (column == 1 ? Token::text : Token::textDim));
    g.setFont (font (column == 1 ? Type::body : Type::monoValue));
    g.drawText (column == 1 ? commandInfo (command).name : keysText (command), space::half, 0, width - space::s1, height,
                juce::Justification::centredLeft, true);
}

void KeyCommandsPanel::cellDoubleClicked (int row, int, const juce::MouseEvent&)
{
    table.selectRow (row);
    startLearning();
}

void KeyCommandsPanel::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::surface));
    if (isLearning)
    {
        g.setColour (colour (Token::accent));
        g.drawRect (getLocalBounds(), 2);
    }
}

void KeyCommandsPanel::resized()
{
    auto r = getLocalBounds().reduced (space::s1);
    auto top = r.removeFromTop (size::control);
    show.setBounds (top.removeFromRight (120));
    top.removeFromRight (space::half);
    search.setBounds (top);
    r.removeFromTop (space::half);
    message.setBounds (r.removeFromBottom (size::control));
    auto buttons = r.removeFromBottom (size::control);
    r.removeFromBottom (space::half);
    const auto w = buttons.getWidth() / 5;
    for (auto* b : { &learn, &add, &clear, &defaults, &resetEverything })
        b->setBounds (buttons.removeFromLeft (w).reduced (1, 0));
    table.setBounds (r);
}
} // namespace weft::ui
