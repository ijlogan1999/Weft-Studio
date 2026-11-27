#include "Library.h"

#include "PluginSettings.h"
#include "Theme.h"

#include "weft/media/Blobs.h"
#include "weft/media/WavFile.h"

#include <fstream>
#include <map>

namespace weft::ui
{
namespace
{
constexpr std::size_t maxLoops = 5000;
const char* const patchExtension = ".weftpatch";

bool audioFile (const std::filesystem::path& p)
{
    auto ext = juce::String (p.extension().string()).toLowerCase();
    return ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac" || ext == ".mp3" || ext == ".ogg";
}

juce::String lower (const std::string& s) { return juce::String (juce::CharPointer_UTF8 (s.c_str())).toLowerCase(); }

bool under (const std::filesystem::path& file, const std::filesystem::path& folder)
{
    const auto f = file.lexically_normal().generic_string();
    auto d = folder.lexically_normal().generic_string();
    if (! d.empty() && d.back() != '/')
        d += '/';
    return f.rfind (d, 0) == 0;
}

std::filesystem::path libraryFile() { return Studio::settingsFolder() / "Library.json"; }

juce::String viewKey (LibraryView v)
{
    switch (v)
    {
        case LibraryView::all: return "all";
        case LibraryView::loops: return "loops";
        case LibraryView::instruments: return "instruments";
        case LibraryView::effects: return "effects";
        case LibraryView::presets: return "presets";
        case LibraryView::patches: return "patches";
    }
    return "all";
}

LibraryView viewFromKey (const juce::String& key)
{
    for (const auto v : libraryViews)
        if (viewKey (v) == key)
            return v;
    return LibraryView::all;
}

bool inView (const LibraryItem& item, LibraryView view)
{
    using K = LibraryItem::Kind;
    switch (view)
    {
        case LibraryView::all: return true;
        case LibraryView::loops: return item.kind == K::loop;
        case LibraryView::instruments: return item.kind == K::instrument;
        case LibraryView::effects: return item.kind == K::effect;
        case LibraryView::presets: return item.kind == K::preset;
        case LibraryView::patches: return item.kind == K::patch;
    }
    return true;
}

bool matches (const LibraryItem& item, const juce::String& text)
{
    if (text.isEmpty())
        return true;
    if (lower (item.name).contains (text))
        return true;
    return std::any_of (item.tags.begin(), item.tags.end(), [&] (const std::string& t) { return lower (t).contains (text); });
}

/** A patch file: its name, whether it's for an instrument track, and the strip (instrument, inserts, volume, pan). */
struct Patch
{
    std::string name;
    bool instrument = false;
    model::ChannelStrip strip;
};

std::optional<Patch> readPatch (const std::filesystem::path& file)
{
    std::ifstream in (file, std::ios::binary);
    if (! in)
        return std::nullopt;
    const std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    const auto v = doc::decode (bytes);
    if (! v || ! v->isMap())
        return std::nullopt;
    Patch p;
    p.name = (*v)["name"].asString();
    p.instrument = (*v)["instrument"].asBool();
    p.strip = model::ChannelStrip::fromValue ((*v)["strip"]);
    return p;
}
} // namespace

juce::String libraryViewName (LibraryView view)
{
    switch (view)
    {
        case LibraryView::all: return "All";
        case LibraryView::loops: return "Loops & One-Shots";
        case LibraryView::instruments: return "Instruments";
        case LibraryView::effects: return "Effects";
        case LibraryView::presets: return "Plug-in Presets";
        case LibraryView::patches: return "Patches";
    }
    return "All";
}

Library::Library (Studio& s) : studio (s)
{
    load();
    rescan();
}

std::filesystem::path Library::patchesFolder() { return Studio::settingsFolder() / "Patches"; }

void Library::load()
{
    folders.clear();
    searches.clear();
    const auto text = juce::File (juce::String (libraryFile().string())).loadFileAsString();
    const auto root = juce::JSON::parse (text);
    if (const auto* locs = root.getProperty ("locations", juce::var()).getArray())
        for (const auto& l : *locs)
            folders.emplace_back (l.toString().toStdString());
    if (const auto* saved = root.getProperty ("smartSearches", juce::var()).getArray())
        for (const auto& s : *saved)
        {
            SmartSearch search;
            search.name = s.getProperty ("name", "").toString().toStdString();
            search.view = viewFromKey (s.getProperty ("view", "all").toString());
            search.text = s.getProperty ("text", "").toString().toStdString();
            if (const auto* tags = s.getProperty ("tags", juce::var()).getArray())
                for (const auto& t : *tags)
                    search.tags.insert (t.toString().toStdString());
            if (! search.name.empty())
                searches.push_back (std::move (search));
        }
}

void Library::save() const
{
    juce::Array<juce::var> locs, saved;
    for (const auto& f : folders)
        locs.add (juce::String (f.string()));
    for (const auto& s : searches)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", juce::String (s.name));
        o->setProperty ("view", viewKey (s.view));
        o->setProperty ("text", juce::String (s.text));
        juce::Array<juce::var> tags;
        for (const auto& t : s.tags)
            tags.add (juce::String (t));
        o->setProperty ("tags", tags);
        saved.add (juce::var (o));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty ("locations", locs);
    root->setProperty ("smartSearches", saved);
    std::error_code ec;
    std::filesystem::create_directories (libraryFile().parent_path(), ec);
    juce::File (juce::String (libraryFile().string())).replaceWithText (juce::JSON::toString (juce::var (root)));
}

void Library::rescan()
{
    all.clear();
    // Loops and one-shots: the audio files in each location, tagged with their folders and format.
    std::size_t loops = 0;
    for (const auto& root : folders)
    {
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator (root, std::filesystem::directory_options::skip_permission_denied, ec);
             ! ec && it != std::filesystem::recursive_directory_iterator() && loops < maxLoops; it.increment (ec))
        {
            if (! it->is_regular_file (ec) || ! audioFile (it->path()))
                continue;
            LibraryItem item;
            item.kind = LibraryItem::Kind::loop;
            item.file = it->path();
            item.name = it->path().stem().string();
            item.tags.push_back (root.filename().string());
            for (const auto& part : std::filesystem::relative (it->path().parent_path(), root, ec))
                if (part != "." && ! part.empty())
                    item.tags.push_back (part.string());
            item.tags.push_back (juce::String (it->path().extension().string()).substring (1).toUpperCase().toStdString());
            all.push_back (std::move (item));
            ++loops;
        }
    }
    // Instruments and effects, built-in first, each tagged with its maker and format; then their saved settings.
    for (const bool instruments : { true, false })
        for (const auto& choice : studio.pluginChoices (instruments))
        {
            LibraryItem item;
            item.kind = instruments ? LibraryItem::Kind::instrument : LibraryItem::Kind::effect;
            item.name = choice.label.toStdString();
            item.plugin = choice;
            item.tags = { choice.plugin.format == "builtin" ? "Built-in" : "VST3" };
            if (choice.group.isNotEmpty())
                item.tags.push_back (choice.group.toStdString());
            all.push_back (item);
            for (const auto& preset : plugin_settings::names (Studio::pluginSettingsRoot(), choice.plugin))
            {
                LibraryItem p;
                p.kind = LibraryItem::Kind::preset;
                p.name = preset;
                p.plugin = choice;
                p.instrumentPreset = instruments;
                p.tags = { choice.label.toStdString(), instruments ? "Instrument" : "Effect" };
                all.push_back (std::move (p));
            }
        }
    // Patches.
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator (patchesFolder(), ec))
        if (entry.path().extension() == patchExtension)
            if (const auto patch = readPatch (entry.path()))
            {
                LibraryItem item;
                item.kind = LibraryItem::Kind::patch;
                item.name = patch->name.empty() ? entry.path().stem().string() : patch->name;
                item.file = entry.path();
                item.tags = { patch->instrument ? "Instrument" : "Audio" };
                if (patch->strip.instrument)
                    item.tags.push_back (patch->strip.instrument->name);
                all.push_back (std::move (item));
            }
    std::stable_sort (all.begin(), all.end(), [] (const LibraryItem& a, const LibraryItem& b) {
        if (a.kind != b.kind)
            return a.kind < b.kind;
        return lower (a.name) < lower (b.name);
    });
}

std::vector<int> Library::results (LibraryView view, const std::string& search, const std::set<std::string>& tags) const
{
    const auto text = lower (search).trim();
    std::vector<int> out;
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        const auto& item = all[i];
        if (! inView (item, view) || ! matches (item, text))
            continue;
        const bool tagged = std::all_of (tags.begin(), tags.end(), [&] (const std::string& t) {
            return std::find (item.tags.begin(), item.tags.end(), t) != item.tags.end();
        });
        if (tagged)
            out.push_back (static_cast<int> (i));
    }
    return out;
}

std::vector<std::string> Library::tags (LibraryView view, const std::string& search) const
{
    std::map<std::string, int> counts;
    for (const auto i : results (view, search, {}))
        for (const auto& t : all[static_cast<std::size_t> (i)].tags)
            ++counts[t];
    std::vector<std::pair<std::string, int>> sorted (counts.begin(), counts.end());
    std::stable_sort (sorted.begin(), sorted.end(), [] (const auto& a, const auto& b) { return a.second > b.second; });
    std::vector<std::string> out;
    for (const auto& [t, n] : sorted)
        out.push_back (t);
    return out;
}

juce::String Library::info (const LibraryItem& item) const
{
    using K = LibraryItem::Kind;
    juce::StringArray lines;
    lines.add (juce::String (item.name));
    switch (item.kind)
    {
        case K::loop:
        {
            lines.add (juce::String (item.file.string()));
            std::error_code ec;
            lines.add (juce::String (static_cast<double> (std::filesystem::file_size (item.file, ec)) / 1024.0, 1) + " KB");
            media::WavReader wav;
            if (wav.open (item.file))
            {
                const auto& i = wav.info();
                lines.add (juce::String (static_cast<double> (i.frames) / i.sampleRate, 2) + " s, " + juce::String (juce::roundToInt (i.sampleRate)) + " Hz, "
                           + (i.channels == 1 ? juce::String ("mono") : juce::String (i.channels) + " channels"));
            }
            break;
        }
        case K::instrument:
        case K::effect:
            lines.add ((item.kind == K::instrument ? "Instrument" : "Effect") + juce::String (" by ") + juce::String (item.plugin->plugin.vendor.empty() ? "Weft" : item.plugin->plugin.vendor));
            lines.add (item.plugin->plugin.format == "builtin" ? juce::String ("Built in") : "VST3 " + juce::String (item.plugin->plugin.version));
            break;
        case K::preset:
            lines.add ("A setting for " + item.plugin->label);
            break;
        case K::patch:
            if (const auto patch = readPatch (item.file))
            {
                lines.add (patch->strip.instrument ? "Instrument: " + juce::String (patch->strip.instrument->name) : juce::String ("No instrument"));
                juce::StringArray fx;
                for (const auto& p : patch->strip.inserts)
                    fx.add (juce::String (p.name));
                lines.add ("Effects: " + (fx.isEmpty() ? juce::String ("none") : fx.joinIntoString (", ")));
                lines.add ("Volume " + juce::String (patch->strip.volumeDb, 1) + " dB, pan " + juce::String (juce::roundToInt (patch->strip.pan * 64.0)));
            }
            break;
    }
    if (! item.tags.empty())
    {
        juce::StringArray t;
        for (const auto& tag : item.tags)
            t.add (juce::String (tag));
        lines.add ("Tags: " + t.joinIntoString (", "));
    }
    return lines.joinIntoString ("\n");
}

bool Library::addLocation (const std::filesystem::path& folder)
{
    std::error_code ec;
    if (! std::filesystem::is_directory (folder, ec) || std::find (folders.begin(), folders.end(), folder) != folders.end())
        return false;
    folders.push_back (folder);
    save();
    rescan();
    return true;
}

bool Library::removeLocation (const std::filesystem::path& folder)
{
    const auto it = std::find (folders.begin(), folders.end(), folder);
    if (it == folders.end())
        return false;
    folders.erase (it);
    save();
    rescan();
    return true;
}

bool Library::saveSmartSearch (const SmartSearch& search)
{
    if (juce::String (search.name).trim().isEmpty())
        return false;
    const auto it = std::find_if (searches.begin(), searches.end(), [&] (const SmartSearch& s) { return s.name == search.name; });
    if (it != searches.end())
        *it = search;
    else
        searches.push_back (search);
    save();
    return true;
}

bool Library::removeSmartSearch (const std::string& name)
{
    const auto before = searches.size();
    std::erase_if (searches, [&] (const SmartSearch& s) { return s.name == name; });
    if (searches.size() == before)
        return false;
    save();
    return true;
}

bool Library::savePatch (const Id& trackId, const std::string& name, std::string* error)
{
    const auto* t = studio.model().track (trackId);
    const auto clean = plugin_settings::cleanName (name);
    if (t == nullptr || clean.empty())
    {
        if (error != nullptr)
            *error = t == nullptr ? "Select a track first." : "Give the patch a name.";
        return false;
    }
    auto strip = t->strip;
    strip.sends.clear(); // a patch doesn't carry the routing
    // A plug-in's state over 64 KB lives in the project as a blob; the patch keeps its parameters only.
    auto keep = [] (model::PluginInstance& p) {
        if (! p.state.isBytes())
            p.state = doc::Value();
    };
    if (strip.instrument)
        keep (*strip.instrument);
    for (auto& p : strip.inserts)
        keep (p);
    const auto v = doc::Value::map().set ("name", clean).set ("instrument", t->kind == model::TrackKind::instrument).set ("strip", strip.toValue());
    std::error_code ec;
    std::filesystem::create_directories (patchesFolder(), ec);
    const auto bytes = doc::encode (v);
    std::ofstream out (patchesFolder() / (clean + patchExtension), std::ios::binary | std::ios::trunc);
    out.write (reinterpret_cast<const char*> (bytes.data()), static_cast<std::streamsize> (bytes.size()));
    if (! out)
    {
        if (error != nullptr)
            *error = "Couldn't write the patch.";
        return false;
    }
    out.close();
    rescan();
    return true;
}

bool Library::preview (const LibraryItem& item)
{
    if (item.kind != LibraryItem::Kind::loop)
    {
        studio.setStatus ("Only loops and one-shots play on their own.");
        return false;
    }
    return studio.previewFile (item.file);
}

bool Library::loadPatch (const LibraryItem& item, const Id& trackId)
{
    // Inside an open gesture: the strip's volume, pan and effects, and its instrument on an instrument track.
    const auto patch = readPatch (item.file);
    const auto* t = studio.model().track (trackId);
    if (! patch || t == nullptr)
        return false;
    auto& s = studio.session();
    std::vector<Id> old;
    for (const auto& p : t->strip.inserts)
        old.push_back (p.id);
    const bool instrumentTrack = t->kind == model::TrackKind::instrument;
    s.setTrackVolume (trackId, patch->strip.volumeDb);
    s.setTrackPan (trackId, patch->strip.pan);
    for (const auto& id : old)
        s.removeInsert (trackId, id);
    if (patch->strip.instrument && instrumentTrack)
    {
        auto p = *patch->strip.instrument;
        p.id.clear();
        s.setInstrument (trackId, p);
    }
    for (auto p : patch->strip.inserts)
    {
        p.id.clear();
        s.addInsert (trackId, p);
    }
    return true;
}

namespace
{
/** A saved setting onto a plug-in, inside the open gesture: its chunk, then its parameters. */
void putSetting (Studio& studio, const Id& trackId, const Id& pluginId, const PluginSetting& setting)
{
    auto& s = studio.session();
    if (! setting.state.empty())
        if (const auto state = media::pluginStateValue (setting.state, studio.projectRoot() / "Blobs"))
            s.setPluginState (trackId, pluginId, *state);
    for (const auto& [id, v] : setting.params)
        s.setPluginParam (trackId, pluginId, id, v);
}
} // namespace

std::optional<Id> Library::newTrackWith (const LibraryItem& item)
{
    using K = LibraryItem::Kind;
    if (studio.previewing())
        return std::nullopt;
    if (item.kind == K::loop)
    {
        engine::DropTarget target;
        target.at = studio.playhead();
        const auto outcome = studio.importFiles ({ item.file }, target);
        if (outcome.tracks.empty())
            return std::nullopt;
        studio.selectTrack (outcome.tracks.front());
        return outcome.tracks.front();
    }
    bool instrument = item.kind == K::instrument || (item.kind == K::preset && item.instrumentPreset);
    if (item.kind == K::patch)
    {
        const auto patch = readPatch (item.file);
        if (! patch)
            return std::nullopt;
        instrument = patch->instrument;
    }
    auto& s = studio.session();
    s.beginGesture ("New Track with " + item.name);
    const auto created = s.createTracks (instrument ? model::TrackKind::instrument : model::TrackKind::audio, 1, 2, studio.selectedTrack());
    if (created.empty())
    {
        s.cancelGesture();
        studio.setStatus (juce::String (s.lastError()));
        return std::nullopt;
    }
    const auto trackId = created.front();
    s.renameTrack (trackId, item.name);
    std::optional<Id> pluginId;
    switch (item.kind)
    {
        case K::instrument: pluginId = s.setInstrument (trackId, studio.withDefaultSetting (item.plugin->plugin)); break;
        case K::effect: pluginId = s.addInsert (trackId, studio.withDefaultSetting (item.plugin->plugin)); break;
        case K::preset:
            pluginId = item.instrumentPreset ? s.setInstrument (trackId, item.plugin->plugin) : s.addInsert (trackId, item.plugin->plugin);
            if (pluginId)
                if (const auto setting = plugin_settings::load (Studio::pluginSettingsRoot(), item.plugin->plugin, item.name))
                    putSetting (studio, trackId, *pluginId, *setting);
            break;
        case K::patch: loadPatch (item, trackId); break;
        case K::loop: break;
    }
    s.endGesture();
    studio.selectTrack (trackId);
    studio.edited();
    return trackId;
}

bool Library::applyTo (const LibraryItem& item, const Id& trackId)
{
    using K = LibraryItem::Kind;
    const auto* t = studio.model().track (trackId);
    if (t == nullptr)
    {
        studio.setStatus ("Select a track first.");
        return false;
    }
    if (studio.previewing())
        return false;
    const bool instrumentTrack = t->kind == model::TrackKind::instrument;
    auto& s = studio.session();
    switch (item.kind)
    {
        case K::loop:
        {
            if (t->kind != model::TrackKind::audio)
            {
                studio.setStatus ("Loops go on audio tracks.");
                return false;
            }
            engine::DropTarget target;
            target.at = studio.playhead();
            target.trackId = trackId;
            return ! studio.importFiles ({ item.file }, target).regions.empty();
        }
        case K::instrument:
            if (! instrumentTrack)
            {
                studio.setStatus ("Instruments go on instrument tracks.");
                return false;
            }
            {
                const auto id = s.setInstrument (trackId, studio.withDefaultSetting (item.plugin->plugin));
                studio.check (id ? commands::EditResult {} : commands::EditResult::fail (s.lastError()));
                studio.edited();
                return id.has_value();
            }
        case K::effect:
        {
            const auto id = s.addInsert (trackId, studio.withDefaultSetting (item.plugin->plugin));
            studio.check (id ? commands::EditResult {} : commands::EditResult::fail (s.lastError()));
            studio.edited();
            return id.has_value();
        }
        case K::preset:
        {
            const auto setting = plugin_settings::load (Studio::pluginSettingsRoot(), item.plugin->plugin, item.name);
            if (! setting)
                return false;
            // The track's own copy of the plug-in takes it; without one, the plug-in is added with it.
            const auto same = [&] (const model::PluginInstance& p) { return p.format == item.plugin->plugin.format && p.pluginUid == item.plugin->plugin.pluginUid; };
            Id target;
            if (t->strip.instrument && same (*t->strip.instrument))
                target = t->strip.instrument->id;
            for (const auto& p : t->strip.inserts)
                if (target.empty() && same (p))
                    target = p.id;
            if (! target.empty())
                return studio.applySetting (target, *setting, "Load " + juce::String (item.name));
            if (item.instrumentPreset && ! instrumentTrack)
            {
                studio.setStatus ("Instruments go on instrument tracks.");
                return false;
            }
            s.beginGesture ("Load " + item.name);
            const auto id = item.instrumentPreset ? s.setInstrument (trackId, item.plugin->plugin) : s.addInsert (trackId, item.plugin->plugin);
            if (id)
                putSetting (studio, trackId, *id, *setting);
            s.endGesture();
            studio.edited();
            return id.has_value();
        }
        case K::patch:
        {
            s.beginGesture ("Load Patch " + item.name);
            const bool done = loadPatch (item, trackId);
            s.endGesture();
            studio.edited();
            return done;
        }
    }
    return false;
}

// The panel ---------------------------------------------------------------------------------------------------

namespace
{
constexpr int maxTagButtons = 24;
constexpr int tagRows = 2;

juce::String kindText (LibraryItem::Kind kind)
{
    using K = LibraryItem::Kind;
    switch (kind)
    {
        case K::loop: return "Loop";
        case K::instrument: return "Instrument";
        case K::effect: return "Effect";
        case K::preset: return "Preset";
        case K::patch: return "Patch";
    }
    return {};
}
} // namespace

LibraryPanel::LibraryPanel (Studio& s) : studio (s), lib (s)
{
    setTitle ("Library");
    setWantsKeyboardFocus (true);
    search.setTextToShowWhenEmpty ("Search", colour (Token::textDim));
    search.setTitle ("Search the Library");
    search.onTextChange = [this] { setSearch (search.getText()); };
    search.onEscapeKey = [this] { search.setText ({}); };
    search.onReturnKey = [this] { newTrackWithSelected(); };
    addAndMakeVisible (search);
    for (auto* b : { &more, &linkButton, &replaceButton, &infoButton, &clearTagsButton, &previewButton, &newTrackButton, &applyButton, &savePatchButton })
        addAndMakeVisible (*b);
    linkButton.setTooltip ("Link to Selection: the view follows the selected track");
    replaceButton.setTooltip ("Replace mode: clicking an item puts it on the selected track");
    infoButton.setTooltip ("View Info (Ctrl+I)");
    clearTagsButton.setTooltip ("Clear the tags (Ctrl+Delete)");
    previewButton.setTooltip ("Preview (Alt+Space)");
    newTrackButton.setTooltip ("New Track with Selected Item (Return)");
    applyButton.setTooltip ("Put the item on the selected track (Ctrl+Return)");
    for (auto* b : { &linkButton, &replaceButton, &infoButton })
        b->setClickingTogglesState (false);
    more.onClick = [this] { showMoreMenu(); };
    linkButton.onClick = [this] { setLinked (! link); };
    replaceButton.onClick = [this] {
        replaceMode = ! replaceMode;
        update();
    };
    infoButton.onClick = [this] { setInfoShown (! showInfo); };
    clearTagsButton.onClick = [this] { clearTags(); };
    previewButton.onClick = [this] { previewSelected(); };
    newTrackButton.onClick = [this] { newTrackWithSelected(); };
    applyButton.onClick = [this] { replacePatch(); };
    savePatchButton.onClick = [this] {
        const auto trackId = studio.selectedTrack();
        if (studio.model().track (trackId) == nullptr)
        {
            studio.setStatus ("Select a track first.");
            return;
        }
        promptForText ("Save Patch", juce::String (studio.model().track (trackId)->name),
                       [safe = juce::Component::SafePointer<LibraryPanel> (this), trackId] (juce::String name) {
                           if (safe == nullptr)
                               return;
                           std::string error;
                           if (! safe->lib.savePatch (trackId, name.toStdString(), &error))
                               safe->studio.setStatus (juce::String (error));
                           else
                               safe->studio.setStatus ("Saved patch " + name.trim() + ".");
                           safe->refresh();
                       });
    };
    for (const auto v : libraryViews)
        viewBox.addItem (libraryViewName (v), static_cast<int> (v) + 1);
    viewBox.setTitle ("Library view");
    viewBox.onChange = [this] {
        if (viewBox.getSelectedId() > 0)
            setView (static_cast<LibraryView> (viewBox.getSelectedId() - 1));
    };
    addChildComponent (viewBox);
    navigation.setModel (&nav);
    navigation.setRowHeight (size::control);
    navigation.setTitle ("Library views");
    addAndMakeVisible (navigation);
    list.setModel (this);
    list.setRowHeight (size::control + space::s1);
    list.setTitle ("Library results");
    addAndMakeVisible (list);
    infoLabel.setFont (font (Type::small));
    infoLabel.setColour (juce::Label::textColourId, colour (Token::text));
    infoLabel.setJustificationType (juce::Justification::topLeft);
    addChildComponent (infoLabel);
    chooseFolder = [safe = juce::Component::SafePointer<LibraryPanel> (this)] (std::function<void (std::filesystem::path)> done) {
        auto chooser = std::make_shared<juce::FileChooser> ("Add Location");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [chooser, done = std::move (done)] (const juce::FileChooser& fc) {
                                  if (fc.getResult() != juce::File())
                                      done (std::filesystem::path (fc.getResult().getFullPathName().toStdString()));
                              });
        juce::ignoreUnused (safe);
    };
    studio.addChangeListener (this);
    refresh();
}

LibraryPanel::~LibraryPanel() { studio.removeChangeListener (this); }

void LibraryPanel::refresh()
{
    lib.rescan();
    buildNavigation();
    update();
}

void LibraryPanel::buildNavigation()
{
    using R = Navigation::Row;
    auto row = [] (R::Kind kind, const juce::String& text) {
        R r;
        r.kind = kind;
        r.text = text;
        return r;
    };
    nav.rows.clear();
    nav.rows.push_back (row (R::Kind::heading, "Library"));
    for (const auto v : libraryViews)
    {
        auto r = row (R::Kind::view, libraryViewName (v));
        r.view = v;
        nav.rows.push_back (r);
    }
    nav.rows.push_back (row (R::Kind::heading, "Locations"));
    for (const auto& f : lib.locations())
    {
        auto r = row (R::Kind::location, juce::String (f.filename().string()));
        r.folder = f;
        nav.rows.push_back (r);
    }
    if (! lib.smartSearches().empty())
    {
        nav.rows.push_back (row (R::Kind::heading, "Smart Searches"));
        for (const auto& saved : lib.smartSearches())
        {
            auto r = row (R::Kind::search, juce::String (saved.name));
            r.saved = saved;
            nav.rows.push_back (r);
        }
    }
    navigation.updateContent();
    navigation.repaint();
}

void LibraryPanel::update()
{
    const auto* before = selectedItem();
    const auto keep = before != nullptr ? std::optional<LibraryItem> (*before) : std::nullopt;
    results = lib.results (shownView, search.getText().toStdString(), applied);
    if (locationFilter)
        std::erase_if (results, [&] (int i) {
            const auto& item = lib.items()[static_cast<std::size_t> (i)];
            return item.kind != LibraryItem::Kind::loop || ! under (item.file, *locationFilter);
        });
    list.updateContent();
    // The selection follows the item, not the row.
    list.deselectAllRows();
    if (keep)
        for (std::size_t r = 0; r < results.size(); ++r)
        {
            const auto& item = lib.items()[static_cast<std::size_t> (results[r])];
            if (item.kind == keep->kind && item.name == keep->name && item.file == keep->file)
                list.selectRow (static_cast<int> (r), false, true);
        }
    // Tags: those of the matching items, most common first, the applied ones always shown.
    auto shownTags = lib.tags (shownView, search.getText().toStdString());
    for (const auto& t : applied)
        if (std::find (shownTags.begin(), shownTags.end(), t) == shownTags.end())
            shownTags.insert (shownTags.begin(), t);
    if (shownTags.size() > static_cast<std::size_t> (maxTagButtons))
        shownTags.resize (static_cast<std::size_t> (maxTagButtons));
    tagButtons.clear();
    for (const auto& t : shownTags)
    {
        auto b = std::make_unique<juce::TextButton> (juce::String (t));
        b->setToggleState (applied.count (t) > 0, juce::dontSendNotification);
        b->setTooltip ("Show only items tagged " + juce::String (t));
        b->onClick = [this, t] { toggleTag (t); };
        addAndMakeVisible (*b);
        tagButtons.push_back (std::move (b));
    }
    clearTagsButton.setEnabled (! applied.empty());
    linkButton.setToggleState (link, juce::dontSendNotification);
    replaceButton.setToggleState (replaceMode, juce::dontSendNotification);
    infoButton.setToggleState (showInfo, juce::dontSendNotification);
    const auto* item = selectedItem();
    const bool editable = ! studio.previewing();
    previewButton.setEnabled (item != nullptr && item->kind == LibraryItem::Kind::loop);
    newTrackButton.setEnabled (item != nullptr && editable);
    applyButton.setEnabled (item != nullptr && editable && studio.model().track (studio.selectedTrack()) != nullptr);
    savePatchButton.setEnabled (studio.model().track (studio.selectedTrack()) != nullptr);
    infoLabel.setText (item != nullptr ? lib.info (*item) : juce::String ("Select an item to see its details."), juce::dontSendNotification);
    infoLabel.setVisible (showInfo);
    viewBox.setSelectedId (static_cast<int> (shownView) + 1, juce::dontSendNotification);
    navigation.repaint();
    resized();
    repaint();
}

void LibraryPanel::setView (LibraryView v)
{
    shownView = v;
    locationFilter.reset();
    applied.clear();
    update();
}

void LibraryPanel::setSearch (const juce::String& text)
{
    if (search.getText() != text)
        search.setText (text, juce::dontSendNotification);
    update();
}

void LibraryPanel::toggleTag (const std::string& tag)
{
    if (applied.count (tag) > 0)
        applied.erase (tag);
    else
        applied.insert (tag);
    update();
}

void LibraryPanel::clearTags()
{
    applied.clear();
    update();
}

void LibraryPanel::setLocationFilter (std::optional<std::filesystem::path> folder)
{
    shownView = LibraryView::loops;
    applied.clear();
    locationFilter = std::move (folder);
    update();
}

void LibraryPanel::applySmartSearch (const SmartSearch& saved)
{
    shownView = saved.view;
    locationFilter.reset();
    applied = saved.tags;
    search.setText (juce::String (saved.text), juce::dontSendNotification);
    update();
}

void LibraryPanel::saveCurrentSearch (const std::string& name)
{
    SmartSearch saved;
    saved.name = juce::String (name).trim().toStdString();
    saved.view = shownView;
    saved.text = search.getText().toStdString();
    saved.tags = applied;
    if (lib.saveSmartSearch (saved))
        buildNavigation();
}

void LibraryPanel::selectResult (int row)
{
    if (row < 0 || row >= static_cast<int> (results.size()))
        list.deselectAllRows();
    else
        list.selectRow (row);
    update();
}

const LibraryItem* LibraryPanel::selectedItem() const
{
    const auto row = list.getSelectedRow();
    if (row < 0 || row >= static_cast<int> (results.size()))
        return nullptr;
    return &lib.items()[static_cast<std::size_t> (results[static_cast<std::size_t> (row)])];
}

std::optional<Id> LibraryPanel::newTrackWithSelected()
{
    const auto* item = selectedItem();
    if (item == nullptr)
        return std::nullopt;
    const auto copy = *item;
    return lib.newTrackWith (copy);
}

bool LibraryPanel::replacePatch()
{
    const auto* item = selectedItem();
    if (item == nullptr)
        return false;
    const auto copy = *item;
    return lib.applyTo (copy, studio.selectedTrack());
}

bool LibraryPanel::previewSelected()
{
    const auto* item = selectedItem();
    return item != nullptr && lib.preview (*item);
}

void LibraryPanel::setInfoShown (bool shown)
{
    showInfo = shown;
    update();
}

void LibraryPanel::setCompact (bool c)
{
    useCompact = c;
    update();
}

void LibraryPanel::setLinked (bool linked)
{
    link = linked;
    linkedTrack.clear();
    changeListenerCallback (nullptr);
    update();
}

void LibraryPanel::focusSearch()
{
    search.grabKeyboardFocus();
    search.selectAll();
}

void LibraryPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Link to Selection: a newly selected track picks the view.
    if (link && studio.selectedTrack() != linkedTrack)
    {
        linkedTrack = studio.selectedTrack();
        if (const auto* t = studio.model().track (linkedTrack))
        {
            shownView = t->kind == model::TrackKind::instrument ? LibraryView::instruments : LibraryView::effects;
            locationFilter.reset();
            applied.clear();
        }
    }
    update();
}

void LibraryPanel::showMoreMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "Add Location...");
    juce::PopupMenu remove;
    for (std::size_t i = 0; i < lib.locations().size(); ++i)
        remove.addItem (100 + static_cast<int> (i), juce::String (lib.locations()[i].string()));
    m.addSubMenu ("Remove Location", remove, ! lib.locations().empty());
    m.addSeparator();
    m.addItem (2, "Save Search as Smart Search...");
    juce::PopupMenu forget;
    for (std::size_t i = 0; i < lib.smartSearches().size(); ++i)
        forget.addItem (200 + static_cast<int> (i), juce::String (lib.smartSearches()[i].name));
    m.addSubMenu ("Remove Smart Search", forget, ! lib.smartSearches().empty());
    m.addSeparator();
    m.addItem (3, "Compact View", true, useCompact);
    m.addItem (4, "Rescan");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (more), [safe = juce::Component::SafePointer<LibraryPanel> (this)] (int item) {
        if (safe == nullptr || item == 0)
            return;
        auto& p = *safe;
        if (item == 1 && p.chooseFolder)
            p.chooseFolder ([safe] (std::filesystem::path folder) {
                if (safe != nullptr && safe->lib.addLocation (folder))
                    safe->refresh();
            });
        else if (item == 2)
            p.promptForText ("Save Smart Search", p.search.getText(), [safe] (juce::String name) {
                if (safe != nullptr)
                    safe->saveCurrentSearch (name.toStdString());
            });
        else if (item == 3)
            p.setCompact (! p.useCompact);
        else if (item == 4)
            p.refresh();
        else if (item >= 200 && item - 200 < static_cast<int> (p.lib.smartSearches().size()))
        {
            p.lib.removeSmartSearch (p.lib.smartSearches()[static_cast<std::size_t> (item - 200)].name);
            p.buildNavigation();
        }
        else if (item >= 100 && item - 100 < static_cast<int> (p.lib.locations().size()))
        {
            p.lib.removeLocation (p.lib.locations()[static_cast<std::size_t> (item - 100)]);
            p.locationFilter.reset();
            p.refresh();
        }
    });
}

bool LibraryPanel::keyPressed (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    if (key.getKeyCode() == 'I' && mods.isCommandDown() && ! mods.isShiftDown())
    {
        setInfoShown (! showInfo);
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::spaceKey && mods.isAltDown())
        return previewSelected() || true;
    if ((key.getKeyCode() == juce::KeyPress::deleteKey || key.getKeyCode() == juce::KeyPress::backspaceKey) && mods.isCommandDown())
    {
        clearTags();
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::returnKey)
    {
        if (mods.isCommandDown())
            replacePatch();
        else
            newTrackWithSelected();
        return true;
    }
    if (key.getKeyCode() == 'F' && mods.isCommandDown())
    {
        focusSearch();
        return true;
    }
    return false;
}

juce::Rectangle<int> LibraryPanel::dividerArea() const
{
    if (useCompact)
        return {};
    return { leftWidth, size::panelHeader, size::gutter, getHeight() - size::panelHeader };
}

void LibraryPanel::mouseDown (const juce::MouseEvent& e) { draggingDivider = dividerArea().expanded (2, 0).contains (e.getPosition()); }

void LibraryPanel::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggingDivider)
        return;
    leftWidth = juce::jlimit (space::s6 * 2, std::max (space::s6 * 2, getWidth() - space::s6 * 3), e.getPosition().x);
    resized();
    repaint();
}

void LibraryPanel::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (size::panelHeader).reduced (space::half);
    more.setBounds (top.removeFromRight (space::s6));
    top.removeFromRight (space::half);
    infoButton.setBounds (top.removeFromRight (space::s4 + space::s1));
    top.removeFromRight (space::half);
    replaceButton.setBounds (top.removeFromRight (space::s6 + space::s1));
    top.removeFromRight (space::half);
    linkButton.setBounds (top.removeFromRight (space::s4 + space::s1));
    top.removeFromRight (space::half);
    search.setBounds (top);

    auto bottom = r.removeFromBottom (size::panelHeader).reduced (space::half);
    const auto w = bottom.getWidth() / 4;
    previewButton.setBounds (bottom.removeFromLeft (w).reduced (space::half / 2, 0));
    newTrackButton.setBounds (bottom.removeFromLeft (w).reduced (space::half / 2, 0));
    applyButton.setBounds (bottom.removeFromLeft (w).reduced (space::half / 2, 0));
    savePatchButton.setBounds (bottom.reduced (space::half / 2, 0));

    viewBox.setVisible (useCompact);
    navigation.setVisible (! useCompact);
    if (useCompact)
        viewBox.setBounds (r.removeFromTop (size::control + space::half).reduced (space::half, space::half / 2));
    else
    {
        leftWidth = juce::jlimit (space::s6 * 2, std::max (space::s6 * 2, getWidth() - space::s6 * 3), leftWidth);
        navigation.setBounds (r.removeFromLeft (leftWidth));
        r.removeFromLeft (size::gutter);
    }

    // The tags, wrapped over a few rows, then Clear Tags.
    auto tagArea = r.removeFromTop ((size::control + space::half) * tagRows + space::half).reduced (space::half);
    int x = tagArea.getX(), y = tagArea.getY(), row = 0;
    const int clearWidth = juce::GlyphArrangement::getStringWidthInt (font (Type::body), clearTagsButton.getButtonText()) + space::s2;
    auto place = [&] (juce::Button& b, int width) {
        if (x + width > tagArea.getRight() && x > tagArea.getX())
        {
            x = tagArea.getX();
            y += size::control + space::half;
            ++row;
        }
        b.setVisible (row < tagRows);
        b.setBounds (x, y, std::min (width, tagArea.getWidth()), size::control);
        x += width + space::half;
    };
    place (clearTagsButton, clearWidth);
    for (auto& b : tagButtons)
        place (*b, juce::GlyphArrangement::getStringWidthInt (font (Type::body), b->getButtonText()) + space::s2);

    if (showInfo)
        infoLabel.setBounds (r.removeFromBottom (std::min (r.getHeight() / 2, space::s6 * 2 + space::s1)).reduced (space::s1, space::half));
    list.setBounds (r);
}

void LibraryPanel::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::surface));
    g.setColour (colour (Token::lineSubtle));
    g.fillRect (0, size::panelHeader - 1, getWidth(), 1);
    g.fillRect (0, getHeight() - size::panelHeader, getWidth(), 1);
    if (! useCompact)
        g.fillRect (dividerArea().withWidth (1).translated (size::gutter / 2, 0));
    if (results.empty())
    {
        g.setColour (colour (Token::textDim));
        g.setFont (font (Type::body));
        const auto text = lib.items().empty() && shownView == LibraryView::loops ? juce::String ("Add a Location (More) to list your loops.")
                                                                               : juce::String ("Nothing matches.");
        g.drawText (text, list.getBounds().reduced (space::s1), juce::Justification::centredTop, true);
    }
}

void LibraryPanel::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= static_cast<int> (results.size()))
        return;
    const auto& item = lib.items()[static_cast<std::size_t> (results[static_cast<std::size_t> (row)])];
    if (selected)
    {
        g.fillAll (colour (Token::hover));
        g.setColour (colour (Token::accent));
        g.fillRect (0, 0, 2, height);
    }
    g.setColour (colour (Token::lineSubtle));
    g.fillRect (space::s1, height - 1, width - space::s1, 1);
    const int left = space::s1;
    g.setColour (colour (Token::text));
    g.setFont (font (Type::body));
    g.drawText (juce::String (item.name), left, 2, width - left - space::s1, height / 2, juce::Justification::bottomLeft, true);
    juce::String detail = kindText (item.kind);
    if (item.plugin && item.kind == LibraryItem::Kind::preset)
        detail << juce::String::fromUTF8 ("  \xc2\xb7  ") << item.plugin->label;
    else if (! item.tags.empty())
        detail << juce::String::fromUTF8 ("  \xc2\xb7  ") << juce::String (item.tags.front());
    g.setColour (colour (Token::textDim));
    g.setFont (font (Type::small));
    g.drawText (detail, left, height / 2, width - left - space::s1, height / 2 - 2, juce::Justification::topLeft, true);
}

void LibraryPanel::listBoxItemClicked (int, const juce::MouseEvent&)
{
    if (replaceMode)
        replacePatch();
    update();
}

void LibraryPanel::listBoxItemDoubleClicked (int, const juce::MouseEvent&) { newTrackWithSelected(); }

void LibraryPanel::returnKeyPressed (int) { newTrackWithSelected(); }

void LibraryPanel::Navigation::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool)
{
    if (row < 0 || row >= getNumRows())
        return;
    using K = Row::Kind;
    const auto& r = rows[static_cast<std::size_t> (row)];
    bool on = false;
    switch (r.kind)
    {
        case K::heading:
            g.setColour (colour (Token::textDim));
            g.setFont (font (Type::caption));
            g.drawText (captionText (r.text), space::s1, 0, width - space::s1, height, juce::Justification::bottomLeft, true);
            return;
        case K::view: on = ! panel.locationFilter && panel.shownView == r.view; break;
        case K::location: on = panel.locationFilter && *panel.locationFilter == r.folder; break;
        case K::search: on = false; break;
    }
    if (on)
    {
        g.fillAll (colour (Token::hover));
        g.setColour (colour (Token::accent));
        g.fillRect (0, 0, 2, height);
    }
    g.setColour (colour (Token::text));
    g.setFont (font (Type::body));
    g.drawText (r.text, space::s2, 0, width - space::s2 - space::half, height, juce::Justification::centredLeft, true);
}

void LibraryPanel::Navigation::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    if (row < 0 || row >= getNumRows())
        return;
    using K = Row::Kind;
    const auto r = rows[static_cast<std::size_t> (row)];
    switch (r.kind)
    {
        case K::heading: break;
        case K::view: panel.setView (r.view); break;
        case K::location: panel.setLocationFilter (r.folder); break;
        case K::search: panel.applySmartSearch (r.saved); break;
    }
}
} // namespace weft::ui
