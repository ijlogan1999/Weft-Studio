// [U] collaboration metadata in the document (spec 03 §9): locks (AT-COL-23), comments,
// chat and version tags as values, version previews, and the read-side helpers.

#include "Fixtures.h"
#include "weft/commands/Collab.h"
#include "weft/project/ProjectFolder.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <random>

using namespace weft;
using namespace weft::test;
using doc::Value;
namespace fs = std::filesystem;

namespace
{
struct TempDir
{
    fs::path path;
    TempDir()
    {
        std::random_device rd;
        path = fs::temp_directory_path() / ("weft-test-" + std::to_string (rd()) + std::to_string (rd()));
        fs::create_directories (path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all (path, ec);
    }
};
} // namespace

TEST_CASE ("AT-COL-23 lock state round-trips through save and load and syncs as a document field", "[U][locks][AT-COL-23][AT-TRK-12]")
{
    TempDir tmp;
    project::ProjectFolder folder (project::ProjectFolder::folderFor (tmp.path, "Locks"));
    REQUIRE (folder.create());
    Scene scene;
    auto& a = scene.session;
    const auto track = scene.audioTrack;
    REQUIRE (a.lockTrack (track, "editing the comp"));
    // Not an undo step (D-COL-35).
    CHECK (a.undoName() != "Lock Track");
    const auto lock = a.model().track (track)->locked;
    REQUIRE (lock);
    CHECK (lock->by == a.user());
    CHECK (lock->at == fixedClock());
    CHECK (lock->note == "editing the comp");
    CHECK (a.document().get (doc::path ({ model::keys::tracks, track, "locked", "by" })) == Value (a.user()));

    REQUIRE (folder.save (a.document()));
    auto opened = folder.open();
    REQUIRE (opened.document);
    Session reopened (std::move (*opened.document), "userB", fixedClock, 7);
    REQUIRE (reopened.model().track (track)->locked);
    CHECK (reopened.model().track (track)->locked->by == a.user());
    CHECK (reopened.model().track (track)->locked->note == "editing the comp");
    CHECK (reopened.lockedByOther (track));

    // Synced like any other field: B sees it, then sees the unlock.
    auto b = joinSession (a, 9, "userB");
    CHECK (b.lockedByOther (track));
    CHECK_FALSE (b.setTrackMute (track, true));
    REQUIRE (a.unlockTrack (track));
    sync (a, b);
    CHECK_FALSE (b.lockedByOther (track));
    CHECK (b.setTrackMute (track, true));
    CHECK (b.model().track (track)->mute);
}

TEST_CASE ("Comments, chat messages and version tags convert to and from document values", "[U][collab]")
{
    model::Comment c;
    c.id = "c1";
    c.pos = 3840;
    c.endPos = 7680;
    c.trackId = "t1";
    c.parentId = "c0";
    c.author = "u_1";
    c.createdAt = 5;
    c.body = "Check @u_2 and @u_3, @u_2 again";
    c.editedAt = 6;
    c.resolved = true;
    c.resolvedBy = "u_2";
    const auto back = model::Comment::fromValue ("c1", c.toValue());
    CHECK (back.toValue() == c.toValue());
    CHECK (back.mentions() == std::vector<UserId> { "u_2", "u_3" });
    auto deleted = c.toValue();
    deleted.set ("deleted", true);
    CHECK (model::Comment::fromValue ("c1", deleted).body.empty());

    model::ChatMessage m { "m1", "u_1", 9, "hi", false };
    CHECK (model::ChatMessage::fromValue (m.toValue()).toValue() == m.toValue());

    model::VersionTag v { "v1", "Mix 1", { std::string (64, 'a') }, "u_1", 10, true };
    CHECK (model::VersionTag::fromValue ("v1", v.toValue()).toValue() == v.toValue());
}

TEST_CASE ("A version's preview is the document at its heads; comments and chat aren't undo steps", "[U][versions][collab]")
{
    Scene scene;
    auto& s = scene.session;
    REQUIRE (s.renameTrack (scene.audioTrack, "Before"));
    const auto v = s.saveVersion ("Mix 1");
    REQUIRE (v);
    REQUIRE (s.renameTrack (scene.audioTrack, "After"));
    const auto preview = s.versionModel (*v);
    REQUIRE (preview);
    CHECK (preview->track (scene.audioTrack)->name == "Before");
    CHECK (s.model().track (scene.audioTrack)->name == "After");
    CHECK_FALSE (s.versionModel ("missing"));

    REQUIRE (s.addComment ({ 0, {}, {}, {} }, "note"));
    REQUIRE (s.sendChat ("hello"));
    CHECK (s.undoName() == "Rename Track");
    REQUIRE (s.undo().done);
    CHECK (s.model().track (scene.audioTrack)->name == "Before");
    CHECK (s.model().comments.size() == 1);
    CHECK (s.model().chat.size() == 1);

    // Restoring a version that names a track deleted since puts it back, with its regions.
    REQUIRE (s.deleteTrack (scene.instTrack));
    REQUIRE (s.restoreVersion (*v));
    CHECK (s.model().track (scene.instTrack) != nullptr);
    CHECK (s.model().region (scene.midiRegion) != nullptr);
}

TEST_CASE ("Chat time links and mention text", "[U][chat][collab]")
{
    Scene scene;
    const auto& m = scene.session.model();
    CHECK (commands::timeLinks ("no links here: 12:345, bar, bars 4, 3:5", m).empty());
    const auto links = commands::timeLinks ("bar 1, 0:01 and bar 2 beat 4", m);
    REQUIRE (links.size() == 3);
    CHECK (links[0].pos == 0);
    CHECK (links[1].pos == 1920); // 1 s at 120 BPM
    CHECK (links[2].pos == 3840 + 3 * 960);
    // Beat past the bar's length: just the bar.
    const auto past = commands::timeLinks ("bar 2 beat 9", m);
    REQUIRE (past.size() == 1);
    CHECK (past[0].length == 5);

    const std::map<UserId, std::string> names { { "u_1", "Ana" }, { "u_2", "Ana Lee" } };
    CHECK (commands::mentionIds ("@ana lee and @Ana, @Anatole", names) == "@u_2 and @u_1, @Anatole");
    CHECK (commands::mentionNames ("@u_2 and @u_1, @u_9", names) == "@Ana Lee and @Ana, @u_9");
}
