// [S] comments, markers, chat, track locks and version history (spec 05 §7, §9, M12),
// and per-user undo across clients (05 §6), through a real weft-relay. The drawing and
// panels are in tests/ui/CollabPanelTests.cpp.

#include "SyncFixtures.h"

#include "weft/commands/Collab.h"
#include "weft/commands/LocalView.h"

#include <atomic>

using namespace weft;
using namespace weft::test;
using commands::CommentFilter;
using commands::Notice;

namespace
{
struct FakeClock
{
    std::shared_ptr<std::atomic<std::int64_t>> now = std::make_shared<std::atomic<std::int64_t>> (1'800'000'000'000);
    std::function<std::int64_t()> clock() const
    {
        return [n = now] { return n->load(); };
    }
    void advance (std::int64_t ms) const { *now += ms; }
};

constexpr std::int64_t hourMs = 60LL * 60 * 1000;

/** The document without collab metadata: what a restore makes equal (D-COL-36). */
doc::Value content (const Session& s)
{
    auto root = s.document().root();
    root.mapRef().erase (model::keys::collab);
    return root;
}

bool hasNotice (const std::vector<Notice>& notices, Notice::Kind kind, const UserId& by, const Id& track)
{
    return std::any_of (notices.begin(), notices.end(), [&] (const Notice& n) { return n.kind == kind && n.by == by && n.trackId == track; });
}

std::vector<std::string> chatBodies (const Peer& p)
{
    std::vector<std::string> out;
    for (const auto& m : p.m().chat)
        out.push_back (m.body);
    return out;
}
} // namespace

TEST_CASE ("AT-COL-19 locking is off by default: two clients edit the same track concurrently with no prompts", "[S][sync][locks][AT-COL-19]")
{
    RelayProcess relay;
    SharedProject project (relay);
    auto b = project.join ("Ben");
    auto& a = *project.owner;
    const auto track = a.s().createTracks (model::TrackKind::instrument, 1).at (0);
    waitConverged ({ &a, b.get() });
    CHECK_FALSE (a.m().track (track)->locked);
    CHECK_FALSE (b->s().lockedByOther (track));

    std::optional<Id> regionA, regionB;
    concurrently (
        a, *b,
        [&] (Peer& p) {
            REQUIRE (p.s().renameTrack (track, "Lead vocal"));
            regionA = p.s().createMidiRegion (track, 0, barTicks);
        },
        [&] (Peer& p) {
            REQUIRE (p.s().setTrackVolume (track, -4.0));
            regionB = p.s().createMidiRegion (track, 2 * barTicks, barTicks);
        });
    REQUIRE (regionA);
    REQUIRE (regionB);
    for (auto* p : { &a, b.get() })
    {
        CHECK (p->m().track (track)->name == "Lead vocal");
        CHECK (p->m().track (track)->strip.volumeDb == -4.0);
        CHECK (p->m().region (*regionA) != nullptr);
        CHECK (p->m().region (*regionB) != nullptr);
        CHECK (p->s().takeNotices().empty());
    }
}

TEST_CASE ("AT-COL-20 a track locked by A refuses B's edits to its fields, mute, strip, regions, automation and takes; solo stays local",
           "[S][sync][locks][AT-COL-20][AT-TRK-12]")
{
    RelayProcess relay;
    SharedProject project (relay);
    auto b = project.join ("Ben");
    auto& a = *project.owner;
    const auto tracks = a.s().createTracks (model::TrackKind::instrument, 2);
    const auto locked = tracks[0], free = tracks[1];
    waitConverged ({ &a, b.get() });
    const auto onLocked = a.s().createMidiRegion (locked, 0, barTicks);
    const auto onFree = b->s().createMidiRegion (free, 0, barTicks);
    REQUIRE (onLocked);
    REQUIRE (onFree);
    const auto note = a.s().addNote (*onLocked, { "", 0, 480, 60, 100 });
    REQUIRE (note);
    waitConverged ({ &a, b.get() });

    REQUIRE (a.s().lockTrack (locked));
    waitConverged ({ &a, b.get() });
    auto& s = b->s();
    REQUIRE (s.lockedByOther (locked));
    CHECK (s.lockText (locked) == "Locked by Owner");
    CHECK (a.s().lockText (locked) == "Locked by you");
    CHECK_FALSE (a.s().lockedByOther (locked));

    const auto before = b->s().document().heads();
    CHECK_FALSE (s.renameTrack (locked, "Mine now"));
    CHECK (s.lastError() == "\"" + a.m().track (locked)->name + "\" is Locked by Owner.");
    CHECK_FALSE (s.setTrackMute (locked, true));
    CHECK_FALSE (s.setTrackVolume (locked, -10.0));
    CHECK_FALSE (s.setTrackPan (locked, 0.5));
    CHECK_FALSE (s.setTrackColor (locked, { 5, {} }));
    model::PluginInstance gain;
    gain.pluginUid = "weft.gain";
    gain.name = "Gain";
    CHECK_FALSE (s.addInsert (locked, gain));
    CHECK_FALSE (s.addSend (locked, a.m().info.masterTrackId));
    CHECK_FALSE (s.createMidiRegion (locked, 4 * barTicks, barTicks));
    CHECK_FALSE (s.moveRegion (*onLocked, 2 * barTicks));
    CHECK_FALSE (s.setRegionMuted (*onLocked, true));
    CHECK_FALSE (s.deleteRegions ({ *onLocked }));
    CHECK_FALSE (s.addNote (*onLocked, { "", 960, 480, 64, 90 }));
    CHECK_FALSE (s.deleteNotes (*onLocked, { *note }));
    CHECK_FALSE (s.moveRegions ({ *onFree }, 0, -1)); // dragging a region onto the locked track
    CHECK_FALSE (s.moveRegion (*onFree, 0, locked));
    CHECK_FALSE (s.pasteRegions (commands::RegionClipboard::of (s.model(), { *onFree }), 8 * barTicks, locked).size() > 0);
    CHECK_FALSE (s.addAutomationPoint (locked, {}, 0, 0.5));
    CHECK_FALSE (s.deleteTrack (locked));
    CHECK_FALSE (s.lockTrack (locked));
    CHECK_FALSE (s.unlockTrack (locked));
    CHECK_FALSE (s.breakLock (locked)); // only the Owner may
    CHECK (b->s().document().heads() == before);

    // Take folders on the locked track.
    const auto folderId = a.s().newId();
    REQUIRE (a.s().edit ("Make Take Folder", [&] (commands::TxnWriter& w) {
        model::TakeFolder f;
        f.id = folderId;
        f.trackId = locked;
        f.takeIds = { *onLocked };
        f.activeTakeId = *onLocked;
        return commands::EditResult { w.put (doc::path ({ model::keys::takeFolders, folderId }), f.toValue()), "" };
    }));
    waitConverged ({ &a, b.get() });
    CHECK_FALSE (s.setActiveTake (folderId, *onLocked));
    CHECK_FALSE (s.edit ("Rename Folder", [&] (commands::TxnWriter& w) {
        return commands::EditResult { w.put (doc::path ({ model::keys::takeFolders, folderId, "name" }), doc::Value ("x")), "" };
    }));

    // Solo is local state, so it stays available (D-COL-7); other tracks are editable.
    commands::LocalView view;
    view.toggleSolo (locked);
    CHECK (view.soloed.contains (locked));
    CHECK (s.renameTrack (free, "Keys"));
    CHECK (s.moveRegion (*onFree, barTicks));
    CHECK (s.addInsert (free, gain));
    CHECK (s.addAutomationPoint (free, {}, 0, 0.5));
    CHECK (s.addNote (*onFree, { "", 960, 480, 64, 90 }));

    // The holder edits freely, and unlocking gives the track back.
    CHECK (a.s().renameTrack (locked, "Drums"));
    REQUIRE (a.s().unlockTrack (locked));
    waitConverged ({ &a, b.get() });
    CHECK_FALSE (s.lockedByOther (locked));
    CHECK (s.setTrackMute (locked, true));
}

TEST_CASE ("AT-COL-21 B's offline edits to a track made before seeing A's lock merge on reconnect, and A gets a notice", "[S][sync][locks][AT-COL-21]")
{
    RelayProcess relay;
    SharedProject project (relay);
    auto b = project.join ("Ben");
    auto& a = *project.owner;
    const auto track = a.s().createTracks (model::TrackKind::instrument, 1).at (0);
    const auto region = a.s().createMidiRegion (track, 0, barTicks);
    REQUIRE (region);
    waitConverged ({ &a, b.get() });

    b->client->setOnline (false);
    REQUIRE (b->s().setRegionName (*region, "Ben's take"));
    REQUIRE (b->s().setTrackVolume (track, -3.0));
    REQUIRE (a.s().lockTrack (track));
    pumpUntil ({ &a }, "the lock sent", [&] { return a.synced(); });
    CHECK (a.s().takeNotices().empty());

    b->client->setOnline (true);
    waitConverged ({ &a, b.get() });
    CHECK (a.m().region (*region)->name == "Ben's take");
    CHECK (a.m().track (track)->strip.volumeDb == -3.0);
    const auto notices = a.s().takeNotices();
    REQUIRE (hasNotice (notices, Notice::Kind::offlineEditsMerged, b->account.userId, track));
    CHECK (notices.front().text == "Ben's offline edits to \"" + a.m().track (track)->name + "\" were merged.");
    // Now that B has the lock, B's client refuses further edits.
    CHECK_FALSE (b->s().setTrackVolume (track, -6.0));
}

TEST_CASE ("AT-COL-22 the Owner breaks a lock after confirmation and the holder is told; locks expire 24 h after the holder's last presence",
           "[S][sync][locks][AT-COL-22]")
{
    RelayProcess relay;
    FakeClock clock;
    SharedProject project (relay, 0, clock.clock());
    auto ana = project.join ("Ana", "editor", 0, {}, clock.clock());
    auto ben = project.join ("Ben", "editor", 0, {}, clock.clock());
    auto& owner = *project.owner;
    const auto track = owner.m().trackOrder.front();
    waitConverged ({ &owner, ana.get(), ben.get() });

    REQUIRE (ana->s().lockTrack (track, "comping"));
    waitConverged ({ &owner, ana.get(), ben.get() });
    CHECK (owner.m().track (track)->locked->note == "comping");
    CHECK_FALSE (ben->s().breakLock (track));
    CHECK (ben->s().lastError() == "Only the project's Owner can break a lock.");
    REQUIRE (owner.s().isOwner());
    REQUIRE (owner.s().breakLock (track));
    waitConverged ({ &owner, ana.get(), ben.get() });
    CHECK_FALSE (ben->s().lockedByOther (track));
    const auto notices = ana->s().takeNotices();
    REQUIRE (hasNotice (notices, Notice::Kind::lockBroken, owner.account.userId, track));
    CHECK (notices.front().text == "Owner broke your lock on \"" + owner.m().track (track)->name + "\".");

    // Ana locks again and leaves; the lock holds for 24 h after she was last seen.
    REQUIRE (ana->s().lockTrack (track));
    waitConverged ({ &owner, ana.get(), ben.get() });
    clock.advance (2 * hourMs); // she's still here: her presence keeps the lock alive
    pumpUntil ({ &owner, ana.get(), ben.get() }, "a poll", [] { return true; });
    const auto anaLeft = clock.now->load();
    ana.reset();
    pumpUntil ({ &owner, ben.get() }, "Ana gone", [&] { return owner.client->collaborators().size() == 1; }, 45);
    clock.advance (anaLeft + 24 * hourMs - 60'000 - clock.now->load());
    CHECK (ben->s().lockedByOther (track));
    CHECK_FALSE (ben->s().renameTrack (track, "Ben's now"));
    clock.advance (61'000);
    CHECK_FALSE (ben->s().lockedByOther (track));
    CHECK (ben->s().lockOf (track) == std::nullopt);
    CHECK (ben->s().renameTrack (track, "Ben's now"));
    REQUIRE (ben->s().lockTrack (track)); // an expired lock can be taken over
    waitConverged ({ &owner, ben.get() });
    CHECK (owner.m().track (track)->locked->by == ben->account.userId);
}

TEST_CASE ("AT-COL-24 comments: pin at bar 9 on track 2, reply, resolve, filters, and the pin outlives its track", "[S][sync][comments][AT-COL-24]")
{
    RelayProcess relay;
    SharedProject project (relay);
    auto b = project.join ("Ben");
    auto& a = *project.owner;
    const auto tracks = a.s().createTracks (model::TrackKind::audio, 1);
    const auto track2 = tracks[0];
    REQUIRE (a.m().trackOrder.at (1) == track2);

    const auto thread = a.s().addComment ({ 8 * barTicks, {}, track2, {} }, "Too busy here?");
    REQUIRE (thread);
    const auto other = a.s().addComment ({ 2 * barTicks, {}, {}, {} }, "Intro is great");
    REQUIRE (other);
    waitConverged ({ &a, b.get() });
    const auto* seen = b->m().comment (*thread);
    REQUIRE (seen != nullptr);
    CHECK (seen->pos == 8 * barTicks);
    CHECK (seen->trackId == track2);
    CHECK (seen->author == a.account.userId);
    CHECK (commands::commentTrack (b->m(), *seen) == track2);
    // Sorted by timeline position.
    const auto all = commands::commentThreads (b->m(), CommentFilter::all, b->account.userId);
    REQUIRE (all.size() == 2);
    CHECK (all[0]->id == *other);
    CHECK (all[1]->id == *thread);

    const auto reply = b->s().replyToComment (*thread, "A little. @" + a.account.userId + " try muting the hats");
    REQUIRE (reply);
    waitConverged ({ &a, b.get() });
    const auto replies = commands::commentReplies (a.m(), *thread);
    REQUIRE (replies.size() == 1);
    CHECK (replies[0]->author == b->account.userId);
    CHECK (replies[0]->pos == 8 * barTicks);
    // A reply to a reply joins the same thread (one level deep).
    const auto nested = a.s().replyToComment (*reply, "Done");
    REQUIRE (nested);
    CHECK (a.m().comment (*nested)->parentId == *thread);

    REQUIRE (a.s().setCommentResolved (*thread, true));
    waitConverged ({ &a, b.get() });
    CHECK (b->m().comment (*thread)->resolved);
    CHECK (b->m().comment (*thread)->resolvedBy == a.account.userId);

    auto ids = [&] (CommentFilter f, const Peer& p) {
        std::vector<Id> out;
        for (const auto* c : commands::commentThreads (p.m(), f, p.account.userId))
            out.push_back (c->id);
        return out;
    };
    CHECK (ids (CommentFilter::open, *b) == std::vector<Id> { *other });
    CHECK (ids (CommentFilter::resolved, *b) == std::vector<Id> { *thread });
    CHECK (ids (CommentFilter::mine, *b) == std::vector<Id> { *thread }); // B replied there
    CHECK (ids (CommentFilter::mine, a) == std::vector<Id> { *other, *thread });
    CHECK (ids (CommentFilter::mentions, a) == std::vector<Id> { *thread });
    CHECK (ids (CommentFilter::mentions, *b).empty());

    // Deleting track 2 leaves the comment at bar 9 on the timeline.
    REQUIRE (b->s().deleteTrack (track2));
    waitConverged ({ &a, b.get() });
    const auto* orphan = a.m().comment (*thread);
    REQUIRE (orphan != nullptr);
    CHECK (orphan->pos == 8 * barTicks);
    CHECK (commands::commentTrack (a.m(), *orphan).empty());
    CHECK (commands::commentThreads (a.m(), CommentFilter::all, a.account.userId).size() == 2);
}

TEST_CASE ("AT-COL-25 a mention notifies B on next connect; only the author edits their comment", "[S][sync][comments][AT-COL-25]")
{
    RelayProcess relay;
    SharedProject project (relay);
    auto b = project.join ("Ben");
    auto& a = *project.owner;
    const auto lastSeen = b->s().now() - 1; // B's last-seen marker (local view state)
    b->client->setOnline (false);

    const std::map<UserId, std::string> names { { b->account.userId, "Ben" }, { a.account.userId, "Owner" } };
    const auto typed = "@Ben can you redo the bridge vocal?";
    const auto body = commands::mentionIds (typed, names);
    CHECK (body == "@" + b->account.userId + " can you redo the bridge vocal?");
    CHECK (commands::mentionNames (body, names) == typed);
    const auto c = a.s().addComment ({ 16 * barTicks, {}, {}, {} }, body);
    REQUIRE (c);
    CHECK (commands::newMentions (a.m(), b->account.userId, lastSeen).size() == 1);
    CHECK (commands::newMentions (a.m(), a.account.userId, lastSeen).empty());
    CHECK (commands::newMentions (b->m(), b->account.userId, lastSeen).empty()); // not here yet
    pumpUntil ({ &a }, "the comment sent", [&] { return a.synced(); });

    b->client->setOnline (true);
    waitConverged ({ &a, b.get() });
    const auto mentions = commands::newMentions (b->m(), b->account.userId, lastSeen);
    REQUIRE (mentions.size() == 1);
    CHECK (mentions[0]->id == *c);
    CHECK (commands::newMentions (b->m(), b->account.userId, b->s().now()).empty()); // once seen, not again

    // Only the author edits or deletes it; anyone may resolve.
    CHECK_FALSE (b->s().editComment (*c, "Never mind"));
    CHECK (b->s().lastError() == "Only Owner can edit this comment.");
    CHECK_FALSE (b->s().deleteComment (*c));
    REQUIRE (a.s().editComment (*c, body + " (the second one)"));
    waitConverged ({ &a, b.get() });
    CHECK (b->m().comment (*c)->body == body + " (the second one)");
    CHECK (b->m().comment (*c)->editedAt.has_value());
    CHECK (b->s().setCommentResolved (*c, true));
    REQUIRE (a.s().deleteComment (*c));
    waitConverged ({ &a, b.get() });
    CHECK (b->m().comment (*c)->deleted);
    CHECK (b->m().comment (*c)->body.empty());
    CHECK (commands::commentThreads (b->m(), CommentFilter::all, b->account.userId).empty());

    // Too long, or empty.
    CHECK_FALSE (a.s().addComment ({ 0, {}, {}, {} }, std::string (4001, 'x')));
    CHECK (a.s().addComment ({ 0, {}, {}, {} }, std::string (4000, 'x')));
    CHECK_FALSE (a.s().addComment ({ 0, {}, {}, {} }, ""));
}

TEST_CASE ("AT-COL-26 markers created, renamed, moved and deleted on A appear on B; concurrent renames converge", "[S][sync][markers][AT-COL-26]")
{
    RelayProcess relay;
    SharedProject project (relay);
    auto b = project.join ("Ben");
    auto& a = *project.owner;
    const auto verse = a.s().addMarker (4 * barTicks, "Verse");
    const auto chorus = a.s().addMarker (12 * barTicks, "Chorus");
    REQUIRE (verse);
    REQUIRE (chorus);
    waitConverged ({ &a, b.get() });
    REQUIRE (b->m().markers.contains (*verse));
    CHECK (b->m().markers.at (*verse)->name == "Verse");
    CHECK (b->m().markers.at (*verse)->createdBy == a.account.userId);

    REQUIRE (a.s().renameMarker (*verse, "Verse 1"));
    REQUIRE (a.s().moveMarker (*verse, 5 * barTicks));
    REQUIRE (a.s().deleteMarker (*chorus));
    waitConverged ({ &a, b.get() });
    CHECK (b->m().markers.at (*verse)->name == "Verse 1");
    CHECK (b->m().markers.at (*verse)->pos == 5 * barTicks);
    CHECK_FALSE (b->m().markers.contains (*chorus));

    concurrently (
        a, *b, [&] (Peer& p) { REQUIRE (p.s().renameMarker (*verse, "First verse")); }, [&] (Peer& p) { REQUIRE (p.s().renameMarker (*verse, "V1")); });
    const auto name = a.m().markers.at (*verse)->name;
    CHECK ((name == "First verse" || name == "V1"));
    CHECK (b->m().markers.at (*verse)->name == name);
}

TEST_CASE ("AT-COL-27 chat: one order on every client, offline messages after reconnect, bar links, a 4,000 character limit", "[S][sync][chat][AT-COL-27]")
{
    RelayProcess relay;
    SharedProject project (relay);
    auto b = project.join ("Ben", "viewer"); // Viewers chat too (D-COL-2)
    auto& a = *project.owner;

    REQUIRE (a.s().sendChat ("Hi Ben"));
    waitConverged ({ &a, b.get() });
    REQUIRE (b->s().sendChat ("Hi! The snare at bar 17 is loud"));
    waitConverged ({ &a, b.get() });
    concurrently (
        a, *b, [&] (Peer& p) { REQUIRE (p.s().sendChat ("Offline from A")); }, [&] (Peer& p) { REQUIRE (p.s().sendChat ("Offline from B")); });
    CHECK (chatBodies (a) == chatBodies (*b));
    REQUIRE (chatBodies (a).size() == 4);
    CHECK (chatBodies (a)[0] == "Hi Ben");
    CHECK (chatBodies (a)[1] == "Hi! The snare at bar 17 is loud");
    CHECK (b->m().chat[0].author == a.account.userId);
    CHECK (commands::unreadChat (b->m(), b->account.userId, {}) == 2);
    CHECK (commands::unreadChat (b->m(), b->account.userId, b->m().chat.back().id) == 0);

    // "bar 17" is a link to bar 17.
    const auto& text = a.m().chat[1].body;
    const auto links = commands::timeLinks (text, a.m());
    REQUIRE (links.size() == 1);
    CHECK (text.substr (links[0].start, links[0].length) == "bar 17");
    CHECK (links[0].pos == 16 * barTicks);
    const auto more = commands::timeLinks ("Bar 3 beat 2, then 1:30, not bar0 or 1:75 or barn 4", a.m());
    REQUIRE (more.size() == 2);
    CHECK (more[0].pos == 2 * barTicks + 960);
    CHECK (more[0].length == std::string ("Bar 3 beat 2").size());
    CHECK (more[1].pos == 180 * 960); // 90 s at 120 BPM

    // Too long; and only the author deletes their message.
    CHECK_FALSE (a.s().sendChat (std::string (4001, 'x')));
    CHECK (a.s().lastError() == "Messages can be at most 4,000 characters.");
    CHECK (a.s().sendChat (std::string (4000, 'x')));
    CHECK_FALSE (a.s().deleteChat (b->m().chat[1].id));
    REQUIRE (b->s().deleteChat (b->m().chat[1].id));
    waitConverged ({ &a, b.get() });
    CHECK (a.m().chat[1].deleted);
    CHECK (a.m().chat[1].body.empty());
    CHECK (a.m().chat.size() == 5);
}

TEST_CASE ("AT-COL-28 Save Version, preview it read-only, restore it everywhere as an undoable change", "[S][sync][versions][AT-COL-28]")
{
    RelayProcess relay;
    SharedProject project (relay);
    auto b = project.join ("Ben");
    auto& a = *project.owner;
    const auto track = a.s().createTracks (model::TrackKind::instrument, 1).at (0);
    REQUIRE (a.s().renameTrack (track, "Vox"));
    const auto region = a.s().createMidiRegion (track, 0, barTicks);
    REQUIRE (region);
    REQUIRE (a.s().addMarker (barTicks, "Start"));
    REQUIRE (a.s().sendChat ("Saving Mix 1"));
    const auto mix1 = a.s().saveVersion ("Mix 1");
    REQUIRE (mix1);
    const auto atMix1 = content (a.s());
    waitConverged ({ &a, b.get() });
    const auto* tag = b->m().version (*mix1);
    REQUIRE (tag != nullptr);
    CHECK (tag->name == "Mix 1");
    CHECK (tag->createdBy == a.account.userId);
    CHECK_FALSE (tag->autoVersion);
    CHECK (tag->heads.size() == 1);

    // More edits from both.
    REQUIRE (b->s().renameTrack (track, "Lead"));
    REQUIRE (b->s().deleteRegions ({ *region }));
    const auto added = b->s().createTracks (model::TrackKind::instrument, 2);
    REQUIRE (a.s().setTrackVolume (track, -8.0));
    REQUIRE (a.s().addComment ({ 0, {}, {}, {} }, "Keep this"));
    waitConverged ({ &a, b.get() });
    const auto beforeRestore = content (a.s());

    // Preview: the old state, read-only, and the current project untouched.
    const auto preview = b->s().versionModel (*mix1);
    REQUIRE (preview);
    CHECK (preview->track (track)->name == "Vox");
    CHECK (preview->region (*region) != nullptr);
    CHECK (preview->trackOrder.size() == 2);
    CHECK (b->m().trackOrder.size() == 4);
    CHECK (b->m().track (track)->name == "Lead");

    // Restore on B: the content equals Mix 1 on both clients; comments, chat and versions stay.
    REQUIRE (b->s().restoreVersion (*mix1));
    waitConverged ({ &a, b.get() });
    CHECK (content (a.s()) == atMix1);
    CHECK (content (b->s()) == atMix1);
    CHECK (a.m().track (track)->name == "Vox");
    CHECK (a.m().region (*region) != nullptr);
    CHECK (a.m().track (added[0]) == nullptr);
    CHECK (a.m().comments.size() == 1);
    CHECK (a.m().chat.size() == 1);
    REQUIRE (a.m().versions.size() == 2);
    bool restoredTag = false;
    for (const auto& [id, v] : a.m().versions)
        restoredTag = restoredTag || (v->name == "Restored 'Mix 1'" && v->createdBy == b->account.userId);
    CHECK (restoredTag);
    CHECK (b->s().undoName() == "Restore 'Mix 1'");

    // Undo of the restore returns to the pre-restore state.
    REQUIRE (b->s().undo().done);
    waitConverged ({ &a, b.get() });
    CHECK (content (a.s()) == beforeRestore);
    CHECK (content (b->s()) == beforeRestore);

    // Viewers can't restore.
    auto viewer = project.join ("Vi", "viewer");
    CHECK_FALSE (viewer->s().restoreVersion (*mix1));
    CHECK_FALSE (viewer->s().saveVersion ("Mine"));
}

TEST_CASE ("AT-COL-29 an automatic version when a user with changes ends their session, none without changes", "[S][sync][versions][AT-COL-29]")
{
    RelayProcess relay;
    SharedProject project (relay);
    auto b = project.join ("Ben");
    auto c = project.join ("Cy");
    auto& a = *project.owner;
    waitConverged ({ &a, b.get(), c.get() });

    REQUIRE (b->s().renameTrack (a.m().trackOrder.front(), "Bass"));
    REQUIRE (c->s().sendChat ("Just listening")); // chat isn't a change to the project
    const auto auto1 = b->s().autoVersion();
    REQUIRE (auto1);
    CHECK_FALSE (c->s().autoVersion());
    CHECK_FALSE (b->s().autoVersion()); // nothing new since
    waitConverged ({ &a, b.get(), c.get() });
    REQUIRE (a.m().versions.size() == 1);
    const auto* v = a.m().version (*auto1);
    REQUIRE (v != nullptr);
    CHECK (v->autoVersion);
    CHECK (v->createdBy == b->account.userId);
    CHECK (v->name == "Ben's session");
}

TEST_CASE ("AT-COL-30 per-user undo through the relay: only what is still mine", "[S][sync][undo][AT-COL-30]")
{
    RelayProcess relay;
    SharedProject project (relay);
    auto b = project.join ("Ben");
    auto& a = *project.owner;
    const auto track = a.m().trackOrder.front();
    model::MediaRef media;
    media.hash = "b3:" + std::string (64, '1');
    media.originalName = "loop.wav";
    media.lengthSamples = 48000 * 8;
    REQUIRE (a.s().addMedia (media));
    const auto r = a.s().createAudioRegion (track, 0, media.hash, 0, 48000 * 2);
    REQUIRE (r);
    waitConverged ({ &a, b.get() });
    constexpr Ticks bar1 = 0, bar3 = 2 * barTicks, bar5 = 4 * barTicks;

    SECTION ("A moves R to bar 3, B changes R's gain, A undoes: R returns to bar 1 and B's gain stays")
    {
        REQUIRE (a.s().moveRegion (*r, bar3));
        waitConverged ({ &a, b.get() });
        REQUIRE (b->s().setRegionGain (*r, -9.0));
        waitConverged ({ &a, b.get() });
        REQUIRE (a.s().undo().done);
        waitConverged ({ &a, b.get() });
        for (auto* p : { &a, b.get() })
        {
            CHECK (p->m().region (*r)->start == bar1);
            CHECK (p->m().region (*r)->audio.gainDb == -9.0);
        }
    }

    SECTION ("A moves R to bar 3, B moves R to bar 5, A undoes: R stays at bar 5")
    {
        REQUIRE (a.s().moveRegion (*r, bar3));
        waitConverged ({ &a, b.get() });
        REQUIRE (b->s().moveRegion (*r, bar5));
        waitConverged ({ &a, b.get() });
        a.s().undo();
        waitConverged ({ &a, b.get() });
        CHECK (a.m().region (*r)->start == bar5);
        CHECK (b->m().region (*r)->start == bar5);
    }

    SECTION ("A edits R, B deletes R, A undoes: nothing happens and the status bar says why")
    {
        REQUIRE (a.s().setRegionGain (*r, -4.0));
        waitConverged ({ &a, b.get() });
        REQUIRE (b->s().deleteRegions ({ *r }));
        waitConverged ({ &a, b.get() });
        const auto heads = a.s().document().heads();
        const auto result = a.s().undo();
        CHECK_FALSE (result.done);
        CHECK (result.message.ends_with ("a region was deleted by someone else."));
        CHECK (a.s().document().heads() == heads);
        CHECK (a.m().region (*r) == nullptr);
    }
}
