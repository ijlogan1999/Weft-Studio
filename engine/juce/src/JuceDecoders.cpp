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

class JuceAudioEncoder final : public AudioEncoder
{
public:
    std::vector<std::string> extensions() const override { return { ".aif", ".aiff", ".flac" }; }

    bool encode (const std::filesystem::path& file, const media::AudioData& audio, int bitDepth, std::string& error) const override
    {
        const juce::File f (juce::String::fromUTF8 (reinterpret_cast<const char*> (file.u8string().c_str())));
        const auto ext = media::lowerExtension (file);
        std::unique_ptr<juce::AudioFormat> format;
        if (ext == ".flac")
            format = std::make_unique<juce::FlacAudioFormat>();
        else if (ext == ".aif" || ext == ".aiff")
            format = std::make_unique<juce::AiffAudioFormat>();
        else
        {
            error = "unknown format " + ext;
            return false;
        }
        f.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = f.createOutputStream();
        if (stream == nullptr)
        {
            error = "the file couldn't be created";
            return false;
        }
        const auto channels = static_cast<int> (audio.channels.size());
        const auto options = juce::AudioFormatWriterOptions {}
                                 .withSampleRate (audio.sampleRate)
                                 .withNumChannels (channels)
                                 .withBitsPerSample (bitDepth);
        auto writer = format->createWriterFor (stream, options);
        if (writer == nullptr)
        {
            error = "this format can't be written at " + std::to_string (bitDepth) + " bits and " + std::to_string (audio.sampleRate) + " Hz";
            return false;
        }
        std::vector<const float*> pointers;
        for (const auto& c : audio.channels)
            pointers.push_back (c.data());
        // In slices: JUCE takes an int frame count.
        const auto frames = static_cast<std::int64_t> (audio.numFrames());
        constexpr std::int64_t slice = 1 << 20;
        for (std::int64_t at = 0; at < frames; at += slice)
        {
            std::vector<const float*> from;
            for (const auto* p : pointers)
                from.push_back (p + at);
            if (! writer->writeFromFloatArrays (from.data(), channels, static_cast<int> (std::min (slice, frames - at))))
            {
                error = "writing failed";
                return false;
            }
        }
        writer.reset(); // finishes the header
        return true;
    }
};
} // namespace

std::shared_ptr<media::AudioDecoder> makePlatformAudioDecoder() { return std::make_shared<JuceAudioDecoder>(); }

std::shared_ptr<const AudioEncoder> makePlatformAudioEncoder() { return std::make_shared<JuceAudioEncoder>(); }

media::DecoderSet importDecoders()
{
    media::DecoderSet set;
    set.add (makePlatformAudioDecoder());
    set.add (makeMp3Decoder()); // tried first: it trims the encoder delay; the platform one is the fallback
    return set;
}
} // namespace weft::engine
