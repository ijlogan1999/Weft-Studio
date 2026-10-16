// Comments, chat, track locks and version history in the window (M12, spec 04 §9.3-9.6):
// the [UI] halves of AT-COL-20, 24, 27, 28 and 29. Two windows in one process share a
// project on a real weft-relay.

#include "UiFixture.h"

#include "../sync/SyncFixtures.h"

#include "ui/Mixer.h"

#include <set>

using namespace weft;
using namespace weft::ui;
using namespace weft::ui::test;

namespace
{
void pump (std::initializer_list<Ui*> uis, const std::string& what, const std::function<bool()>& done, int seconds = 30)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (seconds);
    for (;;)
    {
        for (auto* u : uis)
        {
            u->studio.tick();
            u->settle();
        }
        if (done())
            return;
        juce::String states;
        for (auto* u : uis)
            states << u->studio.syncStatus() << " (" << u->studio.status << "); ";
        INFO ("waiting for " << what << ": " << states);
        REQUIRE (std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
}

bool converged (Ui& a, Ui& b)
{
    return a.studio.syncStatus() == "Synced" && b.studio.syncStatus() == "Synced"
        && a.studio.session().document().heads() == b.studio.session().document().heads();
}

void waitSynced (Ui& a, Ui& b) { pump ({ &a, &b }, "both synced", [&] { return converged (a, b); }); }

/** A shares its project and B joins it as an Editor. */
void shareWith (Ui& a, Ui& b, const weft::test::RelayProcess& relay)
{
    a.studio.signedInAs (weft::test::signUp (relay, "Ana"));
    std::string error;
    REQUIRE (a.studio.shareProject (&error));
    pump ({ &a }, "the upload", [&] { return a.studio.syncStatus().startsWith ("Synced"); });
    const auto invite = a.studio.withRelay ([&] (collab::RelayApi& api) { return api.createInvite (a.m().info.id, "editor"); });
    REQUIRE (invite);
    b.studio.signedInAs (weft::test::signUp (relay, "Ben"));
    const bool joined = b.studio.joinProject (invite->url, &error);
    INFO (error);
    REQUIRE (joined);
    waitSynced (a, b);
}

void forgetAccounts (Ui& ui)
{
    std::error_code ec;
    std::filesystem::remove (ui.studio.accountFile(), ec);
}

TrackHeader& headerFor (Ui& ui, const Id& trackId)
{
    for (const auto& h : ui.main->tracks().headers())
        if (h->trackId == trackId)
            return *h;
    FAIL ("no header");
    throw;
}

/** Types into a text editor and presses a key in it. */
void type (juce::TextEditor& e, const juce::String& text)
{
    e.setText (text, false);
}
} // namespace

TEST_CASE ("AT-COL-20 [UI] a track locked by A is disabled on B with \"Locked by Ana\"; solo stays; the Owner breaks a lock after confirming",
           "[UI][S][locks][AT-COL-20][AT-COL-22][AT-TRK-12]")
{
    weft::test::RelayProcess relay;
    Ui a, b;
    const auto tracks = a.instrumentTracks (2);
    const auto locked = tracks[0], free = tracks[1];
    const auto onLocked = a.studio.session().createMidiRegion (locked, 0, 2 * bar);
    const auto onFree = a.studio.session().createMidiRegion (free, 4 * bar, 2 * bar);
    REQUIRE (onLocked);
    REQUIRE (onFree);
    a.studio.edited();
    shareWith (a, b, relay);

    // A locks track 1 from its header menu.
    auto& aHeader = headerFor (a, locked);
    CHECK_FALSE (aHeader.lock.isVisible());
    REQUIRE (aHeader.menu().getNumItems() > 0);
    aHeader.pickMenu (TrackHeader::itemLock);
    a.settle();
    CHECK (aHeader.lock.isVisible());
    CHECK (aHeader.lock.getTooltip() == "Locked by you (click to unlock)");
    CHECK (aHeader.mute.isEnabled()); // the holder edits freely
    waitSynced (a, b);

    auto& bHeader = headerFor (b, locked);
    CHECK (bHeader.lock.isVisible());
    CHECK (bHeader.lock.getTooltip() == "Locked by Ana");
    CHECK_FALSE (bHeader.mute.isEnabled());
    CHECK (bHeader.mute.getTooltip() == "Locked by Ana");
    CHECK_FALSE (bHeader.name.isEditableOnDoubleClick());
    CHECK (bHeader.solo.isEnabled());
    CHECK (bHeader.arm.isEnabled());
    CHECK (bHeader.getTooltip() == "Locked by Ana");
    CHECK (headerFor (b, free).mute.isEnabled());
    // Solo works on the locked track (local, D-COL-7).
    bHeader.solo.onClick();
    b.settle();
    CHECK (b.studio.localView().soloed.contains (locked));

    // The channel strip too.
    b.studio.layout.bottom = BottomTab::mixer;
    b.studio.sendChangeMessage();
    b.settle();
    auto* strip = b.main->bottom().mixerView().stripFor (locked);
    REQUIRE (strip != nullptr);
    CHECK_FALSE (strip->volume.isEnabled());
    CHECK_FALSE (strip->mute.isEnabled());
    CHECK_FALSE (strip->pan.isEnabled());
    CHECK_FALSE (strip->input.isEnabled());
    CHECK (strip->volume.getTooltip() == "Locked by Ana");
    CHECK (strip->solo.isEnabled());
    for (const auto& slot : strip->inserts)
        CHECK_FALSE (slot->isEnabled());
    CHECK (b.main->bottom().mixerView().stripFor (free)->volume.isEnabled());

    // Its regions can't be picked up, and nothing can be dragged onto it.
    b.studio.selection.clear();
    b.click (b.centreOf (*onLocked));
    CHECK_FALSE (b.studio.selection.contains (*onLocked));
    CHECK (b.studio.status == "Locked by Ana.");
    const auto from = b.centreOf (*onFree);
    const auto to = juce::Point<float> (from.x, b.timeline().laneBounds (1).getCentreY());
    b.timeline().pointerDown (from, {});
    b.timeline().pointerDrag (to, {});
    b.timeline().pointerUp (to, {});
    b.settle();
    CHECK (b.m().region (*onFree)->trackId == free);
    CHECK (b.studio.status.contains ("Locked by Ana"));
    // Keyboard edits to it are refused too.
    b.studio.selectTrack (locked);
    CHECK_FALSE (b.studio.perform (Cmd::lockTrack));

    // B locks track 2; the Owner (A) breaks it after confirming, and B is told.
    b.studio.selectTrack (free);
    REQUIRE (b.studio.perform (Cmd::lockTrack));
    waitSynced (a, b);
    auto& aFree = headerFor (a, free);
    CHECK_FALSE (aFree.mute.isEnabled());
    bool offered = false;
    const auto menu = aFree.menu();
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        offered = offered || it.getItem().itemID == TrackHeader::itemBreakLock;
    CHECK (offered);
    aFree.pickMenu (TrackHeader::itemBreakLock);
    REQUIRE (a.main->projectQuestionPending());
    CHECK (a.main->projectQuestion() == "Ben has locked \"" + juce::String (a.m().track (free)->name) + "\". Break the lock? They'll be told.");
    a.main->answerProject (1);
    a.settle();
    CHECK (aFree.mute.isEnabled());
    pump ({ &a, &b }, "the notice on B", [&] { return ! b.studio.notices.empty(); });
    CHECK (b.studio.notices.front().kind == commands::Notice::Kind::lockBroken);
    CHECK (b.main->lastAlert() == "Ana broke your lock on \"" + juce::String (b.m().track (free)->name) + "\".");
    forgetAccounts (a);
    forgetAccounts (b);
}

TEST_CASE ("AT-COL-24 [UI] Shift+C pins a comment; B sees the pin and the thread, replies; A resolves; filters; the pin outlives its track",
           "[UI][S][comments][AT-COL-24][AT-COL-25]")
{
    weft::test::RelayProcess relay;
    Ui a, b;
    const auto tracks = a.instrumentTracks (1);
    const auto track2 = tracks[0];
    REQUIRE (a.m().trackOrder.at (1) == track2);
    shareWith (a, b, relay);

    // Shift+C at bar 9 with track 2 selected opens the Comments tab's box there.
    a.studio.locate (8 * bar);
    a.studio.selectTrack (track2);
    a.key ('C', juce::ModifierKeys::shiftModifier);
    REQUIRE (a.studio.layout.sidePanel);
    a.settle();
    auto& aSide = a.main->side();
    CHECK (aSide.tabs().getCurrentTabIndex() == SidePanel::commentsTab);
    auto& composer = aSide.comments();
    REQUIRE (composer.draft.isVisible());
    CHECK (composer.draftPlace.getText() == juce::String::fromUTF8 ("New comment at Bar 9 \xc2\xb7 ") + juce::String (a.m().track (track2)->name));
    type (composer.draft, "Too busy here? @Ben");
    REQUIRE (composer.postDraft());
    a.settle();
    CHECK_FALSE (composer.draft.isVisible());
    REQUIRE (a.m().comments.size() == 1);
    const auto thread = a.m().comments.begin()->first;
    CHECK (a.m().comment (thread)->body == "Too busy here? @" + b.studio.session().user());
    CHECK (a.studio.commentText (*a.m().comment (thread)) == "Too busy here? @Ben");

    // Pins: in the marker lane at bar 9, and on track 2's lane.
    const auto x = static_cast<float> (a.studio.view.tickToX (8.0 * bar));
    CHECK (a.ruler().commentAt ({ x + 4.0f, Ruler::rulerHeight + 8.0f }) != nullptr);
    REQUIRE (a.timeline().pinBounds (*a.m().comment (thread)));
    CHECK (a.timeline().pinBounds (*a.m().comment (thread))->getY() >= a.timeline().laneBounds (1).getY());

    // B: the pin and the thread; the Comments tab shows the unread dot until it's looked at.
    waitSynced (a, b);
    auto& bSide = b.main->side();
    CHECK (bSide.tabs().getTabNames()[SidePanel::commentsTab] == juce::String::fromUTF8 ("Comments \xe2\x80\xa2"));
    CHECK (b.studio.mentionsShown == std::vector<Id> { thread }); // B was mentioned
    CHECK (b.studio.status.startsWith ("Ana mentioned you"));
    const auto pin = *b.timeline().pinBounds (*b.m().comment (thread));
    b.click (pin.getCentre());
    CHECK (b.studio.openThread == thread);
    b.studio.layout.sidePanel = true;
    b.studio.sendChangeMessage();
    b.settle();
    bSide.showTab (SidePanel::commentsTab);
    b.settle();
    CHECK (bSide.tabs().getTabNames()[SidePanel::commentsTab] == "Comments");
    auto& view = bSide.comments().thread;
    REQUIRE (view.isVisible());
    REQUIRE (view.comments().size() == 1);
    CHECK (view.edits.empty()); // not B's to edit
    type (view.box, "Agreed, muting the hats");
    REQUIRE (view.submit());
    b.settle();
    CHECK (view.comments().size() == 2);
    CHECK (view.edits.size() == 1); // B's own reply

    // A resolves from the thread; the filters.
    waitSynced (a, b);
    a.studio.openThread = thread;
    a.studio.sendChangeMessage();
    a.settle();
    auto& aView = aSide.comments().thread;
    REQUIRE (aView.isVisible());
    CHECK (aView.resolve.getButtonText() == "Resolve");
    aView.resolve.onClick();
    a.settle();
    CHECK (a.m().comment (thread)->resolved);
    CHECK (aView.resolve.getButtonText() == "Reopen");
    waitSynced (a, b);
    auto& bPanel = bSide.comments();
    auto shown = [&] (juce::TextButton& filter) {
        filter.onClick();
        b.settle();
        auto ids = bPanel.shownThreads();
        filter.onClick(); // back to all
        b.settle();
        return ids;
    };
    b.studio.commentFilter = commands::CommentFilter::all;
    CHECK (shown (bPanel.open).empty());
    CHECK (shown (bPanel.resolved) == std::vector<Id> { thread });
    CHECK (shown (bPanel.mine) == std::vector<Id> { thread });
    CHECK (shown (bPanel.mentions) == std::vector<Id> { thread });

    // Only the author edits: A's Edit puts the text in the box.
    REQUIRE (aView.edits.size() == 1);
    aView.edits[0]->onClick();
    CHECK (aView.box.getText() == "Too busy here? @Ben");
    type (aView.box, "Too busy here? @Ben (bars 9-12)");
    REQUIRE (aView.submit());
    CHECK (a.m().comment (thread)->editedAt.has_value());

    // Deleting track 2 leaves the pin at bar 9 in the marker lane, and none on a track.
    REQUIRE (a.studio.deleteTrack (track2));
    a.settle();
    CHECK (a.ruler().commentAt ({ x + 4.0f, Ruler::rulerHeight + 8.0f }) != nullptr);
    CHECK_FALSE (a.timeline().pinBounds (*a.m().comment (thread)));
    forgetAccounts (a);
    forgetAccounts (b);
}

TEST_CASE ("AT-COL-27 [UI] chat in the side panel: Enter sends, unread count, \"bar 17\" moves the playhead, 4,000 characters at most",
           "[UI][S][chat][AT-COL-27]")
{
    weft::test::RelayProcess relay;
    Ui a, b;
    shareWith (a, b, relay);
    auto& aChat = a.main->side().chat();
    aChat.input.setText ("The snare at bar 17", false);
    aChat.input.moveCaretToEnd();
    aChat.input.keyPressed (juce::KeyPress (juce::KeyPress::returnKey, juce::ModifierKeys::shiftModifier, 0));
    CHECK (aChat.input.getText() == "The snare at bar 17\n");
    aChat.input.insertTextAtCaret ("is loud");
    aChat.input.keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
    a.settle();
    CHECK (aChat.input.isEmpty());
    REQUIRE (a.m().chat.size() == 1);
    CHECK (a.m().chat[0].body == "The snare at bar 17\nis loud");
    CHECK (a.studio.unreadChat() == 0); // your own message

    waitSynced (a, b);
    auto& bSide = b.main->side();
    CHECK (b.studio.unreadChat() == 1);
    CHECK (bSide.tabs().getTabNames()[SidePanel::chatTab] == "Chat (1)");
    b.studio.layout.sidePanel = true;
    b.studio.sendChangeMessage();
    b.settle();
    bSide.showTab (SidePanel::chatTab);
    b.settle();
    CHECK (b.studio.unreadChat() == 0);
    CHECK (bSide.tabs().getTabNames()[SidePanel::chatTab] == "Chat");

    // "bar 17" is a link: clicking it moves B's playhead to bar 17.
    auto& bChat = bSide.chat();
    const auto links = bChat.links();
    REQUIRE (links.size() == 1);
    CHECK (links[0].second == 16 * bar);
    CHECK (links[0].first.getWidth() > 10.0f);
    CHECK_FALSE (bChat.clickAt (links[0].first.getBottomRight() + juce::Point<float> (40.0f, 40.0f)));
    REQUIRE (bChat.clickAt (links[0].first.getCentre()));
    CHECK (b.studio.playhead() == 16 * bar);

    // B answers; both show the same order.
    bChat.input.setText ("Turned it down 2 dB at 0:30", false);
    REQUIRE (bChat.sendTyped());
    waitSynced (a, b);
    REQUIRE (a.m().chat.size() == 2);
    CHECK (a.m().chat[1].body == b.m().chat[1].body);
    std::set<Ticks> targets; // a link wrapped over two lines has a box on each
    for (const auto& link : a.main->side().chat().links())
        targets.insert (link.second);
    CHECK (targets == std::set<Ticks> { 15 * bar, 16 * bar }); // 0:30 at 120 BPM is bar 16

    // Over 4,000 characters: refused, and the text stays to be shortened.
    aChat.input.setText (juce::String::repeatedString ("x", 4001), false);
    CHECK_FALSE (aChat.sendTyped());
    CHECK (a.studio.status == "Messages can be at most 4,000 characters.");
    CHECK (aChat.input.getText().length() == 4001);
    CHECK (a.m().chat.size() == 2);
    forgetAccounts (a);
    forgetAccounts (b);
}

TEST_CASE ("AT-COL-28 [UI] Save Version, preview it read-only under a banner, restore it, undo the restore", "[UI][versions][AT-COL-28]")
{
    Ui ui;
    auto& s = ui.studio.session();
    const auto tracks = ui.instrumentTracks (1);
    REQUIRE (s.renameTrack (tracks[0], "Vox"));
    const auto region = s.createMidiRegion (tracks[0], 0, 2 * bar);
    REQUIRE (region);
    ui.studio.edited();

    // Ctrl+Alt+S opens History with the name box.
    ui.key ('S', juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::altModifier);
    REQUIRE (ui.studio.layout.sidePanel);
    auto& history = ui.main->side().history();
    CHECK (ui.main->side().tabs().getCurrentTabIndex() == SidePanel::historyTab);
    history.name.setText ("Mix 1", false);
    REQUIRE (history.saveTyped());
    ui.settle();
    REQUIRE (history.entries().size() == 1);
    CHECK (history.entries()[0].label == "Mix 1");

    // Further edits.
    REQUIRE (s.renameTrack (tracks[0], "Lead"));
    REQUIRE (s.deleteRegions ({ *region }));
    ui.studio.edited();
    const auto before = s.document().root();

    // Selecting it previews it: the old state, a banner, and edits refused.
    history.click (0);
    ui.settle();
    REQUIRE (ui.studio.previewing());
    CHECK (ui.main->previewBanner().isVisible());
    CHECK (ui.main->previewBanner().message.getText() == juce::String::fromUTF8 ("Viewing version 'Mix 1' \xe2\x80\x93 read-only."));
    CHECK_FALSE (ui.main->readOnlyBanner().isVisible()); // the preview banner says it
    CHECK (ui.main->side().history().entries()[0].label == "Mix 1"); // History lists the live versions
    CHECK (ui.m().track (tracks[0])->name == "Vox");
    REQUIRE (ui.m().region (*region) != nullptr);
    CHECK (headerFor (ui, tracks[0]).name.getText() == "Vox");
    CHECK (ui.timeline().regionAt (ui.centreOf (*region)) != nullptr); // drawn at that state
    ui.studio.selection.set ({ *region });
    CHECK_FALSE (ui.studio.perform (Cmd::deleteSelection));
    INFO (ui.studio.status);
    CHECK (ui.studio.status.contains ("read-only"));
    CHECK (s.document().root() == before);
    CHECK (s.model().track (tracks[0])->name == "Lead");
    CHECK (ui.studio.sendChat ("Listening back to Mix 1")); // comments and chat stay live

    // Return to current, then restore from the banner.
    ui.main->previewBanner().back.onClick();
    ui.settle();
    CHECK_FALSE (ui.studio.previewing());
    CHECK_FALSE (ui.main->previewBanner().isVisible());
    CHECK (ui.m().track (tracks[0])->name == "Lead");
    history.click (0);
    ui.settle();
    ui.main->previewBanner().restore.onClick();
    ui.settle();
    CHECK_FALSE (ui.studio.previewing());
    CHECK (ui.m().track (tracks[0])->name == "Vox");
    CHECK (ui.m().region (*region) != nullptr);
    const auto entries = history.entries();
    REQUIRE (entries.size() == 2);
    CHECK (entries[0].label == "Restored 'Mix 1'");
    REQUIRE (ui.studio.perform (Cmd::undo));
    CHECK (ui.m().track (tracks[0])->name == "Lead");
    CHECK (ui.m().region (*region) == nullptr);
}

TEST_CASE ("AT-COL-29 [UI] closing a project you changed leaves an automatic version; one you didn't change gets none", "[UI][versions][AT-COL-29]")
{
    Ui ui;
    auto changed = ui.studio.projectRoot();
    REQUIRE (ui.studio.session().renameTrack (ui.m().trackOrder.front(), "Guitar"));
    ui.studio.edited();
    REQUIRE (ui.studio.save());
    REQUIRE (ui.studio.newProject (ui.folder.path / "projects", { "Second" }));
    const auto untouched = ui.studio.projectRoot();
    REQUIRE (ui.studio.save());
    REQUIRE (ui.studio.openProject (changed).ok);
    REQUIRE (ui.m().versions.size() == 1);
    const auto& v = *ui.m().versions.begin()->second;
    CHECK (v.autoVersion);
    CHECK (ui.main->side().history().entries().front().day);
    CHECK (ui.main->side().history().entries().front().count == 1);
    REQUIRE (ui.studio.openProject (untouched).ok);
    CHECK (ui.m().versions.empty());
}

// Run with: weft_ui_tests "[.screenshot-m12]" (writes the PNGs into WEFT_SCREENSHOT_DIR or the working folder).
TEST_CASE ("Screenshots of comments, chat, locks and history (M12)", "[.screenshot-m12]")
{
    weft::test::RelayProcess relay;
    Ui a, b;
    const auto tracks = a.instrumentTracks (3);
    for (int i = 0; i < 3; ++i)
        REQUIRE (a.studio.session().renameTrack (tracks[static_cast<std::size_t> (i)], i == 0 ? "Keys" : i == 1 ? "Bass" : "Lead"));
    const auto r1 = a.studio.session().createMidiRegion (tracks[0], 0, 8 * bar);
    const auto r2 = a.studio.session().createMidiRegion (tracks[1], 4 * bar, 8 * bar);
    for (int i = 0; i < 32; ++i)
    {
        model::Note n;
        n.pos = i * 960;
        n.length = 480;
        n.pitch = 60 + (i * 5) % 12;
        REQUIRE (a.studio.session().addNote (*r1, n));
        n.pitch = 36 + (i * 3) % 7;
        REQUIRE (a.studio.session().addNote (*r2, n));
    }
    REQUIRE (a.studio.session().addMarker (0, "Intro"));
    REQUIRE (a.studio.session().addMarker (4 * bar, "Verse"));
    a.studio.edited();
    shareWith (a, b, relay);

    REQUIRE (b.studio.session().lockTrack (tracks[1], "comping the bass"));
    REQUIRE (b.studio.addComment ({ 8 * bar, {}, tracks[0], {} }, "Can the keys drop out here? @Ana"));
    REQUIRE (a.studio.saveVersion ("Rough mix"));
    REQUIRE (b.studio.sendChat ("Locked Bass while I comp it, back in 10"));
    waitSynced (a, b);
    REQUIRE (a.studio.sendChat ("Sure. The fill at bar 7 beat 3 is lovely"));
    REQUIRE (a.studio.replyToComment (a.m().comments.begin()->first, "Yes, from bar 9 to bar 12"));
    waitSynced (a, b);

    a.studio.layout.sidePanel = true;
    a.studio.openThread = a.m().comments.begin()->first;
    a.studio.sendChangeMessage();
    a.settle();
    a.main->side().showTab (SidePanel::commentsTab);
    a.settle();

    const auto dir = juce::File (juce::SystemStats::getEnvironmentVariable ("WEFT_SCREENSHOT_DIR", juce::File::getCurrentWorkingDirectory().getFullPathName()));
    auto save = [&] (juce::Component& c, const juce::String& name) {
        juce::PNGImageFormat png;
        const auto f = dir.getChildFile (name);
        f.deleteFile();
        juce::FileOutputStream out (f);
        REQUIRE (png.writeImageToStream (c.createComponentSnapshot (c.getLocalBounds()), out));
    };
    save (*a.main, "m12-comments.png");
    a.main->side().showTab (SidePanel::chatTab);
    a.settle();
    save (a.main->side(), "m12-chat.png");
    a.main->side().history().click (0);
    a.main->side().showTab (SidePanel::historyTab);
    a.settle();
    save (*a.main, "m12-history.png");
    a.studio.returnToCurrent();
    forgetAccounts (a);
    forgetAccounts (b);
}
