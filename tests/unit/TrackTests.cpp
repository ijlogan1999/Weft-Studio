// Track commands and local track state (spec 06 AT-TRK-4, 5, 7, 10) and the recording
// format and disk checks (AT-REC-18, 19).

#include "Fixtures.h"

#include "weft/commands/LocalView.h"
#include "weft/media/WavFile.h"

#include <catch2/catch_approx.hpp>

#include <filesystem>
#include <random>

using namespace weft;
using namespace weft::test;

namespace
{
std::vector<std::string> trackNames (const Session& s)
{
    std::vector<std::string> names;
    for (const auto& id : s.model().trackOrder)
        names.push_back (s.model().track (id)->name);
    return names;
}
} // namespace

TEST_CASE ("AT-TRK-4: New Track with a count, Delete and Duplicate", "[U][tracks][AT-TRK-4]")
{
    auto s = newSession();
    const auto audio1 = firstTrackOfKind (s, TrackKind::audio);
    const auto keys = s.createTrack (TrackKind::instrument, "Keys").value();

    SECTION ("count 3 goes below the selected track as Audio 2-4, one undo step")
    {
        const auto ids = s.createTracks (TrackKind::audio, 3, 1, audio1);
        REQUIRE (ids.size() == 3);
        CHECK (trackNames (s) == std::vector<std::string> { "Audio 1", "Audio 2", "Audio 3", "Audio 4", "Keys" });
        CHECK (s.model().track (ids[0])->channels == 1);
        CHECK (s.undoName() == "New Tracks");
        REQUIRE (s.undo().done);
        CHECK (trackNames (s) == std::vector<std::string> { "Audio 1", "Keys" });
    }
    SECTION ("Delete removes the track, its regions, automation and sends to it")
    {
        const auto bus = s.createTrack (TrackKind::bus, "Verb").value();
        const auto send = s.addSend (keys, bus, -6.0);
        REQUIRE (send);
        model::MediaRef m;
        m.hash = "b3:aa";
        m.originalName = "a.wav";
        m.lengthSamples = 48000;
        REQUIRE (s.addMedia (m));
        REQUIRE (s.createAudioRegion (audio1, 0, "b3:aa", 0, 48000));
        REQUIRE (s.addAutomationPoint (audio1, model::AutomationTarget {}, 0, 0.5));
        REQUIRE (s.deleteTrack (audio1));
        CHECK (s.model().track (audio1) == nullptr);
        CHECK (s.model().regions.empty());
        CHECK (s.model().automationLanes.empty());
        REQUIRE (s.deleteTrack (bus));
        CHECK (s.model().track (keys)->strip.sends.empty());
    }
    SECTION ("Duplicate copies name, strip and settings; regions only when asked")
    {
        REQUIRE (s.setTrackVolume (audio1, -4.5));
        REQUIRE (s.setTrackPan (audio1, 0.25));
        REQUIRE (s.setTrackMonitor (audio1, "on"));
        model::MediaRef m;
        m.hash = "b3:aa";
        m.originalName = "a.wav";
        m.lengthSamples = 48000;
        REQUIRE (s.addMedia (m));
        const auto region = s.createAudioRegion (audio1, 0, "b3:aa", 0, 48000).value();

        const auto copy = s.duplicateTrack (audio1).value();
        CHECK (trackNames (s) == std::vector<std::string> { "Audio 1", "Audio 1 copy", "Keys" });
        const auto* t = s.model().track (copy);
        CHECK (t->strip.volumeDb == -4.5);
        CHECK (t->strip.pan == 0.25);
        CHECK (t->monitor == "on");
        CHECK (s.model().regionsOnTrack (copy).empty());

        const auto withRegions = s.duplicateTrack (audio1, true).value();
        const auto copied = s.model().regionsOnTrack (withRegions);
        REQUIRE (copied.size() == 1);
        CHECK (copied[0]->id != region);
        CHECK (copied[0]->audio.media == "b3:aa");
    }
    SECTION ("Duplicating MIDI regions copies their notes into a separate sequence")
    {
        const auto r = s.createMidiRegion (keys, 0, 3840).value();
        REQUIRE (s.addNote (r, model::Note {}));
        const auto copy = s.duplicateTrack (keys, true).value();
        const auto copied = s.model().regionsOnTrack (copy);
        REQUIRE (copied.size() == 1);
        CHECK (copied[0]->midi.sequenceId != s.model().region (r)->midi.sequenceId);
        CHECK (s.model().sequence (copied[0]->midi.sequenceId)->notes.size() == 1);
    }
}

TEST_CASE ("AT-TRK-5: moving track 3 above track 1 reorders trackOrder", "[U][tracks][AT-TRK-5]")
{
    auto s = newSession();
    const auto t1 = firstTrackOfKind (s, TrackKind::audio);
    const auto t2 = s.createTrack (TrackKind::audio, "").value();
    const auto t3 = s.createTrack (TrackKind::audio, "").value();
    REQUIRE (s.moveTrack (t3, 0));
    CHECK (s.model().trackOrder == std::vector<Id> { t3, t1, t2 });
}

TEST_CASE ("AT-TRK-7: track color colors regions without their own; recordings follow the track", "[U][tracks][AT-TRK-7]")
{
    auto s = newSession();
    const auto track = firstTrackOfKind (s, TrackKind::audio);
    model::MediaRef m;
    m.hash = "b3:aa";
    m.originalName = "a.wav";
    m.lengthSamples = 96000;
    REQUIRE (s.addMedia (m));
    const auto plain = s.createAudioRegion (track, 0, "b3:aa", 0, 48000).value();
    const auto own = s.createAudioRegion (track, 3840, "b3:aa", 0, 48000).value();
    REQUIRE (s.setRegionColor (own, model::Color { 3, {} }));

    REQUIRE (s.setTrackColor (track, model::Color { 7, {} }));
    CHECK (s.model().regionColor (*s.model().region (plain)).palette == 7);
    CHECK (s.model().regionColor (*s.model().region (own)).palette == 3);

    // A newly recorded region has no override, so it shows swatch 7.
    commands::RecordedAudio rec;
    rec.trackId = track;
    rec.trackName = "Audio 1";
    rec.media.hash = "b3:bb";
    rec.media.lengthSamples = 48000;
    rec.takes.push_back ({ 7680, 0, 48000 });
    REQUIRE (s.commitRecording ({ rec }, {}));
    const model::Region* recorded = nullptr;
    for (const auto* r : s.model().regionsOnTrack (track))
        if (r->audio.media == "b3:bb")
            recorded = r;
    REQUIRE (recorded != nullptr);
    CHECK_FALSE (recorded->color.has_value());
    CHECK (s.model().regionColor (*recorded).palette == 7);
    CHECK (s.model().mediaRef ("b3:bb")->originalName == "Audio 1#1");

    CHECK_FALSE (s.setTrackColor (track, model::Color { 24, {} }));
}

TEST_CASE ("AT-TRK-10: record enable is local view state and off for buses and the master", "[U][tracks][AT-TRK-10]")
{
    auto s = newSession();
    const auto audio = firstTrackOfKind (s, TrackKind::audio);
    const auto bus = s.createTrack (TrackKind::bus, "Bus 1").value();
    const auto before = s.document().allChanges().size();

    commands::LocalView view;
    CHECK (view.toggleArm (s.model(), audio));
    CHECK (view.armed.contains (audio));
    CHECK (view.toggleArm (s.model(), audio));
    CHECK_FALSE (view.armed.contains (audio));
    CHECK_FALSE (view.toggleArm (s.model(), bus));
    CHECK_FALSE (view.toggleArm (s.model(), s.model().info.masterTrackId));
    CHECK (view.armed.empty());
    CHECK (s.document().allChanges().size() == before); // nothing written to the document

    view.toggleArm (s.model(), audio);
    REQUIRE (s.deleteTrack (audio));
    view.prune (s.model());
    CHECK (view.armed.empty());
}

TEST_CASE ("Recording names count up per track and survive in one commit", "[U][record]")
{
    auto s = newSession();
    const auto track = firstTrackOfKind (s, TrackKind::audio);
    commands::RecordedAudio a;
    a.trackId = track;
    a.media.hash = "b3:01";
    a.media.lengthSamples = 48000;
    a.takes.push_back ({ 0, 0, 48000 });
    auto b = a;
    b.media.hash = "b3:02";
    REQUIRE (s.commitRecording ({ a, b }, {}));
    CHECK (s.model().mediaRef ("b3:01")->originalName == "Audio 1#1");
    CHECK (s.model().mediaRef ("b3:02")->originalName == "Audio 1#2");
    CHECK (s.nextRecordingName ("Audio 1") == "Audio 1#3");
    CHECK (s.undoName() == "Record");
}

TEST_CASE ("AT-REC-18: recordings are written at 16, 24 and 32-bit float", "[U][record][media][AT-REC-18]")
{
    std::random_device rd;
    const auto dir = std::filesystem::temp_directory_path() / ("weft-rec18-" + std::to_string (rd()));
    std::filesystem::create_directories (dir);
    std::vector<float> samples { 0.0f, 0.5f, -0.5f, 1.5f, -2.0f, 0.25f };
    const float* planar[1] = { samples.data() };

    for (const int bits : { 16, 24, 32 })
    {
        const auto file = dir / ("take" + std::to_string (bits) + ".wav");
        media::WavWriter w;
        media::BextInfo bext;
        bext.description = "Audio 1";
        REQUIRE (w.open (file, 48000, 1, media::formatForBitDepth (bits), &bext));
        REQUIRE (w.write (planar, static_cast<std::int64_t> (samples.size())));
        REQUIRE (w.close());

        const auto info = media::readWavInfo (file);
        REQUIRE (info);
        CHECK (info->bitsPerSample == bits);
        CHECK (info->isFloat == (bits == 32));
        CHECK (info->frames == 6);
        REQUIRE (info->bext);
        CHECK (info->bext->description == "Audio 1");

        const auto audio = media::readWav (file);
        REQUIRE (audio);
        const auto& ch = audio->channels[0];
        CHECK (ch[1] == Catch::Approx (0.5).margin (1e-4));
        if (bits == 32)
        {
            CHECK (ch[3] == 1.5f); // float keeps samples over full scale
            CHECK (ch[4] == -2.0f);
        }
        else
        {
            CHECK (ch[3] <= 1.0f); // integer formats clip
            CHECK (ch[4] >= -1.0f);
        }
    }
    std::filesystem::remove_all (dir);
}

TEST_CASE ("AT-REC-19: warn when the disk holds less than 10 minutes for the armed tracks", "[U][record][AT-REC-19]")
{
    // Two mono tracks at 48 kHz / 24-bit: 288,000 bytes per second.
    const std::uint64_t perMinute = 60ull * 48000 * 3 * 2;
    CHECK (commands::recordingMinutesAvailable (9 * perMinute, 48000, 24, 2) == Catch::Approx (9.0));
    CHECK (commands::needsDiskSpaceWarning (9 * perMinute, 48000, 24, 2));
    CHECK_FALSE (commands::needsDiskSpaceWarning (11 * perMinute, 48000, 24, 2));
    CHECK_FALSE (commands::needsDiskSpaceWarning (0, 48000, 24, 0)); // nothing armed for audio
}
