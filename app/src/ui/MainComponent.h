#pragma once

// The main window's content (spec 04 §2): control bar, inspector, tracks area, editors,
// side panel and status bar, plus the menus and the keyboard.

#include "BottomArea.h"
#include "ControlBar.h"
#include "Inspector.h"
#include "PluginWindows.h"
#include "SidePanel.h"
#include "TracksArea.h"

namespace weft::ui
{
class DeviceProvider;

/** The No-audio banner (W-0 step 7): shown over the tracks area while no device is open. */
class NoAudioBanner final : public juce::Component
{
public:
    NoAudioBanner();
    void paint (juce::Graphics&) override;
    void resized() override;
    juce::Label message;
    juce::TextButton setup { "Audio Setup..." };
    static constexpr int height = 32;
};

/** The read-only banner (05 §13): a Viewer, or removed from the project. */
class ReadOnlyBanner final : public juce::Component
{
public:
    ReadOnlyBanner();
    void paint (juce::Graphics&) override;
    void resized() override;
    juce::Label message;
    static constexpr int height = 32;
};

/** H · Status bar: the last message, the sync state (04 §10), the tool, snap and the project name. */
class StatusBar final : public juce::Component, private juce::ChangeListener
{
public:
    explicit StatusBar (Studio& studio);
    ~StatusBar() override;
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    static constexpr int height = 24;
    /** Where the sync state is drawn; clicking it calls onSyncClicked. */
    juce::Rectangle<int> syncArea() const;
    std::function<void()> onSyncClicked;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }
    Studio& studio;
};

/** The 4 px gap between two zones, and the handle that resizes them (spec 04 §2, F-UI-1). */
class Gutter final : public juce::Component
{
public:
    explicit Gutter (bool vertical);
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override { repaint(); }
    void mouseDoubleClick (const juce::MouseEvent&) override;

    /** Called with the drag's distance in px since the press (right or down is positive). */
    std::function<void (int)> onDrag;
    std::function<void()> onPress, onDoubleClick;
    const bool vertical;
};

class MainComponent final : public juce::Component, public juce::MenuBarModel, private juce::ChangeListener, private juce::Timer
{
public:
    /** browserRoot: where the file browser starts. */
    MainComponent (Studio& studio, const juce::File& browserRoot);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

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
    PluginWindows& pluginWindows() noexcept { return plugins; }
    /** Delete Track: asks "Remove bus and N sends?" for a bus with sends (W-7 step 6); answer() is what the buttons call. */
    void deleteSelectedTrack();
    bool questionPending() const noexcept { return ! questionTrack.empty(); }
    const juce::String& questionText() const noexcept { return asked; }
    void answer (bool confirmed);

    /** Called when the panel layout changes, so the app can keep it (F-UI-1). */
    std::function<void (const PanelLayout&)> onLayoutChanged;

    // Projects (M9) ------------------------------------------------------------------------
    /** Where Audio & MIDI Settings gets its devices; the app sets JUCE's, tests their own. */
    DeviceProvider* devices = nullptr;
    /** Opens a project, asking about unsaved changes and crash recovery first (W-10). */
    void openProject (const std::filesystem::path& picked);
    /** File › Open Recent item `index`: opens it, or forgets it when it's gone (AT-PRJ-10). */
    void openRecent (int index);
    /** Runs `next` once unsaved changes are saved or dropped; not at all on Cancel. */
    void whenSaved (std::function<void()> next);
    /** The Welcome screen (W-0 step 6). */
    void showWelcome();
    /** A pending project question (save changes, recover) and its answer: 1 and 2 are the
        first and second buttons ("Save" / "Don't Save", "Recover" / "Discard"), 0 is Cancel. */
    bool projectQuestionPending() const noexcept { return projectAnswer != nullptr; }
    const juce::String& projectQuestion() const noexcept { return projectAsked; }
    void answerProject (int choice);
    /** The last notice shown in an alert (missing plug-ins, open errors). */
    const juce::String& lastAlert() const noexcept { return alertText; }
    bool noAudioBannerShown() const { return banner.isVisible(); }
    bool readOnlyBannerShown() const { return readOnly.isVisible(); }
    PreviewBanner& previewBanner() noexcept { return preview; }
    const ReadOnlyBanner& readOnlyBanner() const noexcept { return readOnly; }
    static constexpr int recentMenuBase = 10000, colourMenuBase = 20000;
    /** Navigate menu options kept in the local transport settings (F-TRN-7, F-REC-7). */
    enum TransportItem
    {
        clickDuringRecord = 30001,
        clickDuringPlay,
        clickDuringBoth,
        returnToStart = 30010,
        mergeLinear = 30020,
        mergeCycle
    };

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void handleUiCommand (Cmd command);
    void ask (const juce::String& title, const juce::String& text, const juce::String& first, const juce::String& second,
              std::function<void (int)> answer);
    void alert (const juce::String& title, const juce::String& text);
    void opened (const Studio::OpenResult& result);
    void chooseProjectToOpen();
    /** File › Import Audio: files land at the playhead, on the selected audio track or on new tracks. */
    void chooseAudioToImport();
    /** Runs `next` once signed in, asking first when signed out. */
    void whenSignedIn (std::function<void()> next);

    Studio& studio;
    ControlBar control;
    Inspector inspectorPanel;
    TracksArea tracksArea;
    BottomArea bottomArea;
    SidePanel sidePanel;
    StatusBar status;
    PluginWindows plugins;
    NoAudioBanner banner;
    ReadOnlyBanner readOnly;
    PreviewBanner preview;
    Gutter inspectorEdge { true }, sideEdge { true }, bottomEdge { false };
    static constexpr int minInspector = 184, maxInspector = 424, minSide = 224, maxSide = 520, minBottom = 160;
    int maxBottom() const;
    int dragFrom = 0;    // the panel size when a gutter drag started
    int restoreHeight = 0; // the bottom area's height before a double-click maximised it
    std::unique_ptr<juce::AlertWindow> question, projectAlert;
    std::function<void (int)> projectAnswer;
    juce::String projectAsked, alertText;
    std::unique_ptr<juce::FileChooser> openChooser, importChooser;
    Id questionTrack;
    juce::String asked;
    juce::TooltipWindow tooltips { this, 600 };
    PanelLayout shownLayout;
    std::vector<int> heldKeys; // keyboard MIDI keys currently down
};
} // namespace weft::ui
