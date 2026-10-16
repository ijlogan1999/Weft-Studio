// The Share menu (M10, spec 04 §9.2 and §10): sign in, share a project, invite a Viewer
// who joins from the link and sees the read-only banner, roles and removal from the Share
// dialog, the status bar's sync state and Go Offline. Two windows in one process talk to a
// real weft-relay on localhost. Covers the UI half of AT-COL-4 (the read-only banner).

#include "UiFixture.h"

#include "../sync/SyncFixtures.h"

#include "ui/ShareDialogs.h"

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
        INFO ("waiting for " << what);
        REQUIRE (std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
}

void signUpThroughDialog (Ui& ui, const std::string& url, const std::string& email, const std::string& name)
{
    SignInDialog d (ui.studio);
    d.relay.setText (url);
    d.createAccount.setToggleState (true, juce::sendNotificationSync);
    CHECK (d.displayName.isVisible());
    CHECK (d.signIn.getButtonText() == "Create Account");
    d.email.setText (email);
    d.password.setText ("password123");
    d.displayName.setText (name);
    INFO (d.error.getText());
    REQUIRE (d.submit());
    CHECK (ui.studio.signedIn());
    CHECK (ui.studio.account().displayName == name);
}

juce::StringArray menuLabels (MainComponent& main, int index)
{
    juce::StringArray labels;
    const auto menu = main.getMenuForIndex (index, {});
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        if (! it.getItem().isSeparator)
            labels.add (it.getItem().text);
    return labels;
}
} // namespace

TEST_CASE ("AT-COL-4 [UI] share from the Share menu, a Viewer joins from the link and gets the read-only banner", "[UI][S][AT-COL-4]")
{
    weft::test::RelayProcess relay;
    Ui a;
    REQUIRE (a.main->getMenuBarNames().contains ("Share"));
    CHECK (menuLabels (*a.main, 6).contains ("Sign In..."));
    CHECK (a.studio.syncStatus() == "Local only");

    // Wrong password: the dialog says so and stays signed out.
    {
        SignInDialog d (a.studio);
        d.relay.setText (relay.url());
        d.email.setText ("nobody@example.org");
        d.password.setText ("password123");
        CHECK_FALSE (d.submit());
        CHECK (d.error.getText() == "The email or password is wrong.");
        CHECK_FALSE (a.studio.signedIn());
    }
    signUpThroughDialog (a, relay.url(), "ana@example.org", "Ana");
    CHECK (menuLabels (*a.main, 6).contains ("Sign Out (Ana)"));
    REQUIRE (a.studio.session().renameTrack (a.m().trackOrder.front(), "Drums"));
    a.studio.edited();

    // Share: the dialog asks before uploading, then shows invites and members.
    ShareDialog share (a.studio);
    CHECK (share.proceed.isVisible());
    CHECK (share.uploadNote.getText().contains (relay.url()));
    REQUIRE (share.share());
    CHECK (a.studio.shared());
    CHECK_FALSE (share.proceed.isVisible());
    CHECK (share.link.isVisible());
    REQUIRE (share.members.size() == 1);
    CHECK (share.members[0]->name.getText().startsWith ("Ana (you)"));
    CHECK_FALSE (share.members[0]->role.isEnabled());
    pump ({ &a }, "the upload", [&] { return a.studio.syncStatus() == "Synced"; });
    CHECK (menuLabels (*a.main, 6).contains ("Invite..."));
    CHECK (a.studio.syncDetails().contains (relay.url()));

    // A Viewer link.
    share.role.setSelectedId (2, juce::sendNotificationSync);
    REQUIRE (share.makeLink());
    const auto link = share.link.getText().toStdString();
    CHECK (link.find ("/v1/invites/") != std::string::npos);

    // B joins from the link: the project opens read-only with a banner.
    Ui b;
    signUpThroughDialog (b, relay.url(), "ben@example.org", "Ben");
    {
        JoinDialog join (b.studio);
        join.link.setText ("not a link");
        CHECK_FALSE (join.submit());
        CHECK (join.error.getText() == "That isn't a Weft invite link.");
        join.link.setText (link);
        INFO (join.error.getText());
        REQUIRE (join.submit());
    }
    CHECK (b.m().info.id == a.m().info.id);
    pump ({ &a, &b }, "B synced", [&] { return b.studio.syncStatus() == "Synced"; });
    CHECK (b.m().track (a.m().trackOrder.front())->name == "Drums");
    CHECK (b.main->readOnlyBannerShown());
    CHECK (b.main->readOnlyBanner().message.getText().contains ("Viewer"));
    CHECK_FALSE (b.studio.check (b.studio.session().renameTrack (b.m().trackOrder.front(), "Mine")));
    CHECK_FALSE (a.main->readOnlyBannerShown());

    // A's edits reach B.
    REQUIRE (a.studio.session().renameTrack (a.m().trackOrder.front(), "Drums 2"));
    a.studio.edited();
    pump ({ &a, &b }, "the rename on B", [&] { return b.m().track (a.m().trackOrder.front())->name == "Drums 2"; });

    // The Owner makes B an Editor: the banner goes and B can edit.
    REQUIRE (share.refreshMembers());
    REQUIRE (share.members.size() == 2);
    auto bRow = std::find_if (share.members.begin(), share.members.end(), [] (const auto& r) { return r->member.displayName == "Ben"; });
    REQUIRE (bRow != share.members.end());
    CHECK ((*bRow)->role.isEnabled());
    CHECK ((*bRow)->remove.isVisible());
    (*bRow)->role.setSelectedId (2, juce::sendNotificationSync); // Editor
    pump ({ &a, &b }, "B's new role", [&] { return ! b.main->readOnlyBannerShown(); });
    REQUIRE (b.studio.check (b.studio.session().renameTrack (b.m().trackOrder.front(), "Ben's")));
    b.studio.edited();
    pump ({ &a, &b }, "B's rename on A", [&] { return a.m().track (a.m().trackOrder.front())->name == "Ben's"; });

    // Go Offline: edits count as pending and are sent on the way back.
    a.main->run (Cmd::goOffline);
    CHECK_FALSE (a.studio.online());
    pump ({ &a }, "offline", [&] { return a.studio.syncStatus().startsWith ("Offline"); });
    REQUIRE (a.studio.session().renameTrack (a.m().trackOrder.front(), "Offline edit"));
    a.studio.edited();
    a.studio.tick();
    CHECK (a.studio.syncStatus() == juce::String::fromUTF8 ("Offline \xe2\x80\x93 1 change pending"));
    a.main->run (Cmd::goOffline);
    pump ({ &a, &b }, "back online", [&] {
        return a.studio.syncStatus() == "Synced" && b.m().track (a.m().trackOrder.front())->name == "Offline edit";
    });

    // The Owner removes B: B's project turns read-only with the removal banner.
    REQUIRE (share.refreshMembers());
    bRow = std::find_if (share.members.begin(), share.members.end(), [] (const auto& r) { return r->member.displayName == "Ben"; });
    REQUIRE (bRow != share.members.end());
    REQUIRE (share.removeMember ((*bRow)->member.userId));
    pump ({ &a, &b }, "B removed", [&] { return b.main->readOnlyBannerShown(); });
    CHECK (b.main->readOnlyBanner().message.getText().contains ("removed"));
    CHECK (b.studio.syncStatus() == "Removed from this project");
    REQUIRE (share.refreshMembers());
    CHECK (share.members.size() == 1);

    // Sign out: the project keeps working locally.
    a.main->run (Cmd::signOut);
    CHECK_FALSE (a.studio.signedIn());
    CHECK (a.studio.syncStatus() == juce::String::fromUTF8 ("Signed out \xe2\x80\x93 sign in to sync"));
    CHECK (a.studio.check (a.studio.session().renameTrack (a.m().trackOrder.front(), "Local")));
    // Account files are per machine; the next tests start signed out.
    std::error_code ec;
    std::filesystem::remove (a.studio.accountFile(), ec);
}
