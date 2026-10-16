// AT-EXP-2: bounced WAVs at every bit depth and sample rate read back through an independent
// decoder (JUCE's WAV reader) with the right header and length.

#include "EngineFixtures.h"

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
