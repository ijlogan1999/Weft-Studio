#include "Displays.h"

#include "Mixer.h"

#include "weft/time/Position.h"

namespace weft::ui
{
// Output meter -------------------------------------------------------------------------------

OutputMeter::OutputMeter (Studio& s) : studio (s)
{
    describe (*this, "Output meter", "The master's output level");
}

void OutputMeter::update()
{
    const auto m = studio.takeMeter (Id(), this);
    const auto db = static_cast<float> (engine::gainToDb (m.output));
    // Rises at once, falls about 20 dB a second at the control bar's 20 Hz.
    const auto next = std::max (db, shownDb - 1.0f);
    if (std::abs (next - shownDb) > 0.01f)
    {
        shownDb = std::max (next, -200.0f);
        repaint();
    }
}

void OutputMeter::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().withSizeKeepingCentre (static_cast<float> (getWidth()), 8.0f);
    depth::well (g, r, radius::r1);
    const auto inner = r.reduced (1.0f);
    const auto p = MeterView::proportion (shownDb);
    g.setColour (colour (shownDb > 0.0f ? Token::meterClip : Token::accent));
    g.fillRect (inner.withWidth (inner.getWidth() * p));
}

// Big displays -------------------------------------------------------------------------------

BigDisplay::BigDisplay (Studio& s, Mode m) : mode (m), studio (s)
{
    setSize (m == Mode::beats ? 420 : 520, 140);
    describe (*this, m == Mode::beats ? "Big beats display" : "Big time display");
    shown = text();
    startTimerHz (20);
}

BigDisplay::~BigDisplay() { stopTimer(); }

juce::String BigDisplay::text() const
{
    const auto& m = studio.model();
    const auto t = studio.playhead();
    if (mode == Mode::beats)
        return juce::String (formatBarsBeats (t, m.signatures));
    const auto seconds = m.tempo.tickToSample (static_cast<double> (t)) / m.info.sampleRate;
    return formatDisplayTime (seconds, m.info.sampleRate, studio.layout.bar.timeFormat);
}

void BigDisplay::timerCallback()
{
    if (auto now = text(); now != shown)
    {
        shown = now;
        repaint();
    }
}

void BigDisplay::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::background));
    g.setColour (colour (Token::text));
    // The digits fill the window: the font follows its height, and shrinks for a narrow window.
    auto f = font (Type::monoCounter).withHeight (static_cast<float> (getHeight()) * 0.7f);
    const auto width = juce::GlyphArrangement::getStringWidth (f, shown);
    if (width > static_cast<float> (getWidth()) * 0.92f)
        f = f.withHeight (f.getHeight() * static_cast<float> (getWidth()) * 0.92f / width);
    g.setFont (f);
    g.drawText (shown, getLocalBounds(), juce::Justification::centred, false);
}

// Performance meter ----------------------------------------------------------------------------

PerformanceMeter::PerformanceMeter (Studio& s) : studio (s)
{
    setSize (260, 64);
    describe (*this, "Performance meter", "Audio thread load and dropouts");
    startTimerHz (10);
}

PerformanceMeter::~PerformanceMeter() { stopTimer(); }

juce::String PerformanceMeter::text() const
{
    return "CPU " + juce::String (juce::roundToInt (studio.engine().cpuLoad() * 100.0)) + "%" + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 dropouts "))
           + juce::String (studio.engine().dropouts());
}

void PerformanceMeter::timerCallback() { repaint(); }

void PerformanceMeter::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::surfaceRaised));
    auto r = getLocalBounds().reduced (space::s1);
    const auto textRow = r.removeFromBottom (std::max (14, r.getHeight() / 3));
    const auto bar = r.reduced (0, 2).toFloat();
    depth::well (g, bar, radius::r1);
    const auto load = std::clamp (static_cast<float> (studio.engine().cpuLoad()), 0.0f, 1.0f);
    g.setColour (colour (load > 0.85f ? Token::meterClip : Token::accent));
    g.fillRect (bar.reduced (1.0f).withWidth ((bar.getWidth() - 2.0f) * load));
    g.setColour (colour (Token::textDim));
    g.setFont (font (Type::monoSmall).withHeight (std::max (11.0f, static_cast<float> (textRow.getHeight()) * 0.8f)));
    g.drawText (text(), textRow, juce::Justification::centredLeft, true);
}

// Floating window -------------------------------------------------------------------------------

FloatingWindow::FloatingWindow (const juce::String& title, std::unique_ptr<juce::Component> content, std::function<void (FloatingWindow*)> onClose)
    : juce::DocumentWindow (title, colour (Token::surface), juce::DocumentWindow::closeButton, desktopWindows()), closed (std::move (onClose))
{
    setUsingNativeTitleBar (true);
    const auto w = content->getWidth(), h = content->getHeight();
    setContentOwned (content.release(), true);
    setResizable (true, false);
    setResizeLimits (std::max (120, w / 4), std::max (40, h / 4), 4000, 2000);
    if (desktopWindows())
    {
        centreWithSize (getWidth(), getHeight());
        setAlwaysOnTop (true);
        setVisible (true);
    }
}

void FloatingWindow::closeButtonPressed()
{
    if (closed)
        closed (this);
}

// Toolbar ---------------------------------------------------------------------------------------

Toolbar::Toolbar (Studio& s) : studio (s)
{
    describe (*this, "Toolbar", "Command buttons. Right-click to choose them");
    studio.addChangeListener (this);
    rebuild();
}

Toolbar::~Toolbar() { studio.removeChangeListener (this); }

std::vector<CommandButton*> Toolbar::buttons() const
{
    std::vector<CommandButton*> out;
    for (const auto& b : owned)
        out.push_back (b.get());
    return out;
}

void Toolbar::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (studio.layout.toolbarCommands != shownCommands)
        rebuild();
}

void Toolbar::rebuild()
{
    owned.clear();
    shownCommands = studio.layout.toolbarCommands;
    for (const auto c : shownCommands)
    {
        auto b = std::make_unique<CommandButton> (commandInfo (c).name.upToLastOccurrenceOf ("...", false, false), c);
        b->setMouseClickGrabsKeyboardFocus (false);
        // Right-click on a button opens the same menu as on the bar.
        b->popupMenu = [this] { return customizeMenu(); };
        b->popupPicked = [safe = juce::Component::SafePointer<Toolbar> (this)] (int item) {
            juce::MessageManager::callAsync ([safe, item] { // picking may rebuild the buttons
                if (safe != nullptr)
                    safe->pickCustomize (item);
            });
        };
        auto* raw = b.get();
        b->onClick = [this, c, raw] {
            if (raw->popupClick)
            {
                raw->showPopup();
                return;
            }
            if (run)
                run (c);
            else
                studio.perform (c);
        };
        addAndMakeVisible (*b);
        owned.push_back (std::move (b));
    }
    resized();
    repaint();
}

void Toolbar::resized()
{
    auto r = getLocalBounds().reduced (space::s1, (height - size::control) / 2);
    for (auto& b : owned)
    {
        const auto w = std::min (r.getWidth(), juce::GlyphArrangement::getStringWidthInt (font (Type::body), b->getButtonText()) + 20);
        b->setBounds (r.removeFromLeft (w));
        r.removeFromLeft (space::half);
    }
}

void Toolbar::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::surfaceRaised));
    g.setColour (colour (Token::border));
    g.fillRect (getLocalBounds().removeFromBottom (1));
}

juce::PopupMenu Toolbar::customizeMenu() const
{
    juce::PopupMenu menu;
    const auto& shown = studio.layout.toolbarCommands;
    const auto choices = toolbarChoices();
    for (std::size_t i = 0; i < choices.size(); ++i)
        menu.addItem (static_cast<int> (i) + 1, commandInfo (choices[i]).name, true, std::find (shown.begin(), shown.end(), choices[i]) != shown.end());
    menu.addSeparator();
    menu.addItem (itemSaveDefault, "Save As Default");
    menu.addItem (itemRestoreDefault, "Restore Defaults");
    return menu;
}

void Toolbar::pickCustomize (int item)
{
    auto& l = studio.layout;
    const auto choices = toolbarChoices();
    if (item == itemSaveDefault)
    {
        l.toolbarDefault = l.toolbarCommands;
        studio.setStatus ("These toolbar buttons are now the default.");
    }
    else if (item == itemRestoreDefault)
        l.toolbarCommands = l.toolbarDefault;
    else if (item >= 1 && item <= static_cast<int> (choices.size()))
    {
        // Toggle one; the buttons keep the menu's order.
        const auto c = choices[static_cast<std::size_t> (item - 1)];
        std::vector<Cmd> next;
        const bool had = std::find (l.toolbarCommands.begin(), l.toolbarCommands.end(), c) != l.toolbarCommands.end();
        for (const auto choice : choices)
            if (choice == c ? ! had : std::find (l.toolbarCommands.begin(), l.toolbarCommands.end(), choice) != l.toolbarCommands.end())
                next.push_back (choice);
        l.toolbarCommands = std::move (next);
    }
    else
        return;
    studio.sendChangeMessage();
    if (l.toolbarCommands != shownCommands)
        rebuild();
}

void Toolbar::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        return;
    customizeMenu().showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe = juce::Component::SafePointer<Toolbar> (this)] (int item) {
        if (safe != nullptr)
            safe->pickCustomize (item);
    });
}
} // namespace weft::ui
