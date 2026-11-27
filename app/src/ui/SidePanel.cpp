#include "SidePanel.h"

#include "BottomArea.h"
#include "Library.h"

#include "weft/media/WavFile.h"
#include "weft/time/Position.h"

namespace weft::ui
{
bool SupportedFileFilter::isFileSuitable (const juce::File& file) const
{
    const std::filesystem::path p (file.getFullPathName().toStdString());
    if (! (engine::isMidiFile (p) || decoders.canDecode (p)))
        return false;
    const std::scoped_lock hold (lock);
    const auto name = file.getFileName();
    for (const auto& w : words)
        if (! name.containsIgnoreCase (w))
            return false;
    return true;
}

void SupportedFileFilter::setSearch (const juce::String& text)
{
    auto split = juce::StringArray::fromTokens (text, " ", "");
    split.removeEmptyStrings();
    const std::scoped_lock hold (lock);
    words = split;
}

juce::String SupportedFileFilter::search() const
{
    const std::scoped_lock hold (lock);
    return words.joinIntoString (" ");
}

FileBrowser::FileBrowser (Studio& s, const juce::File& root) : studio (s), filter (s.decoders)
{
    setTitle ("File browser");
    thread.startThread (juce::Thread::Priority::low);
    fileTree.setTitle ("Files");
    fileTree.setDragAndDropDescription ("weft-files");
    fileTree.setMultiSelectEnabled (true);
    fileTree.addListener (this);
    fileTree.setItemHeight (size::control);
    describe (back, "Back", "Go back to the previous folder");
    describe (forward, "Forward", "Go forward again");
    describe (up, "Up", "Go to the parent folder");
    describe (home, "Home", "Go to your home folder");
    describe (project, "Project", "Go to this project's folder");
    back.onClick = [this] { goBack(); };
    forward.onClick = [this] { goForward(); };
    up.onClick = [this] { setRoot (list.getDirectory().getParentDirectory()); };
    home.onClick = [this] { goHome(); };
    project.onClick = [this] { goToProject(); };
    searchBox.setTextToShowWhenEmpty ("Search", colour (Token::textDim));
    searchBox.setTitle ("Search files");
    searchBox.setTooltip ("Show only files whose name has every word typed here");
    searchBox.onTextChange = [this] { setSearch (searchBox.getText()); };
    searchBox.onEscapeKey = [this] { searchBox.setText ({}); };
    describe (play, "Play", "Play the selected file, or stop the preview");
    play.onClick = [this] { playOrStop(); };
    volume.setRange (-60.0, 0.0, 0.1);
    volume.setValue (0.0, juce::dontSendNotification);
    volume.setDoubleClickReturnValue (true, 0.0);
    volume.setTextValueSuffix (" dB");
    describe (volume, "Preview volume");
    volume.onValueChange = [this] { setPreviewVolumeDb (volume.getValue()); };
    path.setFont (font (Type::strong));
    path.setMinimumHorizontalScale (1.0f);
    describe (path, "Folder");
    for (juce::Component* c : { static_cast<juce::Component*> (&back), static_cast<juce::Component*> (&forward), static_cast<juce::Component*> (&up),
                                static_cast<juce::Component*> (&home), static_cast<juce::Component*> (&project), static_cast<juce::Component*> (&path),
                                static_cast<juce::Component*> (&searchBox), static_cast<juce::Component*> (&fileTree), static_cast<juce::Component*> (&play),
                                static_cast<juce::Component*> (&volume) })
        addAndMakeVisible (c);
    setRoot (root);
}

FileBrowser::~FileBrowser()
{
    stopTimer();
    fileTree.removeListener (this);
    thread.stopThread (2000);
}

void FileBrowser::setRoot (const juce::File& folder)
{
    if (historyPos < 0 || history[static_cast<std::size_t> (historyPos)] != folder)
    {
        history.resize (static_cast<std::size_t> (historyPos + 1)); // a new place drops the forward history
        history.push_back (folder);
        constexpr std::size_t keep = 50;
        if (history.size() > keep)
            history.erase (history.begin(), history.end() - static_cast<std::ptrdiff_t> (keep));
        historyPos = static_cast<int> (history.size()) - 1;
    }
    show (folder);
}

void FileBrowser::show (const juce::File& folder)
{
    if (list.getDirectory() == folder)
        list.refresh();
    else
        list.setDirectory (folder, true, true);
    // The folder's name; the whole path is in the tooltip.
    path.setText (folder.getFileName().isEmpty() ? folder.getFullPathName() : folder.getFileName(), juce::dontSendNotification);
    path.setTooltip (folder.getFullPathName());
    updateButtons();
}

void FileBrowser::goBack()
{
    if (! canGoBack())
        return;
    --historyPos;
    show (history[static_cast<std::size_t> (historyPos)]);
}

void FileBrowser::goForward()
{
    if (! canGoForward())
        return;
    ++historyPos;
    show (history[static_cast<std::size_t> (historyPos)]);
}

void FileBrowser::goHome() { setRoot (juce::File::getSpecialLocation (juce::File::userHomeDirectory)); }

void FileBrowser::goToProject()
{
    const juce::File folder (juce::String (studio.projectRoot().string()));
    if (studio.projectRoot().empty() || ! folder.isDirectory())
        return;
    setRoot (folder);
}

void FileBrowser::setSearch (const juce::String& text)
{
    if (searchBox.getText() != text)
        searchBox.setText (text, juce::dontSendNotification);
    filter.setSearch (text);
    list.refresh();
    // Subfolders the tree has opened keep their own lists; reopening the root rebuilds them.
    fileTree.refresh();
}

void FileBrowser::playOrStop()
{
    if (studio.engine().previewing())
    {
        studio.engine().stopPreview();
        updateButtons();
        return;
    }
    const auto file = fileTree.getSelectedFile();
    if (file == juce::File() || file.isDirectory())
    {
        studio.setStatus ("Select an audio file to play.");
        return;
    }
    const std::filesystem::path p (file.getFullPathName().toStdString());
    if (engine::isMidiFile (p))
    {
        studio.setStatus ("MIDI files don't preview yet.");
        return;
    }
    if (studio.previewFile (p))
    {
        play.setButtonText ("Stop");
        startTimer (100);
    }
}

void FileBrowser::setPreviewVolumeDb (double db)
{
    if (std::abs (volume.getValue() - db) > 1e-9)
        volume.setValue (db, juce::dontSendNotification);
    studio.engine().setPreviewGain (db <= -60.0 ? 0.0f : engine::dbToGain (db));
}

void FileBrowser::updateButtons()
{
    back.setEnabled (canGoBack());
    forward.setEnabled (canGoForward());
    project.setEnabled (! studio.projectRoot().empty());
}

void FileBrowser::timerCallback()
{
    // The preview ends by itself; the button goes back to Play when it does.
    if (! studio.engine().previewing())
    {
        play.setButtonText ("Play");
        stopTimer();
    }
}

void FileBrowser::fileClicked (const juce::File& file, const juce::MouseEvent&)
{
    if (file.isDirectory())
        return;
    const std::filesystem::path p (file.getFullPathName().toStdString());
    if (! engine::isMidiFile (p) && studio.previewFile (p)) // click previews (F-LIB-1)
    {
        play.setButtonText ("Stop");
        startTimer (100);
    }
}

void FileBrowser::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (size::panelHeader).reduced (space::s1, space::half);
    const int small = std::max (24, top.getHeight());
    back.setBounds (top.removeFromLeft (small));
    forward.setBounds (top.removeFromLeft (small).withTrimmedLeft (2));
    up.setBounds (top.removeFromLeft (40).withTrimmedLeft (space::half));
    home.setBounds (top.removeFromLeft (52).withTrimmedLeft (space::half));
    project.setBounds (top.removeFromLeft (64).withTrimmedLeft (space::half));
    auto second = r.removeFromTop (size::panelHeader).reduced (space::s1, space::half);
    path.setBounds (second.removeFromLeft (second.getWidth() / 2));
    searchBox.setBounds (second.withTrimmedLeft (space::half));
    auto bottom = r.removeFromBottom (size::panelHeader).reduced (space::s1, space::half);
    play.setBounds (bottom.removeFromLeft (56));
    volume.setBounds (bottom.withTrimmedLeft (space::s1));
    fileTree.setBounds (r);
}

MediaList::MediaList (Studio& s) : studio (s)
{
    setTitle ("Project audio");
    box.setModel (this);
    box.setRowHeight (space::s6 - space::s1);
    box.setTitle ("Project audio");
    box.setMultipleSelectionEnabled (true);
    describe (play, "Play", "Play the selected audio file from its start, or stop the preview");
    play.onClick = [this] { playOrStop(); };
    loop.setClickingTogglesState (true);
    describe (loop, "Loop", "Repeat the preview until it's stopped");
    describe (menuButton, "Audio File", "Add, rename, delete or place the project's audio files");
    menuButton.onClick = [this] {
        fileMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (menuButton), [this] (int item) {
            if (item > 0)
                pick (item);
        });
    };
    volume.setRange (-60.0, 6.0, 0.1);
    volume.setValue (0.0, juce::dontSendNotification);
    volume.setDoubleClickReturnValue (true, 0.0);
    describe (volume, "Preview volume", "Volume of the preview, in dB");
    volume.onValueChange = [this] {
        const auto db = volume.getValue();
        studio.engine().setPreviewGain (db <= -60.0 ? 0.0f : engine::dbToGain (db));
    };
    path.setFont (font (Type::monoSmall));
    path.setColour (juce::Label::textColourId, colour (Token::textDim));
    path.setMinimumHorizontalScale (0.6f);
    describe (path, "File path");
    for (juce::Component* c : std::initializer_list<juce::Component*> { &box, &play, &loop, &menuButton, &volume, &path })
        addAndMakeVisible (c);
    studio.addChangeListener (this);
    rebuild();
}

MediaList::~MediaList()
{
    stopTimer();
    studio.removeChangeListener (this);
}

void MediaList::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (size::panelHeader).reduced (space::s1, space::half);
    play.setBounds (top.removeFromLeft (52));
    loop.setBounds (top.removeFromLeft (56).withTrimmedLeft (space::half));
    menuButton.setBounds (top.removeFromRight (88));
    volume.setBounds (top.reduced (space::s1, 0));
    path.setBounds (r.removeFromBottom (size::control).reduced (space::s1, 0));
    box.setBounds (r);
}

void MediaList::paint (juce::Graphics& g) { g.fillAll (colour (Token::surface)); }

void MediaList::changeListenerCallback (juce::ChangeBroadcaster*)
{
    auto now = arrange::mediaUsage (studio.model());
    const bool same = now.size() == items.size()
                      && std::equal (now.begin(), now.end(), items.begin(), [] (const auto& a, const auto& b) {
                             return a.hash == b.hash && a.regionIds == b.regionIds && a.name == b.name;
                         });
    if (! same)
        rebuild();
    else
        box.repaint(); // region names and positions
}

void MediaList::rebuild()
{
    // Keep the selection across the rebuild, by file and region.
    std::set<std::pair<std::string, Id>> wasSelected;
    for (int i = 0; i < box.getNumSelectedRows(); ++i)
        if (const auto row = box.getSelectedRow (i); row >= 0 && row < getNumRows())
            wasSelected.insert ({ items[shown[static_cast<std::size_t> (row)].file].hash, shown[static_cast<std::size_t> (row)].region });
    items = arrange::mediaUsage (studio.model());
    shown.clear();
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        shown.push_back ({ i, {} });
        if (open.contains (items[i].hash))
            for (const auto& id : items[i].regionIds)
                shown.push_back ({ i, id });
    }
    box.updateContent();
    juce::SparseSet<int> rows;
    for (std::size_t i = 0; i < shown.size(); ++i)
        if (wasSelected.contains ({ items[shown[i].file].hash, shown[i].region }))
            rows.addRange ({ static_cast<int> (i), static_cast<int> (i) + 1 });
    box.setSelectedRows (rows, juce::dontSendNotification);
    selectedRowsChanged (-1);
    box.repaint();
}

std::vector<std::string> MediaList::selectedFiles() const
{
    std::vector<std::string> out;
    for (int i = 0; i < box.getNumSelectedRows(); ++i)
        if (const auto row = box.getSelectedRow (i); row >= 0 && row < static_cast<int> (shown.size()))
        {
            const auto& hash = items[shown[static_cast<std::size_t> (row)].file].hash;
            if (std::find (out.begin(), out.end(), hash) == out.end())
                out.push_back (hash);
        }
    return out;
}

void MediaList::selectFiles (const std::vector<std::string>& hashes)
{
    juce::SparseSet<int> rows;
    for (std::size_t i = 0; i < shown.size(); ++i)
        if (shown[i].region.empty() && std::find (hashes.begin(), hashes.end(), items[shown[i].file].hash) != hashes.end())
            rows.addRange ({ static_cast<int> (i), static_cast<int> (i) + 1 });
    box.setSelectedRows (rows);
}

void MediaList::setOpen (const std::string& hash, bool show)
{
    if (show)
        open.insert (hash);
    else
        open.erase (hash);
    rebuild();
}

void MediaList::showAllRegions (bool show)
{
    open.clear();
    if (show)
        for (const auto& item : items)
            if (item.regions > 0)
                open.insert (item.hash);
    rebuild();
}

std::filesystem::path MediaList::fileFor (const std::string& hash) const { return studio.mediaDir() / media::mediaFileName (hash); }

void MediaList::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= getNumRows())
        return;
    const auto& line = shown[static_cast<std::size_t> (row)];
    const auto& item = items[line.file];
    if (selected)
    {
        g.fillAll (colour (Token::hover));
        g.setColour (colour (Token::accent));
        g.fillRect (0, 0, 2, height);
    }
    g.setColour (colour (Token::lineSubtle));
    g.fillRect (space::s2, height - 1, width - space::s2, 1);
    if (! line.region.empty())
    {
        // A region that plays the file: its name, track and position.
        const auto* r = studio.model().region (line.region);
        if (r == nullptr)
            return;
        const auto* t = studio.model().track (r->trackId);
        const int left = space::s2 + space::s4;
        g.setColour (colour (Token::text));
        g.setFont (font (Type::body));
        g.drawText (juce::String (r->name.empty() ? item.name : r->name), left, 2, width - left - space::s1, height / 2, juce::Justification::bottomLeft, true);
        g.setColour (colour (Token::textDim));
        g.setFont (font (Type::monoSmall));
        g.drawText (juce::String (formatBarsBeats (r->start, studio.model().signatures)) + (t != nullptr ? "  ·  " + juce::String (t->name) : juce::String()), left,
                    height / 2, width - left - space::s1, height / 2 - 2, juce::Justification::topLeft, true);
        return;
    }
    // A file: a disclosure triangle when regions play it, then its name and details.
    const int left = space::s2 + space::s3;
    if (item.regions > 0)
    {
        juce::Path tri;
        const auto cx = static_cast<float> (space::s2 + 4), cy = static_cast<float> (height) / 2.0f;
        if (open.contains (item.hash))
            tri.addTriangle (cx - 4, cy - 2, cx + 4, cy - 2, cx, cy + 3);
        else
            tri.addTriangle (cx - 2, cy - 4, cx - 2, cy + 4, cx + 3, cy);
        g.setColour (colour (Token::textDim));
        g.fillPath (tri);
    }
    g.setColour (colour (Token::text));
    g.setFont (font (Type::body));
    g.drawText (juce::String (item.name), left, 2, width - left - space::s1, height / 2, juce::Justification::bottomLeft, true);
    g.setColour (colour (Token::textDim));
    g.setFont (font (Type::monoSmall));
    juce::String info;
    if (item.sampleRate > 0)
        info << juce::String (item.sampleRate / 1000.0, item.sampleRate % 1000 == 0 ? 0 : 1) << " kHz  ·  " << item.bitDepth << "-bit  ·  "
             << (item.channels == 1 ? "Mono" : item.channels == 2 ? "Stereo" : juce::String (item.channels) + " ch") << "  ·  ";
    info << juce::String (item.seconds, 1) << " s  ·  " << (item.regions == 1 ? juce::String ("1 region") : juce::String (item.regions) + " regions");
    g.drawText (info, left, height / 2, width - left - space::s1, height / 2 - 2, juce::Justification::topLeft, true);
}

void MediaList::listBoxItemClicked (int row, const juce::MouseEvent& e)
{
    if (row < 0 || row >= getNumRows())
        return;
    const auto line = shown[static_cast<std::size_t> (row)];
    if (e.mods.isPopupMenu())
    {
        if (! box.isRowSelected (row))
            box.selectRow (row);
        fileMenu().showMenuAsync (juce::PopupMenu::Options(), [this] (int item) {
            if (item > 0)
                pick (item);
        });
        return;
    }
    if (line.region.empty())
    {
        // The triangle opens and closes the file's regions.
        if (e.x < space::s2 + space::s3 && items[line.file].regions > 0)
            setOpen (items[line.file].hash, ! open.contains (items[line.file].hash));
        return;
    }
    // A region row selects that region in the tracks area.
    if (const auto* r = studio.model().region (line.region))
    {
        const auto id = r->id;
        const auto track = r->trackId;
        studio.selection.set ({ id });
        studio.selectTrack (track);
        studio.sendChangeMessage();
    }
}

void MediaList::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    if (row >= 0 && row < getNumRows() && shown[static_cast<std::size_t> (row)].region.empty())
        startPreview (items[shown[static_cast<std::size_t> (row)].file].hash);
}

void MediaList::selectedRowsChanged (int)
{
    const auto files = selectedFiles();
    path.setText (files.size() == 1 ? juce::String (fileFor (files.front()).string()) : juce::String(), juce::dontSendNotification);
}

bool MediaList::startPreview (const std::string& hash)
{
    if (! studio.previewFile (fileFor (hash)))
        return false;
    previewHash = hash;
    play.setButtonText ("Stop");
    startTimer (50);
    return true;
}

void MediaList::playOrStop()
{
    if (! previewHash.empty() || studio.engine().previewing())
    {
        studio.engine().stopPreview();
        previewHash.clear();
        play.setButtonText ("Play");
        stopTimer();
        return;
    }
    const auto files = selectedFiles();
    if (files.empty())
    {
        studio.setStatus ("Select an audio file to play.");
        return;
    }
    startPreview (files.front());
}

void MediaList::timerCallback()
{
    // The preview ends by itself: Loop plays it again, otherwise the button goes back to Play.
    if (studio.engine().previewing())
        return;
    if (loop.getToggleState() && ! previewHash.empty() && studio.model().mediaRef (previewHash) != nullptr && startPreview (previewHash))
        return;
    previewHash.clear();
    play.setButtonText ("Play");
    stopTimer();
}

juce::PopupMenu MediaList::fileMenu() const
{
    const auto files = selectedFiles();
    const auto* track = studio.model().track (studio.selectedTrack());
    const bool onAudio = track != nullptr && track->kind == model::TrackKind::audio;
    juce::PopupMenu m;
    m.addItem (itemAddFile, "Add Audio File...");
    m.addItem (itemAddRegion, "Add Region to Selected Track", ! files.empty() && onAudio);
    m.addItem (itemAddToTracks, "Add to New Tracks", ! files.empty());
    m.addItem (itemRename, "Rename...", files.size() == 1);
    m.addItem (itemDelete, files.size() > 1 ? "Delete Files" : "Delete File", ! files.empty());
    m.addSeparator();
    m.addItem (itemSelectUnused, "Select Unused");
    m.addItem (itemShowAll, "Show All Regions");
    m.addItem (itemHideAll, "Hide All Regions");
    m.addSeparator();
    m.addItem (itemReveal, "Show in File Manager", files.size() == 1);
    return m;
}

void MediaList::pick (int item)
{
    const auto files = selectedFiles();
    switch (item)
    {
        case itemAddFile:
        {
            auto add = [this] (std::vector<std::filesystem::path> chosen) {
                if (chosen.empty())
                    return;
                engine::DropTarget target;
                target.mediaOnly = true;
                const auto before = arrange::mediaUsage (studio.model());
                studio.importFiles (chosen, target);
                std::vector<std::string> added;
                for (const auto& u : arrange::mediaUsage (studio.model()))
                    if (std::none_of (before.begin(), before.end(), [&] (const auto& b) { return b.hash == u.hash; }))
                        added.push_back (u.hash);
                rebuild();
                selectFiles (added);
            };
            if (chooseFiles)
                chooseFiles (add);
            else
            {
                chooser = std::make_unique<juce::FileChooser> ("Add Audio Files", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
                chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                                      [add] (const juce::FileChooser& fc) {
                                          std::vector<std::filesystem::path> chosen;
                                          for (const auto& f : fc.getResults())
                                              chosen.emplace_back (f.getFullPathName().toStdString());
                                          add (chosen);
                                      });
            }
            break;
        }
        case itemAddRegion: addRegions (false); break;
        case itemAddToTracks: addRegions (true); break;
        case itemRename:
        {
            if (files.size() != 1)
                break;
            const auto hash = files.front();
            const auto* ref = studio.model().mediaRef (hash);
            if (ref == nullptr)
                break;
            promptForText ("Rename Audio File", juce::String (ref->originalName), [this, hash] (juce::String text) {
                if (text.trim().isNotEmpty())
                    studio.check (studio.session().renameMedia (hash, text.trim().toStdString()));
            });
            break;
        }
        case itemDelete: deleteFiles(); break;
        case itemSelectUnused:
        {
            std::vector<std::string> unused;
            for (const auto& u : items)
                if (u.regions == 0)
                    unused.push_back (u.hash);
            if (unused.empty())
                studio.setStatus ("No unused audio files.");
            selectFiles (unused);
            break;
        }
        case itemShowAll: showAllRegions (true); break;
        case itemHideAll: showAllRegions (false); break;
        case itemReveal:
            if (files.size() == 1)
                juce::File (juce::String (fileFor (files.front()).string())).revealToUser();
            break;
        default: break;
    }
}

void MediaList::addRegions (bool newTracks)
{
    const auto files = selectedFiles();
    if (files.empty())
        return;
    auto& s = studio.session();
    const auto* selected = studio.model().track (studio.selectedTrack());
    if (! newTracks && (selected == nullptr || selected->kind != model::TrackKind::audio))
    {
        studio.setStatus ("Select an audio track.");
        return;
    }
    Id track = newTracks ? Id() : selected->id;
    Ticks at = studio.playhead();
    std::vector<Id> made;
    s.beginGesture (newTracks ? "Add to Tracks" : "Add Region");
    for (const auto& hash : files)
    {
        const auto* ref = studio.model().mediaRef (hash);
        if (ref == nullptr)
            continue;
        const auto length = ref->lengthSamples;
        const auto channels = ref->channels;
        const auto name = std::filesystem::path (ref->originalName).stem().string();
        if (newTracks)
        {
            const auto created = s.createTrack (model::TrackKind::audio, name.empty() ? std::string ("Audio") : name, std::clamp (channels, 1, 2));
            if (! created)
                continue;
            track = *created;
        }
        if (const auto region = s.createAudioRegion (track, newTracks ? studio.playhead() : at, hash, 0, length))
        {
            made.push_back (*region);
            // On one track, the files go one after another.
            at = static_cast<Ticks> (std::ceil (studio.model().regionEndTick (*studio.model().region (*region))));
        }
        else
            studio.setStatus (juce::String (s.lastError()));
    }
    s.endGesture();
    if (! made.empty())
        studio.selection.set (made);
    studio.edited();
}

void MediaList::deleteFiles()
{
    const auto files = selectedFiles();
    if (files.empty())
        return;
    std::vector<Id> regions;
    for (const auto& u : items)
        if (std::find (files.begin(), files.end(), u.hash) != files.end())
            regions.insert (regions.end(), u.regionIds.begin(), u.regionIds.end());
    if (std::find (files.begin(), files.end(), previewHash) != files.end())
        playOrStop();
    // The files and every region that plays them, as one undo step.
    auto& s = studio.session();
    s.beginGesture (files.size() == 1 ? "Delete File" : "Delete Files");
    auto result = regions.empty() ? commands::EditResult {} : s.deleteRegions (regions);
    if (result)
        result = s.removeMedia (files);
    if (result)
        s.endGesture();
    else
    {
        studio.setStatus (juce::String (result.error));
        s.cancelGesture();
    }
    studio.selection.clear();
    studio.edited();
}

SidePanel::SidePanel (Studio& s, const juce::File& browserRoot) : studio (s)
{
    setTitle ("Side panel");
    fileBrowser = new FileBrowser (studio, browserRoot);
    mediaList = new MediaList (studio);
    commentsPanel = new CommentsPanel (studio);
    chatPanel = new ChatPanel (studio);
    historyPanel = new HistoryPanel (studio);
    notesPanel = new NotesPanel (studio);
    listsPanel = new ListsPanel (studio);
    libraryPanel = new LibraryPanel (studio);
    const auto tab = colour (Token::surface);
    tabbed.addTab ("Browser", tab, fileBrowser, true);
    tabbed.addTab ("Media", tab, mediaList, true);
    tabbed.addTab ("Comments", tab, commentsPanel, true);
    tabbed.addTab ("Chat", tab, chatPanel, true);
    tabbed.addTab ("History", tab, historyPanel, true);
    tabbed.addTab ("Notes", tab, notesPanel, true);
    tabbed.addTab ("Lists", tab, listsPanel, true);
    tabbed.addTab ("Library", tab, libraryPanel, true);
    tabbed.setTabBarDepth (size::panelHeader);
    tabbed.setOutline (0);
    addAndMakeVisible (tabbed);
    studio.addChangeListener (this);
    refreshTabs();
}

SidePanel::~SidePanel() { studio.removeChangeListener (this); }

void SidePanel::showTab (Tab tab) { tabbed.setCurrentTabIndex (static_cast<int> (tab)); }

void SidePanel::refreshTabs()
{
    // Looking at a tab reads it.
    const bool visible = studio.layout.sidePanel;
    const auto current = tabbed.getCurrentTabIndex();
    if (visible && current == commentsTab)
        studio.markCommentsSeen();
    if (visible && current == chatTab)
        studio.markChatRead();
    const auto unread = studio.unreadChat();
    const auto comments = studio.commentsUnread() ? juce::String::fromUTF8 ("Comments \xe2\x80\xa2") : juce::String ("Comments");
    const juce::String chat = unread > 0 ? "Chat (" + juce::String (unread) + ")" : juce::String ("Chat");
    if (tabbed.getNumTabs() <= historyTab)
        return;
    if (tabbed.getTabNames()[commentsTab] != comments)
        tabbed.setTabName (commentsTab, comments);
    if (tabbed.getTabNames()[chatTab] != chat)
        tabbed.setTabName (chatTab, chat);
}

void SidePanel::paint (juce::Graphics& g) { g.fillAll (colour (Token::surface)); }
void SidePanel::resized() { tabbed.setBounds (getLocalBounds().withTrimmedLeft (1)); }
} // namespace weft::ui
