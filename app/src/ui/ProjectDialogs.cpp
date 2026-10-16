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
    g.setFont (juce::FontOptions (13.0f));
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

    const auto& m = info.metronomeDefaults;
    metronome.setToggleState (m.enabled, juce::dontSendNotification);
    describe (metronome, "Metronome on", "Whether the metronome is on when the project opens");
    metronomeDuring.addItemList ({ "Recording", "Playback", "Recording and playback" }, 1);
    metronomeDuring.setSelectedId (m.during == "play" ? 2 : m.during == "both" ? 3 : 1, juce::dontSendNotification);
    describe (metronomeDuring, "Metronome plays during");
    for (int bars = 0; bars <= 4; ++bars)
        countIn.addItem (bars == 0 ? juce::String ("None") : juce::String (bars) + (bars == 1 ? " bar" : " bars"), bars + 1);
    countIn.setSelectedId (std::clamp (m.countInBars, 0, 4) + 1, juce::dontSendNotification);
    describe (countIn, "Count-in");
    metronomeLevel.setRange (-60.0, 6.0, 0.5);
    metronomeLevel.setValue (m.levelDb, juce::dontSendNotification);
    metronomeLevel.setTextValueSuffix (" dB");
    metronomeLevel.setSliderStyle (juce::Slider::LinearHorizontal);
    metronomeLevel.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);
    describe (metronomeLevel, "Metronome level");

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
    for (auto* c : std::initializer_list<juce::Component*> { &name, &bitDepth, &sampleRate, &rateNote, &metronome, &metronomeDuring, &countIn,
                                                             &metronomeLevel, &ok, &cancel, &error })
        addAndMakeVisible (c);
    setSize (500, 310);
}

bool ProjectSettingsDialog::apply()
{
    auto& session = studio.session();
    const auto& info = studio.model().info;
    std::vector<commands::EditResult> results;
    const auto newName = name.getText().trim().toStdString();
    if (newName != info.name)
        results.push_back (session.renameProject (newName));
    if (bitDepth.getSelectedId() != info.recordBitDepth)
        results.push_back (session.setRecordBitDepth (bitDepth.getSelectedId()));
    if (sampleRate.isEnabled() && sampleRate.getSelectedId() != info.sampleRate)
        results.push_back (session.setSampleRate (sampleRate.getSelectedId()));
    model::MetronomeSettings m = info.metronomeDefaults;
    m.enabled = metronome.getToggleState();
    m.during = metronomeDuring.getSelectedId() == 2 ? "play" : metronomeDuring.getSelectedId() == 3 ? "both" : "record";
    m.countInBars = countIn.getSelectedId() - 1;
    m.levelDb = metronomeLevel.getValue();
    const auto& old = info.metronomeDefaults;
    if (m.enabled != old.enabled || m.during != old.during || m.countInBars != old.countInBars || std::abs (m.levelDb - old.levelDb) > 1e-9)
        results.push_back (session.setMetronomeDefaults (m));
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
    drawLabels (g, { "Name", "Recording", "Sample rate", "Metronome", "Plays during", "Count-in", "Level" });
}

void ProjectSettingsDialog::resized()
{
    const int w = getWidth() - controlX - 20;
    name.setBounds (controlX, 16, w, 26);
    bitDepth.setBounds (controlX, 16 + rowHeight, 160, 26);
    sampleRate.setBounds (controlX, 16 + 2 * rowHeight, 160, 26);
    rateNote.setBounds (controlX + 170, 16 + 2 * rowHeight, w - 170, 26);
    metronome.setBounds (controlX, 16 + 3 * rowHeight, w, 26);
    metronomeDuring.setBounds (controlX, 16 + 4 * rowHeight, 220, 26);
    countIn.setBounds (controlX, 16 + 5 * rowHeight, 120, 26);
    metronomeLevel.setBounds (controlX, 16 + 6 * rowHeight, w, 26);
    error.setBounds (labelX, getHeight() - 42, getWidth() - 240, 28);
    ok.setBounds (getWidth() - 200, getHeight() - 42, 84, 28);
    cancel.setBounds (getWidth() - 104, getHeight() - 42, 84, 28);
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
    g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    g.drawText ("Weft Studio", labelX, 12, getWidth() - 40, 30, juce::Justification::centredLeft);
    g.setFont (juce::FontOptions (13.0f));
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
