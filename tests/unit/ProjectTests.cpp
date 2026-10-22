// AT-PRJ-1, AT-PRJ-2, AT-PRJ-4, AT-PRJ-9, AT-PRJ-13 (the commands), the recent projects
// list and the recovery log format.

#include "Fixtures.h"
#include "weft/project/ProjectFiles.h"
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

TEST_CASE ("AT-PRJ-1: a new project has exactly the default contents", "[U][project][AT-PRJ-1]")
{
    auto s = newSession();
    const auto& m = s.model();
    CHECK (m.info.sampleRate == 48000);
    CHECK (m.info.schemaVersion == 1);
    REQUIRE (m.storedTempoPoints.size() == 1);
    CHECK (m.storedTempoPoints[0].pos == 0);
    CHECK (m.storedTempoPoints[0].bpm == 120.0);
    REQUIRE (m.storedSignaturePoints.size() == 1);
    CHECK (m.storedSignaturePoints[0].bar == 0);
    CHECK (m.storedSignaturePoints[0].numerator == 4);
    CHECK (m.storedSignaturePoints[0].denominator == 4);
    REQUIRE (m.tracks.size() == 2);
    REQUIRE (m.trackOrder.size() == 1);
    const auto* audio = m.track (m.trackOrder[0]);
    CHECK (audio->kind == model::TrackKind::audio);
    CHECK (audio->name == "Audio 1");
    CHECK (m.track (m.info.masterTrackId)->kind == model::TrackKind::master);
    CHECK (m.regions.empty());
    CHECK (m.midiSequences.empty());
    CHECK (m.media.empty());
    CHECK (m.markers.empty());
    CHECK (m.storedLanes.empty());
    CHECK (m.takeFolders.empty());
    CHECK (m.info.metronomeDefaults.countInBars == 1);
    CHECK (audio->automationMode == "read");
    CHECK_FALSE (s.canUndo()); // creating the project is not an undo step
}

TEST_CASE ("AT-PRJ-2: supported sample rates are stored, others rejected", "[U][project][AT-PRJ-2]")
{
    for (int rate : { 44100, 48000, 88200, 96000 })
    {
        commands::NewProjectOptions o;
        o.sampleRate = rate;
        auto s = commands::Session::createNew (o, "u", nullptr, fixedClock, 3);
        REQUIRE (s);
        CHECK (s->document().get (doc::path ({ "sampleRate" }))->asInt() == rate);
    }
    for (int rate : { 0, 22050, 32000, 47999, 192000 })
    {
        commands::NewProjectOptions o;
        o.sampleRate = rate;
        std::string error;
        CHECK_FALSE (commands::Session::createNew (o, "u", &error, fixedClock, 3));
        CHECK (error.find (std::to_string (rate)) != std::string::npos);
    }
}

TEST_CASE ("AT-PRJ-4: an interrupted save leaves the previous file intact", "[U][project][AT-PRJ-4]")
{
    TempDir tmp;
    project::ProjectFolder folder (project::ProjectFolder::folderFor (tmp.path, "Atomic"));
    REQUIRE (folder.create());
    Scene scene;
    auto& s = scene.session;
    REQUIRE (folder.save (s.document()));
    const auto savedRoot = s.document().root();
    const auto savedBytes = project::readFile (folder.documentFile()).value();

    for (int i = 0; i < 400; ++i) // grow the document well past one write chunk
    {
        model::Note n;
        n.pos = i * 10;
        n.pitch = i % 128;
        REQUIRE (s.addNote (scene.midiRegion, n));
    }
    bool faulted = false;
    std::string error;
    const bool ok = folder.save (s.document(), [&] (std::size_t written, std::size_t total) {
        if (written * 2 >= total)
        {
            faulted = true;
            return false;
        }
        return true;
    }, &error);
    CHECK_FALSE (ok);
    CHECK (faulted);
    CHECK (project::readFile (folder.documentFile()).value() == savedBytes);
    auto opened = folder.open();
    REQUIRE (opened.document);
    CHECK (opened.document->root() == savedRoot);

    // The next save succeeds and replaces the leftover temp file.
    REQUIRE (folder.save (s.document()));
    auto reopened = folder.open();
    REQUIRE (reopened.document);
    CHECK (reopened.document->root() == s.document().root());
}

TEST_CASE ("AT-PRJ-9: a newer schema is refused without touching the file", "[U][project][AT-PRJ-9]")
{
    TempDir tmp;
    project::ProjectFolder folder (tmp.path / "Future.weft");
    REQUIRE (folder.create());
    auto s = newSession();
    REQUIRE (s.document().put (doc::path ({ "schemaVersion" }), Value (2)) == doc::Status::ok);
    s.document().commit ("test", 0);
    REQUIRE (folder.save (s.document()));
    const auto bytes = project::readFile (folder.documentFile()).value();
    const auto modified = fs::last_write_time (folder.documentFile());

    const auto opened = folder.open();
    CHECK_FALSE (opened.document);
    CHECK (opened.error.find ("format 2") != std::string::npos);
    CHECK (project::readFile (folder.documentFile()).value() == bytes);
    CHECK (fs::last_write_time (folder.documentFile()) == modified);
}

TEST_CASE ("Recovery log replays changes and stops at a torn tail", "[U][project]")
{
    TempDir tmp;
    project::ProjectFolder folder (tmp.path / "Log.weft");
    REQUIRE (folder.create());
    auto s = newSession();
    REQUIRE (folder.save (s.document()));
    s.onLocalChange = [&] (const doc::Bytes& change, std::int64_t t) { REQUIRE (folder.log().append (change, t)); };
    REQUIRE (s.renameProject ("One"));
    REQUIRE (s.addMarker (960, "A"));
    const auto twoChanges = s.document().root();
    REQUIRE (s.addMarker (1920, "B"));
    REQUIRE (folder.log().read().size() == 3);

    {
        auto opened = folder.open();
        REQUIRE (opened.recoveryAvailable());
        CHECK (opened.recovered->root() == s.document().root());
    }

    // Tear the last record, as a crash in the middle of an append would.
    const auto logFile = folder.log().file();
    fs::resize_file (logFile, fs::file_size (logFile) - 3);
    CHECK (folder.log().read().size() == 2);
    auto opened = folder.open();
    REQUIRE (opened.recoveryAvailable());
    CHECK (opened.recovered->root() == twoChanges);
    REQUIRE (folder.recover (opened));
    CHECK_FALSE (folder.log().hasEntries());
    auto again = folder.open();
    CHECK_FALSE (again.recoveryAvailable());
    CHECK (again.document->root() == twoChanges);
}

TEST_CASE ("crc32 matches the standard check value", "[U][project]")
{
    const std::string check = "123456789";
    CHECK (project::crc32 ({ reinterpret_cast<const std::uint8_t*> (check.data()), check.size() }) == 0xCBF43926u);
}

TEST_CASE ("AT-PRJ-13 project settings: record bit depth, metronome defaults, sample rate only without media", "[U][project][AT-PRJ-13]")
{
    auto s = commands::Session::createNew ({}, "userA").value();
    CHECK (s.renameProject ("Demo"));
    CHECK (s.model().info.name == "Demo");

    CHECK (s.setRecordBitDepth (16));
    CHECK (s.model().info.recordBitDepth == 16);
    CHECK_FALSE (s.setRecordBitDepth (20));
    CHECK (s.model().info.recordBitDepth == 16);

    model::MetronomeSettings m;
    m.enabled = false;
    m.during = "both";
    m.countInBars = 2;
    m.levelDb = -6;
    CHECK (s.setMetronomeDefaults (m));
    CHECK (s.model().info.metronomeDefaults.during == "both");
    CHECK (s.model().info.metronomeDefaults.countInBars == 2);
    m.during = "never";
    CHECK_FALSE (s.setMetronomeDefaults (m));

    CHECK (s.setSampleRate (96000));
    CHECK (s.model().info.sampleRate == 96000);
    CHECK_FALSE (s.setSampleRate (22050));
    model::MediaRef media;
    media.hash = "b3:" + std::string (64, 'a');
    REQUIRE (s.addMedia (media));
    const auto refused = s.setSampleRate (44100);
    CHECK_FALSE (refused);
    CHECK (refused.error == "The sample rate can't change once the project has audio.");
    CHECK (s.model().info.sampleRate == 96000);

    // Each change is its own undo step.
    REQUIRE (s.undo().done); // media
    REQUIRE (s.undo().done); // sample rate
    CHECK (s.model().info.sampleRate == 48000);
}

TEST_CASE ("Recent projects keep the ten newest, newest first, and survive a restart", "[U][project][AT-PRJ-10]")
{
    TempDir dir;
    const auto store = dir.path / "settings" / "recent-projects.txt";
    project::RecentProjects recent (store);
    recent.load();
    CHECK (recent.items().empty());
    for (int i = 1; i <= 11; ++i)
        recent.add (dir.path / ("P" + std::to_string (i) + ".weft"));
    REQUIRE (recent.items().size() == 10);
    CHECK (recent.items().front() == dir.path / "P11.weft");
    CHECK (recent.items().back() == dir.path / "P2.weft");

    recent.add (dir.path / "P5.weft"); // reopened: back to the top, not twice
    CHECK (recent.items().front() == dir.path / "P5.weft");
    CHECK (std::count (recent.items().begin(), recent.items().end(), dir.path / "P5.weft") == 1);

    // Non-ASCII names round-trip through the file.
    const auto named = dir.path / project::fromUtf8 ("Caf\xc3\xa9 \xe6\x9b\xb2.weft");
    recent.add (named);
    project::RecentProjects reloaded (store);
    reloaded.load();
    CHECK (reloaded.items() == recent.items());
    CHECK (reloaded.items().front() == named);
    CHECK_FALSE (project::RecentProjects::available (named));

    reloaded.remove (named);
    CHECK (reloaded.items().front() == dir.path / "P5.weft");
}

TEST_CASE ("A picked project.automerge or its folder both name the project", "[U][project]")
{
    TempDir dir;
    const auto root = dir.path / "Song.weft";
    project::ProjectFolder folder (root);
    REQUIRE (folder.create());
    auto s = commands::Session::createNew ({}, "userA").value();
    REQUIRE (folder.save (s.document()));
    CHECK (project::projectRootFor (root / "project.automerge") == root);
    CHECK (project::projectRootFor (root) == root);
    CHECK (project::isProjectFolder (root));
    CHECK_FALSE (project::isProjectFolder (dir.path));
}

// Shared folder (D-COL-45): two computers simulated by two device ids on one folder.

namespace
{
Session sessionFrom (project::ProjectFolder& folder, std::uint64_t seed, const std::string& user)
{
    auto opened = folder.open();
    REQUIRE (opened.document);
    return Session (std::move (*opened.document), user, fixedClock, seed);
}

void applyAll (Session& s, const std::vector<std::vector<std::uint8_t>>& files)
{
    for (const auto& bytes : files)
        REQUIRE (s.applyChanges (bytes) == doc::Status::ok);
}
} // namespace

TEST_CASE ("Shared folder: each computer writes only its own file and merges the others'", "[U][project][shared-folder]")
{
    TempDir tmp;
    const auto root = project::ProjectFolder::folderFor (tmp.path, "Band");
    project::ProjectFolder laptop (root, "aaaaaaaaaaaaaaaa");
    REQUIRE (laptop.create());
    auto a = newSession (1, "userA");
    REQUIRE (laptop.save (a.document()));
    CHECK_FALSE (laptop.shared());
    REQUIRE (laptop.makeShared (a.document()));
    CHECK (laptop.shared());
    CHECK (fs::exists (laptop.ownPeerFile()));
    const auto sharedState = project::readFile (laptop.documentFile()).value();

    project::ProjectFolder desktop (root, "bbbbbbbbbbbbbbbb");
    CHECK (desktop.shared());
    auto b = sessionFrom (desktop, 2, "userB");

    // Both edit at once, then save: neither touches project.automerge or the other's file.
    REQUIRE (a.renameProject ("Band Demo"));
    REQUIRE (laptop.save (a.document()));
    const auto track = b.model().trackOrder.front();
    REQUIRE (b.renameTrack (track, "Bass"));
    REQUIRE (desktop.save (b.document()));
    CHECK (project::readFile (laptop.documentFile()).value() == sharedState);
    CHECK (fs::exists (desktop.ownPeerFile()));

    applyAll (a, laptop.pollPeers());
    applyAll (b, desktop.pollPeers());
    for (const auto* s : { &a, &b })
    {
        CHECK (s->model().info.name == "Band Demo");
        CHECK (s->model().track (track)->name == "Bass");
    }
    CHECK (a.document().heads() == b.document().heads());

    // Nothing new: nothing to merge.
    CHECK (laptop.pollPeers().empty());
    CHECK (desktop.pollPeers().empty());

    // A third computer opening the folder gets everyone's changes.
    project::ProjectFolder third (root, "cccccccccccccccc");
    auto c = sessionFrom (third, 3, "userC");
    CHECK (c.model().info.name == "Band Demo");
    CHECK (c.model().track (track)->name == "Bass");
}

TEST_CASE ("Shared folder: conflicted copies are merged and half-synced files wait", "[U][project][shared-folder]")
{
    TempDir tmp;
    const auto root = project::ProjectFolder::folderFor (tmp.path, "Band");
    project::ProjectFolder laptop (root, "aaaaaaaaaaaaaaaa");
    REQUIRE (laptop.create());
    auto a = newSession (1, "userA");
    REQUIRE (laptop.makeShared (a.document()));
    auto b = joinSession (a, 2, "userB");
    const auto peers = root / "Peers";

    // A sync app that saw two writers keeps both: "<name> (1).automerge".
    REQUIRE (b.renameProject ("From the copy"));
    REQUIRE (project::writeFileAtomic (peers / "bbbbbbbbbbbbbbbb (1).automerge", b.document().save()));
    // A file still arriving is cut short.
    auto c = joinSession (a, 3, "userC");
    REQUIRE (c.renameTrack (c.model().trackOrder.front(), "Keys"));
    auto full = c.document().save();
    const std::vector<std::uint8_t> half (full.begin(), full.begin() + static_cast<std::ptrdiff_t> (full.size() / 2));
    REQUIRE (project::writeFileAtomic (peers / "cccccccccccccccc.automerge", half));

    applyAll (a, laptop.pollPeers());
    CHECK (a.model().info.name == "From the copy");
    CHECK (a.model().track (a.model().trackOrder.front())->name != "Keys");

    REQUIRE (project::writeFileAtomic (peers / "cccccccccccccccc.automerge", full));
    applyAll (a, laptop.pollPeers());
    CHECK (a.model().track (a.model().trackOrder.front())->name == "Keys");
}

TEST_CASE ("Shared folder: each computer keeps its own recovery log", "[U][project][shared-folder]")
{
    TempDir tmp;
    const auto root = project::ProjectFolder::folderFor (tmp.path, "Band");
    project::ProjectFolder local (root);
    REQUIRE (local.create());
    CHECK (local.log().file().filename() == "changes.log");

    project::ProjectFolder laptop (root, "aaaaaaaaaaaaaaaa");
    CHECK (laptop.log().file().filename() == "changes.log"); // not shared yet
    auto a = newSession();
    REQUIRE (laptop.makeShared (a.document()));
    CHECK (laptop.log().file().filename() == "changes-aaaaaaaaaaaaaaaa.log");

    // The desktop's unsaved changes are never offered to the laptop as its own to recover.
    project::ProjectFolder desktop (root, "bbbbbbbbbbbbbbbb");
    CHECK (desktop.log().file().filename() == "changes-bbbbbbbbbbbbbbbb.log");
    auto b = sessionFrom (desktop, 2, "userB");
    b.onLocalChange = [&] (const doc::Bytes& change, std::int64_t time) { REQUIRE (desktop.log().append (change, time)); };
    REQUIRE (b.renameProject ("Unsaved"));
    CHECK (desktop.open().recoveryAvailable());
    CHECK_FALSE (project::ProjectFolder (root, "aaaaaaaaaaaaaaaa").open().recoveryAvailable());
}
