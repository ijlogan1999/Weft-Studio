// AT-UI-1, AT-UI-2, AT-UI-7: panels, themes and control names.

#include "UiFixture.h"

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
}
