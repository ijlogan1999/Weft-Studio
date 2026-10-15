// Recording acceptance tests (spec 06 §AT-REC, workflows W-1 and W-2), run on the NullDevice.

#include "EngineFixtures.h"

#include "weft/engine/midi/KeyboardMidi.h"
#include "weft/media/Hash.h"

#include <catch2/catch_approx.hpp>

#include <cmath>
#include <numbers>

using namespace weft;
using namespace weft::test;

namespace
{
/** Transport settings for recording tests: no count-in, metronome as given. */
void recordSettings (Rig& rig, bool click = false, bool mergeLinear = false, bool mergeCycle = true)
{
    TransportSettings s;
    s.metronomeOn = click;
    s.metronomeDuring = ClickDuring::record;
    s.countInBars = 0;
    s.midiMergeLinear = mergeLinear;
    s.midiMergeCycle = mergeCycle;
    rig.engine.setSettings (s);
}

/** Distinct, exactly representable input values: positive on input 1, negative on input 2. */
float inValue (int channel, std::int64_t frame) { return static_cast<float> ((frame % 5000) + 1) / 8192.0f * (channel == 0 ? 1.0f : -1.0f); }

void rampInputs (Rig& rig)
{
    rig.device.setInputGenerator ([] (int ch, std::int64_t start, float* dest, int n) {
        for (int i = 0; i < n; ++i)
            dest[i] = inValue (ch, start + i);
    });
}

void constantInputs (Rig& rig, float value)
{
    rig.device.setInputGenerator ([value] (int, std::int64_t, float* dest, int n) { std::fill_n (dest, n, value); });
}

const float minus6dB = std::pow (10.0f, -6.0f / 20.0f);

void sineInputs (Rig& rig)
{
    rig.device.setInputGenerator ([] (int, std::int64_t start, float* dest, int n) {
        for (int i = 0; i < n; ++i)
            dest[i] = minus6dB * static_cast<float> (std::sin (2.0 * std::numbers::pi * 1000.0 * static_cast<double> (start + i) / 48000.0));
    });
}

/** A mono or stereo audio track on the given first input, monitoring off unless asked. */
Id inputTrack (Rig& rig, const std::string& name, int channels, int firstInput, const std::string& monitor = "off")
{
    const auto id = rig.session.createTrack (TrackKind::audio, name, channels).value();
    model::InputAssignment in;
    in.kind = model::InputAssignment::Kind::audio;
    in.firstChannel = firstInput;
    REQUIRE (rig.session.setTrackInput (id, in));
    REQUIRE (rig.session.setTrackMonitor (id, monitor));
    rig.sync();
    return id;
}

/** An instrument track playing into a probe that logs what it receives. */
std::pair<Id, std::shared_ptr<ProbeInstrument>> probeTrack (Rig& rig, int midiChannel = 0)
{
    const auto id = rig.session.createTrack (TrackKind::instrument, "Keys").value();
    if (midiChannel != 0)
    {
        model::InputAssignment in;
        in.kind = model::InputAssignment::Kind::midi;
        in.channel = midiChannel;
        REQUIRE (rig.session.setTrackInput (id, in));
    }
    auto probe = std::make_shared<ProbeInstrument>();
    rig.rack.set (id, probe);
    rig.sync();
    return { id, probe };
}

std::vector<const model::Note*> notesOf (const Rig& rig, const model::Region& r)
{
    const auto* seq = rig.session.model().sequence (r.midi.sequenceId);
    REQUIRE (seq != nullptr);
    return seq->sortedNotes();
}

const model::Region* topmost (const std::vector<const model::Region*>& regions)
{
    REQUIRE_FALSE (regions.empty());
    return *std::max_element (regions.begin(), regions.end(), [] (const auto* a, const auto* b) { return a->z < b->z; });
}

std::size_t wavFilesIn (const std::filesystem::path& dir)
{
    std::size_t n = 0;
    std::error_code ec;
    if (std::filesystem::exists (dir, ec))
        for (const auto& e : std::filesystem::directory_iterator (dir))
            n += e.path().extension() == ".wav" ? 1 : 0;
    return n;
}
} // namespace

// Audio recording ---------------------------------------------------------------------

TEST_CASE ("AT-REC-1: a mono recording becomes a 24-bit BWF in Media named by its hash", "[E][record][AT-REC-1]")
{
    Rig rig;
    recordSettings (rig);
    sineInputs (rig);
    const auto track = inputTrack (rig, "Audio 1", 1, 0);
    rig.engine.setArmedTracks ({ track });

    REQUIRE (rig.engine.record (0).recording);
    rig.run (2 * barSamples);
    const auto outcome = rig.finishRecording();
    REQUIRE (outcome);
    CHECK (outcome->committed);
    CHECK (outcome->droppedFrames == 0);

    const auto regions = rig.regionsOn (track);
    REQUIRE (regions.size() == 1);
    const auto& r = *regions[0];
    CHECK (r.start == 0);
    CHECK (std::abs (r.length - 2 * barSamples) <= 256);
    CHECK (rig.session.undoName() == "Record");

    const auto file = rig.mediaFile (r.audio.media);
    REQUIRE (std::filesystem::exists (file));
    CHECK (media::hashFile (file) == r.audio.media);
    const auto info = media::readWavInfo (file);
    REQUIRE (info);
    CHECK (info->bitsPerSample == 24);
    CHECK_FALSE (info->isFloat);
    CHECK (info->sampleRate == 48000.0);
    CHECK (info->channels == 1);
    REQUIRE (info->bext);
    CHECK (info->bext->originator == "Weft Studio");
    CHECK (info->bext->description == "Audio 1");

    const auto* m = rig.session.model().mediaRef (r.audio.media);
    REQUIRE (m != nullptr);
    CHECK (m->originalName == "Audio 1#1");
    CHECK (m->bitDepth == 24);
    CHECK (m->lengthSamples == info->frames);
    CHECK (wavFilesIn (rig.folder.path / "Recovery") == 0);

    // The recorded sine is what came in, to 24-bit precision.
    const auto audio = rig.regionFile (r.id);
    for (std::int64_t i = 0; i < 1000; ++i)
        REQUIRE (audio.channels[0][static_cast<std::size_t> (i)]
                 == Catch::Approx (minus6dB * std::sin (2.0 * std::numbers::pi * 1000.0 * static_cast<double> (i) / 48000.0)).margin (1e-6));
}

TEST_CASE ("AT-REC-2: recordings are shifted by the round-trip latency plus the user offset", "[E][record][AT-REC-2]")
{
    const auto recordClicks = [] (int userOffset) {
        Rig rig;
        rig.device.setLoopback (512);
        rig.reopen (256);
        REQUIRE (rig.engine.recordLatency() == 512);
        rig.engine.setRecordDelay (userOffset);
        recordSettings (rig, true);
        const auto track = inputTrack (rig, "Audio 1", 1, 0); // monitoring off: no feedback loop
        rig.engine.setArmedTracks ({ track });
        REQUIRE (rig.engine.record (0).recording);
        rig.run (barSamples + 1000);
        REQUIRE (rig.finishRecording());
        const auto regions = rig.regionsOn (track);
        REQUIRE (regions.size() == 1);
        CHECK (regions[0]->audio.sourceOffset == 512 + userOffset);
        // Click onsets relative to the region start.
        const auto audio = rig.regionFile (regions[0]->id);
        std::vector<std::int64_t> clicks;
        for (const auto at : onsets (audio.channels[0]))
            clicks.push_back (at - regions[0]->audio.sourceOffset);
        return clicks;
    };

    const auto aligned = recordClicks (0);
    REQUIRE (aligned.size() >= 4);
    for (std::size_t i = 0; i < 4; ++i)
        CHECK (std::abs (aligned[i] - static_cast<std::int64_t> (i) * 24000) <= 1); // where the metronome clicked

    const auto shifted = recordClicks (100);
    REQUIRE (shifted.size() >= 4);
    for (std::size_t i = 0; i < 4; ++i)
        CHECK (shifted[i] == aligned[i] - 100);
}

TEST_CASE ("AT-REC-3: a recording under 100 ms leaves no region and no file", "[E][record][AT-REC-3]")
{
    Rig rig;
    recordSettings (rig);
    constantInputs (rig, 0.25f);
    const auto track = inputTrack (rig, "Audio 1", 1, 0);
    rig.engine.setArmedTracks ({ track });
    REQUIRE (rig.engine.record (0).recording);
    rig.run (4000); // 83 ms
    const auto outcome = rig.finishRecording();
    REQUIRE (outcome);
    CHECK_FALSE (outcome->committed);
    CHECK (rig.regionsOn (track).empty());
    CHECK (rig.session.model().media.empty());
    CHECK (wavFilesIn (rig.folder.path / "Recovery") == 0);
    CHECK (wavFilesIn (rig.folder.path / "Media") == 0);
    CHECK_FALSE (rig.engine.recordingPending());
}

TEST_CASE ("AT-REC-4: recording over a region goes on top and leaves the old one intact", "[E][record][AT-REC-4]")
{
    Rig rig;
    recordSettings (rig);
    constantInputs (rig, 0.25f);
    const auto track = rig.audioTrack();
    REQUIRE (rig.session.setTrackMonitor (track, "off"));
    const auto old = rig.addAudioRegion (track, 0, barSamples);
    const auto before = *rig.session.model().region (old);
    rig.engine.setArmedTracks ({ track });

    REQUIRE (rig.engine.record (0).recording);
    rig.run (barSamples);
    REQUIRE (rig.finishRecording());
    const auto regions = rig.regionsOn (track);
    REQUIRE (regions.size() == 2);
    const auto* recorded = topmost (regions);
    CHECK (recorded->id != old);
    const auto* oldNow = rig.session.model().region (old);
    REQUIRE (oldNow != nullptr);
    CHECK (recorded->z > oldNow->z);
    CHECK (oldNow->start == before.start);
    CHECK (oldNow->length == before.length);
    CHECK (oldNow->audio.sourceOffset == before.audio.sourceOffset);

    rig.engine.setArmedTracks ({});
    rig.engine.play (0);
    auto out = rig.run (barSamples);
    CHECK (out[1000] == 0.25f); // the recording covers the old region

    REQUIRE (rig.session.moveRegion (recorded->id, 4 * barTicks));
    rig.sync();
    rig.engine.play (0);
    out = rig.run (barSamples);
    CHECK (out[1000] == rampValue (1000)); // moved away: the old region is audible again
}

TEST_CASE ("AT-REC-5: four tracks record together and one undo removes them all", "[E][record][AT-REC-5]")
{
    Rig rig;
    recordSettings (rig);
    rampInputs (rig);
    const auto t1 = inputTrack (rig, "Audio 1", 1, 0);
    const auto t2 = inputTrack (rig, "Audio 2", 1, 1);
    const auto t3 = inputTrack (rig, "Audio 3", 2, 0);
    const auto t4 = inputTrack (rig, "Audio 4", 1, 0);
    rig.engine.setArmedTracks ({ t1, t2, t3, t4 });

    REQUIRE (rig.engine.record (0).recording);
    rig.run (barSamples);
    const auto outcome = rig.finishRecording();
    REQUIRE (outcome);
    CHECK (outcome->committed);
    CHECK (outcome->files == 4);

    std::set<std::string> hashes;
    std::int64_t length = -1;
    for (const auto& t : { t1, t2, t3, t4 })
    {
        const auto regions = rig.regionsOn (t);
        REQUIRE (regions.size() == 1);
        CHECK (regions[0]->start == 0);
        hashes.insert (regions[0]->audio.media);
        const auto* m = rig.session.model().mediaRef (regions[0]->audio.media);
        REQUIRE (m != nullptr);
        if (length < 0)
            length = m->lengthSamples;
        CHECK (m->lengthSamples == length);
    }
    CHECK (hashes.size() == 4);
    CHECK (wavFilesIn (rig.folder.path / "Media") == 4);

    // Each file holds its own input.
    const auto first = [&] (const Id& t) { return rig.regionFile (rig.regionsOn (t)[0]->id); };
    const auto a1 = first (t1), a2 = first (t2), a3 = first (t3);
    CHECK (a1.channels[0][100] == inValue (0, 100));
    CHECK (a2.channels[0][100] == inValue (1, 100));
    REQUIRE (a3.numChannels() == 2);
    CHECK (a3.channels[0][100] == inValue (0, 100));
    CHECK (a3.channels[1][100] == inValue (1, 100));

    REQUIRE (rig.session.undo().done);
    for (const auto& t : { t1, t2, t3, t4 })
        CHECK (rig.regionsOn (t).empty());
}

TEST_CASE ("AT-REC-10: input monitoring follows Auto, On and Off", "[E][record][monitor][AT-REC-10]")
{
    Rig rig;
    recordSettings (rig);
    constantInputs (rig, 0.25f);
    const auto track = inputTrack (rig, "Audio 1", 2, 0, "auto");
    const auto audible = [&] { return peak (rig.run (2048), 512, 1536) > 0.2f; };
    const auto monitor = [&] (const char* mode) {
        REQUIRE (rig.session.setTrackMonitor (track, mode));
        rig.sync();
    };

    SECTION ("Auto")
    {
        rig.engine.setArmedTracks ({ track });
        CHECK (audible()); // armed and stopped
        rig.engine.play (0);
        CHECK_FALSE (audible()); // armed and playing
        rig.engine.stop();
        REQUIRE (rig.engine.record (0).recording);
        CHECK (audible()); // recording
        rig.engine.stop();
        rig.engine.setArmedTracks ({});
        CHECK_FALSE (audible()); // not armed
        rig.engine.play (0);
        CHECK_FALSE (audible());
    }
    SECTION ("On")
    {
        monitor ("on");
        CHECK (audible());
        rig.engine.play (0);
        CHECK (audible());
        rig.engine.setArmedTracks ({ track });
        CHECK (audible());
    }
    SECTION ("Off")
    {
        monitor ("off");
        rig.engine.setArmedTracks ({ track });
        CHECK_FALSE (audible());
        REQUIRE (rig.engine.record (0).recording);
        CHECK_FALSE (audible());
    }
}

namespace
{
/** Records a stereo track over a one-bar cycle for `frames` and returns the track. */
Id recordCycle (Rig& rig, std::int64_t frames)
{
    recordSettings (rig);
    rampInputs (rig);
    const auto track = rig.audioTrack();
    REQUIRE (rig.session.setTrackMonitor (track, "off"));
    REQUIRE (rig.session.setCycle (true, 0, barTicks));
    rig.sync();
    rig.engine.setArmedTracks ({ track });
    REQUIRE (rig.engine.record (0).recording);
    rig.run (frames);
    const auto outcome = rig.finishRecording();
    REQUIRE (outcome);
    REQUIRE (outcome->committed);
    rig.engine.setArmedTracks ({});
    return track;
}

const model::TakeFolder& onlyFolder (const Rig& rig)
{
    REQUIRE (rig.session.model().takeFolders.size() == 1);
    return *rig.session.model().takeFolders.begin()->second;
}
} // namespace

TEST_CASE ("AT-REC-11: cycle recording makes one take folder with a take per pass", "[E][record][takes][AT-REC-11]")
{
    Rig rig;
    const auto track = recordCycle (rig, 7 * barSamples / 2); // 3.5 passes
    const auto& folder = onlyFolder (rig);
    CHECK (folder.trackId == track);
    CHECK (folder.start == 0);
    CHECK (folder.endTicks == barTicks);
    REQUIRE (folder.takeIds.size() == 4); // the half pass is over a beat long
    CHECK (folder.activeTakeId == folder.takeIds.back());

    const std::array<std::int64_t, 4> offsets { 0, barSamples, 2 * barSamples, 3 * barSamples };
    const std::array<std::int64_t, 4> lengths { barSamples, barSamples, barSamples, barSamples / 2 };
    std::set<std::string> files;
    for (std::size_t i = 0; i < 4; ++i)
    {
        const auto* take = rig.session.model().region (folder.takeIds[i]);
        REQUIRE (take != nullptr);
        CHECK (take->start == 0);
        CHECK (take->takeFolderId == folder.id);
        CHECK (take->audio.sourceOffset == offsets[i]); // + latency, which is 0 here
        CHECK (take->length == lengths[i]);
        files.insert (take->audio.media);
    }
    CHECK (files.size() == 1);
}

TEST_CASE ("AT-REC-12: a final partial pass under a beat is dropped", "[E][record][takes][AT-REC-12]")
{
    Rig rig;
    recordCycle (rig, 31 * barSamples / 10); // 3.1 passes: the last is 0.4 beat
    const auto& folder = onlyFolder (rig);
    REQUIRE (folder.takeIds.size() == 3);
    CHECK (folder.activeTakeId == folder.takeIds.back());
    CHECK (rig.session.model().region (folder.takeIds.back())->length == barSamples);
}

TEST_CASE ("AT-REC-13: selecting a take makes it the one that plays", "[E][record][takes][AT-REC-13]")
{
    Rig rig;
    recordCycle (rig, 7 * barSamples / 2);
    REQUIRE (rig.session.setCycle (false, 0, barTicks));
    rig.sync();
    rig.engine.play (0);
    auto out = rig.run (barSamples / 2);
    CHECK (out[1000] == inValue (0, 3 * barSamples + 1000)); // take 4 is active

    const auto folder = onlyFolder (rig);
    REQUIRE (rig.session.setActiveTake (folder.id, folder.takeIds[1]));
    CHECK (rig.session.model().takeFolders.at (folder.id)->activeTakeId == folder.takeIds[1]);
    rig.sync();
    rig.engine.play (0);
    out = rig.run (barSamples / 2);
    CHECK (out[1000] == inValue (0, barSamples + 1000)); // take 2 only
    CHECK (out[30000] == inValue (0, barSamples + 30000));
}

TEST_CASE ("AT-REC-16: autopunch records the punch range with faded 10 ms handles", "[E][record][punch][AT-REC-16]")
{
    Rig rig;
    recordSettings (rig);
    constantInputs (rig, 0.25f);
    const auto track = rig.audioTrack();
    REQUIRE (rig.session.setTrackMonitor (track, "off"));
    rig.addAudioRegion (track, 0, 6 * barSamples);
    REQUIRE (rig.session.setPunch (true, 2 * barTicks, 3 * barTicks)); // bar 3 to bar 4
    rig.sync();
    rig.engine.setArmedTracks ({ track });

    REQUIRE (rig.engine.record (0).recording);
    rig.run (5 * barSamples); // bar 1 to bar 6
    REQUIRE (rig.finishRecording());
    const auto regions = rig.regionsOn (track);
    REQUIRE (regions.size() == 2);
    const auto* punched = topmost (regions);
    const auto handle = 480; // 10 ms
    const auto startSample = punched->start * 25;
    CHECK (std::abs (startSample - (2 * barSamples - handle)) < 25); // within a tick
    CHECK (startSample + punched->length == 3 * barSamples + handle);
    CHECK (punched->audio.fadeIn.length == handle);
    CHECK (punched->audio.fadeOut.length == handle);

    rig.engine.setArmedTracks ({});
    rig.engine.play (0);
    const auto out = rig.run (5 * barSamples);
    CHECK (out[100000] == rampValue (100000));                  // before the punch: the original
    CHECK (out[240000] == 0.25f);                               // inside: the new recording
    CHECK (out[3 * barSamples + 2000] == rampValue (3 * barSamples + 2000)); // after it
    // Halfway through the fade-in the two cross at equal power.
    const auto mid = startSample + handle / 2;
    const float g = std::sin (std::numbers::pi_v<float> / 4.0f);
    CHECK (out[static_cast<std::size_t> (mid)] == Catch::Approx (g * 0.25f + g * rampValue (mid)).margin (1e-3));
}

TEST_CASE ("AT-REC-17: an armed track meters its input while stopped", "[E][record][meter][AT-REC-17]")
{
    Rig rig;
    sineInputs (rig);
    const auto track = inputTrack (rig, "Audio 1", 1, 0);
    rig.engine.setArmedTracks ({ track });
    rig.run (4800);
    const auto m = rig.engine.takeMeter (track);
    CHECK (20.0 * std::log10 (m.input) == Catch::Approx (-6.0).margin (0.1));
    CHECK (rig.engine.takeMeter (track).input == 0.0f); // read resets the peak
}

TEST_CASE ("Recording stops but playback continues on R while recording (D-W1-2)", "[E][record]")
{
    Rig rig;
    recordSettings (rig);
    constantInputs (rig, 0.25f);
    const auto track = inputTrack (rig, "Audio 1", 1, 0);
    rig.engine.setArmedTracks ({ track });
    REQUIRE (rig.engine.record (0).recording);
    rig.run (barSamples);
    rig.engine.stopRecording();
    rig.run (1000);
    CHECK (rig.engine.state() == TransportState::playing);
    const auto outcome = rig.engine.finishRecording (rig.session);
    REQUIRE (outcome);
    CHECK (outcome->committed);
    REQUIRE (rig.regionsOn (track).size() == 1);
    CHECK (rig.regionsOn (track)[0]->length == barSamples);
}

TEST_CASE ("A track deleted during recording gets a recovered track (D-W1-6)", "[E][record]")
{
    Rig rig;
    recordSettings (rig);
    constantInputs (rig, 0.25f);
    const auto track = inputTrack (rig, "Vox", 1, 0);
    rig.engine.setArmedTracks ({ track });
    REQUIRE (rig.engine.record (0).recording);
    rig.run (barSamples);
    REQUIRE (rig.session.deleteTrack (track)); // a collaborator's delete, in effect
    rig.sync();
    const auto outcome = rig.finishRecording();
    REQUIRE (outcome);
    CHECK (outcome->committed);
    const model::Track* recovered = nullptr;
    for (const auto& [id, t] : rig.session.model().tracks)
        if (t->name == "Vox (recovered)")
            recovered = t.get();
    REQUIRE (recovered != nullptr);
    CHECK (rig.regionsOn (recovered->id).size() == 1);
}

// MIDI recording --------------------------------------------------------------------------

TEST_CASE ("AT-REC-7: a played note is recorded at its tick into a bar-long region", "[E][record][midi][AT-REC-7]")
{
    Rig rig;
    recordSettings (rig);
    auto [keys, probe] = probeTrack (rig);
    rig.engine.setArmedTracks ({ keys });
    const auto base = rig.engine.deviceFrames();

    REQUIRE (rig.engine.record (0).recording);
    REQUIRE (rig.engine.sendMidi (0x90, 60, 100, base + 48000));
    REQUIRE (rig.engine.sendMidi (0x80, 60, 0, base + 72000));
    rig.run (barSamples);
    const auto outcome = rig.finishRecording();
    REQUIRE (outcome);
    CHECK (outcome->committed);

    const auto regions = rig.regionsOn (keys);
    REQUIRE (regions.size() == 1);
    CHECK (regions[0]->start == 0);
    CHECK (regions[0]->length == barTicks);
    const auto notes = notesOf (rig, *regions[0]);
    REQUIRE (notes.size() == 1);
    CHECK (notes[0]->pos == 1920);
    CHECK (notes[0]->length == 960);
    CHECK (notes[0]->pitch == 60);
    CHECK (notes[0]->velocity == 100);

    // The instrument heard it live, on time.
    const auto heard = std::find_if (probe->seen.begin(), probe->seen.end(), [] (const auto& s) { return s.event.isNoteOn(); });
    REQUIRE (heard != probe->seen.end());
    CHECK (heard->frame == base + 48000);
}

TEST_CASE ("AT-REC-8: held notes and pedal close at stop; controllers keep their channel", "[E][record][midi][AT-REC-8]")
{
    Rig rig;
    recordSettings (rig);
    auto [keys, probe] = probeTrack (rig);
    rig.engine.setArmedTracks ({ keys });
    const auto base = rig.engine.deviceFrames();
    REQUIRE (rig.engine.record (0).recording);
    rig.engine.sendMidi (0x90, 60, 90, base + 24000);
    rig.engine.sendMidi (0xB0, 64, 127, base + 30000);
    rig.engine.sendMidi (0xE2, 0, 64, base + 40000); // pitch bend centre, channel 3
    rig.engine.sendMidi (0xB1, 7, 90, base + 41000); // volume, channel 2
    rig.run (72000);
    REQUIRE (rig.finishRecording());

    const auto regions = rig.regionsOn (keys);
    REQUIRE (regions.size() == 1);
    const auto notes = notesOf (rig, *regions[0]);
    REQUIRE (notes.size() == 1);
    CHECK (notes[0]->pos == 960);
    CHECK (notes[0]->length == 1920); // closed at the stop position

    const auto* seq = rig.session.model().sequence (regions[0]->midi.sequenceId);
    std::vector<model::CcEvent> ccs;
    for (const auto& [id, c] : seq->controllers)
        ccs.push_back (c);
    std::sort (ccs.begin(), ccs.end(), [] (const auto& a, const auto& b) { return a.pos < b.pos; });
    REQUIRE (ccs.size() == 4);
    CHECK ((ccs[0].kind == "cc" && ccs[0].number == 64 && ccs[0].value == 127 && ccs[0].pos == 1200 && ccs[0].channel == 1));
    CHECK ((ccs[1].kind == "pitchBend" && ccs[1].value == 0 && ccs[1].channel == 3 && ccs[1].pos == 1600));
    CHECK ((ccs[2].kind == "cc" && ccs[2].number == 7 && ccs[2].value == 90 && ccs[2].channel == 2 && ccs[2].pos == 1640));
    CHECK ((ccs[3].kind == "cc" && ccs[3].number == 64 && ccs[3].value == 0 && ccs[3].pos == 2880 && ccs[3].channel == 1));
}

TEST_CASE ("AT-REC-9: recording MIDI with nothing played creates no region", "[E][record][midi][AT-REC-9]")
{
    Rig rig;
    recordSettings (rig);
    auto [keys, probe] = probeTrack (rig);
    rig.engine.setArmedTracks ({ keys });
    REQUIRE (rig.engine.record (0).recording);
    rig.run (barSamples);
    const auto outcome = rig.finishRecording();
    REQUIRE (outcome);
    CHECK_FALSE (outcome->committed);
    CHECK (rig.regionsOn (keys).empty());
    CHECK (rig.session.undoName() != "Record");
}

TEST_CASE ("AT-REC-14: cycle MIDI recording merges every pass into one region", "[E][record][midi][AT-REC-14]")
{
    Rig rig;
    recordSettings (rig);
    auto [keys, probe] = probeTrack (rig);
    REQUIRE (rig.session.setCycle (true, 0, barTicks));
    rig.sync();
    rig.engine.setArmedTracks ({ keys });
    const auto base = rig.engine.deviceFrames();
    REQUIRE (rig.engine.record (0).recording);
    const auto hit = [&] (int pitch, std::int64_t at) {
        rig.engine.sendMidi (0x90, static_cast<std::uint8_t> (pitch), 100, base + at);
        rig.engine.sendMidi (0x80, static_cast<std::uint8_t> (pitch), 0, base + at + 1000);
    };
    hit (36, 0);                       // pass 1: kick on beats 1 and 3
    hit (36, 48000);
    hit (38, barSamples + 24000);      // pass 2: snare on beats 2 and 4
    hit (38, barSamples + 72000);
    rig.run (2 * barSamples);
    REQUIRE (rig.finishRecording());

    const auto regions = rig.regionsOn (keys);
    REQUIRE (regions.size() == 1);
    CHECK (regions[0]->start == 0);
    CHECK (regions[0]->length == barTicks);
    const auto notes = notesOf (rig, *regions[0]);
    REQUIRE (notes.size() == 4);
    CHECK ((notes[0]->pos == 0 && notes[0]->pitch == 36));
    CHECK ((notes[1]->pos == 960 && notes[1]->pitch == 38));
    CHECK ((notes[2]->pos == 1920 && notes[2]->pitch == 36));
    CHECK ((notes[3]->pos == 2880 && notes[3]->pitch == 38));
}

TEST_CASE ("AT-REC-15: linear MIDI recording replaces by default and merges on request", "[E][record][midi][AT-REC-15]")
{
    const auto run = [] (bool merge) {
        Rig rig;
        recordSettings (rig, false, merge);
        auto [keys, probe] = probeTrack (rig);
        const auto existing = rig.session.createMidiRegion (keys, 0, barTicks).value();
        model::Note n;
        n.pos = 0;
        n.pitch = 50;
        REQUIRE (rig.session.addNote (existing, n));
        rig.sync();
        rig.engine.setArmedTracks ({ keys });
        const auto base = rig.engine.deviceFrames();
        REQUIRE (rig.engine.record (0).recording);
        rig.engine.sendMidi (0x90, 60, 100, base + 48000);
        rig.engine.sendMidi (0x80, 60, 0, base + 72000);
        rig.run (barSamples);
        REQUIRE (rig.finishRecording());
        return std::make_tuple (rig.regionsOn (keys).size(), existing, [&] {
            std::vector<std::pair<Id, std::size_t>> counts;
            for (const auto* r : rig.regionsOn (keys))
                counts.emplace_back (r->id, notesOf (rig, *r).size());
            return counts;
        }());
    };

    SECTION ("Replace")
    {
        const auto [count, existing, notes] = run (false);
        CHECK (count == 2);
        for (const auto& [id, n] : notes)
            CHECK (n == 1); // the old region keeps its note, the new one has the played note
    }
    SECTION ("Merge")
    {
        const auto [count, existing, notes] = run (true);
        REQUIRE (count == 1);
        CHECK (notes[0].first == existing);
        CHECK (notes[0].second == 2);
    }
}

TEST_CASE ("AT-REC-20: the computer keyboard plays and records notes", "[E][record][midi][AT-REC-20]")
{
    KeyboardMidi kb;
    std::vector<KeyboardMidi::Message> out;
    CHECK_FALSE (kb.keyDown ('a', false, out)); // mode off: the key keeps its normal command
    CHECK (out.empty());

    kb.toggle (out);
    REQUIRE (kb.enabled());
    REQUIRE (kb.keyDown ('a', false, out));
    REQUIRE (out.size() == 1);
    CHECK ((out[0].status == 0x90 && out[0].data1 == 60 && out[0].data2 == 100));
    CHECK (kb.keyDown ('a', true, out)); // auto-repeat: used, but no retrigger
    CHECK (out.size() == 1);
    REQUIRE (kb.keyUp ('a', out));
    CHECK ((out[1].status == 0x80 && out[1].data1 == 60));
    out.clear();

    kb.keyDown ('K', false, out);
    CHECK (out.back().data1 == 72);
    kb.keyUp ('k', out);
    kb.keyDown ('x', false, out); // octave up
    kb.keyDown ('a', false, out);
    CHECK (out.back().data1 == 72);
    kb.keyDown ('z', false, out);
    kb.keyUp ('a', out);
    CHECK ((out.back().status == 0x80 && out.back().data1 == 72)); // releases the note it started
    kb.keyDown ('c', false, out); // velocity -20
    kb.keyDown ('a', false, out);
    CHECK ((out.back().data1 == 60 && out.back().data2 == 80));
    kb.keyUp ('a', out);
    for (int i = 0; i < 10; ++i)
        kb.keyDown ('c', false, out);
    CHECK (kb.velocity() == 1);
    for (int i = 0; i < 10; ++i)
        kb.keyDown ('v', false, out);
    CHECK (kb.velocity() == 127);
    CHECK_FALSE (kb.keyDown ('1', false, out)); // number keys keep jumping to markers

    // Played notes reach the engine and are recorded.
    Rig rig;
    recordSettings (rig);
    auto [keys, probe] = probeTrack (rig);
    rig.engine.setSelectedTrack (keys);
    rig.engine.setArmedTracks ({ keys });
    REQUIRE (rig.engine.record (0).recording);
    out.clear();
    kb.keyDown ('a', false, out);
    for (const auto& m : out)
        rig.engine.sendMidi (m.status, m.data1, m.data2);
    rig.run (24000);
    out.clear();
    kb.keyUp ('a', out);
    for (const auto& m : out)
        rig.engine.sendMidi (m.status, m.data1, m.data2);
    rig.run (24000);
    REQUIRE (rig.finishRecording());
    const auto regions = rig.regionsOn (keys);
    REQUIRE (regions.size() == 1);
    const auto notes = notesOf (rig, *regions[0]);
    REQUIRE (notes.size() == 1);
    CHECK (notes[0]->pitch == 60);
    CHECK (notes[0]->velocity == 127);
    CHECK (notes[0]->pos == 0);
    CHECK (notes[0]->length == 960);
}
