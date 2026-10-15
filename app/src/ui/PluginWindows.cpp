#include "PluginWindows.h"

#include "weft/engine/plugins/HostedPlugin.h"

#include <cmath>

namespace weft::ui
{
// Generic editor ------------------------------------------------------------------------------

GenericPluginEditor::GenericPluginEditor (std::shared_ptr<engine::Processor> processor, const juce::String& name) : proc (std::move (processor))
{
    setTitle (name);
    message.setJustificationType (juce::Justification::centred);
    message.setFont (juce::FontOptions (13.0f));
    addChildComponent (message);
    if (proc == nullptr || proc->isPlaceholder())
    {
        message.setText (name + ": Not installed on this computer. Its settings are kept.", juce::dontSendNotification);
        message.setVisible (true);
        setSize (420, 120);
        return;
    }
    infos = proc->parameters();
    for (std::size_t i = 0; i < infos.size(); ++i)
    {
        const auto& info = infos[i];
        Row row;
        row.label = std::make_unique<juce::Label> (juce::String(), juce::String (info.name));
        row.label->setFont (juce::FontOptions (12.0f));
        row.slider = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
        auto& s = *row.slider;
        s.setRange (0.0, 1.0, info.steps > 1 ? 1.0 / (info.steps - 1) : 0.0);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, true, 90, 20);
        s.setDoubleClickReturnValue (true, info.defaultValue, juce::ModifierKeys::altModifier);
        const auto index = static_cast<int> (i);
        s.textFromValueFunction = [p = proc.get(), index] (double v) { return juce::String (p->parameterText (index, v)); };
        s.setValue (proc->parameter (index), juce::dontSendNotification);
        describe (s, juce::String (info.name));
        // The window's poll sees the new value and writes it to the session.
        s.onValueChange = [this, index, slider = &s] { proc->setParameter (index, slider->getValue()); };
        addAndMakeVisible (*row.label);
        addAndMakeVisible (s);
        rows.push_back (std::move (row));
    }
    setSize (440, std::max (80, 16 + static_cast<int> (rows.size()) * rowHeight));
    startTimerHz (15);
}

void GenericPluginEditor::paint (juce::Graphics& g) { g.fillAll (colour (Token::surface)); }

void GenericPluginEditor::resized()
{
    message.setBounds (getLocalBounds().reduced (12));
    auto r = getLocalBounds().reduced (8);
    for (auto& row : rows)
    {
        auto line = r.removeFromTop (rowHeight).reduced (0, 3);
        row.label->setBounds (line.removeFromLeft (120));
        row.slider->setBounds (line);
    }
}

void GenericPluginEditor::timerCallback()
{
    // Follows changes made elsewhere (undo, a collaborator, automation later).
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (! rows[i].slider->isMouseButtonDown())
        {
            const auto v = proc->parameter (static_cast<int> (i));
            if (std::abs (rows[i].slider->getValue() - v) > 1e-9)
                rows[i].slider->setValue (v, juce::dontSendNotification);
        }
}

// Window ----------------------------------------------------------------------------------------

PluginWindow::PluginWindow (Studio& s, PluginWindows& owner, const Id& id)
    : juce::DocumentWindow ("Plug-in", colour (Token::surface), juce::DocumentWindow::closeButton, desktopWindows()), pluginId (id), studio (s), windows (owner)
{
    Id trackId;
    const auto* plugin = studio.pluginInstance (pluginId, &trackId);
    const auto* track = studio.model().track (trackId);
    const auto name = plugin != nullptr ? juce::String (plugin->name) : juce::String ("Plug-in");
    setName (name + (track != nullptr ? " · " + juce::String (track->name) : juce::String()));
    setUsingNativeTitleBar (true);
    if (desktopWindows())
        setAlwaysOnTop (true);
    proc = studio.processor (pluginId);

    juce::Component* content = nullptr;
    if (auto* hosted = dynamic_cast<engine::HostedPlugin*> (proc.get()); hosted != nullptr && hosted->hasEditor())
        content = hosted->instance().createEditorIfNeeded();
    if (content == nullptr)
        content = new GenericPluginEditor (proc, name);
    setContentOwned (content, true);
    setResizable (false, false);
    if (proc != nullptr && ! proc->isPlaceholder())
    {
        infos = proc->parameters();
        for (std::size_t i = 0; i < infos.size(); ++i)
            lastSeen.push_back (proc->parameter (static_cast<int> (i)));
    }
    seenEdits = studio.edits();
    startTimerHz (15);
}

PluginWindow::~PluginWindow()
{
    stopTimer();
    clearContentComponent(); // the editor goes before the plug-in it edits
}

GenericPluginEditor* PluginWindow::generic() const { return dynamic_cast<GenericPluginEditor*> (getContentComponent()); }

void PluginWindow::pollParameters()
{
    if (proc == nullptr || proc->isPlaceholder() || studio.pluginInstance (pluginId) == nullptr)
        return;
    // Values the session set (undo, a collaborator) aren't the editor's changes.
    const auto rebase = studio.edits() != seenEdits;
    std::vector<std::pair<std::string, double>> changes;
    for (std::size_t i = 0; i < infos.size(); ++i)
    {
        const auto v = proc->parameter (static_cast<int> (i));
        if (! rebase && std::abs (v - lastSeen[i]) > 1e-7)
            changes.emplace_back (infos[i].id, v);
        lastSeen[i] = v;
    }
    if (! changes.empty())
        studio.pluginParamsChanged (pluginId, changes);
    seenEdits = studio.edits();
}

void PluginWindow::closeButtonPressed() { windows.close (pluginId); }

void PluginWindow::moved()
{
    juce::DocumentWindow::moved();
    windows.positions[pluginId] = getPosition();
}

// Windows -------------------------------------------------------------------------------------

PluginWindows::PluginWindows (Studio& s) : studio (s)
{
    studio.openPluginEditor = [this] (const Id& id) { open (id); };
    studio.addChangeListener (this);
}

PluginWindows::~PluginWindows()
{
    studio.removeChangeListener (this);
    studio.openPluginEditor = nullptr;
    closeAll();
}

PluginWindow* PluginWindows::find (const Id& pluginId) const
{
    const auto it = windows.find (pluginId);
    return it == windows.end() ? nullptr : it->second.get();
}

void PluginWindows::open (const Id& pluginId)
{
    if (auto* w = find (pluginId))
    {
        w->toFront (true);
        return;
    }
    if (studio.pluginInstance (pluginId) == nullptr)
        return;
    auto w = std::make_unique<PluginWindow> (studio, *this, pluginId);
    if (const auto it = positions.find (pluginId); it != positions.end())
        w->setTopLeftPosition (it->second);
    else
        w->centreWithSize (w->getWidth(), w->getHeight());
    if (desktopWindows())
        w->setVisible (true);
    windows[pluginId] = std::move (w);
}

void PluginWindows::close (const Id& pluginId)
{
    const auto it = windows.find (pluginId);
    if (it == windows.end())
        return;
    it->second->pollParameters();
    if (studio.pluginInstance (pluginId) != nullptr)
        studio.writePluginState (pluginId);
    positions[pluginId] = it->second->getPosition();
    windows.erase (it);
}

void PluginWindows::closeAll()
{
    while (! windows.empty())
        close (windows.begin()->first);
}

void PluginWindows::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // A removed or replaced plug-in closes its window.
    for (auto it = windows.begin(); it != windows.end();)
        it = studio.pluginInstance (it->first) == nullptr ? windows.erase (it) : std::next (it);
}

// Plug-in manager -------------------------------------------------------------------------------

namespace
{
juce::String statusText (const engine::PluginInfo& p)
{
    switch (p.status)
    {
        case engine::PluginStatus::ok: return "OK";
        case engine::PluginStatus::disabled: return "Disabled";
        case engine::PluginStatus::failed: return "Failed: " + juce::String (p.error);
    }
    return {};
}
} // namespace

PluginManager::PluginManager (Studio& s) : studio (s)
{
    setTitle ("Plug-in Manager");
    auto& header = table.getHeader();
    header.addColumn ("Name", colName, 200);
    header.addColumn ("Vendor", colVendor, 140);
    header.addColumn ("Format", colFormat, 70);
    header.addColumn ("Status", colStatus, 160);
    header.addColumn ("Enabled", colEnabled, 70);
    table.setModel (this);
    table.setRowHeight (24);
    describe (table, "Plug-ins");
    addAndMakeVisible (table);
    describe (rescan, "Rescan", "Look for new or changed plug-ins");
    describe (retry, "Retry Failed", "Scan plug-ins that failed last time again");
    describe (close, "Close");
    rescan.onClick = [this] { scan (false); };
    retry.onClick = [this] { scan (true); };
    close.onClick = [this] {
        if (onClose)
            onClose();
    };
    for (auto* c : std::initializer_list<juce::Component*> { &rescan, &retry, &close, &summary })
        addAndMakeVisible (c);
    refresh();
    setSize (680, 420);
}

void PluginManager::refresh()
{
    const auto& all = studio.plugins().plugins();
    const auto failed = std::count_if (all.begin(), all.end(), [] (const auto& p) { return p.status == engine::PluginStatus::failed; });
    summary.setText (juce::String (all.size()) + " VST3 plug-ins, " + juce::String (failed) + " failed. Weft's own effects are always available.",
                     juce::dontSendNotification);
    table.updateContent();
    table.repaint();
}

void PluginManager::scan (bool retryFailed)
{
    summary.setText ("Scanning...", juce::dontSendNotification);
    studio.rescanPlugins (retryFailed);
    refresh();
}

int PluginManager::rows() const { return static_cast<int> (studio.plugins().plugins().size()); }

juce::String PluginManager::cellText (int row, int column) const
{
    const auto& all = studio.plugins().plugins();
    if (row < 0 || row >= static_cast<int> (all.size()))
        return {};
    const auto& p = all[static_cast<std::size_t> (row)];
    switch (column)
    {
        case colName: return juce::String (p.name.empty() ? std::filesystem::path (p.file).filename().string() : p.name);
        case colVendor: return juce::String (p.vendor);
        case colFormat: return p.format == "vst3" ? juce::String ("VST3") : juce::String (p.format);
        case colStatus: return statusText (p);
        case colEnabled: return p.status == engine::PluginStatus::ok ? "Yes" : "No";
        default: return {};
    }
}

void PluginManager::setEnabled (int row, bool enabled)
{
    const auto& all = studio.plugins().plugins();
    if (row < 0 || row >= static_cast<int> (all.size()))
        return;
    studio.plugins().setEnabled (all[static_cast<std::size_t> (row)].uid, enabled);
    studio.plugins().save();
    refresh();
}

void PluginManager::paintRowBackground (juce::Graphics& g, int, int, int, bool selected)
{
    g.fillAll (selected ? colour (Token::surfaceRaised) : colour (Token::surface));
}

void PluginManager::paintCell (juce::Graphics& g, int row, int column, int width, int height, bool)
{
    if (column == colEnabled)
        return;
    const auto& all = studio.plugins().plugins();
    const auto failed = row < static_cast<int> (all.size()) && all[static_cast<std::size_t> (row)].status == engine::PluginStatus::failed;
    g.setColour (failed && column == colStatus ? colour (Token::warning) : colour (Token::text));
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (cellText (row, column), 4, 0, width - 8, height, juce::Justification::centredLeft, true);
}

juce::Component* PluginManager::refreshComponentForCell (int row, int column, bool, juce::Component* existing)
{
    if (column != colEnabled)
    {
        delete existing;
        return nullptr;
    }
    auto* toggle = dynamic_cast<juce::ToggleButton*> (existing);
    if (toggle == nullptr)
    {
        delete existing;
        toggle = new juce::ToggleButton();
    }
    const auto& all = studio.plugins().plugins();
    const auto& p = all[static_cast<std::size_t> (row)];
    toggle->setEnabled (p.status != engine::PluginStatus::failed);
    toggle->setToggleState (p.status == engine::PluginStatus::ok, juce::dontSendNotification);
    describe (*toggle, "Enable " + juce::String (p.name));
    toggle->onClick = [this, row, toggle] { setEnabled (row, toggle->getToggleState()); };
    return toggle;
}

void PluginManager::paint (juce::Graphics& g) { g.fillAll (colour (Token::surfaceRaised)); }

void PluginManager::resized()
{
    auto r = getLocalBounds().reduced (12);
    auto buttons = r.removeFromBottom (28);
    close.setBounds (buttons.removeFromRight (90));
    buttons.removeFromRight (8);
    retry.setBounds (buttons.removeFromLeft (110));
    buttons.removeFromLeft (8);
    rescan.setBounds (buttons.removeFromLeft (90));
    r.removeFromBottom (8);
    summary.setBounds (r.removeFromBottom (22));
    table.setBounds (r);
}

void PluginManager::show (Studio& studio, juce::Component* parent)
{
    auto* manager = new PluginManager (studio);
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (manager);
    options.dialogTitle = "Plug-in Manager";
    options.dialogBackgroundColour = colour (Token::surfaceRaised);
    options.componentToCentreAround = parent;
    options.useNativeTitleBar = true;
    options.resizable = true;
    auto* window = options.launchAsync();
    manager->onClose = [window] {
        if (window != nullptr)
            window->exitModalState (0);
    };
}
} // namespace weft::ui
