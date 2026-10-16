// AT-EXP-11 (Replace / Keep both) and the Bounce dialog (W-9): Ctrl+B, defaults, progress,
// the completion notice and Add to Project.

#include "UiFixture.h"

#include "ui/BounceDialog.h"

using namespace weft;
using namespace weft::ui;
using namespace weft::ui::test;

namespace
{
/** A track with a 1 s sine at bar 1, so the default range is the region. */
void sineProject (Ui& ui)
{
    const auto file = writeSine (ui.folder.path / "browse" / "sine.wav");
    const auto outcome = ui.studio.importFiles ({ file }, { 0, {} });
    REQUIRE (outcome.regions.size() == 1);
    ui.settle();
}

void runBounce (Ui& ui)
{
    for (int i = 0; i < 1000 && ui.studio.bouncing(); ++i)
        ui.studio.tick();
    REQUIRE_FALSE (ui.studio.bouncing());
    ui.settle();
}
} // namespace

TEST_CASE ("Bounce dialog: Ctrl+B is in the File menu; defaults follow W-9", "[UI][bounce]")
{
    Ui ui;
    CHECK (commandInfo (Cmd::bounce).shortcut == "Ctrl+B");
    const auto menu = ui.main->getMenuForIndex (0, "File");
    bool listed = false;
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        listed = listed || it.getItem().text == "Bounce";
    CHECK (listed);

    sineProject (ui);
    REQUIRE (ui.studio.session().setEndMarker (2 * bar));
    ui.studio.edited();
    BounceDialog dialog (ui.studio);
    CHECK (dialog.start.getText() == "1.1.1.0");
    CHECK (dialog.end.getText() == "3.1.1.0");
    CHECK (dialog.tail.getToggleState());
    CHECK (dialog.mode.getText() == "Offline");
    CHECK (dialog.wav.getToggleState());
    CHECK (dialog.bitDepth.getText() == "24-bit");
    CHECK_FALSE (dialog.mp3.getToggleState());
    CHECK (dialog.bitrate.getText() == "320 kbps");
    CHECK (dialog.dither.getText() == "Auto");
    CHECK (dialog.title.getText() == juce::String (ui.m().info.name));
    const auto s = dialog.settings();
    CHECK (s.range.from == 0);
    CHECK (s.range.to == 2 * bar);
    CHECK (s.folder == ui.studio.projectRoot() / "Bounces");
    CHECK (s.name == ui.m().info.name);
    CHECK (s.wav->bitDepth == 24);
    CHECK_FALSE (s.mp3.has_value());

    // MP3 at VBR quality 4 with tags.
    dialog.mp3.setToggleState (true, juce::sendNotificationSync);
    dialog.mp3.onClick();
    dialog.bitrate.setSelectedId (14, juce::sendNotificationSync);
    dialog.artist.setText ("Ian", false);
    const auto withMp3 = dialog.settings();
    REQUIRE (withMp3.mp3.has_value());
    CHECK (withMp3.mp3->vbr);
    CHECK (withMp3.mp3->vbrQuality == 4);
    CHECK (withMp3.mp3->artist == "Ian");
}

TEST_CASE ("AT-EXP-11 bouncing to an existing name asks Replace / Keep both; Keep both writes name (2).wav", "[UI][bounce]")
{
    Ui ui;
    sineProject (ui);
    BounceDialog dialog (ui.studio);
    dialog.name.setText ("Mix", false);
    dialog.end.setText ("2.1.1.0", false);

    // First bounce: nothing in the way, no question.
    dialog.bounceButton.onClick();
    CHECK (dialog.question().isEmpty());
    CHECK (dialog.stage() == BounceDialog::Stage::rendering);
    runBounce (ui);
    CHECK (dialog.stage() == BounceDialog::Stage::done);
    const auto folder = ui.studio.projectRoot() / "Bounces";
    CHECK (std::filesystem::exists (folder / "Mix.wav"));
    CHECK (dialog.notice.getText() == "Bounced Mix.wav.");

    // Again with the same name: the question, then Keep both.
    BounceDialog again (ui.studio);
    again.name.setText ("Mix", false);
    again.end.setText ("2.1.1.0", false);
    again.bounceButton.onClick();
    CHECK (again.question() == "Mix.wav already exists.");
    CHECK (again.stage() == BounceDialog::Stage::settings);
    again.answer (BounceDialog::keepBothAnswer);
    runBounce (ui);
    CHECK (std::filesystem::exists (folder / "Mix (2).wav"));
    CHECK (again.notice.getText() == "Bounced Mix (2).wav.");

    // Replace overwrites Mix.wav and adds nothing.
    const auto before = std::filesystem::last_write_time (folder / "Mix.wav");
    BounceDialog third (ui.studio);
    third.name.setText ("Mix", false);
    third.end.setText ("2.1.1.0", false);
    third.tail.setToggleState (false, juce::dontSendNotification);
    third.bounceButton.onClick();
    third.answer (BounceDialog::replaceAnswer);
    runBounce (ui);
    CHECK_FALSE (std::filesystem::exists (folder / "Mix (3).wav"));
    CHECK (std::filesystem::last_write_time (folder / "Mix.wav") >= before);
    const auto info = media::readWavInfo (folder / "Mix.wav");
    REQUIRE (info.has_value());
    CHECK (info->frames == 96000); // one bar, no tail
    CHECK (info->bitsPerSample == 24);

    // Cancel at the question leaves everything alone.
    BounceDialog fourth (ui.studio);
    fourth.name.setText ("Mix", false);
    fourth.bounceButton.onClick();
    fourth.answer (BounceDialog::cancelAnswer);
    CHECK_FALSE (ui.studio.bouncing());
    CHECK (fourth.stage() == BounceDialog::Stage::settings);
}

TEST_CASE ("Bounce: Cancel stops rendering; Add to Project imports a muted track", "[UI][bounce]")
{
    Ui ui;
    sineProject (ui);
    const auto tracks = ui.m().trackOrder.size();
    {
        BounceDialog dialog (ui.studio);
        dialog.name.setText ("Long", false);
        dialog.end.setText ("200.1.1.0", false);
        dialog.bounceButton.onClick();
        REQUIRE (ui.studio.bouncing());
        dialog.cancel.onClick();
        CHECK_FALSE (ui.studio.bouncing());
        CHECK (dialog.stage() == BounceDialog::Stage::settings);
        CHECK (dialog.warning.getText() == "Bounce cancelled.");
        CHECK_FALSE (std::filesystem::exists (ui.studio.projectRoot() / "Bounces" / "Long.wav"));
    }

    BounceDialog dialog (ui.studio);
    dialog.name.setText ("Keep", false);
    dialog.bounceButton.onClick();
    runBounce (ui);
    REQUIRE (dialog.stage() == BounceDialog::Stage::done);
    dialog.addToProject.onClick();
    ui.settle();
    REQUIRE (ui.m().trackOrder.size() == tracks + 1);
    const auto* added = ui.m().track (ui.m().trackOrder.back());
    REQUIRE (added != nullptr);
    CHECK (added->mute);
    CHECK_FALSE (dialog.addToProject.isEnabled());
}

// Run with: weft_ui_tests "[.screenshot-bounce]" (writes WEFT_SCREENSHOT or ./bounce.png).
TEST_CASE ("Screenshot of the Bounce dialog", "[.screenshot-bounce]")
{
    Ui ui;
    sineProject (ui);
    BounceDialog dialog (ui.studio);
    dialog.mp3.setToggleState (true, juce::dontSendNotification);
    dialog.mp3.onClick();
    dialog.artist.setText ("Ian", false);
    dialog.album.setText ("Demos", false);
    dialog.year.setText ("2026", false);
    dialog.setVisible (true);
    const auto image = dialog.createComponentSnapshot (dialog.getLocalBounds());
    const auto* env = std::getenv ("WEFT_SCREENSHOT");
    juce::File out (env != nullptr ? juce::String (env) : juce::File::getCurrentWorkingDirectory().getChildFile ("bounce.png").getFullPathName());
    out.deleteFile();
    juce::FileOutputStream stream (out);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
}
