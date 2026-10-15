// AT-UI-1, AT-UI-2, AT-UI-7: panels, themes and control names.

#include "UiFixture.h"

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
