// AT-EXP-2: bounced WAVs at every bit depth and sample rate read back through an independent
// decoder (JUCE's WAV reader) with the right header and length.

#include "EngineFixtures.h"

#include "weft/engine/media/JuceDecoders.h"
#include "weft/engine/render/Bounce.h"
#include "weft/media/Resample.h"

#include <juce_audio_formats/juce_audio_formats.h>

using namespace weft;
using namespace weft::test;

TEST_CASE ("AT-EXP-2 WAV 16/24/32-float at 44.1/48/96 kHz read back with the right header and length", "[E][bounce]")
{
    Rig rig;
    rig.addAudioRegion (rig.audioTrack(), 0, 5 * barSamples);
    TempFolder out;
    juce::WavAudioFormat wav;
    for (const int bits : { 16, 24, 32 })
        for (const double rate : { 44100.0, 48000.0, 96000.0 })
        {
            DYNAMIC_SECTION (bits << " bit, " << rate << " Hz")
            {
                BounceSettings s;
                s.range = { 0, 4 * barTicks };
                s.includeTail = false;
                s.wav = WavFormat { bits, rate };
                s.folder = out.path;
                s.name = "Mix " + std::to_string (bits) + " " + std::to_string (static_cast<int> (rate));
                BounceJob job (rig.session.model(), s, { &rig.media, {}, rig.folder.path, nullptr, {} });
                job.render();
                const auto result = job.finish();
                REQUIRE (result.ok);
                auto stream = std::make_unique<juce::FileInputStream> (juce::File (juce::String (result.files[0].u8string().c_str())));
                REQUIRE (stream->openedOk());
                std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (stream.release(), true));
                REQUIRE (reader != nullptr);
                CHECK (reader->sampleRate == rate);
                CHECK (reader->numChannels == 2);
                CHECK (static_cast<int> (reader->bitsPerSample) == bits);
                CHECK (reader->usesFloatingPointData == (bits == 32));
                const auto expected = media::resampledLength (384000, 48000.0, rate);
                CHECK (std::abs (reader->lengthInSamples - expected) <= 1);
                if (rate == 44100.0)
                    CHECK (std::abs (reader->lengthInSamples - 352800) <= 1);
                // The content survives: the decoder's samples match the bounce's mix at 48 kHz.
                if (rate == 48000.0)
                {
                    juce::AudioBuffer<float> buffer (2, 4096);
                    REQUIRE (reader->read (&buffer, 0, 4096, 10000, true, true));
                    const float lsb = bits == 16 ? 1.6f / 32768.0f : 1.01f / 8388608.0f; // 16-bit carries dither
                    for (int i = 0; i < 4096; i += 97)
                        CHECK (std::abs (buffer.getSample (0, i) - job.mix().channels[0][static_cast<std::size_t> (10000 + i)]) <= lsb);
                }
            }
        }
}

TEST_CASE ("AT-EXP-16 AIFF and FLAC bounces read back at 16 and 24 bits (32 writes 24); the MP3 tag carries the tempo", "[E][bounce][AT-EXP-16]")
{
    Rig rig;
    rig.addAudioRegion (rig.audioTrack(), 0, 2 * barSamples);
    TempFolder out;
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    for (const int bits : { 16, 24, 32 })
    {
        BounceSettings s;
        s.range = { 0, barTicks };
        s.includeTail = false;
        s.wav.reset();
        s.aiff = WavFormat { bits, 0 };
        s.flac = WavFormat { bits, 0 };
        s.folder = out.path;
        s.name = "Mix " + std::to_string (bits);
        BounceJob::Context ctx { &rig.media, {}, rig.folder.path, nullptr, {} };
        ctx.encoder = makePlatformAudioEncoder();
        BounceJob job (rig.session.model(), s, ctx);
        job.render();
        const auto result = job.finish();
        REQUIRE (result.ok);
        REQUIRE (result.files.size() == 2);
        CHECK (result.files[0].extension() == ".aif");
        CHECK (result.files[1].extension() == ".flac");
        for (const auto& file : result.files)
        {
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File (juce::String (file.u8string().c_str()))));
            REQUIRE (reader != nullptr);
            CHECK (reader->numChannels == 2);
            CHECK (reader->lengthInSamples == barSamples);
            CHECK (static_cast<int> (reader->bitsPerSample) == std::min (bits, 24));
            CHECK (reader->sampleRate == 48000.0);
        }
    }
    // Without an encoder the bounce says it can't write the format.
    BounceSettings s;
    s.range = { 0, barTicks };
    s.wav.reset();
    s.flac = WavFormat {};
    s.folder = out.path;
    s.name = "No encoder";
    BounceJob job (rig.session.model(), s, { &rig.media, {}, rig.folder.path, nullptr, {} });
    job.render();
    const auto failed = job.finish();
    CHECK_FALSE (failed.ok);
    CHECK (failed.error.find ("can't write flac") != std::string::npos);

    // Include Tempo Information writes the tempo at the range start as the tag's BPM.
    REQUIRE (rig.session.setTempoAt (0, 96.0));
    rig.sync();
    s.flac.reset();
    s.mp3 = Mp3Format {};
    s.name = "Tagged";
    BounceJob mp3Job (rig.session.model(), s, { &rig.media, {}, rig.folder.path, nullptr, {} });
    mp3Job.render();
    const auto tagged = mp3Job.finish();
    REQUIRE (tagged.ok);
    juce::MemoryBlock data;
    REQUIRE (juce::File (juce::String (tagged.files.at (0).u8string().c_str())).loadFileAsData (data));
    std::string bytes (static_cast<const char*> (data.getData()), std::min<std::size_t> (200, data.getSize()));
    CHECK (bytes.find (std::string ("TBPM")) != std::string::npos);
    CHECK (bytes.find (std::string ("\x03" "96", 3)) != std::string::npos);
}
