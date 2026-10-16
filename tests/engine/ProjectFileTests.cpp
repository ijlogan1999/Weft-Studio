// Save and load (spec 06 §1, W-10): a full round trip with a bit-identical bounce (AT-PRJ-3),
// Save As with and without copying media (AT-PRJ-6, AT-PRJ-7), missing media and Locate…
// (AT-PRJ-8), and MIDI inputs switched off in preferences (AT-UI-6, the part without hardware).

#include "EngineFixtures.h"

#include "weft/engine/arrange/OfflineRender.h"
#include "weft/engine/dsp/BuiltinEffects.h"
#include "weft/engine/midi/MidiInputs.h"
#include "weft/engine/render/Bounce.h"
#include "weft/media/MediaStore.h"
#include "weft/project/ProjectFiles.h"
#include "weft/project/ProjectFolder.h"

#include <cmath>
#include <fstream>
#include <numbers>

using namespace weft;
using namespace weft::test;
namespace fs = std::filesystem;

namespace
{
/** A mono tone written into `mediaDir` under its hash, registered in the session. */
std::string addToneMedia (Session& session, const fs::path& mediaDir, double freq, std::int64_t frames)
{
    media::AudioData a;
    a.sampleRate = 48000;
    a.channels.assign (1, std::vector<float> (static_cast<std::size_t> (frames)));
    for (std::int64_t i = 0; i < frames; ++i)
        a.channels[0][static_cast<std::size_t> (i)] = static_cast<float> (0.25 * std::sin (2.0 * std::numbers::pi * freq * static_cast<double> (i) / 48000.0));
    const auto hash = media::storeMedia (a, media::SampleFormat::int24, mediaDir);
    REQUIRE (hash.has_value());
    model::MediaRef m;
    m.hash = *hash;
    m.originalName = "tone " + std::to_string (static_cast<int> (freq)) + ".wav";
    m.lengthSamples = frames;
    m.sizeBytes = static_cast<std::int64_t> (fs::file_size (mediaDir / media::mediaFileName (*hash)));
    REQUIRE (session.addMedia (m));
    return *hash;
}

/** A saved project folder with an audio region on "Audio 1". */
struct SavedProject
{
    TempFolder temp;
    fs::path root = temp.path / "Song.weft";
    project::ProjectFolder folder { root };
    Session session = newP0();
    std::string hash;

    SavedProject()
    {
        REQUIRE (folder.create());
        hash = addToneMedia (session, folder.mediaDir(), 440.0, 48000);
        REQUIRE (session.createAudioRegion (session.model().trackOrder.front(), 0, hash, 0, 48000));
        REQUIRE (folder.save (session.document()));
    }
};

void loadMedia (MediaPool& pool, const model::ProjectModel& model, const fs::path& root)
{
    const auto folders = project::mediaFolders (root);
    for (const auto& [hash, ref] : model.media)
        if (const auto file = project::findMediaFile (folders, hash))
            pool.load (file->parent_path(), hash);
}

media::AudioData render4Bars (const model::ProjectModel& model, MediaPool& pool, const fs::path& root)
{
    OfflineRenderOptions o;
    o.frames = 4 * barSamples;
    o.projectFolder = root;
    return renderOffline (model, pool, o);
}
} // namespace

TEST_CASE ("AT-PRJ-3 save, close and reopen gives an equal document and a bit-identical 4-bar bounce", "[E][project][AT-PRJ-3]")
{
    TempFolder temp;
    const auto root = temp.path / "Round Trip.weft";
    project::ProjectFolder folder (root);
    REQUIRE (folder.create());
    auto s = newP0();

    const auto audio1 = s.model().trackOrder.front();
    const auto audio2 = s.createTrack (TrackKind::audio, "Audio 2", 1).value();
    const auto keys = s.createTrack (TrackKind::instrument, "Keys").value();
    const auto bus = s.createTrack (TrackKind::bus, "Bus 1").value();

    // 3 regions: two audio, one MIDI with 10 notes.
    const auto low = addToneMedia (s, folder.mediaDir(), 220.0, 2 * barSamples);
    const auto high = addToneMedia (s, folder.mediaDir(), 660.0, barSamples);
    REQUIRE (s.createAudioRegion (audio1, 0, low, 0, 2 * barSamples));
    REQUIRE (s.createAudioRegion (audio2, barTicks, high, 0, barSamples));
    const auto midi = s.createMidiRegion (keys, 0, 4 * barTicks).value();
    for (int i = 0; i < 10; ++i)
    {
        model::Note n;
        n.pos = i * 960;
        n.length = 480;
        n.pitch = 60 + i;
        n.velocity = 90;
        REQUIRE (s.addNote (midi, n));
    }

    // 1 send to the bus, a reverb on the bus with a non-default mix.
    REQUIRE (s.addSend (audio1, bus, -6.0));
    model::PluginInstance reverb;
    reverb.format = "builtin";
    reverb.pluginUid = WeftReverb::pluginUid;
    reverb.name = "Weft Reverb";
    reverb.params["mix"] = 0.7;
    REQUIRE (s.addInsert (bus, reverb));

    // 1 automation lane with 3 points, and 2 markers.
    model::AutomationTarget volume;
    volume.kind = "volume";
    REQUIRE (s.addAutomationPoint (audio1, volume, 0, 0.5));
    REQUIRE (s.addAutomationPoint (audio1, volume, barTicks, 0.8));
    REQUIRE (s.addAutomationPoint (audio1, volume, 2 * barTicks, 0.3));
    REQUIRE (s.addMarker (0, "Intro"));
    REQUIRE (s.addMarker (2 * barTicks, "Verse"));

    const auto& m = s.model();
    CHECK (m.regions.size() == 3);
    CHECK (m.automationLanes.size() == 1);
    CHECK (m.markers.size() == 2);

    MediaPool before;
    loadMedia (before, m, root);
    const auto bounceBefore = render4Bars (m, before, root);
    double energy = 0;
    for (const auto v : bounceBefore.channels[0])
        energy += std::abs (v);
    REQUIRE (energy > 0);

    REQUIRE (folder.save (s.document()));
    const auto saved = s.document().root();

    // Closed: everything is read back from disk.
    project::ProjectFolder reopened (root);
    auto opened = reopened.open();
    REQUIRE (opened.document.has_value());
    CHECK_FALSE (opened.recoveryAvailable());
    CHECK (opened.document->root() == saved);

    Session s2 (std::move (*opened.document), "userA");
    MediaPool after;
    loadMedia (after, s2.model(), root);
    CHECK (project::missingMedia (s2.model(), project::mediaFolders (root)).empty());
    const auto bounceAfter = render4Bars (s2.model(), after, root);
    REQUIRE (bounceAfter.channels.size() == bounceBefore.channels.size());
    CHECK (bounceAfter.channels == bounceBefore.channels);
}

TEST_CASE ("AT-PRJ-6 Save As with Copy media copies every referenced file and leaves the original alone", "[E][project][AT-PRJ-6]")
{
    SavedProject p;
    // A second, unreferenced file in Media is not copied.
    const auto unused = addToneMedia (p.session, p.folder.mediaDir(), 880.0, 4800);
    REQUIRE (p.folder.save (p.session.document()));

    std::vector<fs::path> originalFiles;
    for (const auto& e : fs::recursive_directory_iterator (p.root))
        originalFiles.push_back (e.path());
    const auto originalDoc = project::readFile (p.folder.documentFile());

    const auto copy = p.temp.path / "Copy.weft";
    std::string error;
    REQUIRE (project::saveProjectAs (p.session.document(), p.session.model(), p.root, copy, { true }, &error));
    CHECK (error.empty());

    CHECK (fs::exists (copy / "Media" / media::mediaFileName (p.hash)));
    CHECK_FALSE (fs::exists (copy / "Media" / media::mediaFileName (unused)));
    CHECK_FALSE (fs::exists (copy / "media-links.txt"));
    CHECK (fs::exists (copy / "Media" / "media-index.json"));

    // The original folder is unchanged.
    std::vector<fs::path> nowFiles;
    for (const auto& e : fs::recursive_directory_iterator (p.root))
        nowFiles.push_back (e.path());
    CHECK (nowFiles == originalFiles);
    CHECK (project::readFile (p.folder.documentFile()) == originalDoc);

    // The new project opens with no missing media, even with the original gone.
    fs::remove_all (p.root);
    project::ProjectFolder copied (copy);
    auto opened = copied.open();
    REQUIRE (opened.document.has_value());
    Session s (std::move (*opened.document), "userA");
    CHECK (project::missingMedia (s.model(), project::mediaFolders (copy)).empty());

    // Save As never overwrites an existing folder.
    CHECK_FALSE (project::saveProjectAs (s.document(), s.model(), copy, copy, { true }, &error));
}

TEST_CASE ("AT-PRJ-7 Save As without copying references the original media", "[E][project][AT-PRJ-7]")
{
    SavedProject p;
    const auto linked = p.temp.path / "Linked.weft";
    REQUIRE (project::saveProjectAs (p.session.document(), p.session.model(), p.root, linked, { false }));
    CHECK_FALSE (fs::exists (linked / "Media" / media::mediaFileName (p.hash)));
    CHECK (fs::exists (linked / "media-links.txt"));

    project::ProjectFolder folder (linked);
    auto opened = folder.open();
    REQUIRE (opened.document.has_value());
    Session s (std::move (*opened.document), "userA");
    const auto folders = project::mediaFolders (linked);
    CHECK (project::missingMedia (s.model(), folders).empty());
    const auto file = project::findMediaFile (folders, p.hash);
    REQUIRE (file.has_value());
    CHECK (fs::equivalent (*file, p.folder.mediaDir() / media::mediaFileName (p.hash)));
    MediaPool pool;
    loadMedia (pool, s.model(), linked);
    CHECK (pool.find (p.hash) != nullptr);

    // While the original exists; without it the media is missing.
    fs::remove_all (p.root);
    CHECK (project::missingMedia (s.model(), project::mediaFolders (linked)) == std::vector<std::string> { p.hash });
}

TEST_CASE ("AT-PRJ-8 a missing media file opens flagged, and Locate finds it by content under any name", "[E][project][AT-PRJ-8]")
{
    SavedProject p;
    const auto mediaFile = p.folder.mediaDir() / media::mediaFileName (p.hash);
    const auto elsewhere = p.temp.path / "Backup" / "nested";
    fs::create_directories (elsewhere);
    fs::copy_file (mediaFile, elsewhere / "my vocal take.wav");
    // A decoy of the same size with different bytes is hashed and skipped.
    {
        auto bytes = project::readFile (mediaFile).value();
        bytes.back() ^= 0x5a;
        std::ofstream (elsewhere / "decoy.wav", std::ios::binary).write (reinterpret_cast<const char*> (bytes.data()), static_cast<std::streamsize> (bytes.size()));
    }
    fs::remove (mediaFile);

    project::ProjectFolder folder (p.root);
    auto opened = folder.open();
    REQUIRE (opened.document.has_value()); // open succeeds
    Session s (std::move (*opened.document), "userA");
    MediaPool pool;
    loadMedia (pool, s.model(), p.root);
    const auto missing = project::missingMedia (s.model(), project::mediaFolders (p.root));
    REQUIRE (missing == std::vector<std::string> { p.hash });
    CHECK (regionsMissingMedia (s.model(), pool) == 1); // the region is flagged missing

    std::map<std::string, std::int64_t> wanted { { p.hash, s.model().mediaRef (p.hash)->sizeBytes } };
    const auto found = project::locateMedia (p.temp.path / "Backup", wanted, folder.mediaDir());
    CHECK (found == std::vector<std::string> { p.hash });
    CHECK (fs::exists (mediaFile));
    CHECK (project::missingMedia (s.model(), project::mediaFolders (p.root)).empty());
    pool.load (folder.mediaDir(), p.hash);
    CHECK (regionsMissingMedia (s.model(), pool) == 0);
}

TEST_CASE ("AT-UI-6 a MIDI input disabled in preferences stops reaching tracks; enabling restores it", "[E][midi][AT-UI-6]")
{
    Rig rig;
    auto& s = rig.session;
    const auto keys = s.createTrack (TrackKind::instrument, "Keys").value();
    auto probe = std::make_shared<ProbeInstrument>();
    rig.rack.set (keys, probe);
    rig.sync();
    rig.engine.setArmedTracks ({ keys });
    rig.device.render (512);

    MidiInputRouter router (rig.engine);
    const auto notesSeen = [&] {
        rig.device.render (512);
        int n = 0;
        for (const auto& e : probe->seen)
            n += e.event.isNoteOn() ? 1 : 0;
        return n;
    };

    CHECK (router.deliver ("USB Keys", 0x90, 60, 100));
    CHECK (notesSeen() == 1);

    router.setDisabled ({ "USB Keys" });
    CHECK_FALSE (router.enabled ("USB Keys"));
    CHECK_FALSE (router.deliver ("USB Keys", 0x90, 62, 100));
    CHECK (router.deliver ("Other Port", 0x90, 64, 100)); // other inputs still play
    CHECK (router.deliver ({}, 0x90, 65, 100));           // the computer keyboard is always on
    CHECK (notesSeen() == 3);

    router.setDisabled ({});
    CHECK (router.deliver ("USB Keys", 0x90, 67, 100));
    CHECK (notesSeen() == 4);
}
