#pragma once

// Hosting VST3 plug-ins through JUCE (F-PLG-1). This header has no JUCE types; the
// editor windows go through HostedPlugin.h.

#include "weft/engine/graph/Instrument.h"
#include "weft/engine/plugins/PluginCatalog.h"
#include "weft/model/Model.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace weft::engine
{
/** A class in a VST3 module's factory, read straight from the module (no instance made). */
struct Vst3Class
{
    std::string uid; // 32 hex digits
    std::string name, category;
};
/** The audio-module classes of a .vst3 bundle or file. Loads the module into this process. */
std::vector<Vst3Class> readVst3Classes (const std::filesystem::path& file, std::string* error = nullptr);

class Vst3Host
{
public:
    explicit Vst3Host (const PluginCatalog& catalog);
    ~Vst3Host();

    /** Loads the plug-in for an instance (message thread). Null when it isn't in the catalog or won't load. */
    std::shared_ptr<Processor> create (const model::PluginInstance& plugin, bool instrument, std::string* error = nullptr);
    /** Loads a plug-in from a file by class (tests and the scanner). */
    std::shared_ptr<Processor> createFromFile (const std::filesystem::path& file, const std::string& name, std::string* error = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    const PluginCatalog& catalog;
};
} // namespace weft::engine
