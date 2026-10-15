#pragma once

// The list of plug-ins found on this computer (F-PLG-2, F-PLG-3). Scanning runs each
// plug-in file in a separate scanner process, so a plug-in that crashes while loading is
// marked failed instead of taking the app down. Local to this computer; never synced.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace weft::engine
{
enum class PluginStatus
{
    ok,
    disabled, // found and working, hidden from insert menus by the user
    failed    // the scanner crashed, timed out or found nothing loadable
};

struct PluginInfo
{
    std::string uid; // VST3: the 32-hex class ID; empty for a failed file
    std::string format = "vst3";
    std::string name, vendor, version, category;
    bool instrument = false;
    std::string file;              // the .vst3 bundle or file
    std::int64_t fileTime = 0;     // modification time when scanned
    PluginStatus status = PluginStatus::ok;
    std::string error;             // why it failed
};

/** One line of scanner output for a plug-in class, and its parser. */
std::string scannerLine (const PluginInfo& info);
std::optional<PluginInfo> parseScannerLine (const std::string& line);

class PluginCatalog
{
public:
    /** `file`: where the catalog is kept (JSON). Empty: in memory only. */
    explicit PluginCatalog (std::filesystem::path file = {});

    bool load();
    bool save() const;

    const std::vector<PluginInfo>& plugins() const noexcept { return entries; }
    /** A working (ok or disabled) plug-in by uid. */
    const PluginInfo* find (const std::string& uid) const;
    /** Plug-ins for insert or instrument menus: ok, of the kind asked, by vendor then name. */
    std::vector<PluginInfo> menu (bool instruments) const;
    void setEnabled (const std::string& uid, bool enabled);

    struct ScanOptions
    {
        std::vector<std::filesystem::path> folders;
        std::filesystem::path scanner;   // the weft-plugin-scanner executable
        bool retryFailed = false;        // "Retry failed" in the plug-in manager
        double timeoutSeconds = 60;
    };
    struct ScanResult
    {
        int scanned = 0, found = 0, failed = 0, skipped = 0;
    };
    /** Scans new or changed plug-in files; known ones are skipped (failed ones too, unless retryFailed). */
    ScanResult scan (const ScanOptions& options, const std::function<void (const std::string& file)>& progress = {});
    /** Runs the scanner on one file and records what it reports. */
    void scanFile (const std::filesystem::path& file, const ScanOptions& options, ScanResult& result);

    /** The standard VST3 folders of this platform. */
    static std::vector<std::filesystem::path> defaultFolders();

private:
    std::filesystem::path storage;
    std::vector<PluginInfo> entries;
};
} // namespace weft::engine
