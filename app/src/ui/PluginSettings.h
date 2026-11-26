#pragma once

// Plug-in settings (D-W6-12): a plug-in's parameters and state chunk saved under a name, kept on this
// computer per plug-in (not in the project), so any project can load them.

#include "weft/model/Model.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace weft::ui
{
struct PluginSetting
{
    std::string name;
    std::map<std::string, double> params; // ParamId -> normalized 0..1
    std::vector<std::uint8_t> state;      // the plug-in's own chunk

    bool operator== (const PluginSetting& o) const { return params == o.params && state == o.state; }
};

namespace plugin_settings
{
/** The folder that holds a plug-in's settings, under `root` (Studio::settingsFolder() / "Plug-in Settings"). */
std::filesystem::path folder (const std::filesystem::path& root, const model::PluginInstance& plugin);
/** The saved settings' names, sorted without regard to case; the default isn't listed. */
std::vector<std::string> names (const std::filesystem::path& root, const model::PluginInstance& plugin);
/** Writes a setting under its name (replacing one of that name). `asDefault` writes the plug-in's default instead. */
bool save (const std::filesystem::path& root, const model::PluginInstance& plugin, const PluginSetting& setting, bool asDefault, std::string* error = nullptr);
std::optional<PluginSetting> load (const std::filesystem::path& root, const model::PluginInstance& plugin, const std::string& name);
std::optional<PluginSetting> loadDefault (const std::filesystem::path& root, const model::PluginInstance& plugin);
bool remove (const std::filesystem::path& root, const model::PluginInstance& plugin, const std::string& name);
/** A name that can be a file name: trimmed, without / \ : * ? " < > |; empty when nothing is left. */
std::string cleanName (const std::string& name);
} // namespace plugin_settings
} // namespace weft::ui
