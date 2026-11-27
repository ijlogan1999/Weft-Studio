// AT-PRJ-1, AT-PRJ-2, AT-PRJ-4, AT-PRJ-9, AT-PRJ-13 (the commands), the recent projects
// list and the recovery log format.

#include "Fixtures.h"
#include "weft/media/MidiFile.h"
#include "weft/project/ProjectFiles.h"
#include "weft/project/ProjectFolder.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
    CHECK (m.info.metronomeDefaults.during == "both"); // D-W1-10: K clicks in playback too
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

TEST_CASE ("AT-PRJ-15 the project key: names, typing, Set Key with undo, MIDI key signature", "[U][project][AT-PRJ-15]")
{
    using model::KeySignature;
    CHECK (KeySignature {}.name() == "C major");
    CHECK (KeySignature { 6, true }.name() == "F# minor");
    CHECK (KeySignature { 10, false }.name() == "Bb major");
    CHECK (KeySignature { 1, false }.name() == "Db major");
    CHECK (KeySignature { 1, true }.name() == "C# minor");
    CHECK (KeySignature { 7, false }.fifths() == 1);
    CHECK (KeySignature { 5, false }.fifths() == -1);
    CHECK (KeySignature { 9, true }.fifths() == 0);
    CHECK (KeySignature { 0, true }.fifths() == -3);
    CHECK (KeySignature { 4, true }.fifths() == 1);
    for (int minor = 0; minor < 2; ++minor)
        for (int tonic = 0; tonic < 12; ++tonic)
        {
            const KeySignature k { tonic, minor == 1 };
            CHECK (KeySignature::fromFifths (k.fifths(), k.minor) == k);
        }

    CHECK (KeySignature::parse ("F# minor") == KeySignature { 6, true });
    CHECK (KeySignature::parse ("f#m") == KeySignature { 6, true });
    CHECK (KeySignature::parse ("Bb") == KeySignature { 10, false });
    CHECK (KeySignature::parse ("bb min") == KeySignature { 10, true });
    CHECK (KeySignature::parse ("Eb Major") == KeySignature { 3, false });
    CHECK (KeySignature::parse ("CM") == KeySignature { 0, false });
    CHECK (KeySignature::parse ("Cb") == KeySignature { 11, false });
    CHECK (KeySignature::parse ("A\xE2\x99\xAD") == KeySignature { 8, false }); // A flat sign
    CHECK_FALSE (KeySignature::parse ("H"));
    CHECK_FALSE (KeySignature::parse ("C dorian"));
    CHECK_FALSE (KeySignature::parse (""));

    auto s = commands::Session::createNew ({}, "userA").value();
    CHECK (s.model().info.key == KeySignature {});
    REQUIRE (s.setKey ({ 2, true }));
    CHECK (s.undoName() == "Set Key");
    CHECK (s.model().info.key.name() == "D minor");
    CHECK_FALSE (s.setKey ({ 2, true }));  // already in it
    CHECK_FALSE (s.setKey ({ 12, false })); // no such tonic
    REQUIRE (s.undo().done);
    CHECK (s.model().info.key == KeySignature {});
    REQUIRE (s.redo().done);
    CHECK (s.model().info.key == KeySignature { 2, true });
    CHECK (KeySignature::fromValue (KeySignature { 11, true }.toValue()) == KeySignature { 11, true });
    CHECK (KeySignature::fromValue (Value()) == KeySignature {}); // projects from before the key

    // MIDI files carry it as a key signature event.
    media::MidiFileData file;
    file.tracks.emplace_back();
    file.keys.push_back ({ 0, -3, true });
    const auto parsed = media::parseMidiFile (media::encodeMidiFile (file));
    REQUIRE (parsed);
    REQUIRE (parsed->keys.size() == 1);
    CHECK (parsed->keys[0].fifths == -3);
    CHECK (parsed->keys[0].minor);
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

TEST_CASE ("AT-PRJ-16 project count-in, pan law and MIDI input filter", "[U][project][AT-PRJ-16]")
{
    auto s = commands::Session::createNew ({}, "userA").value();
    CHECK (s.model().info.panLawDb == -3.0);
    CHECK (s.model().info.midiInputFilter == 0);

    // Count-in: none, 1 to 6 bars, or 1 or 2 beats.
    model::MetronomeSettings m = s.model().info.metronomeDefaults;
    for (const int n : { 6, -2, 3 })
    {
        m.countInBars = n;
        CHECK (s.setMetronomeDefaults (m));
        CHECK (s.model().info.metronomeDefaults.countInBars == n);
    }
    for (const int n : { 7, -3 })
    {
        m.countInBars = n;
        CHECK_FALSE (s.setMetronomeDefaults (m));
    }
    CHECK (model::countInName (0) == "None");
    CHECK (model::countInName (1) == "1 bar");
    CHECK (model::countInName (4) == "4 bars");
    CHECK (model::countInName (-1) == "1 beat");
    CHECK (model::countInName (-2) == "2 beats");
    CHECK (model::countInIndex (-2) == 8);
    CHECK (model::countInIndex (99) == 0);

    // Pan law: one undo step; only the four laws.
    CHECK (s.setPanLaw (-6.0));
    CHECK (s.model().info.panLawDb == -6.0);
    CHECK (s.undoName() == "Set Pan Law");
    CHECK_FALSE (s.setPanLaw (-6.0));
    CHECK_FALSE (s.setPanLaw (-2.0));
    REQUIRE (s.undo().done);
    CHECK (s.model().info.panLawDb == -3.0);
    REQUIRE (s.setPanLaw (-4.5));

    // MIDI input filter: bits of model::midiFilter.
    using namespace model::midiFilter;
    CHECK (s.setMidiInputFilter (notes | pitchBend));
    CHECK (s.undoName() == "Set MIDI Input Filter");
    CHECK_FALSE (s.setMidiInputFilter (64));
    CHECK (drops (notes, 0x93, 100));
    CHECK_FALSE (drops (notes, 0x93, 0)); // a velocity-0 note-on ends a note: it passes
    CHECK_FALSE (drops (notes, 0x80, 0));
    CHECK (drops (controllers, 0xb0, 1));
    CHECK (drops (programChange, 0xc5, 0));
    CHECK (drops (channelPressure, 0xd0, 0));
    CHECK (drops (polyPressure, 0xa0, 0));
    CHECK (drops (pitchBend, 0xe0, 0));
    CHECK_FALSE (drops (all & ~pitchBend, 0xe0, 0));

    // All three are in the document and survive reopening.
    const auto bytes = s.document().save();
    commands::Session reopened (doc::Document::load (bytes).value(), "userB");
    CHECK (reopened.model().info.metronomeDefaults.countInBars == 3);
    CHECK (reopened.model().info.panLawDb == -4.5);
    CHECK (reopened.model().info.midiInputFilter == (notes | pitchBend));
}

TEST_CASE ("AT-TMP-11 key changes along the timeline: add, move, edit, delete, the key at a tick", "[U][project][AT-TMP-11]")
{
    using model::KeySignature;
    auto s = commands::Session::createNew ({}, "userA").value();
    REQUIRE (s.setKey ({ 2, false })); // D major from the start
    CHECK_FALSE (s.addKeyChange (0, { 7, false }));  // the start is the project key
    const auto change = s.addKeyChange (8 * 3840, { 7, false }); // G major from bar 9
    REQUIRE (change);
    CHECK (s.undoName() == "Add Key Change");
    CHECK_FALSE (s.addKeyChange (8 * 3840, { 9, true })); // one per position
    CHECK (s.model().keyAt (0) == KeySignature { 2, false });
    CHECK (s.model().keyAt (8 * 3840 - 1) == KeySignature { 2, false });
    CHECK (s.model().keyAt (8 * 3840) == KeySignature { 7, false });
    CHECK (s.model().keyAt (100000) == KeySignature { 7, false });

    // The key display edits the key in effect where it is.
    REQUIRE (s.setKeyAt (10 * 3840, { 4, true }));
    CHECK (s.model().keyChanges.at (*change)->key == KeySignature { 4, true });
    CHECK (s.model().info.key == KeySignature { 2, false });
    REQUIRE (s.setKeyAt (3840, { 5, false }));
    CHECK (s.model().info.key == KeySignature { 5, false });
    CHECK (s.undoName() == "Set Key");

    REQUIRE (s.setKeyChange (*change, 4 * 3840, { 4, true }));
    CHECK (s.undoName() == "Key Change");
    CHECK (s.model().keyAt (4 * 3840) == KeySignature { 4, true });
    CHECK_FALSE (s.setKeyChange (*change, 0, { 4, true }));
    const auto second = s.addKeyChange (12 * 3840, { 0, false });
    REQUIRE (second);
    CHECK_FALSE (s.setKeyChange (*second, 4 * 3840, { 0, false })); // onto the other one
    REQUIRE (s.model().keyChangesInOrder().size() == 2);
    CHECK (s.model().keyChangesInOrder()[0]->id == *change);

    // Saved with the project.
    const auto bytes = s.document().save();
    commands::Session reopened (doc::Document::load (bytes).value(), "userB");
    CHECK (reopened.model().keyAt (13 * 3840) == KeySignature { 0, false });
    CHECK (reopened.model().keyChanges.size() == 2);

    REQUIRE (s.deleteKeyChange (*second));
    CHECK (s.undoName() == "Delete Key Change");
    CHECK (s.model().keyAt (13 * 3840) == KeySignature { 4, true });
    REQUIRE (s.undo().done);
    CHECK (s.model().keyAt (13 * 3840) == KeySignature { 0, false });
}

TEST_CASE ("AT-ARR-44 arrangement sections: add, name, colour, resize, no overlaps, delete", "[U][arrange][AT-ARR-44]")
{
    auto s = commands::Session::createNew ({}, "userA").value();
    const auto intro = s.addSection (0, 4 * 3840);
    REQUIRE (intro);
    CHECK (s.undoName() == "Add Section");
    CHECK (s.model().sections.at (*intro)->name == "Intro");
    const auto verse = s.addSection (4 * 3840, 8 * 3840);
    REQUIRE (verse);
    CHECK (s.model().sections.at (*verse)->name == "Verse");
    CHECK_FALSE (s.addSection (10 * 3840, 4 * 3840)); // inside the verse
    CHECK_FALSE (s.addSection (-1, 3840));
    CHECK (s.model().sectionAt (5 * 3840)->id == *verse);
    CHECK (s.model().sectionAt (12 * 3840) == nullptr);

    REQUIRE (s.renameSection (*verse, "Verse 1"));
    CHECK (s.undoName() == "Rename Section");
    CHECK_FALSE (s.renameSection (*verse, ""));
    REQUIRE (s.setSectionColor (*verse, model::Color { 3, {} }));
    CHECK (s.model().sections.at (*verse)->color->palette == 3);
    CHECK_FALSE (s.setSectionRange (*verse, 2 * 3840, 4 * 3840)); // over the intro
    REQUIRE (s.setSectionRange (*verse, 4 * 3840, 4 * 3840));
    CHECK (s.undoName() == "Resize Section");
    CHECK (s.model().sections.at (*verse)->end() == 8 * 3840);

    const auto bytes = s.document().save();
    commands::Session reopened (doc::Document::load (bytes).value(), "userB");
    REQUIRE (reopened.model().sectionsInOrder().size() == 2);
    CHECK (reopened.model().sectionsInOrder()[1]->name == "Verse 1");

    REQUIRE (s.deleteSection (*intro));
    CHECK (s.undoName() == "Delete Section");
    CHECK (s.model().sections.size() == 1);
}

TEST_CASE ("AT-ARR-46 moving and copying a section takes its regions, markers and key changes along", "[U][arrange][AT-ARR-46]")
{
    constexpr Ticks bar = 3840;
    auto s = commands::Session::createNew ({}, "userA").value();
    const auto inst = s.createTrack (model::TrackKind::instrument, "").value();
    const auto intro = s.addSection (0, 4 * bar).value();
    const auto verse = s.addSection (4 * bar, 4 * bar).value();
    const auto inIntro = s.createMidiRegion (inst, bar, bar).value();
    const auto inVerse = s.createMidiRegion (inst, 5 * bar, bar).value();
    const auto across = s.createMidiRegion (inst, 3 * bar, 2 * bar).value(); // crosses the intro's end
    const auto marker = s.addMarker (6 * bar, "Hook").value();
    const auto key = s.addKeyChange (4 * bar, model::KeySignature { 2, false }).value();
    const auto starts = [&] {
        std::vector<Ticks> out;
        for (const auto& [id, r] : s.model().regions)
            out.push_back (r->start);
        std::sort (out.begin(), out.end());
        return out;
    };

    // Dropped inside another section, or at its own edge: refused, or nothing happens.
    CHECK_FALSE (s.moveSection (intro, 6 * bar, false));
    REQUIRE (s.moveSection (intro, 0, false));
    CHECK (s.model().regions.size() == 3);

    // Moving the intro after the verse: one undo step; the crossing region is split at the edge.
    REQUIRE (s.moveSection (intro, 8 * bar, false));
    CHECK (s.undoName() == "Move Section");
    CHECK (s.model().sections.at (verse)->start == 0);
    CHECK (s.model().sections.at (intro)->start == 4 * bar);
    CHECK (s.model().regions.size() == 4);
    CHECK (s.model().regions.at (inIntro)->start == 5 * bar);
    CHECK (s.model().regions.at (inVerse)->start == bar);
    CHECK (s.model().regions.at (across)->start == 7 * bar);
    CHECK (starts() == std::vector<Ticks> { 0, bar, 5 * bar, 7 * bar });
    CHECK (s.model().markers.at (marker)->pos == 2 * bar);
    CHECK (s.model().keyChanges.at (key)->pos == 0);
    REQUIRE (s.undo().done);
    CHECK (s.model().sections.at (intro)->start == 0);
    CHECK (s.model().regions.size() == 3);
    CHECK (s.model().regions.at (inIntro)->start == bar);
    CHECK (s.model().markers.at (marker)->pos == 6 * bar);

    // Copying the verse to the start: everything from there moves right by a verse, and the twin
    // section holds copies of its regions.
    REQUIRE (s.moveSection (verse, 0, true));
    CHECK (s.undoName() == "Copy Section");
    REQUIRE (s.model().sections.size() == 3);
    const auto order = s.model().sectionsInOrder();
    CHECK (order[0]->name == "Verse");
    CHECK (order[0]->start == 0);
    CHECK (order[1]->id == intro);
    CHECK (order[1]->start == 4 * bar);
    CHECK (order[2]->id == verse);
    CHECK (order[2]->start == 8 * bar);
    CHECK (s.model().regions.at (inVerse)->start == 9 * bar);
    CHECK (s.model().regions.at (inIntro)->start == 5 * bar);
    CHECK (s.model().markers.at (marker)->pos == 10 * bar);
    CHECK (s.model().regions.size() == 6); // three, plus the split, plus two copies
    std::size_t inTwin = 0;
    for (const auto& [id, r] : s.model().regions)
        if (r->start < 4 * bar)
            ++inTwin;
    CHECK (inTwin == 2);
    REQUIRE (s.undo().done);
    CHECK (s.model().sections.size() == 2);
    CHECK (s.model().regions.size() == 3);
}

TEST_CASE ("AT-PRJ-21 project and track notes: set, undo, unchanged is refused, saved with the project", "[U][project][AT-PRJ-21]")
{
    TempDir tmp;
    project::ProjectFolder folder (tmp.path / "Notes.weft");
    REQUIRE (folder.create());
    auto s = newSession();
    const auto track = s.model().trackOrder.front();
    CHECK (s.model().info.notes.empty());
    CHECK (s.model().track (track)->notes.empty());

    REQUIRE (s.setProjectNotes ("Mix by Friday.\nBass is too loud."));
    CHECK (s.undoName() == "Project Notes");
    REQUIRE (s.setTrackNotes (track, "Use take 3."));
    CHECK (s.undoName() == "Track Notes");
    CHECK (s.model().info.notes == "Mix by Friday.\nBass is too loud.");
    CHECK (s.model().track (track)->notes == "Use take 3.");
    // The same text again isn't a step.
    CHECK_FALSE (s.setTrackNotes (track, "Use take 3."));
    CHECK_FALSE (s.setProjectNotes ("Mix by Friday.\nBass is too loud."));
    CHECK_FALSE (s.setTrackNotes ("no-such-track", "x"));

    REQUIRE (s.undo().done);
    CHECK (s.model().track (track)->notes.empty());
    REQUIRE (s.redo().done);
    CHECK (s.model().track (track)->notes == "Use take 3.");

    REQUIRE (folder.save (s.document()));
    auto opened = folder.open();
    REQUIRE (opened.document);
    Session reopened (std::move (*opened.document), "userB", fixedClock, 7);
    CHECK (reopened.model().info.notes == "Mix by Friday.\nBass is too loud.");
    CHECK (reopened.model().track (track)->notes == "Use take 3.");

    // Clearing the notes removes them from the file.
    REQUIRE (s.setProjectNotes (""));
    CHECK (s.model().info.notes.empty());
    const auto stored = s.document().get (doc::path ({ "notes" }));
    CHECK ((! stored || stored->isNull()));
}

TEST_CASE ("AT-ARR-47 a marker's text: set, unchanged refused, undo, saved with the project", "[U][arrange][AT-ARR-47]")
{
    TempDir tmp;
    project::ProjectFolder folder (tmp.path / "Markers.weft");
    REQUIRE (folder.create());
    auto s = newSession();
    const auto marker = s.addMarker (3840, "Verse");
    REQUIRE (marker);
    CHECK (s.model().markers.at (*marker)->text.empty());
    REQUIRE (s.setMarkerText (*marker, "Double the vocal here."));
    CHECK (s.undoName() == "Marker Text");
    CHECK (s.model().markers.at (*marker)->text == "Double the vocal here.");
    CHECK_FALSE (s.setMarkerText (*marker, "Double the vocal here."));
    CHECK_FALSE (s.setMarkerText ("missing", "x"));

    REQUIRE (folder.save (s.document()));
    auto opened = folder.open();
    REQUIRE (opened.document);
    Session reopened (std::move (*opened.document), "userB", fixedClock, 7);
    CHECK (reopened.model().markers.at (*marker)->text == "Double the vocal here.");

    REQUIRE (s.undo().done);
    CHECK (s.model().markers.at (*marker)->text.empty());
}
