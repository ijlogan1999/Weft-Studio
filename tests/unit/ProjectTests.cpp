// AT-PRJ-1, AT-PRJ-2, AT-PRJ-4, AT-PRJ-9 and the recovery log format.

#include "Fixtures.h"
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
