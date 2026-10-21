#include "weft/engine/media/JuceDecoders.h"

#include "weft/engine/media/Mp3Decoder.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace weft::engine
{
namespace
{
class JuceAudioDecoder final : public media::AudioDecoder
{
public:
    JuceAudioDecoder()
    {
        formats.registerFormat (new juce::AiffAudioFormat(), false);
        formats.registerFormat (new juce::FlacAudioFormat(), false);
        formats.registerFormat (new juce::MP3AudioFormat(), false);
    }

    std::vector<std::string> extensions() const override { return { ".aif", ".aiff", ".aifc", ".flac", ".mp3" }; }

    std::optional<media::AudioData> decode (const std::filesystem::path& file, std::string* error) const override
    {
        const juce::File f (juce::String (file.u8string().c_str()));
        std::unique_ptr<juce::AudioFormatReader> reader (const_cast<juce::AudioFormatManager&> (formats).createReaderFor (f));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels == 0)
        {
            if (error != nullptr)
                *error = "The file couldn't be decoded.";
            return std::nullopt;
        }
        const auto frames = static_cast<int> (std::min<juce::int64> (reader->lengthInSamples, std::numeric_limits<int>::max()));
        juce::AudioBuffer<float> buffer (static_cast<int> (reader->numChannels), frames);
        if (! reader->read (&buffer, 0, frames, 0, true, true))
        {
            if (error != nullptr)
                *error = "The file couldn't be decoded.";
            return std::nullopt;
        }
        media::AudioData out;
        out.sampleRate = reader->sampleRate;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            out.channels.emplace_back (buffer.getReadPointer (ch), buffer.getReadPointer (ch) + frames);
        return out;
    }

private:
    juce::AudioFormatManager formats;
};
} // namespace

std::shared_ptr<media::AudioDecoder> makePlatformAudioDecoder() { return std::make_shared<JuceAudioDecoder>(); }

media::DecoderSet importDecoders()
{
    media::DecoderSet set;
    set.add (makePlatformAudioDecoder());
    set.add (makeMp3Decoder()); // tried first: it trims the encoder delay; the platform one is the fallback
    return set;
}
} // namespace weft::engine
