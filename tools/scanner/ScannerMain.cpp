// weft-plugin-scanner: loads one plug-in file in its own process and reports the plug-ins
// in it (F-PLG-2). If a plug-in crashes or hangs here, only this process dies; the app
// marks the file as failed.
//
//   weft-plugin-scanner --scan <file.vst3> --out <result file>
//
// Each working class is a line in the result file (see scannerLine), then "DONE".

#include "weft/engine/plugins/PluginCatalog.h"
#include "weft/engine/plugins/Vst3Host.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <iostream>

namespace
{
int scan (const juce::String& path, const juce::File& out)
{
    using namespace weft::engine;
    std::string error;
    const auto classes = readVst3Classes (path.toStdString(), &error);
    if (classes.empty())
    {
        std::cerr << (error.empty() ? "no plug-in classes" : error) << "\n";
        return out.replaceWithText ("DONE\n") ? 0 : 3;
    }

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile (types, path);
    juce::StringArray lines;
    for (auto* desc : types)
    {
        // Make it, run a block of silence through it, and let it go: what the host will do.
        juce::String message;
        auto instance = format.createInstanceFromDescription (*desc, 48000.0, 512, message);
        if (! instance)
        {
            std::cerr << desc->name << ": " << message << "\n";
            continue;
        }
        instance->enableAllBuses();
        instance->prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buffer (std::max ({ 2, instance->getTotalNumInputChannels(), instance->getTotalNumOutputChannels() }), 512);
        buffer.clear();
        juce::MidiBuffer midi;
        instance->processBlock (buffer, midi);
        instance->releaseResources();

        PluginInfo info;
        for (const auto& c : classes)
            if (c.name == desc->name.toStdString())
                info.uid = c.uid;
        if (info.uid.empty() && classes.size() == 1)
            info.uid = classes.front().uid;
        if (info.uid.empty())
            continue;
        info.name = desc->name.toStdString();
        info.vendor = desc->manufacturerName.toStdString();
        info.version = desc->version.toStdString();
        info.category = desc->category.toStdString();
        info.instrument = desc->isInstrument;
        lines.add (scannerLine (info));
    }
    lines.add ("DONE");
    return out.replaceWithText (lines.joinIntoString ("\n") + "\n") ? 0 : 3;
}
} // namespace

int main (int argc, char* argv[])
{
    juce::String path;
    juce::File out;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (juce::String (argv[i]) == "--scan")
            path = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (argv[++i])).getFullPathName();
        else if (juce::String (argv[i]) == "--out")
            out = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (argv[++i]));
    }
    if (path.isEmpty() || out == juce::File())
    {
        std::cerr << "usage: weft-plugin-scanner --scan <file.vst3> --out <result file>\n";
        return 64;
    }
    const juce::ScopedJuceInitialiser_GUI juce;
    return scan (path, out);
}
