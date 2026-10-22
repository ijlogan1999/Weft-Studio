#include "weft/project/ProjectFolder.h"

#include "weft/model/Model.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>

#if defined(_WIN32)
    #include <io.h>
#else
    #include <unistd.h>
#endif

namespace weft::project
{
namespace
{
constexpr std::array<char, 8> logMagic { 'W', 'E', 'F', 'T', 'L', 'O', 'G', '1' };
constexpr std::size_t chunkSize = 4 * 1024; // small enough that fault tests can stop mid-file

void setError (std::string* out, std::string message)
{
    if (out != nullptr)
        *out = std::move (message);
}

void putU32 (std::vector<std::uint8_t>& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back (static_cast<std::uint8_t> (v >> (8 * i)));
}

void putI64 (std::vector<std::uint8_t>& out, std::int64_t v)
{
    const auto u = static_cast<std::uint64_t> (v);
    for (int i = 0; i < 8; ++i)
        out.push_back (static_cast<std::uint8_t> (u >> (8 * i)));
}

std::uint64_t getLE (const std::uint8_t* p, int n)
{
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i)
        v |= static_cast<std::uint64_t> (p[i]) << (8 * i);
    return v;
}

bool syncFile (std::FILE* f)
{
    if (std::fflush (f) != 0)
        return false;
#if defined(_WIN32)
    return _commit (_fileno (f)) == 0;
#else
    return fsync (fileno (f)) == 0;
#endif
}
} // namespace

std::uint32_t crc32 (std::span<const std::uint8_t> bytes) noexcept
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const auto b : bytes)
    {
        crc ^= b;
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool writeFileAtomic (const fs::path& target, std::span<const std::uint8_t> bytes, const WriteFaultHook& fault, std::string* error)
{
    auto tmp = target;
    tmp += ".tmp";
    std::FILE* f = nullptr;
#if defined(_WIN32)
    if (_wfopen_s (&f, tmp.c_str(), L"wb") != 0)
        f = nullptr;
#else
    f = std::fopen (tmp.c_str(), "wb");
#endif
    if (f == nullptr)
    {
        setError (error, "Couldn't write " + tmp.string());
        return false;
    }
    std::size_t written = 0;
    while (written < bytes.size())
    {
        if (fault && ! fault (written, bytes.size()))
        {
            std::fclose (f); // simulated crash: the target is untouched, the temp file is left behind
            setError (error, "Write interrupted");
            return false;
        }
        const auto n = std::min (chunkSize, bytes.size() - written);
        if (std::fwrite (bytes.data() + written, 1, n, f) != n)
        {
            std::fclose (f);
            setError (error, "Couldn't write " + tmp.string() + " (disk full?)");
            return false;
        }
        written += n;
    }
    const bool synced = syncFile (f);
    std::fclose (f);
    if (! synced)
    {
        setError (error, "Couldn't flush " + tmp.string());
        return false;
    }
    std::error_code ec;
    fs::rename (tmp, target, ec); // replaces the target atomically
    if (ec)
    {
        setError (error, "Couldn't replace " + target.string() + ": " + ec.message());
        return false;
    }
    return true;
}

std::optional<std::vector<std::uint8_t>> readFile (const fs::path& file)
{
    std::ifstream in (file, std::ios::binary);
    if (! in)
        return std::nullopt;
    return std::vector<std::uint8_t> (std::istreambuf_iterator<char> (in), {});
}

// RecoveryLog -------------------------------------------------------------------

bool RecoveryLog::append (std::span<const std::uint8_t> change, std::int64_t timeMs)
{
    std::error_code ec;
    const bool fresh = ! fs::exists (path, ec) || fs::file_size (path, ec) < logMagic.size();
    std::vector<std::uint8_t> record;
    if (fresh)
        record.insert (record.end(), logMagic.begin(), logMagic.end());
    putU32 (record, static_cast<std::uint32_t> (change.size()));
    putU32 (record, crc32 (change));
    putI64 (record, timeMs);
    record.insert (record.end(), change.begin(), change.end());

    std::FILE* f = nullptr;
#if defined(_WIN32)
    if (_wfopen_s (&f, path.c_str(), fresh ? L"wb" : L"ab") != 0)
        f = nullptr;
#else
    f = std::fopen (path.c_str(), fresh ? "wb" : "ab");
#endif
    if (f == nullptr)
        return false;
    const bool good = std::fwrite (record.data(), 1, record.size(), f) == record.size();
    // Flushed to the OS on every change, so a killed process loses nothing (AT-PRJ-11).
    // Not fsynced: a power cut may lose the last changes; the saved project stays intact.
    const bool flushed = std::fflush (f) == 0;
    std::fclose (f);
    return good && flushed;
}

std::vector<RecoveryLog::Entry> RecoveryLog::read() const
{
    std::vector<Entry> out;
    const auto bytes = readFile (path);
    if (! bytes || bytes->size() < logMagic.size() || ! std::equal (logMagic.begin(), logMagic.end(), bytes->begin()))
        return out;
    std::size_t pos = logMagic.size();
    while (pos + 16 <= bytes->size())
    {
        const auto* p = bytes->data() + pos;
        const auto len = static_cast<std::size_t> (getLE (p, 4));
        const auto crc = static_cast<std::uint32_t> (getLE (p + 4, 4));
        const auto time = static_cast<std::int64_t> (getLE (p + 8, 8));
        if (pos + 16 + len > bytes->size())
            break; // torn tail
        std::span<const std::uint8_t> change (p + 16, len);
        if (crc32 (change) != crc)
            break;
        out.push_back ({ time, { change.begin(), change.end() } });
        pos += 16 + len;
    }
    return out;
}

bool RecoveryLog::hasEntries() const
{
    std::error_code ec;
    return fs::exists (path, ec) && fs::file_size (path, ec) > logMagic.size();
}

bool RecoveryLog::clear()
{
    std::error_code ec;
    if (! fs::exists (path, ec))
        return true;
    return fs::remove (path, ec) && ! ec;
}

bool RecoveryLog::archive (std::int64_t timeMs)
{
    std::error_code ec;
    if (! fs::exists (path, ec))
        return true;
    const auto dir = path.parent_path() / "archive";
    fs::create_directories (dir, ec);
    auto target = dir / ("changes-" + std::to_string (timeMs) + ".log");
    for (int n = 1; fs::exists (target, ec); ++n)
        target = dir / ("changes-" + std::to_string (timeMs) + "-" + std::to_string (n) + ".log");
    fs::rename (path, target, ec);
    return ! ec;
}

// ProjectFolder -------------------------------------------------------------------

namespace
{
fs::path recoveryLogFor (const fs::path& dir, const std::string& device)
{
    std::error_code ec;
    const bool sharedFolder = ! device.empty() && fs::is_directory (dir / "Peers", ec);
    return dir / "Recovery" / (sharedFolder ? "changes-" + device + ".log" : std::string ("changes.log"));
}
} // namespace

ProjectFolder::ProjectFolder (fs::path folder, std::string deviceId)
    : dir (std::move (folder)), device (std::move (deviceId)), recovery (recoveryLogFor (dir, device))
{
}

fs::path ProjectFolder::folderFor (const fs::path& parent, const std::string& projectName)
{
    const auto name = projectName + ".weft";
    return parent / fs::path (std::u8string (name.begin(), name.end()));
}

bool ProjectFolder::create (std::string* error)
{
    std::error_code ec;
    for (const char* sub : { "Media", "Bounces", "Cache", "Recovery" })
    {
        fs::create_directories (dir / sub, ec);
        if (ec)
        {
            setError (error, "Couldn't create " + (dir / sub).string() + ": " + ec.message());
            return false;
        }
    }
    return true;
}

bool ProjectFolder::save (doc::Document& document, const WriteFaultHook& fault, std::string* error)
{
    const auto bytes = document.save();
    const auto target = usesPeerFile() ? ownPeerFile() : documentFile();
    if (! writeFileAtomic (target, bytes, fault, error))
        return false;
    recovery.clear();
    if (usesPeerFile())
    {
        std::error_code ec;
        seenPeers[target] = { fs::last_write_time (target, ec), fs::file_size (target, ec) };
    }
    return true;
}

bool ProjectFolder::shared() const
{
    std::error_code ec;
    return fs::is_directory (peersDir(), ec);
}

bool ProjectFolder::makeShared (doc::Document& document, std::string* error)
{
    if (device.empty())
    {
        setError (error, "This computer has no device id.");
        return false;
    }
    std::error_code ec;
    fs::create_directories (peersDir(), ec);
    if (ec)
    {
        setError (error, "Couldn't create " + peersDir().string() + ": " + ec.message());
        return false;
    }
    // The log moves to this computer's own name, so a collaborator never sees it as theirs.
    const bool hadLog = recovery.hasEntries();
    RecoveryLog own (recoveryLogFor (dir, device));
    if (hadLog && recovery.file() != own.file())
        fs::rename (recovery.file(), own.file(), ec);
    recovery = std::move (own);
    return writeFileAtomic (documentFile(), document.save(), {}, error) && save (document, {}, error);
}

std::vector<fs::path> ProjectFolder::peerFiles() const
{
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator (peersDir(), ec))
        if (entry.is_regular_file (ec) && entry.path().extension() == ".automerge")
            files.push_back (entry.path());
    std::sort (files.begin(), files.end());
    return files;
}

std::vector<std::vector<std::uint8_t>> ProjectFolder::pollPeers()
{
    std::vector<std::vector<std::uint8_t>> changed;
    if (! shared())
        return changed;
    for (const auto& file : peerFiles())
    {
        if (file == ownPeerFile())
            continue;
        std::error_code ec;
        const Stamp stamp { fs::last_write_time (file, ec), fs::file_size (file, ec) };
        if (ec)
            continue;
        if (const auto it = seenPeers.find (file); it != seenPeers.end() && it->second == stamp)
            continue;
        auto bytes = readFile (file);
        // Half-synced files don't load: leave them unseen and try again next time.
        if (! bytes || ! doc::Document::load (*bytes))
            continue;
        seenPeers[file] = stamp;
        changed.push_back (std::move (*bytes));
    }
    return changed;
}

std::optional<std::string> checkSchemaVersion (const doc::Document& document)
{
    const auto v = document.get (doc::path ({ "schemaVersion" })).value_or (doc::Value (1)).asInt (1);
    if (v > model::schemaVersion)
        return "This project was saved by a newer version of Weft Studio (project format " + std::to_string (v)
             + "). Update Weft Studio to open it; this version supports format " + std::to_string (model::schemaVersion) + ".";
    return std::nullopt;
}

ProjectFolder::Opened ProjectFolder::open() const
{
    Opened result;
    std::error_code ec;
    std::optional<doc::Document> saved;
    if (fs::exists (documentFile(), ec))
    {
        const auto bytes = readFile (documentFile());
        if (! bytes)
        {
            result.error = "Couldn't read " + documentFile().string();
            return result;
        }
        saved = doc::Document::load (*bytes);
        if (! saved)
        {
            result.error = "The project file is damaged and can't be opened.";
            return result;
        }
    }
    // A shared folder: everyone's files on top of the state it was shared with (D-COL-45).
    if (saved && shared())
        for (const auto& file : peerFiles())
            if (const auto bytes = readFile (file); bytes && doc::Document::load (*bytes))
                saved->apply (*bytes);
    const auto entries = recovery.read();
    if (! saved && entries.empty())
    {
        result.error = "There is no project in " + dir.string();
        return result;
    }
    if (saved)
        if (auto tooNew = checkSchemaVersion (*saved))
        {
            result.error = *tooNew;
            return result;
        }

    if (! entries.empty())
    {
        // Saved state (or an empty document) plus every logged change.
        std::optional<doc::Document> replay = saved ? doc::Document::load (saved->save()) : std::optional<doc::Document> (doc::Document());
        for (const auto& e : entries)
            replay->apply (e.change);
        const auto savedHeads = saved ? saved->heads() : doc::Bytes {};
        if (replay->heads() != savedHeads && ! checkSchemaVersion (*replay))
            result.recovered = std::move (replay);
    }
    if (! saved)
    {
        // Crashed before the first save: only the log exists.
        if (! result.recovered)
        {
            result.error = "There is no project in " + dir.string();
            return result;
        }
        saved = doc::Document();
    }
    result.document = std::move (saved);
    return result;
}

bool ProjectFolder::recover (Opened& opened, std::string* error)
{
    if (! opened.recovered)
        return true;
    opened.document = std::move (opened.recovered);
    opened.recovered.reset();
    return save (*opened.document, {}, error);
}

bool ProjectFolder::discard (Opened& opened, std::int64_t timeMs)
{
    opened.recovered.reset();
    return recovery.archive (timeMs);
}
} // namespace weft::project
