#pragma once

// The Library (D-UI-53, P-BRW-1 to P-BRW-15): one place to find sounds and load them. Loops and one-shots from the
// folders you add (Locations), the built-in and installed instruments and effects, their saved settings (plug-in
// presets) and patches (a track's instrument, effects, volume and pan saved by name). Search, tags, saved searches;
// preview, a new track with the item, or the item on the selected track.

#include "Studio.h"
#include "Widgets.h"

#include <filesystem>
#include <set>

namespace weft::ui
{
struct LibraryItem
{
    enum class Kind
    {
        loop,
        instrument,
        effect,
        preset,
        patch
    };
    Kind kind = Kind::loop;
    std::string name;
    std::vector<std::string> tags;
    std::filesystem::path file;                 // a loop's audio file, a patch's file
    std::optional<Studio::PluginChoice> plugin; // an instrument, an effect, or the plug-in a preset is for
    bool instrumentPreset = false;              // a preset for an instrument (else an effect)
};

enum class LibraryView
{
    all,
    loops,
    instruments,
    effects,
    presets,
    patches
};
juce::String libraryViewName (LibraryView view);
inline constexpr LibraryView libraryViews[] = { LibraryView::all, LibraryView::loops, LibraryView::instruments, LibraryView::effects,
                                                LibraryView::presets, LibraryView::patches };

struct SmartSearch
{
    std::string name;
    LibraryView view = LibraryView::all;
    std::string text;
    std::set<std::string> tags;
};

/** What the Library lists and how its items are used. Locations and Smart Searches are this computer's
    (Library.json in the settings folder); patches are files in its Patches folder. */
class Library
{
public:
    explicit Library (Studio& studio);

    /** Lists everything again (folders are read now). */
    void rescan();
    const std::vector<LibraryItem>& items() const noexcept { return all; }
    /** The items in a view matching the search text (in the name or a tag, any case) and carrying every tag. */
    std::vector<int> results (LibraryView view, const std::string& search, const std::set<std::string>& tags) const;
    /** The tags of the items in a view that match the search, most common first. */
    std::vector<std::string> tags (LibraryView view, const std::string& search) const;
    /** Details for View Info. */
    juce::String info (const LibraryItem& item) const;

    const std::vector<std::filesystem::path>& locations() const noexcept { return folders; }
    bool addLocation (const std::filesystem::path& folder);
    bool removeLocation (const std::filesystem::path& folder);
    const std::vector<SmartSearch>& smartSearches() const noexcept { return searches; }
    /** Saves a search under its name (replacing one of that name). */
    bool saveSmartSearch (const SmartSearch& search);
    bool removeSmartSearch (const std::string& name);

    static std::filesystem::path patchesFolder();
    /** Saves the track's instrument, effects, volume and pan as a patch. */
    bool savePatch (const Id& trackId, const std::string& name, std::string* error = nullptr);

    /** Plays a loop or one-shot (other items have nothing to hear without a track). */
    bool preview (const LibraryItem& item);
    /** New Track with Selected Item: a track made from the item, selected; one undo step. */
    std::optional<Id> newTrackWith (const LibraryItem& item);
    /** Replace Patch: the item on the track (instrument, effect added, preset, patch, or a loop at the playhead);
        one undo step. */
    bool applyTo (const LibraryItem& item, const Id& trackId);

private:
    void load();
    void save() const;
    bool loadPatch (const LibraryItem& item, const Id& trackId);
    Studio& studio;
    std::vector<LibraryItem> all;
    std::vector<std::filesystem::path> folders;
    std::vector<SmartSearch> searches;
};

class LibraryPanel final : public juce::Component, private juce::ListBoxModel, private juce::ChangeListener
{
public:
    explicit LibraryPanel (Studio& studio);
    ~LibraryPanel() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;

    Library& library() noexcept { return lib; }
    void setView (LibraryView view);
    LibraryView view() const noexcept { return shownView; }
    void setSearch (const juce::String& text);
    void toggleTag (const std::string& tag);
    const std::set<std::string>& appliedTags() const noexcept { return applied; }
    void clearTags();
    /** Locations: loops from one folder only (none: every location). */
    void setLocationFilter (std::optional<std::filesystem::path> folder);
    void applySmartSearch (const SmartSearch& search);
    void saveCurrentSearch (const std::string& name);
    const std::vector<int>& shown() const noexcept { return results; }
    void selectResult (int row);
    const LibraryItem* selectedItem() const;
    /** The results' row actions (Return, Ctrl+Return, Alt+Space, Ctrl+I). */
    std::optional<Id> newTrackWithSelected();
    bool replacePatch();
    bool previewSelected();
    void setInfoShown (bool shown);
    bool infoShown() const noexcept { return showInfo; }
    void setCompact (bool compact);
    bool compact() const noexcept { return useCompact; }
    /** Link to Selection: the view follows the selected track (Instruments for an instrument track, Effects for others). */
    void setLinked (bool linked);
    bool linked() const noexcept { return link; }
    /** Replace mode: clicking a result puts it on the selected track straight away. */
    bool replaceMode = false;
    void refresh();
    void focusSearch();

    juce::TextEditor search;
    juce::TextButton more { "More" }, linkButton { "Link" }, replaceButton { "Replace" }, infoButton { "Info" }, clearTagsButton { "Clear Tags" };
    juce::TextButton previewButton { "Preview" }, newTrackButton { "New Track" }, applyButton { "Replace Patch" }, savePatchButton { "Save Patch..." };
    juce::ComboBox viewBox; // compact navigation
    juce::ListBox navigation, list;
    std::vector<std::unique_ptr<juce::TextButton>> tagButtons;
    juce::Label infoLabel;
    /** Asks for a name (tests answer at once). */
    std::function<void (const juce::String& title, const juce::String& current, std::function<void (juce::String)>)> promptForText = askForText;
    /** Picks a folder for Add Location (tests answer at once). */
    std::function<void (std::function<void (std::filesystem::path)>)> chooseFolder;
    int leftWidth = 140;

private:
    // The results list; the navigation list has its own model.
    int getNumRows() override { return static_cast<int> (results.size()); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    void returnKeyPressed (int lastRowSelected) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void update();
    void showMoreMenu();
    juce::Rectangle<int> dividerArea() const;

    struct Navigation final : public juce::ListBoxModel
    {
        explicit Navigation (LibraryPanel& p) : panel (p) {}
        struct Row
        {
            enum class Kind { heading, view, location, search } kind = Kind::heading;
            juce::String text;
            LibraryView view = LibraryView::all;
            std::filesystem::path folder;
            SmartSearch saved;
        };
        std::vector<Row> rows;
        int getNumRows() override { return static_cast<int> (rows.size()); }
        void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
        void listBoxItemClicked (int row, const juce::MouseEvent&) override;
        LibraryPanel& panel;
    };
    void buildNavigation();

    Studio& studio;
    Library lib;
    Navigation nav { *this };
    LibraryView shownView = LibraryView::all;
    std::set<std::string> applied;
    std::optional<std::filesystem::path> locationFilter;
    std::vector<int> results;
    bool showInfo = false, useCompact = false, link = false;
    Id linkedTrack;
    bool draggingDivider = false;
};
} // namespace weft::ui
