#include "weft/engine/plugins/PluginCatalog.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <set>
#include <sstream>

namespace weft::engine
{
namespace
{
std::string clean (std::string s)
{
    std::replace_if (s.begin(), s.end(), [] (char c) { return c == '\t' || c == '\n' || c == '\r'; }, ' ');
    return s;
}

const char* statusName (PluginStatus s)
{
    switch (s)
    {
        case PluginStatus::disabled: return "disabled";
        case PluginStatus::failed: return "failed";
        case PluginStatus::ok: break;
    }
    return "ok";
}

PluginStatus statusFrom (const juce::String& s)
{
    if (s == "disabled")
        return PluginStatus::disabled;
    if (s == "failed")
        return PluginStatus::failed;
    return PluginStatus::ok;
}

std::int64_t modified (const std::filesystem::path& p)
{
    return juce::File (juce::String (p.string())).getLastModificationTime().toMilliseconds();
}
} // namespace

std::string scannerLine (const PluginInfo& i)
{
    return "PLUGIN\t" + clean (i.uid) + "\t" + clean (i.name) + "\t" + clean (i.vendor) + "\t" + clean (i.version) + "\t" + clean (i.category) + "\t"
         + (i.instrument ? "1" : "0");
}

std::optional<PluginInfo> parseScannerLine (const std::string& line)
{
    std::vector<std::string> fields;
    std::stringstream in (line);
    for (std::string f; std::getline (in, f, '\t');)
        fields.push_back (f);
    if (fields.size() != 7 || fields[0] != "PLUGIN" || fields[1].size() != 32)
        return std::nullopt;
    PluginInfo info;
    info.uid = fields[1];
    info.name = fields[2];
    info.vendor = fields[3];
    info.version = fields[4];
    info.category = fields[5];
    info.instrument = fields[6] == "1";
    return info;
}

PluginCatalog::PluginCatalog (std::filesystem::path file) : storage (std::move (file)) {}

bool PluginCatalog::load()
{
    entries.clear();
    if (storage.empty())
        return false;
    const auto json = juce::JSON::parse (juce::File (juce::String (storage.string())));
    const auto* list = json.getProperty ("plugins", {}).getArray();
    if (list == nullptr)
        return false;
    for (const auto& v : *list)
    {
        PluginInfo i;
        i.uid = v.getProperty ("uid", {}).toString().toStdString();
        i.format = v.getProperty ("format", "vst3").toString().toStdString();
        i.name = v.getProperty ("name", {}).toString().toStdString();
        i.vendor = v.getProperty ("vendor", {}).toString().toStdString();
        i.version = v.getProperty ("version", {}).toString().toStdString();
        i.category = v.getProperty ("category", {}).toString().toStdString();
        i.instrument = static_cast<bool> (v.getProperty ("instrument", false));
        i.file = v.getProperty ("file", {}).toString().toStdString();
        i.fileTime = static_cast<juce::int64> (v.getProperty ("fileTime", 0));
        i.status = statusFrom (v.getProperty ("status", "ok").toString());
        i.error = v.getProperty ("error", {}).toString().toStdString();
        entries.push_back (std::move (i));
    }
    return true;
}

bool PluginCatalog::save() const
{
    if (storage.empty())
        return false;
    juce::Array<juce::var> list;
    for (const auto& i : entries)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("uid", juce::String (i.uid));
        o->setProperty ("format", juce::String (i.format));
        o->setProperty ("name", juce::String (i.name));
        o->setProperty ("vendor", juce::String (i.vendor));
        o->setProperty ("version", juce::String (i.version));
        o->setProperty ("category", juce::String (i.category));
        o->setProperty ("instrument", i.instrument);
        o->setProperty ("file", juce::String (i.file));
        o->setProperty ("fileTime", static_cast<juce::int64> (i.fileTime));
        o->setProperty ("status", statusName (i.status));
        if (! i.error.empty())
            o->setProperty ("error", juce::String (i.error));
        list.add (juce::var (o));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty ("plugins", list);
    const juce::File file (juce::String (storage.string()));
    file.getParentDirectory().createDirectory();
    return file.replaceWithText (juce::JSON::toString (juce::var (root)));
}

const PluginInfo* PluginCatalog::find (const std::string& uid) const
{
    for (const auto& i : entries)
        if (i.uid == uid && i.status != PluginStatus::failed)
            return &i;
    return nullptr;
}

std::vector<PluginInfo> PluginCatalog::menu (bool instruments) const
{
    std::vector<PluginInfo> out;
    for (const auto& i : entries)
        if (i.status == PluginStatus::ok && i.instrument == instruments)
            out.push_back (i);
    std::sort (out.begin(), out.end(), [] (const PluginInfo& a, const PluginInfo& b) {
        return std::tie (a.vendor, a.name) < std::tie (b.vendor, b.name);
    });
    return out;
}

void PluginCatalog::setEnabled (const std::string& uid, bool enabled)
{
    for (auto& i : entries)
        if (i.uid == uid && i.status != PluginStatus::failed)
            i.status = enabled ? PluginStatus::ok : PluginStatus::disabled;
}

std::vector<std::filesystem::path> PluginCatalog::defaultFolders()
{
    std::vector<std::filesystem::path> out;
    const auto paths = juce::VST3PluginFormat().getDefaultLocationsToSearch();
    for (int i = 0; i < paths.getNumPaths(); ++i)
        out.emplace_back (paths[i].getFullPathName().toStdString());
    return out;
}

PluginCatalog::ScanResult PluginCatalog::scan (const ScanOptions& options, const std::function<void (const std::string&)>& progress)
{
    ScanResult result;
    juce::FileSearchPath search;
    for (const auto& f : options.folders)
        search.add (juce::File (juce::String (f.string())));
    juce::VST3PluginFormat format;
    const auto files = format.searchPathsForPlugins (search, true, false);

    // Entries for files that are gone are dropped.
    std::set<std::string> present;
    for (const auto& f : files)
        present.insert (f.toStdString());
    std::erase_if (entries, [&] (const PluginInfo& i) { return ! present.contains (i.file); });

    for (const auto& f : files)
    {
        const std::filesystem::path path (f.toStdString());
        const auto time = modified (path);
        const auto known = std::find_if (entries.begin(), entries.end(), [&] (const PluginInfo& i) { return i.file == path.string(); });
        if (known != entries.end() && known->fileTime == time && (known->status != PluginStatus::failed || ! options.retryFailed))
        {
            ++result.skipped;
            continue;
        }
        if (progress)
            progress (path.string());
        scanFile (path, options, result);
    }
    save();
    return result;
}

void PluginCatalog::scanFile (const std::filesystem::path& file, const ScanOptions& options, ScanResult& result)
{
    ++result.scanned;
    // Keep the user's disabled choices across a rescan.
    std::set<std::string> disabled;
    for (const auto& i : entries)
        if (i.file == file.string() && i.status == PluginStatus::disabled)
            disabled.insert (i.uid);
    std::erase_if (entries, [&] (const PluginInfo& i) { return i.file == file.string(); });

    // The scanner writes its findings to a file; its own output goes nowhere, so a chatty
    // plug-in can't block it, and a hung one is killed at the timeout.
    const auto resultFile = juce::File::createTempFile (".scan");
    juce::ChildProcess child;
    juce::StringArray args { juce::String (options.scanner.string()), "--scan", juce::String (file.string()), "--out", resultFile.getFullPathName() };
    std::vector<PluginInfo> found;
    std::string error;
    if (! child.start (args, 0))
        error = "Couldn't start the plug-in scanner.";
    else if (! child.waitForProcessToFinish (static_cast<int> (options.timeoutSeconds * 1000)))
    {
        child.kill();
        error = "The plug-in took too long to load.";
    }
    else
    {
        // The scanner ends a finished scan with "DONE"; without it, the process died (a crash
        // by signal can report exit code 0).
        juce::StringArray lines;
        resultFile.readLines (lines);
        lines.removeEmptyStrings();
        if (child.getExitCode() != 0 || lines.isEmpty() || lines[lines.size() - 1] != "DONE")
            error = "The plug-in crashed while loading.";
        else
        {
            for (const auto& line : lines)
                if (auto info = parseScannerLine (line.toStdString()))
                    found.push_back (*info);
            if (found.empty())
                error = "No plug-ins found in the file.";
        }
    }
    resultFile.deleteFile();

    const auto time = modified (file);
    if (! error.empty())
    {
        PluginInfo failed;
        failed.file = file.string();
        failed.name = file.stem().string();
        failed.fileTime = time;
        failed.status = PluginStatus::failed;
        failed.error = error;
        entries.push_back (std::move (failed));
        ++result.failed;
        return;
    }
    for (auto& info : found)
    {
        info.file = file.string();
        info.fileTime = time;
        if (disabled.contains (info.uid))
            info.status = PluginStatus::disabled;
        entries.push_back (std::move (info));
        ++result.found;
    }
}
} // namespace weft::engine
