// AT-UI-1, AT-UI-2, AT-UI-7: panels, themes and control names.

#include "UiFixture.h"

#include "ui/ProjectDialogs.h"
#include "ui/UndoHistory.h"

#include <cstdlib>
#include <tuple>

using namespace weft;
using namespace weft::ui;
using namespace weft::ui::test;

TEST_CASE ("AT-UI-1 panel toggles show and hide panels; layout persists", "[UI]")
{
    Ui ui;
    auto& main = *ui.main;
    std::vector<PanelLayout> saved;
    main.onLayoutChanged = [&] (const PanelLayout& l) { saved.push_back (l); };

    CHECK (main.inspector().isVisible());
    CHECK_FALSE (main.side().isVisible());
    CHECK_FALSE (main.bottom().isVisible());

    struct Toggle
    {
        Cmd cmd;
        std::function<bool()> shown;
    };
    const std::vector<Toggle> toggles {
        { Cmd::toggleInspector, [&] { return main.inspector().isVisible(); } },
        { Cmd::toggleSidePanel, [&] { return main.side().isVisible(); } },
        { Cmd::toggleMixer, [&] { return main.bottom().isVisible() && ui.studio.layout.bottom == BottomTab::mixer; } },
        { Cmd::toggleAudioEditor, [&] { return main.bottom().isVisible() && main.bottom().audioEditor().isVisible(); } },
        { Cmd::toggleGlobalTracks, [&] { return main.tracks().ruler().preferredHeight() > Ruler::rulerHeight; } },
    };
    for (const auto& t : toggles)
    {
        const auto before = t.shown();
        main.run (t.cmd);
        ui.settle();
        CHECK (t.shown() != before);
        // The tracks area always stays visible with room to work.
        CHECK (main.tracks().isVisible());
        CHECK (main.tracks().getHeight() >= 160);
        CHECK (main.tracks().getWidth() > 300);
        main.run (t.cmd);
        ui.settle();
        CHECK (t.shown() == before);
    }
    REQUIRE_FALSE (saved.empty());

    // Everything on at once still leaves the tracks area.
    for (auto c : { Cmd::toggleSidePanel, Cmd::toggleMixer })
        main.run (c);
    ui.settle();
    CHECK (main.tracks().getHeight() >= 160);

    // Restart: the saved layout text restores the same panels.
    const auto text = saved.back().toString();
    const auto restored = PanelLayout::fromString (text);
    CHECK (restored == ui.studio.layout);
    ui.main.reset();
    ui.studio.layout = PanelLayout {};
    ui.studio.layout = restored;
    ui.main = std::make_unique<MainComponent> (ui.studio, juce::File (juce::String (ui.folder.path.string())));
    ui.main->setSize (1400, 860);
    ui.main->setVisible (true);
    ui.settle();
    CHECK (ui.main->side().isVisible());
    CHECK (ui.main->bottom().isVisible());
    CHECK (ui.main->inspector().isVisible());
    CHECK (PanelLayout::fromString ("not json") == PanelLayout {});
}

TEST_CASE ("AT-UI-2 theme switch without restart; text pairs meet WCAG AA", "[UI]")
{
    for (const bool dark : { true, false })
    {
        Theme::get().setDark (dark);
        for (const auto& pair : textPairs())
        {
            INFO ((dark ? "dark " : "light ") << static_cast<int> (pair.text) << " on " << static_cast<int> (pair.background));
            CHECK (contrastRatio (colour (pair.text), colour (pair.background)) >= 4.5);
        }
        // Region names sit on region colours.
        for (int i = 0; i < paletteSize; ++i)
        {
            INFO ("palette " << i);
            CHECK (contrastRatio (colour (Token::regionText), paletteColour (i)) >= 4.5);
        }
    }

    Ui ui;
    auto snapshotColour = [&] {
        const auto image = ui.main->createComponentSnapshot (ui.main->getLocalBounds());
        // A point in the tracks area well clear of any text.
        const auto p = ui.main->tracks().getBounds().getCentre().translated (0, 120);
        return image.getPixelAt (p.x, p.y);
    };
    const auto darkPixel = snapshotColour();
    const auto darkWindow = Theme::get().lookAndFeel().findColour (juce::ResizableWindow::backgroundColourId);
    ui.main->run (Cmd::toggleTheme);
    ui.settle();
    CHECK_FALSE (Theme::get().isDark());
    CHECK (snapshotColour() != darkPixel);
    CHECK (Theme::get().lookAndFeel().findColour (juce::ResizableWindow::backgroundColourId) != darkWindow);
    CHECK_FALSE (ui.studio.layout.darkTheme);
    ui.main->run (Cmd::toggleTheme);
    ui.settle();
    CHECK (snapshotColour() == darkPixel);
}

namespace
{
bool shownIn (juce::Component& c, juce::Component& root)
{
    for (auto* p = &c; p != nullptr; p = p->getParentComponent())
    {
        if (! p->isVisible())
            return false;
        if (p == &root)
            return true;
    }
    return false;
}

bool insideJuceInternals (juce::Component& c, juce::Component& root)
{
    for (auto* p = c.getParentComponent(); p != nullptr && p != &root; p = p->getParentComponent())
        if (dynamic_cast<juce::Slider*> (p) || dynamic_cast<juce::ComboBox*> (p) || dynamic_cast<juce::ScrollBar*> (p)
            || dynamic_cast<juce::TabbedButtonBar*> (p) || dynamic_cast<juce::TreeView*> (p) || dynamic_cast<juce::ListBox*> (p))
            return true;
    return false;
}
} // namespace

TEST_CASE ("AT-UI-7 every control has a name and a tooltip with its shortcut", "[UI]")
{
    Ui ui;
    auto& main = *ui.main;
    ui.instrumentTracks (1);
    main.run (Cmd::toggleSidePanel);
    ui.settle();

    int checked = 0;
    auto walk = [&] {
        forEachComponent (main, [&] (juce::Component& c) {
            if (! shownIn (c, main) || insideJuceInternals (c, main))
                return;
            const bool control = dynamic_cast<juce::Button*> (&c) || dynamic_cast<juce::ComboBox*> (&c) || dynamic_cast<juce::Slider*> (&c)
                                 || (dynamic_cast<juce::Label*> (&c) && static_cast<juce::Label&> (c).isEditableOnDoubleClick());
            if (! control || dynamic_cast<juce::TabBarButton*> (&c))
                return;
            ++checked;
            const auto name = c.getTitle().isNotEmpty() ? c.getTitle() : juce::String();
            INFO ("component " << c.getName() << " / " << name << " in " << (c.getParentComponent() ? c.getParentComponent()->getTitle() : ""));
            CHECK (name.isNotEmpty());
            auto* tip = dynamic_cast<juce::TooltipClient*> (&c);
            REQUIRE (tip != nullptr);
            CHECK (tip->getTooltip().isNotEmpty());
            if (auto* cb = dynamic_cast<CommandButton*> (&c))
            {
                const auto& info = commandInfo (cb->command);
                if (info.shortcut.isNotEmpty())
                    CHECK (tip->getTooltip().contains (info.shortcut));
                CHECK (tip->getTooltip().startsWith (c.getTitle()));
            }
        });
    };
    walk();
    // The mixer and editor tabs, and a selected region for the inspector.
    const auto track = ui.m().trackOrder.back();
    const auto region = ui.studio.session().createMidiRegion (track, 0, bar);
    REQUIRE (region);
    ui.studio.selection.set ({ *region });
    ui.studio.edited();
    for (auto c : { Cmd::toggleMixer, Cmd::toggleAudioEditor })
    {
        main.run (c);
        ui.settle();
        walk();
    }
    CHECK (checked > 40);

    // The accessibility handler reports the same names.
    forEachComponent (main.controlBar(), [&] (juce::Component& c) {
        if (auto* b = dynamic_cast<CommandButton*> (&c))
            if (auto* h = b->getAccessibilityHandler())
                CHECK (h->getTitle().isNotEmpty());
    });
}

TEST_CASE ("AT-UI-10 the inspector edits every selected region and shows a dash where they differ", "[UI][AT-UI-10]")
{
    Ui ui;
    const auto tracks = ui.instrumentTracks (1);
    auto& s = ui.studio.session();
    const auto a = *s.createMidiRegion (tracks[0], 0, bar);
    const auto b = *s.createMidiRegion (tracks[0], 2 * bar, bar);
    REQUIRE (s.setRegionTranspose (b, 5));
    ui.studio.selection.set ({ a, b });
    ui.studio.edited();
    ui.settle();
    auto& inspector = ui.main->inspector();
    const juce::String dash (juce::CharPointer_UTF8 ("\xe2\x80\x94"));
    const auto text = [] (juce::Slider& sl) { return sl.getTextFromValue (sl.getValue()); };

    CHECK (inspector.regionName.getText() == "2 regions");
    REQUIRE (inspector.regionTranspose.isVisible());
    CHECK (text (inspector.regionTranspose) == dash);   // 0 and +5
    CHECK (text (inspector.regionVelocity) != dash);    // both 0
    CHECK_FALSE (inspector.regionMute.getToggleState());

    // One edit sets both regions, as one undo step.
    inspector.regionTranspose.setValue (-12.0, juce::sendNotificationSync);
    ui.settle();
    CHECK (ui.region (a).midi.transpose == -12);
    CHECK (ui.region (b).midi.transpose == -12);
    CHECK (text (inspector.regionTranspose) != dash);
    ui.studio.session().undo();
    ui.studio.edited();
    ui.settle();
    CHECK (ui.region (a).midi.transpose == 0);
    CHECK (ui.region (b).midi.transpose == 5);

    inspector.regionMute.setToggleState (true, juce::sendNotificationSync);
    ui.settle();
    CHECK (ui.region (a).muted);
    CHECK (ui.region (b).muted);
    ui.studio.session().undo();
    ui.studio.edited();
    ui.settle();
    CHECK_FALSE (ui.region (a).muted);
    CHECK_FALSE (ui.region (b).muted);

    // An audio region and a MIDI region together: only what both have is offered, and it sets both.
    const auto file = writeSine (ui.folder.path / "sine.wav");
    engine::DropTarget target;
    target.at = 4 * bar;
    target.trackId = ui.m().trackOrder.front();
    const auto outcome = ui.studio.importFiles ({ file }, target);
    REQUIRE (outcome.regions.size() == 1);
    const auto audio = outcome.regions.front();
    ui.studio.selection.set ({ a, audio });
    ui.studio.edited();
    ui.settle();
    CHECK_FALSE (inspector.regionTranspose.isVisible());
    CHECK_FALSE (inspector.regionGain.isVisible());
    REQUIRE (inspector.regionDelay.isVisible());
    inspector.regionDelay.setValue (20.0, juce::sendNotificationSync);
    ui.settle();
    CHECK (ui.region (a).delayMs == Catch::Approx (20.0));
    CHECK (ui.region (audio).delayMs == Catch::Approx (20.0));
}

TEST_CASE ("AT-UI-11 a narrow control bar keeps the count-in and CPU reading in a More menu", "[UI][AT-UI-11]")
{
    Ui ui; // 1400 px wide: too narrow for everything
    auto& barUi = ui.main->controlBar();
    const auto labels = [] (const juce::PopupMenu& menu) {
        juce::StringArray out;
        for (juce::PopupMenu::MenuItemIterator it (menu, true); it.next();)
            out.add (it.getItem().text);
        return out;
    };
    REQUIRE (barUi.moreButton().isVisible());
    const auto items = labels (barUi.overflowMenu());
    CHECK (items.contains ("2-bar count-in"));
    bool cpuLine = false;
    for (const auto& item : items)
        cpuLine = cpuLine || item.startsWith ("CPU");
    CHECK (cpuLine);
    barUi.pickOverflow (ControlBar::overflowCountIn + 2);
    CHECK (ui.studio.engine().settings().countInBars == 2);
    barUi.pickOverflow (ControlBar::overflowCountIn);
    CHECK (ui.studio.engine().settings().countInBars == 0);

    // Wide enough for everything: no More button.
    ui.main->setSize (2200, 860);
    ui.settle();
    CHECK_FALSE (barUi.moreButton().isVisible());

    // The Navigate menu has the count-in at any width, and Shift+K turns it off and back on.
    const auto navigate = labels (ui.main->getMenuForIndex (4, "Navigate"));
    CHECK (navigate.contains ("Count-in"));
    CHECK (navigate.contains ("2 Bars"));
    ui.main->menuItemSelected (MainComponent::countInTwo, 4);
    CHECK (ui.studio.engine().settings().countInBars == 2);
    REQUIRE (ui.key ('K', juce::ModifierKeys::shiftModifier));
    CHECK (ui.studio.engine().settings().countInBars == 0);
    REQUIRE (ui.key ('K', juce::ModifierKeys::shiftModifier));
    CHECK (ui.studio.engine().settings().countInBars == 2);
}

// Not an acceptance test: renders the window with some content to a PNG for reports.
// Run with: weft_ui_tests "[.screenshot]" (writes WEFT_SCREENSHOT or ./arrange.png).
TEST_CASE ("Screenshot of the arrange window", "[.screenshot]")
{
    Ui ui;
    auto& s = ui.studio.session();
    const auto audio = ui.m().trackOrder.front();
    // Plucked notes, so the waveform has the shape of a real part rather than a flat tone.
    const auto file = ui.folder.path / "Guitar.wav";
    {
        media::AudioData a;
        a.sampleRate = 48000;
        const auto n = static_cast<std::size_t> (6.0 * a.sampleRate);
        a.channels.assign (2, std::vector<float> (n));
        const double freqs[] = { 196.0, 246.9, 293.7, 392.0, 329.6, 246.9 };
        for (std::size_t i = 0; i < n; ++i)
        {
            const auto t = static_cast<double> (i) / a.sampleRate;
            const auto note = static_cast<int> (t / 0.25);
            const auto since = t - note * 0.25;
            const auto accent = note % 4 == 0 ? 0.9 : 0.45 + 0.1 * (note % 3);
            const auto v = accent * std::exp (-since * 9.0) * std::sin (2.0 * 3.141592653589793 * freqs[note % 6] * t);
            a.channels[0][i] = static_cast<float> (v);
            a.channels[1][i] = static_cast<float> (v * 0.9);
        }
        REQUIRE (media::writeWav (file, a, media::SampleFormat::int24));
    }
    engine::DropTarget target;
    target.at = bar;
    target.trackId = audio;
    REQUIRE (ui.studio.importFiles ({ file }, target).regions.size() == 1);
    const auto keys = ui.instrumentTracks (2);
    s.renameTrack (keys[0], "Keys");
    s.renameTrack (keys[1], "Bass");
    for (const auto& [track, startBar, pitch] : std::vector<std::tuple<Id, int, int>> { { keys[0], 0, 60 }, { keys[0], 4, 64 }, { keys[1], 2, 36 } })
    {
        const auto r = *s.createMidiRegion (track, startBar * bar, 2 * bar);
        for (int i = 0; i < 8; ++i)
        {
            model::Note n;
            n.pos = static_cast<Ticks> (i) * bar / 4;
            n.length = bar / 4;
            n.pitch = pitch + (i * 5) % 12;
            s.addNote (r, n);
        }
    }
    s.addMarker (0, "Intro");
    s.addMarker (4 * bar, "Verse");
    ui.studio.layout.sidePanel = true;
    ui.studio.selection.clear();
    ui.studio.edited();
    ui.settle();
    ui.main->run (Cmd::zoomToFit);
    ui.studio.selection.set ({ ui.m().regionsOnTrack (audio).front()->id });
    ui.studio.edited();
    ui.settle();
    const auto image = ui.main->createComponentSnapshot (ui.main->getLocalBounds());
    const auto* env = std::getenv ("WEFT_SCREENSHOT");
    juce::File out (env != nullptr ? juce::String (env) : juce::File::getCurrentWorkingDirectory().getChildFile ("arrange.png").getFullPathName());
    out.deleteFile();
    juce::FileOutputStream stream (out);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));

    // Zoomed in on the start of the audio region, to check the waveform stays crisp.
    ui.studio.view.pixelsPerTick *= 24.0;
    ui.studio.view.originTick = bar;
    ui.studio.edited();
    ui.settle();
    const auto zoomed = ui.main->createComponentSnapshot (ui.main->getLocalBounds());
    const auto zoomFile = out.getSiblingFile (out.getFileNameWithoutExtension() + "-zoom.png");
    zoomFile.deleteFile();
    juce::FileOutputStream zoomStream (zoomFile);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (zoomed, zoomStream));

    // The browser's rows, drawn with the look and feel the way the tree draws them (the tree
    // itself only lays out rows from the message loop, which this test doesn't run).
    auto& side = ui.main->side();
    const auto header = side.createComponentSnapshot (side.getLocalBounds().withHeight (size::panelHeader * 2));
    const int rowH = size::control;
    juce::Image rows (juce::Image::RGB, side.getWidth(), header.getHeight() + 6 * rowH, true);
    {
        juce::Graphics g (rows);
        g.fillAll (colour (Token::surface));
        g.drawImageAt (header, 0, 0);
        const std::tuple<const char*, bool, const char*> entries[] = {
            { "Drums", true, "" }, { "Vocals", true, "" }, { "Bass take 2.wav", false, "1.6 MB" },
            { "Guitar DI.wav", false, "1.6 MB" }, { "Room tone.wav", false, "1.6 MB" }, { "Keys.mid", false, "2 KB" } };
        int i = 0;
        for (const auto& [name, folder, bytes] : entries)
        {
            juce::Graphics::ScopedSaveState save (g);
            g.setOrigin (0, header.getHeight() + i * rowH);
            g.reduceClipRegion (0, 0, rows.getWidth(), rowH);
            Theme::get().lookAndFeel().drawFileBrowserRow (g, rows.getWidth(), rowH, {}, name, nullptr, bytes, {}, folder, i == 3, i,
                                                            side.browser().tree());
            ++i;
        }
    }
    const auto browserFile = out.getSiblingFile (out.getFileNameWithoutExtension() + "-browser.png");
    browserFile.deleteFile();
    juce::FileOutputStream browserStream (browserFile);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (rows, browserStream));

    // The audio editor on the Guitar region, trimmed so both parts of the file show.
    const auto guitar = ui.m().regionsOnTrack (audio).front()->id;
    REQUIRE (s.trimAudioStartSamples (guitar, 24000));
    REQUIRE (s.trimRegionLength (guitar, 96000));
    ui.studio.editorRegion = guitar;
    ui.main->run (Cmd::toggleAudioEditor);
    ui.studio.edited();
    ui.settle();
    const auto editor = ui.main->createComponentSnapshot (ui.main->getLocalBounds());
    const auto editorFile = out.getSiblingFile (out.getFileNameWithoutExtension() + "-editor.png");
    editorFile.deleteFile();
    juce::FileOutputStream editorStream (editorFile);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (editor, editorStream));
}

TEST_CASE ("D-UI-13 controls take keyboard focus with a visible ring, and clicks don't steal it", "[U][UI]")
{
    Ui ui;
    ui.instrumentTracks (1);
    ui.main->run (Cmd::toggleMixer);
    ui.settle();
    int checked = 0;
    forEachComponent (*ui.main, [&] (juce::Component& c) {
        const bool control = dynamic_cast<juce::Button*> (&c) != nullptr || dynamic_cast<juce::Slider*> (&c) != nullptr || dynamic_cast<juce::ComboBox*> (&c) != nullptr;
        // A disabled control can't take focus (JUCE), e.g. the browser's Back with nothing to go back to.
        if (! control || dynamic_cast<juce::TabBarButton*> (&c) != nullptr || ! c.isVisible() || ! c.isEnabled() || c.getParentComponent() == nullptr
            || dynamic_cast<juce::ScrollBar*> (c.getParentComponent()) != nullptr || dynamic_cast<juce::Slider*> (c.getParentComponent()) != nullptr
            || dynamic_cast<juce::TabbedButtonBar*> (c.getParentComponent()) != nullptr) // the tab bar's own overflow button
            return;
        ++checked;
        INFO (c.getTitle() << " in " << c.getParentComponent()->getTitle());
        CHECK (c.getWantsKeyboardFocus());
        CHECK (c.hasFocusOutline());
        CHECK_FALSE (c.getMouseClickGrabsKeyboardFocus());
    });
    CHECK (checked > 40);
    // The ring itself: the theme makes one for any focusable component.
    CHECK (Theme::get().lookAndFeel().createFocusOutlineForComponent (*ui.main) != nullptr);
}

TEST_CASE ("AT-PRL-24 the inspector sets the more MIDI region parameters and Reverse", "[UI][AT-PRL-24]")
{
    Ui ui;
    const auto tracks = ui.instrumentTracks (1);
    auto& s = ui.studio.session();
    const auto a = *s.createMidiRegion (tracks[0], 0, bar);
    ui.studio.selection.set ({ a });
    ui.studio.edited();
    ui.settle();
    auto& in = ui.main->inspector();
    REQUIRE (in.regionDynamics.isVisible());
    CHECK (in.regionDynamics.getText() == "Off");
    CHECK (in.regionGate.getText() == "Off");
    CHECK (in.regionClipLength.getToggleState());
    CHECK (in.regionDelayUnit.getText() == "ms");
    CHECK_FALSE (in.regionReverse.isVisible());

    in.regionDynamics.setSelectedItemIndex (2, juce::sendNotificationSync); // 2:1
    in.regionGate.setText ("Legato", juce::sendNotificationSync);
    in.regionClipLength.setToggleState (false, juce::sendNotificationSync);
    ui.settle();
    CHECK (ui.region (a).midi.dynamics == 50);
    CHECK (ui.region (a).midi.gate == model::MidiRegionData::gateLegato);
    CHECK_FALSE (ui.region (a).midi.clipLength);
    CHECK (s.undoName() == "Region Clip Length");

    // Q-Range and Q-Flam need a quantize value, and keep it.
    CHECK_FALSE (in.quantizeRange.isEnabled());
    in.regionQuantize.setSelectedId (in.regionQuantize.getNumItems(), juce::sendNotificationSync);
    ui.settle();
    REQUIRE (in.quantizeRange.isEnabled());
    const auto grid = ui.region (a).midi.quantize->grid;
    in.quantizeRange.setValue (30, juce::sendNotificationSync);
    in.quantizeFlam.setValue (-10, juce::sendNotificationSync);
    ui.settle();
    REQUIRE (ui.region (a).midi.quantize);
    CHECK (ui.region (a).midi.quantize->grid == grid);
    CHECK (ui.region (a).midi.quantize->range == 30);
    CHECK (ui.region (a).midi.quantize->flam == -10);

    // Delay in ticks.
    in.regionDelayUnit.setSelectedId (2, juce::sendNotificationSync);
    ui.settle();
    CHECK (ui.region (a).delayInTicks);
    CHECK (in.regionDelay.getMaximum() == 960.0);
    in.regionDelay.setValue (240, juce::sendNotificationSync);
    ui.settle();
    CHECK (ui.region (a).delayTicks == 240);
    CHECK (in.regionDelay.getTextFromValue (240) == "240");

    // Edit › MIDI Region Parameters.
    const auto menu = ui.main->getMenuForIndex (1, "Edit");
    juce::StringArray names;
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        if (it.getItem().text == "MIDI Region Parameters")
            for (juce::PopupMenu::MenuItemIterator sub (*it.getItem().subMenu); sub.next();)
                names.add (sub.getItem().text);
    CHECK (names == juce::StringArray { "Apply All Parameters Permanently", "Apply All Except Delay" });
    CHECK (ui.main->run (Cmd::applyRegionParametersExceptDelay));
    CHECK (s.undoName() == "Apply All Except Delay");
    CHECK_FALSE (ui.region (a).midi.quantize);
    CHECK (ui.region (a).midi.dynamics == 100);
    CHECK (ui.region (a).delayTicks == 240);
    ui.settle();
    CHECK (in.regionDynamics.getText() == "Off");

    // Reverse for audio regions, as a tick box.
    const auto file = writeSine (ui.folder.path / "sine.wav");
    engine::DropTarget target;
    target.at = 4 * bar;
    target.trackId = ui.m().trackOrder.front();
    const auto outcome = ui.studio.importFiles ({ file }, target);
    REQUIRE (outcome.regions.size() == 1);
    const auto audio = outcome.regions.front();
    ui.studio.selection.set ({ audio });
    ui.studio.edited();
    ui.settle();
    REQUIRE (in.regionReverse.isVisible());
    CHECK_FALSE (in.regionReverse.getToggleState());
    in.regionReverse.setToggleState (true, juce::sendNotificationSync);
    ui.settle();
    CHECK (ui.region (audio).audio.reversed);
    CHECK (in.regionReverse.getToggleState());
    CHECK (s.undoName() == "Reverse");
}

TEST_CASE ("AT-UI-13 the Settings window", "[UI][AT-UI-13]")
{
    Ui ui;
    Preferences saved;
    int saves = 0;
    ui.studio.onPreferencesChanged = [&] (const Preferences& p) {
        saved = p;
        ++saves;
    };
    SettingsDialog dialog (ui.studio);
    CHECK (dialog.startup.getSelectedId() == 1);
    CHECK (dialog.recentItems.getSelectedId() == 10);
    CHECK (dialog.softwareMonitoring.getToggleState());
    CHECK (dialog.appearance.getSelectedId() == 1);
    CHECK (dialog.middleC.getSelectedId() == 3);

    // Each change applies at once and is saved.
    dialog.middleC.setSelectedId (4, juce::sendNotificationSync);
    CHECK (commands::noteName (60) == "C4");
    CHECK (saves == 1);
    CHECK (saved.middleC == 4);
    dialog.middleC.setSelectedId (3, juce::sendNotificationSync);
    CHECK (commands::noteName (60) == "C3");

    dialog.appearance.setSelectedId (2, juce::sendNotificationSync);
    CHECK_FALSE (ui.studio.layout.darkTheme);
    CHECK_FALSE (Theme::get().isDark());
    ui.main->run (Cmd::toggleTheme); // the View menu's switch still works, and sets Dark
    CHECK (ui.studio.preferences().appearance == Preferences::Appearance::dark);
    CHECK (Theme::get().isDark());

    dialog.softwareMonitoring.setToggleState (false, juce::sendNotificationSync);
    CHECK_FALSE (ui.studio.engine().localState().softwareMonitoring);
    dialog.monitorSelectedOnly.setToggleState (true, juce::sendNotificationSync);
    CHECK (ui.studio.engine().localState().monitorSelectedOnly);

    dialog.recentItems.setSelectedId (5, juce::sendNotificationSync);
    CHECK (ui.studio.recentProjects().limit() == 5);
    dialog.widePlayhead.setToggleState (true, juce::sendNotificationSync);
    CHECK (Theme::get().widePlayhead);
    dialog.startup.setSelectedId (2, juce::sendNotificationSync);
    CHECK (saved.startup == Preferences::Startup::lastProject);

    // Help tags off removes the tooltips.
    dialog.helpTags.setToggleState (false, juce::sendNotificationSync);
    ui.settle();
    CHECK_FALSE (ui.studio.preferences().helpTags);

    // The settings read back as written.
    const auto again = Preferences::fromString (saved.toString());
    CHECK (again == saved);
    CHECK (again.recentItems == 5);
    CHECK_FALSE (again.softwareMonitoring);

    // A dropout says so in the status bar, unless that's turned off.
    ui.studio.engine().addDeviceDropouts (1);
    ui.studio.tick();
    CHECK (ui.studio.status.startsWith ("The audio engine couldn't keep up"));
    dialog.overloadMessage.setToggleState (false, juce::sendNotificationSync);
    ui.studio.setStatus ({});
    ui.studio.engine().addDeviceDropouts (1);
    ui.studio.tick();
    CHECK (ui.studio.status.isEmpty());

    // File › Settings... (Ctrl+,).
    CHECK (commandInfo (Cmd::settings).shortcut == "Ctrl+,");
    const auto menu = ui.main->getMenuForIndex (0, "File");
    bool listed = false;
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        listed = listed || it.getItem().text == "Settings...";
    CHECK (listed);

    // Put things back for the other tests.
    ui.studio.onPreferencesChanged = nullptr;
    ui.studio.setPreferences ({});
    Theme::get().widePlayhead = false;
}

TEST_CASE ("AT-UI-14 customize the control bar, its master control and display formats; toolbar; big displays", "[UI][AT-UI-14]")
{
    Ui ui;
    ui.main->setSize (2400, 860);
    ui.settle();
    auto& bar = ui.main->controlBar();
    using L = ControlBarLayout;

    // The menu lists every item, ticked; unticking Tempo and Key hides them, and Revert brings back the last change.
    juce::StringArray items;
    const auto menu = bar.customizeMenu();
    for (juce::PopupMenu::MenuItemIterator it (menu, true); it.next();)
        items.add (it.getItem().text);
    for (const auto* name : { "Tempo", "Key", "Count-in", "CPU Meter", "Master Volume", "Output Meter", "Samples", "BPM Without Decimals",
                              "Open Big Beats Display", "Apply Defaults", "Save As Default", "Revert" })
        CHECK (items.contains (name));
    REQUIRE (bar.tempoDisplay().isVisible());
    bar.pickCustomize (ControlBar::itemShowBase + 4); // Tempo
    CHECK_FALSE (bar.tempoDisplay().isVisible());
    bar.pickCustomize (ControlBar::itemShowBase + 6); // Key
    CHECK_FALSE (bar.keyDisplay().isVisible());
    bar.pickCustomize (ControlBar::itemRevert);
    CHECK (bar.keyDisplay().isVisible());
    CHECK_FALSE (bar.tempoDisplay().isVisible());
    // Save As Default, change again, Apply Defaults: back to the saved layout.
    bar.pickCustomize (ControlBar::itemSaveDefault);
    CHECK (ui.studio.layout.barDefault.items == (L::allItems & ~L::tempoDisplay));
    bar.pickCustomize (ControlBar::itemShowBase + 12); // CPU meter
    CHECK_FALSE (bar.cpuDisplay().isVisible());
    bar.pickCustomize (ControlBar::itemApplyDefaults);
    CHECK (bar.cpuDisplay().isVisible());
    CHECK_FALSE (bar.tempoDisplay().isVisible());

    // The master volume slider sets the master's fader; the meter takes its place, or neither shows.
    REQUIRE (bar.masterVolumeSlider().isVisible());
    CHECK_FALSE (bar.outputMeterView().isVisible());
    bar.masterVolumeSlider().setValue (-6.0, juce::sendNotificationSync);
    ui.settle();
    CHECK (ui.m().track (ui.m().info.masterTrackId)->strip.volumeDb == Catch::Approx (-6.0));
    bar.pickCustomize (ControlBar::itemMasterBase + 2);
    CHECK_FALSE (bar.masterVolumeSlider().isVisible());
    CHECK (bar.outputMeterView().isVisible());
    bar.pickCustomize (ControlBar::itemMasterBase);
    CHECK_FALSE (bar.outputMeterView().isVisible());

    // Display formats: time as samples or timecode, tempo without decimals.
    bar.pickCustomize (ControlBar::itemShowBase + 4); // tempo back
    ui.studio.locate (2 * 3840 + 960); // bar 3 beat 2 at 120 BPM: 4.5 s
    bar.pickCustomize (ControlBar::itemTimeBase + 1);
    bar.refresh();
    CHECK (bar.timeDisplay().getText() == "216000");
    bar.pickCustomize (ControlBar::itemTimeBase + 3);
    bar.refresh();
    CHECK (bar.timeDisplay().getText() == "00:00:04:12");
    bar.pickCustomize (ControlBar::itemTempoBase);
    bar.refresh();
    CHECK (bar.tempoDisplay().getText() == "120");
    CHECK (formatDisplayTime (62.345, 48000, L::TimeFormat::milliseconds) == "1:02.345");
    CHECK (formatDisplayTime (3725.5, 48000, L::TimeFormat::timecode30) == "01:02:05:15");

    // View › Control Bar hides the bar; View › Toolbar (or its button) shows the toolbar with the default buttons.
    REQUIRE (ui.studio.perform (Cmd::toggleControlBar));
    ui.settle();
    CHECK_FALSE (bar.isVisible());
    REQUIRE (ui.studio.perform (Cmd::toggleControlBar));
    REQUIRE (ui.studio.perform (Cmd::toggleToolbar));
    ui.settle();
    CHECK (bar.isVisible());
    auto& toolbar = ui.main->toolbar();
    REQUIRE (toolbar.isVisible());
    CHECK (toolbar.getY() == bar.getBottom());
    REQUIRE (toolbar.buttons().size() == defaultToolbarCommands().size());
    CHECK (toolbar.buttons()[0]->getButtonText() == "Split at Playhead");
    // Add Split at Cycle (second in the menu): it takes its place in the menu's order. Restore Defaults removes it.
    toolbar.pickCustomize (2);
    REQUIRE (toolbar.buttons().size() == defaultToolbarCommands().size() + 1);
    CHECK (toolbar.buttons()[1]->command == Cmd::splitAtCycle);
    toolbar.pickCustomize (Toolbar::itemRestoreDefault);
    CHECK (toolbar.buttons().size() == defaultToolbarCommands().size());
    CHECK (toolbar.buttons()[6]->command == Cmd::zoomToFit);

    // The layout survives a restart.
    ui.studio.layout.toolbarCommands = { Cmd::bounce, Cmd::join };
    const auto again = PanelLayout::fromString (ui.studio.layout.toString());
    CHECK (again.toolbar);
    CHECK (again.bar == ui.studio.layout.bar);
    CHECK (again.barDefault == ui.studio.layout.barDefault);
    CHECK (again.toolbarCommands == std::vector<Cmd> { Cmd::bounce, Cmd::join });

    // Big displays and the performance meter open as windows; several can be open.
    REQUIRE (ui.studio.perform (Cmd::bigBeatsDisplay));
    REQUIRE (ui.studio.perform (Cmd::bigTimeDisplay));
    REQUIRE (ui.studio.perform (Cmd::bigBeatsDisplay));
    REQUIRE (ui.main->floatingWindows().size() == 3);
    auto* beats = dynamic_cast<BigDisplay*> (ui.main->floatingWindows()[0]->getContentComponent());
    auto* time = dynamic_cast<BigDisplay*> (ui.main->floatingWindows()[1]->getContentComponent());
    REQUIRE (beats != nullptr);
    REQUIRE (time != nullptr);
    CHECK (beats->text() == bar.positionDisplay().getText());
    CHECK (time->text() == "00:00:04:12");
    bar.onPerformanceMeter (2); // Ctrl+double-click on the CPU reading: twice the size
    REQUIRE (ui.main->floatingWindows().size() == 4);
    auto* meter = dynamic_cast<PerformanceMeter*> (ui.main->floatingWindows()[3]->getContentComponent());
    REQUIRE (meter != nullptr);
    CHECK (meter->getWidth() == 520);
    CHECK (meter->text().startsWith ("CPU "));
}

TEST_CASE ("AT-UI-15 screensets, Undo History, panes in their own windows, window views and Close Project", "[UI][AT-UI-15]")
{
    Ui ui;
    auto& studio = ui.studio;

    // Screensets: each keeps the layout it was left with; one never used starts as a copy.
    REQUIRE (studio.layout.inspector);
    REQUIRE (studio.perform (Cmd::screenset2));
    CHECK (studio.screenset == 2);
    REQUIRE (studio.perform (Cmd::toggleInspector));
    CHECK_FALSE (studio.layout.inspector);
    REQUIRE (studio.perform (Cmd::screenset1));
    CHECK (studio.layout.inspector);
    REQUIRE (studio.perform (Cmd::screenset2));
    CHECK_FALSE (studio.layout.inspector);
    // Recall Screenset 1x, then 3: screenset 13.
    REQUIRE (studio.perform (Cmd::screensetTens1));
    REQUIRE (studio.perform (Cmd::marker3));
    CHECK (studio.screenset == 13);
    CHECK (studio.playhead() == 0); // the number key didn't go to a marker
    // They're kept as text by the app and read back.
    const auto saved = studio.screensetsToString();
    studio.screensets.clear();
    studio.screensetsFromString (saved);
    CHECK (studio.screenset == 13);
    CHECK (studio.screensets.size() == 3);
    CHECK_FALSE (studio.screensets.at (2).inspector);

    // Undo History lists the steps; clicking a step goes back or forward to it.
    auto& s = studio.session();
    const auto track = ui.m().trackOrder.front();
    REQUIRE (s.renameTrack (track, "One"));
    REQUIRE (s.renameTrack (track, "Two"));
    studio.edited();
    REQUIRE (studio.perform (Cmd::undoHistory));
    auto* window = ui.main->windowNamed ("Undo History");
    REQUIRE (window != nullptr);
    auto* history = dynamic_cast<UndoHistory*> (window->getContentComponent());
    REQUIRE (history != nullptr);
    const auto rows = history->getNumRows();
    CHECK (history->rowText (0) == "Start");
    CHECK (history->rowText (rows - 1) == "Rename Track");
    CHECK (history->currentRow() == rows - 1);
    history->goTo (rows - 3);
    CHECK (ui.m().track (track)->name == "Audio 1");
    CHECK (history->getNumRows() == rows); // the undone steps stay listed, to redo
    history->goTo (rows - 1);
    CHECK (ui.m().track (track)->name == "Two");
    REQUIRE (studio.perform (Cmd::undoHistory)); // already open: brought forward, not doubled
    CHECK (ui.main->floatingWindows().size() == 1);

    // The panes in their own windows; Close Window closes the last one opened.
    REQUIRE (studio.perform (Cmd::mixerWindow));
    REQUIRE (studio.perform (Cmd::pianoRollWindow));
    REQUIRE (studio.perform (Cmd::regionInspectorWindow));
    CHECK (ui.main->windowNamed ("Mixer") != nullptr);
    CHECK (dynamic_cast<MixerView*> (ui.main->windowNamed ("Mixer")->getContentComponent()) != nullptr);
    CHECK (ui.main->windowNamed ("Piano Roll") != nullptr);
    CHECK (ui.main->floatingWindows().size() == 4);
    REQUIRE (studio.perform (Cmd::closeWindow));
    CHECK (ui.main->windowNamed ("Inspector") == nullptr);
    CHECK (ui.main->floatingWindows().size() == 3);

    // Cycle Through Window Views: none, Mixer, Piano Roll, Audio Editor, and back.
    studio.layout.bottom = BottomTab::none;
    REQUIRE (studio.perform (Cmd::cycleWindowViews));
    CHECK (studio.layout.bottom == BottomTab::mixer);
    REQUIRE (studio.perform (Cmd::cycleWindowViewsBack));
    REQUIRE (studio.perform (Cmd::cycleWindowViewsBack));
    CHECK (studio.layout.bottom == BottomTab::audioEditor);

    // Close Project: an empty untitled project in its place.
    const auto before = ui.m().info.id;
    REQUIRE (studio.closeProject());
    CHECK (ui.m().info.id != before);
    CHECK (ui.m().trackOrder.size() == 1);
}
