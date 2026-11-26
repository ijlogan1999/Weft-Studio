#include "ProjectDialogs.h"

#include <cmath>

namespace weft::ui
{
namespace
{
constexpr int labelX = 20, controlX = 150, rowHeight = 34;

void drawLabels (juce::Graphics& g, const juce::StringArray& labels)
{
    g.fillAll (colour (Token::surfaceRaised));
    g.setColour (colour (Token::text));
    g.setFont (font (Type::body));
    for (int i = 0; i < labels.size(); ++i)
        g.drawText (labels[i], labelX, 16 + i * rowHeight, controlX - labelX - 8, 26, juce::Justification::centredLeft);
}

void addRates (juce::ComboBox& box, int selected)
{
    for (const int r : { 44100, 48000, 88200, 96000 })
        box.addItem (juce::String (r / 1000.0, r % 1000 == 0 ? 0 : 1) + " kHz", r);
    box.setSelectedId (selected, juce::dontSendNotification);
}

void styleError (juce::Label& l) { l.setColour (juce::Label::textColourId, colour (Token::warning)); }

juce::String text (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }
} // namespace

juce::String pathText (const std::filesystem::path& p)
{
    const auto s = p.u8string();
    return juce::String::fromUTF8 (reinterpret_cast<const char*> (s.c_str()), static_cast<int> (s.size()));
}

std::filesystem::path textPath (const juce::String& s)
{
    const auto bytes = s.toStdString();
    return std::filesystem::path (std::u8string (bytes.begin(), bytes.end()));
}

// New Project ------------------------------------------------------------------------------

NewProjectDialog::NewProjectDialog (Studio& s) : studio (s)
{
    name.setText ("Untitled", false);
    describe (name, "Project name");
    addRates (sampleRate, 48000);
    describe (sampleRate, "Sample rate", "Can be changed in Project Settings until the project has audio");
    describe (create, "Create", "Create the project (Enter)");
    describe (cancel, "Cancel", "Close (Esc)");
    create.addShortcut (juce::KeyPress (juce::KeyPress::returnKey));
    cancel.addShortcut (juce::KeyPress (juce::KeyPress::escapeKey));
    create.onClick = [this] {
        if (createProject() && onClose)
            onClose();
    };
    cancel.onClick = [this] {
        if (onClose)
            onClose();
    };
    styleError (error);
    for (auto* c : std::initializer_list<juce::Component*> { &name, &sampleRate, &create, &cancel, &error })
        addAndMakeVisible (c);
    setSize (420, 170);
}

commands::NewProjectOptions NewProjectDialog::options() const
{
    commands::NewProjectOptions o;
    o.name = name.getText().trim().toStdString();
    o.sampleRate = sampleRate.getSelectedId();
    return o;
}

bool NewProjectDialog::createProject()
{
    const auto o = options();
    if (o.name.empty())
    {
        error.setText ("A project needs a name.", juce::dontSendNotification);
        return false;
    }
    std::string why;
    if (! studio.newProject (studio.projectsFolder(), o, &why))
    {
        error.setText (text (why), juce::dontSendNotification);
        return false;
    }
    if (onCreated)
        onCreated();
    return true;
}

void NewProjectDialog::paint (juce::Graphics& g) { drawLabels (g, { "Name", "Sample rate" }); }

void NewProjectDialog::resized()
{
    name.setBounds (controlX, 16, getWidth() - controlX - 20, 26);
    sampleRate.setBounds (controlX, 16 + rowHeight, 140, 26);
    error.setBounds (labelX, 16 + 2 * rowHeight, getWidth() - 40, 22);
    create.setBounds (getWidth() - 200, getHeight() - 42, 84, 28);
    cancel.setBounds (getWidth() - 104, getHeight() - 42, 84, 28);
}

// Save As ------------------------------------------------------------------------------------

SaveAsDialog::SaveAsDialog (Studio& s) : studio (s)
{
    name.setText (text (studio.model().info.name) + " copy", false);
    folder.setText (pathText (studio.projectRoot().parent_path()), false);
    describe (name, "Project name");
    describe (folder, "Folder", "Where the new project folder goes");
    describe (choose, "Choose...", "Pick the folder");
    copyMedia.setToggleState (true, juce::dontSendNotification);
    describe (copyMedia, "Copy media", "Copy the audio files into the new project; off: keep using them where they are");
    choose.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser> ("Save the project in", juce::File (folder.getText()));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe = juce::Component::SafePointer<SaveAsDialog> (this)] (const juce::FileChooser& fc) {
                                  if (safe != nullptr && fc.getResult() != juce::File())
                                      safe->folder.setText (fc.getResult().getFullPathName(), false);
                              });
    };
    describe (save, "Save", "Save the project under the new name (Enter)");
    describe (cancel, "Cancel", "Close (Esc)");
    save.addShortcut (juce::KeyPress (juce::KeyPress::returnKey));
    cancel.addShortcut (juce::KeyPress (juce::KeyPress::escapeKey));
    save.onClick = [this] {
        if (saveProject() && onClose)
            onClose();
    };
    cancel.onClick = [this] {
        if (onClose)
            onClose();
    };
    styleError (error);
    for (auto* c : std::initializer_list<juce::Component*> { &name, &folder, &choose, &copyMedia, &save, &cancel, &error })
        addAndMakeVisible (c);
    setSize (520, 210);
}

std::filesystem::path SaveAsDialog::target() const
{
    return textPath (folder.getText().trim()) / textPath (name.getText().trim() + ".weft");
}

bool SaveAsDialog::saveProject()
{
    if (name.getText().trim().isEmpty())
    {
        error.setText ("A project needs a name.", juce::dontSendNotification);
        return false;
    }
    std::string why;
    if (! studio.saveAs (target(), copyMedia.getToggleState(), &why))
    {
        error.setText (text (why), juce::dontSendNotification);
        return false;
    }
    return true;
}

void SaveAsDialog::paint (juce::Graphics& g) { drawLabels (g, { "Name", "Folder" }); }

void SaveAsDialog::resized()
{
    name.setBounds (controlX, 16, getWidth() - controlX - 20, 26);
    folder.setBounds (controlX, 16 + rowHeight, getWidth() - controlX - 120, 26);
    choose.setBounds (getWidth() - 110, 16 + rowHeight, 90, 26);
    copyMedia.setBounds (controlX, 16 + 2 * rowHeight, getWidth() - controlX - 20, 26);
    error.setBounds (labelX, 16 + 3 * rowHeight, getWidth() - 40, 22);
    save.setBounds (getWidth() - 200, getHeight() - 42, 84, 28);
    cancel.setBounds (getWidth() - 104, getHeight() - 42, 84, 28);
}

// Project Settings ---------------------------------------------------------------------------

ProjectSettingsDialog::ProjectSettingsDialog (Studio& s) : studio (s)
{
    const auto& info = studio.model().info;
    name.setText (text (info.name), false);
    describe (name, "Project name");
    bitDepth.addItem ("16-bit", 16);
    bitDepth.addItem ("24-bit", 24);
    bitDepth.addItem ("32-bit float", 32);
    bitDepth.setSelectedId (info.recordBitDepth, juce::dontSendNotification);
    describe (bitDepth, "Recording bit depth", "The format of new recordings");
    addRates (sampleRate, info.sampleRate);
    const bool rateEditable = studio.model().media.empty();
    sampleRate.setEnabled (rateEditable);
    describe (sampleRate, "Sample rate", rateEditable ? "The project's sample rate" : "Fixed once the project has audio");
    rateNote.setText (rateEditable ? "" : "Fixed once there's audio.", juce::dontSendNotification);
    rateNote.setColour (juce::Label::textColourId, colour (Token::textDim));
    for (int minor = 0; minor < 2; ++minor)
        for (int tonic = 0; tonic < 12; ++tonic)
            key.addItem (keyText ({ tonic, minor == 1 }), tonic + 1 + 12 * minor);
    key.setSelectedId (info.key.tonic + 1 + (info.key.minor ? 12 : 0), juce::dontSendNotification);
    describe (key, "Key", "The project's key; written to exported MIDI files");

    const auto& m = info.metronomeDefaults;
    metronome.setToggleState (m.enabled, juce::dontSendNotification);
    describe (metronome, "Metronome on", "Whether the metronome is on when the project opens");
    metronomeDuring.addItemList ({ "Recording", "Playback", "Recording and playback" }, 1);
    metronomeDuring.setSelectedId (m.during == "play" ? 2 : m.during == "both" ? 3 : 1, juce::dontSendNotification);
    describe (metronomeDuring, "Metronome plays during");
    for (const auto choice : model::countInChoices)
        countIn.addItem (model::countInName (choice), countIn.getNumItems() + 1);
    countIn.setSelectedId (model::countInIndex (m.countInBars) + 1, juce::dontSendNotification);
    describe (countIn, "Count-in");
    metronomeLevel.setRange (-60.0, 6.0, 0.5);
    metronomeLevel.setValue (m.levelDb, juce::dontSendNotification);
    metronomeLevel.setTextValueSuffix (" dB");
    metronomeLevel.setSliderStyle (juce::Slider::LinearHorizontal);
    metronomeLevel.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);
    describe (metronomeLevel, "Metronome level");

    static const char* const lawNames[] = { "0 dB (centre at full level)", "-3 dB (equal power)", "-4.5 dB", "-6 dB (linear)" };
    static_assert (std::size (lawNames) == model::panLaws.size());
    for (std::size_t i = 0; i < model::panLaws.size(); ++i)
        panLaw.addItem (lawNames[i], static_cast<int> (i) + 1);
    panLaw.setSelectedId (model::panLawIndex (info.panLawDb) + 1, juce::dontSendNotification);
    describe (panLaw, "Pan law", "How loud a mono track panned to the centre plays on each side");
    for (int i = 0; i < 6; ++i)
    {
        ignoreMidi[i].setToggleState ((info.midiInputFilter & (1 << i)) != 0, juce::dontSendNotification);
        describe (ignoreMidi[i], "Ignore " + ignoreMidi[i].getButtonText().toLowerCase(), "Live MIDI input of this kind is neither played nor recorded");
        addAndMakeVisible (ignoreMidi[i]);
    }

    describe (ok, "OK", "Apply the settings (Enter)");
    describe (cancel, "Cancel", "Close without changes (Esc)");
    ok.addShortcut (juce::KeyPress (juce::KeyPress::returnKey));
    cancel.addShortcut (juce::KeyPress (juce::KeyPress::escapeKey));
    ok.onClick = [this] {
        if (apply() && onClose)
            onClose();
    };
    cancel.onClick = [this] {
        if (onClose)
            onClose();
    };
    styleError (error);
    for (auto* c : std::initializer_list<juce::Component*> { &name, &bitDepth, &sampleRate, &rateNote, &key, &metronome, &metronomeDuring, &countIn,
                                                             &metronomeLevel, &panLaw, &ok, &cancel, &error })
        addAndMakeVisible (c);
    setSize (500, 344 + 3 * rowHeight);
}

bool ProjectSettingsDialog::apply()
{
    auto& session = studio.session();
    const auto& info = studio.model().info;
    const auto oldPanLaw = model::panLawIndex (info.panLawDb) + 1; // by value: the edits below replace the model
    const auto oldFilter = info.midiInputFilter;
    std::vector<commands::EditResult> results;
    const auto newName = name.getText().trim().toStdString();
    if (newName != info.name)
        results.push_back (session.renameProject (newName));
    if (bitDepth.getSelectedId() != info.recordBitDepth)
        results.push_back (session.setRecordBitDepth (bitDepth.getSelectedId()));
    if (sampleRate.isEnabled() && sampleRate.getSelectedId() != info.sampleRate)
        results.push_back (session.setSampleRate (sampleRate.getSelectedId()));
    const model::KeySignature chosen { (key.getSelectedId() - 1) % 12, key.getSelectedId() > 12 };
    if (key.getSelectedId() > 0 && chosen != info.key)
        results.push_back (session.setKey (chosen));
    model::MetronomeSettings m = info.metronomeDefaults;
    m.enabled = metronome.getToggleState();
    m.during = metronomeDuring.getSelectedId() == 2 ? "play" : metronomeDuring.getSelectedId() == 3 ? "both" : "record";
    m.countInBars = model::countInChoices[static_cast<std::size_t> (std::max (1, countIn.getSelectedId()) - 1)];
    m.levelDb = metronomeLevel.getValue();
    const auto& old = info.metronomeDefaults;
    if (m.enabled != old.enabled || m.during != old.during || m.countInBars != old.countInBars || std::abs (m.levelDb - old.levelDb) > 1e-9)
        results.push_back (session.setMetronomeDefaults (m));
    if (panLaw.getSelectedId() > 0 && panLaw.getSelectedId() != oldPanLaw)
        results.push_back (session.setPanLaw (model::panLaws[static_cast<std::size_t> (panLaw.getSelectedId() - 1)]));
    int filter = 0;
    for (int i = 0; i < 6; ++i)
        filter |= ignoreMidi[i].getToggleState() ? 1 << i : 0;
    if (filter != oldFilter)
        results.push_back (session.setMidiInputFilter (filter));
    studio.edited();
    for (const auto& r : results)
        if (! r)
        {
            error.setText (text (r.error), juce::dontSendNotification);
            return false;
        }
    return true;
}

void ProjectSettingsDialog::paint (juce::Graphics& g)
{
    drawLabels (g, { "Name", "Recording", "Sample rate", "Key", "Metronome", "Plays during", "Count-in", "Level", "Pan law", "Ignore MIDI" });
}

void ProjectSettingsDialog::resized()
{
    const int w = getWidth() - controlX - 20;
    name.setBounds (controlX, 16, w, 26);
    bitDepth.setBounds (controlX, 16 + rowHeight, 160, 26);
    sampleRate.setBounds (controlX, 16 + 2 * rowHeight, 160, 26);
    rateNote.setBounds (controlX + 170, 16 + 2 * rowHeight, w - 170, 26);
    key.setBounds (controlX, 16 + 3 * rowHeight, 160, 26);
    metronome.setBounds (controlX, 16 + 4 * rowHeight, w, 26);
    metronomeDuring.setBounds (controlX, 16 + 5 * rowHeight, 220, 26);
    countIn.setBounds (controlX, 16 + 6 * rowHeight, 120, 26);
    metronomeLevel.setBounds (controlX, 16 + 7 * rowHeight, w, 26);
    panLaw.setBounds (controlX, 16 + 8 * rowHeight, 240, 26);
    for (int i = 0; i < 6; ++i)
        ignoreMidi[i].setBounds (controlX + (i % 3) * (w / 3), 16 + (9 + i / 3) * rowHeight, w / 3, 26);
    error.setBounds (labelX, getHeight() - 42, getWidth() - 240, 28);
    ok.setBounds (getWidth() - 200, getHeight() - 42, 84, 28);
    cancel.setBounds (getWidth() - 104, getHeight() - 42, 84, 28);
}

// Settings -----------------------------------------------------------------------------------

SettingsDialog::SettingsDialog (Studio& s) : studio (s)
{
    const auto& p = studio.preferences();
    startup.addItem ("Show the Welcome window", 1);
    startup.addItem ("Open the most recent project", 2);
    startup.setSelectedId (p.startup == Preferences::Startup::lastProject ? 2 : 1, juce::dontSendNotification);
    describe (startup, "When Weft Studio starts");
    startup.onChange = [this] { change ([this] (Preferences& x) { x.startup = startup.getSelectedId() == 2 ? Preferences::Startup::lastProject : Preferences::Startup::welcome; }); };
    for (int n : { 5, 10, 20, 50 })
        recentItems.addItem (juce::String (n), n);
    recentItems.setSelectedId (p.recentItems, juce::dontSendNotification);
    describe (recentItems, "Recent projects", "How many projects File › Open Recent lists");
    recentItems.onChange = [this] { change ([this] (Preferences& x) { x.recentItems = recentItems.getSelectedId(); }); };

    softwareMonitoring.setToggleState (p.softwareMonitoring, juce::dontSendNotification);
    describe (softwareMonitoring, "Software monitoring", "Off: no track plays its input through, whatever its monitoring setting (for hardware monitoring)");
    softwareMonitoring.onClick = [this] { change ([this] (Preferences& x) { x.softwareMonitoring = softwareMonitoring.getToggleState(); }); };
    monitorSelectedOnly.setToggleState (p.monitorSelectedOnly, juce::dontSendNotification);
    describe (monitorSelectedOnly, "Monitor only the selected track", "Only the selected track plays its input through");
    monitorSelectedOnly.onClick = [this] { change ([this] (Preferences& x) { x.monitorSelectedOnly = monitorSelectedOnly.getToggleState(); }); };
    overloadMessage.setToggleState (p.overloadMessage, juce::dontSendNotification);
    describe (overloadMessage, "Say when the audio drops out", "A message in the status bar when the audio engine can't keep up");
    overloadMessage.onClick = [this] { change ([this] (Preferences& x) { x.overloadMessage = overloadMessage.getToggleState(); }); };
    describe (audioDevice, Cmd::audioSettings);
    audioDevice.onClick = [this] {
        if (onAudioSettings)
            onAudioSettings();
    };

    appearance.addItem ("Dark", 1);
    appearance.addItem ("Light", 2);
    appearance.addItem ("Follow the system", 3);
    appearance.setSelectedId (p.appearance == Preferences::Appearance::light ? 2 : p.appearance == Preferences::Appearance::system ? 3 : 1, juce::dontSendNotification);
    describe (appearance, "Appearance");
    appearance.onChange = [this] {
        change ([this] (Preferences& x) {
            x.appearance = appearance.getSelectedId() == 2   ? Preferences::Appearance::light
                           : appearance.getSelectedId() == 3 ? Preferences::Appearance::system
                                                             : Preferences::Appearance::dark;
        });
    };
    middleC.addItem ("C3 (Weft, Yamaha)", 3);
    middleC.addItem ("C4 (scientific)", 4);
    middleC.setSelectedId (p.middleC, juce::dontSendNotification);
    describe (middleC, "Middle C", "How note 60 is named in the piano roll and the inspector");
    middleC.onChange = [this] { change ([this] (Preferences& x) { x.middleC = middleC.getSelectedId(); }); };
    widePlayhead.setToggleState (p.widePlayhead, juce::dontSendNotification);
    describe (widePlayhead, "Wide playhead", "A thicker playhead line, easier to see");
    widePlayhead.onClick = [this] { change ([this] (Preferences& x) { x.widePlayhead = widePlayhead.getToggleState(); }); };
    helpTags.setToggleState (p.helpTags, juce::dontSendNotification);
    describe (helpTags, "Show help tags", "Tooltips when the pointer rests on a control");
    helpTags.onClick = [this] { change ([this] (Preferences& x) { x.helpTags = helpTags.getToggleState(); }); };
    close.onClick = [this] {
        if (onClose)
            onClose();
    };
    close.addShortcut (juce::KeyPress (juce::KeyPress::escapeKey));
    for (juce::Component* c : std::initializer_list<juce::Component*> { &startup, &recentItems, &softwareMonitoring, &monitorSelectedOnly, &overloadMessage, &audioDevice,
                                                                          &appearance, &middleC, &widePlayhead, &helpTags, &close })
        addAndMakeVisible (c);
    setSize (520, 16 + 12 * rowHeight + 50);
}

void SettingsDialog::change (const std::function<void (Preferences&)>& edit)
{
    auto p = studio.preferences();
    edit (p);
    if (! (p == studio.preferences()))
        studio.setPreferences (p);
}

void SettingsDialog::paint (juce::Graphics& g)
{
    // Section captions, then the labels of the rows that have them.
    g.setFont (font (Type::strong));
    g.setColour (colour (Token::text));
    for (const auto& [row, text] : { std::pair { 0, "General" }, { 3, "Audio" }, { 8, "View" } })
        g.drawText (text, labelX, 16 + row * rowHeight, controlX - labelX, 26, juce::Justification::centredLeft);
    g.setFont (font (Type::body));
    g.setColour (colour (Token::textDim));
    for (const auto& [row, text] : { std::pair { 1, "On start" }, { 2, "Recent" }, { 9, "Appearance" }, { 10, "Middle C" } })
        g.drawText (text, labelX, 16 + row * rowHeight, controlX - labelX - 8, 26, juce::Justification::centredLeft);
}

void SettingsDialog::resized()
{
    const int w = getWidth() - controlX - 20;
    const auto at = [&] (int row) { return 16 + row * rowHeight; };
    startup.setBounds (controlX, at (1), w, 26);
    recentItems.setBounds (controlX, at (2), 80, 26);
    softwareMonitoring.setBounds (controlX, at (4), w, 26);
    monitorSelectedOnly.setBounds (controlX, at (5), w, 26);
    overloadMessage.setBounds (controlX, at (6), w, 26);
    audioDevice.setBounds (controlX, at (7), 200, 26);
    appearance.setBounds (controlX, at (9), 200, 26);
    middleC.setBounds (controlX, at (10), 200, 26);
    widePlayhead.setBounds (controlX, at (11), w / 2, 26);
    helpTags.setBounds (controlX + w / 2, at (11), w / 2, 26);
    close.setBounds (getWidth() - 104, getHeight() - 42, 84, 28);
}

// Missing Media ----------------------------------------------------------------------------

MissingMediaDialog::MissingMediaDialog (Studio& s) : studio (s)
{
    message.setJustificationType (juce::Justification::topLeft);
    describe (locate, "Locate...", "Search a folder for the missing files (matched by content, whatever their names)");
    describe (proceed, "Continue", "Open the project without them; their regions show as missing");
    proceed.addShortcut (juce::KeyPress (juce::KeyPress::escapeKey));
    locate.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser> ("Locate missing media in", juce::File (pathText (studio.projectRoot().parent_path())));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe = juce::Component::SafePointer<MissingMediaDialog> (this)] (const juce::FileChooser& fc) {
                                  if (safe != nullptr && fc.getResult() != juce::File())
                                      safe->locateIn (textPath (fc.getResult().getFullPathName()));
                              });
    };
    proceed.onClick = [this] {
        if (onClose)
            onClose();
    };
    for (auto* c : std::initializer_list<juce::Component*> { &message, &locate, &proceed })
        addAndMakeVisible (c);
    refresh();
    setSize (440, 160);
}

void MissingMediaDialog::refresh()
{
    const auto n = static_cast<int> (studio.missingMedia().size());
    message.setText (n == 0 ? juce::String ("All audio files were found.")
                            : juce::String (n) + (n == 1 ? " audio file is" : " audio files are")
                                  + " missing. Their regions show as missing until you locate them.",
                     juce::dontSendNotification);
    locate.setEnabled (n > 0);
    proceed.setButtonText (n == 0 ? "Close" : "Continue");
}

void MissingMediaDialog::locateIn (const std::filesystem::path& folder)
{
    const auto found = studio.locateMedia (folder);
    studio.setStatus (found == 0 ? juce::String ("No matching files in that folder.")
                                 : "Found " + juce::String (found) + (found == 1 ? " file." : " files."));
    refresh();
}

void MissingMediaDialog::paint (juce::Graphics& g) { g.fillAll (colour (Token::surfaceRaised)); }

void MissingMediaDialog::resized()
{
    message.setBounds (labelX, 16, getWidth() - 40, 70);
    locate.setBounds (getWidth() - 200, getHeight() - 42, 84, 28);
    proceed.setBounds (getWidth() - 104, getHeight() - 42, 84, 28);
}

// Welcome ------------------------------------------------------------------------------------

WelcomeDialog::WelcomeDialog (Studio& s, std::function<void (Cmd)> runCommand, std::function<void (const std::filesystem::path&)> openRecent)
    : studio (s)
{
    describe (newProject, "New Project...", "Create a project with a name and sample rate (Ctrl+N)");
    describe (open, "Open...", "Open a project (Ctrl+O)");
    describe (startEmpty, "Start with an Empty Project", "Work in the untitled project that's already open");
    newProject.onClick = [this, runCommand] {
        if (onClose)
            onClose();
        runCommand (Cmd::newProject);
    };
    open.onClick = [this, runCommand] {
        if (onClose)
            onClose();
        runCommand (Cmd::openProject);
    };
    startEmpty.onClick = [this] {
        if (onClose)
            onClose();
    };
    for (const auto& root : studio.recentProjects().items())
    {
        auto label = pathText (root.filename()).upToLastOccurrenceOf (".weft", false, false);
        auto* b = recent.add (new juce::TextButton (label));
        const bool there = project::RecentProjects::available (root);
        describe (*b, label, there ? pathText (root) : "Not found: " + pathText (root));
        b->setEnabled (there);
        b->onClick = [this, root, openRecent] {
            if (onClose)
                onClose();
            openRecent (root);
        };
        addAndMakeVisible (b);
    }
    for (auto* c : std::initializer_list<juce::Component*> { &newProject, &open, &startEmpty })
        addAndMakeVisible (c);
    setSize (460, 150 + 32 * std::max (1, recent.size()));
}

void WelcomeDialog::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::surfaceRaised));
    g.setColour (colour (Token::text));
    g.setFont (font (Type::display));
    g.drawText ("Weft Studio", labelX, 12, getWidth() - 40, 30, juce::Justification::centredLeft);
    g.setFont (font (Type::body));
    g.setColour (colour (Token::textDim));
    g.drawText (recent.isEmpty() ? "No recent projects yet." : "Recent projects", labelX, 96, getWidth() - 40, 22, juce::Justification::centredLeft);
}

void WelcomeDialog::resized()
{
    newProject.setBounds (labelX, 52, 130, 30);
    open.setBounds (labelX + 140, 52, 90, 30);
    startEmpty.setBounds (labelX + 240, 52, getWidth() - labelX - 260, 30);
    for (int i = 0; i < recent.size(); ++i)
        recent[i]->setBounds (labelX, 122 + i * 32, getWidth() - 40, 28);
}
} // namespace weft::ui
