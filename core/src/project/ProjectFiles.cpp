#include "weft/project/ProjectFiles.h"

#include "weft/media/Hash.h"
#include "weft/media/WavFile.h"
#include "weft/project/ProjectFolder.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

namespace weft::project
{
namespace
{
constexpr const char* linksFile = "media-links.txt";

void setError (std::string* out, std::string message)
{
    if (out != nullptr)
        *out = std::move (message);
}

std::vector<fs::path> readLines (const fs::path& file)
{
    std::vector<fs::path> out;
    std::ifstream in (file, std::ios::binary);
    std::string line;
    while (std::getline (in, line))
    {
        if (! line.empty() && line.back() == '\r')
            line.pop_back();
        if (! line.empty())
            out.push_back (fromUtf8 (line));
    }
    return out;
}

bool writeLines (const fs::path& file, const std::vector<fs::path>& lines)
{
    std::string text;
    for (const auto& l : lines)
        text += utf8 (l) + "\n";
    const auto* p = reinterpret_cast<const std::uint8_t*> (text.data());
    return writeFileAtomic (file, { p, text.size() });
}

std::string jsonString (const std::string& s)
{
    std::string out = "\"";
    for (const unsigned char c : s)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    char buf[8];
                    std::snprintf (buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                }
                else
                    out += static_cast<char> (c);
        }
    }
    return out + "\"";
}

std::set<std::string> referencedMedia (const model::ProjectModel& model)
{
    std::set<std::string> out;
    for (const auto& [id, r] : model.regions)
        if (! r->isMidi && ! r->audio.media.empty())
            out.insert (r->audio.media);
    return out;
}

bool copyTree (const fs::path& from, const fs::path& to, std::string* error)
{
    std::error_code ec;
    if (! fs::exists (from, ec))
        return true;
    fs::create_directories (to, ec);
    fs::copy (from, to, fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
    if (ec)
    {
        setError (error, "Couldn't copy " + utf8 (from.filename()) + ": " + ec.message());
        return false;
    }
    return true;
}
} // namespace

std::string utf8 (const fs::path& p)
{
    const auto s = p.u8string();
    return { s.begin(), s.end() };
}

fs::path fromUtf8 (const std::string& s) { return fs::path (std::u8string (s.begin(), s.end())); }

fs::path projectRootFor (const fs::path& picked)
{
    std::error_code ec;
    if (picked.filename() == "project.automerge" && ! fs::is_directory (picked, ec))
        return picked.parent_path();
    return picked;
}

bool isProjectFolder (const fs::path& root)
{
    std::error_code ec;
    return fs::is_regular_file (root / "project.automerge", ec);
}

std::vector<fs::path> mediaFolders (const fs::path& root)
{
    std::vector<fs::path> out { root / "Media" };
    for (auto& p : readLines (root / linksFile))
        if (std::find (out.begin(), out.end(), p) == out.end())
            out.push_back (std::move (p));
    return out;
}

std::optional<fs::path> findMediaFile (const std::vector<fs::path>& folders, const std::string& hash)
{
    std::error_code ec;
    for (const auto& f : folders)
    {
        auto file = f / media::mediaFileName (hash);
        if (fs::is_regular_file (file, ec))
            return file;
    }
    return std::nullopt;
}

std::vector<std::string> missingMedia (const model::ProjectModel& model, const std::vector<fs::path>& folders)
{
    std::vector<std::string> out;
    for (const auto& hash : referencedMedia (model))
        if (! findMediaFile (folders, hash))
            out.push_back (hash);
    return out;
}

std::vector<std::string> locateMedia (const fs::path& searchFolder, const std::map<std::string, std::int64_t>& wanted, const fs::path& mediaDir)
{
    std::vector<std::string> restored;
    if (wanted.empty())
        return restored;
    std::set<std::int64_t> sizes;
    bool anySize = false;
    for (const auto& [hash, size] : wanted)
    {
        if (size > 0)
            sizes.insert (size);
        else
            anySize = true;
    }
    std::error_code ec;
    fs::create_directories (mediaDir, ec);
    auto it = fs::recursive_directory_iterator (searchFolder, fs::directory_options::skip_permission_denied, ec);
    for (; ! ec && it != fs::recursive_directory_iterator(); it.increment (ec))
    {
        if (! it->is_regular_file (ec))
            continue;
        const auto size = static_cast<std::int64_t> (it->file_size (ec));
        if (ec || (! anySize && ! sizes.contains (size)))
            continue;
        const auto hash = media::hashFile (it->path());
        if (! hash || ! wanted.contains (*hash))
            continue;
        if (std::find (restored.begin(), restored.end(), *hash) != restored.end())
            continue;
        const auto dest = mediaDir / media::mediaFileName (*hash);
        std::error_code copyError;
        if (! fs::exists (dest, copyError))
            fs::copy_file (it->path(), dest, copyError);
        if (! copyError)
            restored.push_back (*hash);
        if (restored.size() == wanted.size())
            break;
    }
    return restored;
}

bool writeMediaIndex (const model::ProjectModel& model, const fs::path& mediaDir, std::string* error)
{
    std::string text = "{\n";
    bool first = true;
    for (const auto& [hash, ref] : model.media)
    {
        if (! first)
            text += ",\n";
        first = false;
        text += "  " + jsonString (media::mediaFileName (hash)) + ": " + jsonString (ref->originalName);
    }
    text += "\n}\n";
    std::error_code ec;
    fs::create_directories (mediaDir, ec);
    const auto* p = reinterpret_cast<const std::uint8_t*> (text.data());
    return writeFileAtomic (mediaDir / "media-index.json", { p, text.size() }, {}, error);
}

bool saveProjectAs (doc::Document& document, const model::ProjectModel& model, const fs::path& from, const fs::path& to,
                    const SaveAsOptions& options, std::string* error)
{
    std::error_code ec;
    if (fs::exists (to, ec))
    {
        setError (error, utf8 (to.filename()) + " already exists.");
        return false;
    }
    ProjectFolder target (to);
    if (! target.create (error))
        return false;

    const auto sources = mediaFolders (from);
    if (options.copyMedia)
    {
        for (const auto& hash : referencedMedia (model))
        {
            const auto file = findMediaFile (sources, hash);
            if (! file)
                continue; // still missing; Locate… can find it later
            fs::copy_file (*file, target.mediaDir() / media::mediaFileName (hash), fs::copy_options::skip_existing, ec);
            if (ec)
            {
                setError (error, "Couldn't copy media: " + ec.message());
                return false;
            }
        }
    }
    else
    {
        std::vector<fs::path> links;
        for (const auto& f : sources)
            links.push_back (fs::absolute (f, ec));
        if (! writeLines (to / linksFile, links))
        {
            setError (error, "Couldn't write the media links.");
            return false;
        }
    }
    if (! copyTree (from / "Blobs", to / "Blobs", error) || ! copyTree (from / "Bounces", to / "Bounces", error))
        return false;
    writeMediaIndex (model, target.mediaDir());
    return target.save (document, {}, error);
}

// Recent projects -------------------------------------------------------------------------

void RecentProjects::load()
{
    list = readLines (file);
    if (list.size() > maxItems)
        list.resize (maxItems);
}

bool RecentProjects::save() const
{
    if (file.empty())
        return true;
    std::error_code ec;
    fs::create_directories (file.parent_path(), ec);
    return writeLines (file, list);
}

void RecentProjects::add (const fs::path& root)
{
    std::erase (list, root);
    list.insert (list.begin(), root);
    if (list.size() > maxItems)
        list.resize (maxItems);
    save();
}

void RecentProjects::remove (const fs::path& root)
{
    std::erase (list, root);
    save();
}
} // namespace weft::project
