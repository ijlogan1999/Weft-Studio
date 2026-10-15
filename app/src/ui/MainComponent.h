#pragma once

// The main window's content (spec 04 §2): control bar, inspector, tracks area, editors,
// side panel and status bar, plus the menus and the keyboard.

#include "BottomArea.h"
#include "ControlBar.h"
#include "Inspector.h"
#include "SidePanel.h"
#include "TracksArea.h"

namespace weft::ui
{
/** H · Status bar: the last message, the tool, snap and the project name. */
class StatusBar final : public juce::Component, private juce::ChangeListener
{
public:
    explicit StatusBar (Studio& studio);
    ~StatusBar() override;
    void paint (juce::Graphics&) override;
    static constexpr int height = 24;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }
    Studio& studio;
};

class MainComponent final : public juce::Component, public juce::MenuBarModel, private juce::ChangeListener, private juce::Timer
{
public:
    /** browserRoot: where the file browser starts. */
    MainComponent (Studio& studio, const juce::File& browserRoot);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;

    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int index, const juce::String& name) override;
    void menuItemSelected (int itemId, int topLevelMenuIndex) override;

    /** Runs a command as the keyboard or a menu would (tests use it). */
    bool run (Cmd command);
    /** The tool menu (T): a popup at the pointer; tests can pick from it directly. */
    juce::PopupMenu toolMenu() const;
    /** What choosing an item from toolMenu() does. */
    void pickTool (int menuItemId);

    ControlBar& controlBar() noexcept { return control; }
    Inspector& inspector() noexcept { return inspectorPanel; }
    TracksArea& tracks() noexcept { return tracksArea; }
    BottomArea& bottom() noexcept { return bottomArea; }
    SidePanel& side() noexcept { return sidePanel; }
    StatusBar& statusBar() noexcept { return status; }

    /** Called when the panel layout changes, so the app can keep it (F-UI-1). */
    std::function<void (const PanelLayout&)> onLayoutChanged;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void handleUiCommand (Cmd command);

    Studio& studio;
    ControlBar control;
    Inspector inspectorPanel;
    TracksArea tracksArea;
    BottomArea bottomArea;
    SidePanel sidePanel;
    StatusBar status;
    juce::TooltipWindow tooltips { this, 600 };
    PanelLayout shownLayout;
    std::vector<int> heldKeys; // keyboard MIDI keys currently down
};
} // namespace weft::ui
