#include "PluginSettings.h"

#include <juce_core/juce_core.h>

#include <algorithm>

namespace weft::ui::plugin_settings
{
namespace
{
constexpr const char* extension = ".setting";
constexpr const char* defaultFile = "default.setting-default";

juce::File fileIn (const std::filesystem::path& dir, const std::string& leaf)
{
    return juce::File (juce::String (dir.string())).getChildFile (juce::String (juce::CharPointer_UTF8 (leaf.c_str())));
}

std::optional<PluginSetting> read (const juce::File& file, const std::string& name)
{
    if (! file.existsAsFile())
        return std::nullopt;
    const auto json = juce::JSON::parse (file.loadFileAsString());
    auto* object = json.getDynamicObject();
    if (object == nullptr)
        return std::nullopt;
    PluginSetting s;
    s.name = name;
    if (const auto* params = object->getProperty ("params").getArray())
        for (const auto& p : *params)
            if (const auto id = p["id"].toString(); id.isNotEmpty())
                s.params[id.toStdString()] = std::clamp (static_cast<double> (p["value"]), 0.0, 1.0);
    juce::MemoryOutputStream bytes;
    if (juce::Base64::convertFromBase64 (bytes, object->getProperty ("state").toString()))
    {
        const auto* data = static_cast<const std::uint8_t*> (bytes.getData());
        s.state.assign (data, data + bytes.getDataSize());
    }
    return s;
}
} // namespace

std::string cleanName (const std::string& name)
{
    auto text = juce::String (juce::CharPointer_UTF8 (name.c_str())).removeCharacters ("/\\:*?\"<>|").trim();
    while (text.startsWithChar ('.'))
        text = text.substring (1).trim();
    return text.substring (0, 120).toStdString();
}

std::filesystem::path folder (const std::filesystem::path& root, const model::PluginInstance& plugin)
{
    const auto leaf = juce::File::createLegalFileName (juce::String (plugin.format + "-" + plugin.pluginUid));
    return root / leaf.toStdString();
}

std::vector<std::string> names (const std::filesystem::path& root, const model::PluginInstance& plugin)
{
    std::vector<std::string> out;
    const auto dir = juce::File (juce::String (folder (root, plugin).string()));
    for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, juce::String ("*") + extension))
        out.push_back (f.getFileNameWithoutExtension().toStdString());
    std::sort (out.begin(), out.end(), [] (const std::string& a, const std::string& b) {
        return juce::String (juce::CharPointer_UTF8 (a.c_str())).compareIgnoreCase (juce::String (juce::CharPointer_UTF8 (b.c_str()))) < 0;
    });
    return out;
}

bool save (const std::filesystem::path& root, const model::PluginInstance& plugin, const PluginSetting& setting, bool asDefault, std::string* error)
{
    const auto name = cleanName (setting.name);
    if (! asDefault && name.empty())
    {
        if (error != nullptr)
            *error = "Give the setting a name.";
        return false;
    }
    juce::Array<juce::var> params; // [{ "id", "value" }]: parameter ids needn't be identifiers
    for (const auto& [id, v] : setting.params)
    {
        auto* entry = new juce::DynamicObject();
        entry->setProperty ("id", juce::String (juce::CharPointer_UTF8 (id.c_str())));
        entry->setProperty ("value", v);
        params.add (juce::var (entry));
    }
    auto* object = new juce::DynamicObject();
    object->setProperty ("plugin", juce::String (plugin.name));
    object->setProperty ("format", juce::String (plugin.format));
    object->setProperty ("uid", juce::String (plugin.pluginUid));
    object->setProperty ("params", params);
    object->setProperty ("state", juce::Base64::toBase64 (setting.state.data(), setting.state.size()));
    const auto dir = folder (root, plugin);
    std::error_code ec;
    std::filesystem::create_directories (dir, ec);
    const auto file = fileIn (dir, asDefault ? std::string (defaultFile) : name + extension);
    if (! file.replaceWithText (juce::JSON::toString (juce::var (object))))
    {
        if (error != nullptr)
            *error = "Couldn't write " + file.getFullPathName().toStdString() + ".";
        return false;
    }
    return true;
}

std::optional<PluginSetting> load (const std::filesystem::path& root, const model::PluginInstance& plugin, const std::string& name)
{
    const auto clean = cleanName (name);
    if (clean.empty())
        return std::nullopt;
    return read (fileIn (folder (root, plugin), clean + extension), clean);
}

std::optional<PluginSetting> loadDefault (const std::filesystem::path& root, const model::PluginInstance& plugin)
{
    return read (fileIn (folder (root, plugin), defaultFile), "Default");
}

bool remove (const std::filesystem::path& root, const model::PluginInstance& plugin, const std::string& name)
{
    const auto clean = cleanName (name);
    return ! clean.empty() && fileIn (folder (root, plugin), clean + extension).deleteFile();
}
} // namespace weft::ui::plugin_settings
