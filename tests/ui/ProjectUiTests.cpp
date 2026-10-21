// Save/load UX (M9, W-0 and W-10): New Project (AT-PRJ-1), the title's "*" (AT-PRJ-5), Open
// Recent (AT-PRJ-10), Project Settings (AT-PRJ-13), first-run Audio Setup and No-audio mode
// (AT-UI-8), and the questions around opening: unsaved changes, crash recovery, missing
// plug-ins and missing media.

#include "UiFixture.h"

#include "ui/AudioSetup.h"
#include "ui/ProjectDialogs.h"

#include "weft/media/Hash.h"
#include "weft/media/MediaStore.h"
#include "weft/project/ProjectFiles.h"

using namespace weft;
using namespace weft::ui;
using namespace weft::ui::test;
namespace fs = std::filesystem;

namespace
{
constexpr int ctrl = juce::ModifierKeys::commandModifier;

/** A saved project on disk with `tracks` extra audio tracks; returns its folder. */
fs::path makeProject (const fs::path& parent, const std::string& name, int tracks = 0)
{
    const auto root = project::ProjectFolder::folderFor (parent, name);
    project::ProjectFolder folder (root);
    REQUIRE (folder.create());
    commands::NewProjectOptions o;
    o.name = name;
    auto s = commands::Session::createNew (o, "local").value();
    for (int i = 0; i < tracks; ++i)
        REQUIRE (s.createTrack (model::TrackKind::audio, "Extra " + std::to_string (i + 1)));
    REQUIRE (folder.save (s.document()));
    return root;
}

/** Labels of File › Open Recent. */
juce::StringArray recentLabels (MainComponent& main)
{
    juce::StringArray labels;
    const auto file = main.getMenuForIndex (0, "File");
    for (juce::PopupMenu::MenuItemIterator it (file); it.next();)
        if (it.getItem().text == "Open Recent" && it.getItem().subMenu != nullptr)
            for (juce::PopupMenu::MenuItemIterator sub (*it.getItem().subMenu); sub.next();)
                labels.add (sub.getItem().text);
    return labels;
}

/** A device that never opens: the machine has no usable output. */
class DeadDevice final : public engine::AudioDevice
{
public:
    std::string name() const override { return "Dead"; }
    bool open (const engine::DeviceConfig&) override { return false; }
    void close() override {}
    bool start (engine::AudioCallback&) override { return false; }
    void stop() override {}
    bool isRunning() const override { return false; }
    engine::DeviceConfig config() const override { return {}; }
};

/** The machine as the tests see it: one null driver with one device each way and a MIDI input. */
class TestDevices final : public DeviceProvider
{
public:
    std::vector<std::string> drivers() override { return { "Null" }; }
    std::string defaultDriver() override { return "Null"; }
    std::vector<std::string> outputs (const std::string&) override { return { "Null Out" }; }
    std::vector<std::string> inputs (const std::string&) override { return { "Null In" }; }
    std::string defaultOutput (const std::string&) override { return "Null Out"; }
    std::string defaultInput (const std::string&) override { return "Null In"; }
    Capabilities capabilities (const std::string&, const std::string&, const std::string&) override
    {
        return { { 44100.0, 48000.0, 96000.0 }, { 128, 256, 480, 512, 1024 }, 48000.0, 512 };
    }
    std::unique_ptr<engine::AudioDevice> make (const AudioSettings&) override
    {
        if (dead)
            return std::make_unique<DeadDevice>();
        auto d = std::make_unique<engine::NullDevice>();
        last = d.get();
        return d;
    }
    std::vector<std::string> midiInputs() override { return { "USB Keys" }; }

    engine::NullDevice* last = nullptr;
    bool dead = false;
};

void forgetAudioSettings()
{
    std::error_code ec;
    fs::remove (audioSettingsFile(), ec);
}
} // namespace

TEST_CASE ("AT-PRJ-1 New Project at 48 kHz has the default contents; File menu has New, Open, Save As", "[UI][project][AT-PRJ-1]")
{
    Ui ui;
    const auto scratch = ui.studio.projectRoot();
    CHECK (commandInfo (Cmd::newProject).shortcut == "Ctrl+N");
    CHECK (commandInfo (Cmd::openProject).shortcut == "Ctrl+O");
    CHECK (commandInfo (Cmd::saveAs).shortcut == "Ctrl+Shift+S");
    juce::StringArray file;
    const auto menu = ui.main->getMenuForIndex (0, "File");
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        if (! it.getItem().isSeparator)
            file.add (it.getItem().text);
    CHECK (file == juce::StringArray { "New Project", "Open", "Open Recent", "Save", "Save As", "Save Version...", "Bounce", "Project Settings", "Audio & MIDI Settings" });

    NewProjectDialog dialog (ui.studio);
    CHECK (dialog.name.getText() == "Untitled");
    CHECK (dialog.sampleRate.getSelectedId() == 48000);
    dialog.name.setText ("Song", false);
    REQUIRE (dialog.createProject());
    ui.settle();

    const auto& m = ui.m();
    CHECK (m.info.name == "Song");
    CHECK (m.info.sampleRate == 48000);
    CHECK (ui.studio.projectRoot() == ui.studio.projectsFolder() / "Song.weft");
    REQUIRE (m.storedTempoPoints.size() == 1);
    CHECK (m.storedTempoPoints[0].pos == 0);
    CHECK (m.storedTempoPoints[0].bpm == Catch::Approx (120.0));
    REQUIRE (m.storedSignaturePoints.size() == 1);
    CHECK (m.storedSignaturePoints[0].numerator == 4);
    CHECK (m.storedSignaturePoints[0].denominator == 4);
    REQUIRE (m.trackOrder.size() == 1);
    CHECK (m.track (m.trackOrder[0])->name == "Audio 1");
    CHECK (m.track (m.info.masterTrackId) != nullptr);
    CHECK (m.tracks.size() == 2);
    CHECK (m.regions.empty());
    CHECK (m.markers.empty());
    // The untouched "Untitled" project it replaced leaves nothing behind.
    CHECK_FALSE (fs::exists (scratch));

    // Another at 96 kHz; the name is taken, so it becomes "Song 2".
    NewProjectDialog again (ui.studio);
    again.name.setText ("Song", false);
    again.sampleRate.setSelectedId (96000);
    REQUIRE (again.createProject());
    CHECK (ui.m().info.name == "Song 2");
    CHECK (ui.m().info.sampleRate == 96000);
    CHECK (fs::exists (ui.studio.projectsFolder() / "Song.weft")); // it was saved, so it stays
}

TEST_CASE ("AT-PRJ-5 the title shows * after an edit and not after Ctrl+S", "[UI][project][AT-PRJ-5]")
{
    Ui ui;
    CHECK (ui.studio.windowTitle() == "Untitled - Weft Studio");
    CHECK (ui.main->getName() == "Untitled - Weft Studio");
    ui.instrumentTracks (1);
    CHECK (ui.studio.windowTitle() == "Untitled* - Weft Studio");
    CHECK (ui.main->getName() == "Untitled* - Weft Studio");
    CHECK (ui.key ('S', ctrl));
    CHECK (ui.main->getName() == "Untitled - Weft Studio");
    CHECK_FALSE (ui.studio.dirty());
    // Undo is an edit too.
    CHECK (ui.key ('Z', ctrl));
    CHECK (ui.main->getName() == "Untitled* - Weft Studio");
}

TEST_CASE ("AT-PRJ-10 Open Recent lists the 10 most recent, newest first; a deleted one is unavailable and removed when clicked", "[UI][project][AT-PRJ-10]")
{
    Ui ui;
    for (const auto& item : std::vector<fs::path> (ui.studio.recentProjects().items()))
        ui.studio.recentProjects().remove (item);
    CHECK (recentLabels (*ui.main).isEmpty());

    std::vector<fs::path> roots;
    for (int i = 1; i <= 11; ++i)
    {
        roots.push_back (makeProject (ui.folder.path / "library", "Project " + std::to_string (i)));
        ui.main->openProject (roots.back());
        ui.settle();
        REQUIRE (ui.studio.projectRoot() == roots.back());
    }
    auto labels = recentLabels (*ui.main);
    REQUIRE (labels.size() == 10);
    CHECK (labels[0] == "Project 11");
    CHECK (labels[9] == "Project 2");

    fs::remove_all (roots[4]); // "Project 5": 7th in the list
    labels = recentLabels (*ui.main);
    CHECK (labels[6] == "Project 5 (not found)");
    ui.main->menuItemSelected (MainComponent::recentMenuBase + 7, 0);
    ui.settle();
    CHECK (ui.studio.projectRoot() == roots.back()); // nothing opened
    labels = recentLabels (*ui.main);
    CHECK (labels.size() == 9);
    CHECK_FALSE (labels.contains ("Project 5 (not found)"));

    // Clicking an available one opens it.
    ui.main->menuItemSelected (MainComponent::recentMenuBase + 2, 0);
    ui.settle();
    CHECK (ui.studio.projectRoot() == roots[9]);
    CHECK (recentLabels (*ui.main)[0] == "Project 10");

    // The Welcome screen lists the same projects.
    WelcomeDialog welcome (ui.studio, [] (Cmd) {}, [] (const fs::path&) {});
    REQUIRE (welcome.recent.size() == 9);
    CHECK (welcome.recent[0]->getButtonText() == "Project 10");
}

TEST_CASE ("AT-PRJ-13 Project Settings changes name and record bit depth; sample rate locks once there's media", "[UI][project][AT-PRJ-13]")
{
    Ui ui;
    {
        ProjectSettingsDialog dialog (ui.studio);
        CHECK (dialog.name.getText() == "Untitled");
        CHECK (dialog.bitDepth.getSelectedId() == 24);
        CHECK (dialog.sampleRate.isEnabled());
        dialog.name.setText ("Demo", false);
        dialog.bitDepth.setSelectedId (16);
        dialog.sampleRate.setSelectedId (44100);
        dialog.countIn.setSelectedId (3); // 2 bars
        REQUIRE (dialog.apply());
    }
    ui.settle();
    CHECK (ui.m().info.name == "Demo");
    CHECK (ui.m().info.recordBitDepth == 16);
    CHECK (ui.m().info.sampleRate == 44100);
    CHECK (ui.m().info.metronomeDefaults.countInBars == 2);
    CHECK (ui.studio.windowTitle().startsWith ("Demo*"));

    // The next recording is 16-bit.
    REQUIRE (ui.studio.session().setSampleRate (48000));
    ui.studio.edited();
    auto& device = static_cast<engine::NullDevice&> (ui.studio.device());
    device.setInputGenerator ([] (int, std::int64_t frame, float* dest, int n) {
        for (int i = 0; i < n; ++i)
            dest[i] = 0.25f * static_cast<float> (std::sin (static_cast<double> (frame + i) * 0.05));
    });
    auto settings = ui.studio.engine().settings();
    settings.countInBars = 0;
    settings.metronomeOn = false;
    ui.studio.engine().setSettings (settings);
    ui.studio.engine().setRecordWriterThread (false);
    ui.studio.toggleArm (ui.m().trackOrder.front());
    REQUIRE (ui.studio.perform (Cmd::record));
    ui.renderAudio (48000);
    REQUIRE (ui.studio.perform (Cmd::playStop));
    ui.renderAudio (2048);
    for (int i = 0; i < 50 && ui.m().media.empty(); ++i)
    {
        ui.studio.tick();
        ui.renderAudio (512);
    }
    REQUIRE (ui.m().media.size() == 1);
    const auto& recorded = *ui.m().media.begin()->second;
    CHECK (recorded.bitDepth == 16);
    media::WavReader reader;
    REQUIRE (reader.open (ui.studio.mediaDir() / media::mediaFileName (recorded.hash)));
    CHECK (reader.info().bitsPerSample == 16);
    CHECK_FALSE (reader.info().isFloat);

    ProjectSettingsDialog locked (ui.studio);
    CHECK_FALSE (locked.sampleRate.isEnabled());
    CHECK (locked.rateNote.getText() == "Fixed once there's audio.");
}

TEST_CASE ("New and Open ask to save unsaved changes: Cancel stays, Don't Save drops them, Save keeps them", "[UI][project]")
{
    Ui ui;
    const auto other = makeProject (ui.folder.path / "library", "Other", 2);
    ui.instrumentTracks (1);
    const auto first = ui.studio.projectRoot();

    ui.main->openProject (other);
    REQUIRE (ui.main->projectQuestionPending());
    CHECK (ui.main->projectQuestion() == "Save changes to \"Untitled\"?");
    ui.main->answerProject (0); // Cancel
    CHECK (ui.studio.projectRoot() == first);
    CHECK (ui.studio.dirty());

    ui.main->openProject (other);
    ui.main->answerProject (1); // Save
    ui.settle();
    CHECK (ui.studio.projectRoot() == other);
    CHECK (ui.m().trackOrder.size() == 3);
    CHECK (fs::exists (first)); // kept: it was saved with the new track
    {
        project::ProjectFolder saved (first);
        auto opened = saved.open();
        REQUIRE (opened.document.has_value());
        CHECK_FALSE (opened.recoveryAvailable());
    }

    // Don't Save: the changes are dropped and won't be offered as recovered later.
    ui.instrumentTracks (1);
    ui.main->openProject (first);
    ui.main->answerProject (2);
    ui.settle();
    CHECK (ui.studio.projectRoot() == first);
    CHECK_FALSE (ui.main->projectQuestionPending());
    ui.main->openProject (other);
    ui.settle();
    CHECK_FALSE (ui.main->projectQuestionPending()); // no recovery question
    CHECK (ui.m().trackOrder.size() == 3);

    // An untouched project doesn't ask.
    ui.main->run (Cmd::newProject);
    CHECK_FALSE (ui.main->projectQuestionPending());
}

TEST_CASE ("Opening a project with unsaved changes from a crash asks to Recover or Discard", "[UI][project]")
{
    Ui ui;
    const auto root = ui.folder.path / "library" / "Crashed.weft";
    {
        project::ProjectFolder folder (root);
        REQUIRE (folder.create());
        auto s = commands::Session::createNew ({}, "local").value();
        REQUIRE (folder.save (s.document()));
        s.onLocalChange = [&] (const doc::Bytes& change, std::int64_t time) { folder.log().append (change, time); };
        REQUIRE (s.createTrack (model::TrackKind::audio, "Unsaved"));
    }

    ui.main->openProject (root);
    REQUIRE (ui.main->projectQuestionPending());
    CHECK (ui.main->projectQuestion().startsWith ("Recover unsaved changes from "));
    ui.main->answerProject (1); // Recover
    ui.settle();
    CHECK (ui.studio.projectRoot() == root);
    CHECK (ui.m().trackOrder.size() == 2);

    // Crash again, then Discard: the saved state, and the log archived.
    {
        project::ProjectFolder folder (root);
        auto opened = folder.open();
        commands::Session s (std::move (*opened.document), "local");
        s.onLocalChange = [&] (const doc::Bytes& change, std::int64_t time) { folder.log().append (change, time); };
        REQUIRE (s.createTrack (model::TrackKind::audio, "Lost"));
    }
    ui.main->openProject (root);
    REQUIRE (ui.main->projectQuestionPending());
    ui.main->answerProject (2); // Discard
    ui.settle();
    CHECK (ui.m().trackOrder.size() == 2);
    CHECK (fs::exists (root / "Recovery" / "archive"));
    CHECK_FALSE (project::RecoveryLog (root / "Recovery" / "changes.log").hasEntries());
}

TEST_CASE ("Opening a project lists missing plug-ins and offers Locate for missing media", "[UI][project]")
{
    Ui ui;
    const auto root = ui.folder.path / "library" / "Gaps.weft";
    std::string hash;
    {
        project::ProjectFolder folder (root);
        REQUIRE (folder.create());
        auto s = commands::Session::createNew ({}, "local").value();
        const auto track = s.model().trackOrder.front();
        model::PluginInstance p;
        p.format = "vst3";
        p.pluginUid = "0123456789ABCDEF0123456789ABCDEF";
        p.name = "Not Installed Verb";
        p.state = doc::Value (doc::Bytes { 1, 2, 3 });
        REQUIRE (s.addInsert (track, p));
        const auto sine = writeSine (ui.folder.path / "browse" / "take.wav");
        const auto bytes = project::readFile (sine).value();
        hash = media::hashBytes (bytes);
        model::MediaRef ref;
        ref.hash = hash;
        ref.originalName = "take.wav";
        ref.lengthSamples = 48000;
        ref.channels = 2;
        ref.sizeBytes = static_cast<std::int64_t> (bytes.size());
        REQUIRE (s.addMedia (ref));
        REQUIRE (s.createAudioRegion (track, 0, hash, 0, 48000));
        REQUIRE (folder.save (s.document()));
    }

    ui.main->openProject (root);
    ui.settle();
    REQUIRE (ui.studio.projectRoot() == root);
    CHECK (ui.main->lastAlert().contains ("Not Installed Verb"));
    CHECK (ui.studio.missingPlugins() == std::vector<std::string> { "Not Installed Verb" });
    CHECK (ui.studio.missingMedia() == std::vector<std::string> { hash });
    CHECK (engine::regionsMissingMedia (ui.m(), ui.studio.media()) == 1);

    MissingMediaDialog dialog (ui.studio);
    CHECK (dialog.message.getText() == "1 audio file is missing. Their regions show as missing until you locate them.");
    CHECK (dialog.locate.isEnabled());
    dialog.locateIn (ui.folder.path / "browse");
    CHECK (dialog.message.getText() == "All audio files were found.");
    CHECK (dialog.proceed.getButtonText() == "Close");
    CHECK (ui.studio.missingMedia().empty());
    CHECK (engine::regionsMissingMedia (ui.m(), ui.studio.media()) == 0);
    CHECK (fs::exists (root / "Media" / media::mediaFileName (hash)));
}

TEST_CASE ("Save As moves the project to a new folder under the new name", "[UI][project]")
{
    Ui ui;
    const auto file = writeSine (ui.folder.path / "browse" / "sine.wav");
    REQUIRE (ui.studio.importFiles ({ file }, { 0, {} }).regions.size() == 1);
    ui.settle();
    const auto first = ui.studio.projectRoot();

    SaveAsDialog dialog (ui.studio);
    CHECK (dialog.name.getText() == "Untitled copy");
    CHECK (dialog.copyMedia.getToggleState());
    dialog.name.setText ("Final Mix", false);
    dialog.folder.setText (pathText (ui.folder.path / "elsewhere"), false);
    REQUIRE (dialog.saveProject());
    ui.settle();
    const auto target = ui.folder.path / "elsewhere" / "Final Mix.weft";
    CHECK (ui.studio.projectRoot() == target);
    CHECK (ui.m().info.name == "Final Mix");
    CHECK (ui.main->getName() == "Final Mix - Weft Studio");
    CHECK (ui.studio.recentProjects().items().front() == target);
    CHECK (ui.studio.missingMedia().empty());
    CHECK (fs::exists (first)); // the original stays

    // Its name on disk now matches: saving again to the same place fails.
    SaveAsDialog clash (ui.studio);
    clash.name.setText ("Final Mix", false);
    clash.folder.setText (pathText (ui.folder.path / "elsewhere"), false);
    CHECK_FALSE (clash.saveProject());
    CHECK (clash.error.getText() == "Final Mix.weft already exists.");
}

TEST_CASE ("AT-UI-8 first launch shows Audio Setup; Test plays 1 kHz at -18 dBFS for 1 s; Continue saves; no device means No-audio mode", "[UI][audio][AT-UI-8]")
{
    forgetAudioSettings();
    CHECK_FALSE (loadAudioSettings().has_value()); // first launch: the app shows Audio Setup

    Ui ui;
    TestDevices devices;
    const auto defaults = defaultAudioSettings (devices);
    CHECK (defaults.driver == "Null");
    CHECK (defaults.output == "Null Out");
    CHECK (defaults.sampleRate == 48000.0);
    CHECK (defaults.bufferSize == 480); // 10 ms

    AudioSetupDialog dialog (ui.studio, devices, true);
    CHECK (dialog.proceed.getButtonText() == "Continue");
    CHECK_FALSE (dialog.cancel.isVisible());
    CHECK (dialog.sampleRate.getSelectedId() == 48000);
    CHECK (dialog.bufferSize.getSelectedId() == 480);
    CHECK (dialog.input.getText() == "Null In");
    REQUIRE (dialog.midiInputs.size() == 1);
    CHECK (dialog.midiInputs[0]->getToggleState());

    dialog.playTest();
    REQUIRE (devices.last != nullptr);
    REQUIRE (&ui.studio.device() == devices.last);
    CHECK (ui.studio.audioRunning());
    CHECK (dialog.latency.getText().startsWith ("Round-trip latency: "));
    devices.last->setCaptureCapacity (2 * 48000);
    devices.last->render (60000, 480);
    const auto out = devices.last->capturedOutput (0);
    const auto right = devices.last->capturedOutput (1);
    REQUIRE (out.size() >= 60000);
    float peak = 0;
    std::int64_t first = -1, last = -1, upward = 0;
    bool sameOnBoth = true;
    for (std::size_t i = 0; i < 60000; ++i)
    {
        peak = std::max (peak, std::abs (out[i]));
        if (out[i] != 0.0f)
        {
            if (first < 0)
                first = static_cast<std::int64_t> (i);
            last = static_cast<std::int64_t> (i);
        }
        if (i > 0 && out[i - 1] < 0.0f && out[i] >= 0.0f)
            ++upward;
        sameOnBoth = sameOnBoth && right[i] == out[i];
    }
    CHECK (sameOnBoth); // outputs 1-2
    CHECK (20.0 * std::log10 (peak) == Catch::Approx (-18.0).margin (0.05));
    REQUIRE (first >= 0);
    CHECK ((last - first + 1) == Catch::Approx (48000).margin (2)); // one second
    CHECK (upward == Catch::Approx (1000).margin (2));               // 1 kHz

    // Continue saves the settings (with the MIDI input switched off) and applies them.
    dialog.midiInputs[0]->setToggleState (false, juce::dontSendNotification);
    bool saved = false;
    dialog.onSaved = [&] { saved = true; };
    REQUIRE (dialog.apply());
    CHECK (saved);
    const auto stored = loadAudioSettings();
    REQUIRE (stored.has_value()); // second launch skips the dialog
    CHECK (stored->driver == "Null");
    CHECK (stored->bufferSize == 480);
    CHECK (stored->disabledMidiInputs == std::set<std::string> { "USB Keys" });
    CHECK_FALSE (ui.studio.midiInputs().enabled ("USB Keys"));

    // Preferences reopen with the saved choices.
    AudioSetupDialog prefs (ui.studio, devices, false);
    CHECK (prefs.proceed.getButtonText() == "OK");
    CHECK (prefs.cancel.isVisible());
    CHECK_FALSE (prefs.midiInputs[0]->getToggleState());

    // No openable device: the window opens anyway in No-audio mode, with a banner.
    CHECK_FALSE (ui.main->noAudioBannerShown());
    devices.dead = true;
    CHECK_FALSE (ui.studio.setAudioDevice (devices.make ({}), {}));
    ui.settle();
    CHECK_FALSE (ui.studio.audioRunning());
    CHECK (ui.main->noAudioBannerShown());
    CHECK_FALSE (ui.studio.perform (Cmd::record)); // recording is off
    CHECK (ui.studio.status.startsWith ("Recording needs an audio device"));
    CHECK (ui.studio.perform (Cmd::playStop)); // the transport plays silently against the system clock
    juce::Thread::sleep (60);
    ui.studio.tick();
    CHECK (ui.studio.playing());
    CHECK (ui.studio.playhead() > 0);
    CHECK (ui.studio.perform (Cmd::playStop));
    forgetAudioSettings();
}

TEST_CASE ("Starting with no audio device shows the No-audio banner", "[UI][audio][AT-UI-8]")
{
    TempFolder folder;
    Studio studio (std::make_unique<DeadDevice>(), folder.path / "projects");
    MainComponent main (studio, juce::File (pathText (folder.path)));
    main.setSize (1200, 800);
    CHECK_FALSE (studio.audioRunning());
    CHECK (main.noAudioBannerShown());
    CHECK (studio.status == "No audio device: Weft is running without sound.");
}

// Run with: weft_ui_tests "[.screenshot-m9]" (writes the PNGs into WEFT_SCREENSHOT_DIR or the working folder).
TEST_CASE ("Screenshots of the M9 dialogs", "[.screenshot-m9]")
{
    Ui ui;
    TestDevices devices;
    const auto* env = std::getenv ("WEFT_SCREENSHOT_DIR");
    const auto dir = env != nullptr ? juce::File (juce::String (env)) : juce::File::getCurrentWorkingDirectory();
    auto shoot = [&] (juce::Component& c, const char* name) {
        c.setVisible (true);
        const auto image = c.createComponentSnapshot (c.getLocalBounds());
        auto out = dir.getChildFile (name);
        out.deleteFile();
        juce::FileOutputStream stream (out);
        REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    };
    makeProject (ui.folder.path / "library", "Demo Song");
    ui.studio.recentProjects().add (ui.folder.path / "library" / "Demo Song.weft");
    ui.studio.recentProjects().add (ui.folder.path / "library" / "Moved Away.weft");
    AudioSetupDialog setup (ui.studio, devices, true);
    shoot (setup, "audio-setup.png");
    ProjectSettingsDialog settings (ui.studio);
    shoot (settings, "project-settings.png");
    WelcomeDialog welcome (ui.studio, [] (Cmd) {}, [] (const fs::path&) {});
    shoot (welcome, "welcome.png");
    forgetAudioSettings();
}

TEST_CASE ("Share in a Synced Folder moves the project there; it saves itself and merges other computers' changes", "[UI][project][shared-folder]")
{
    Ui ui;
    ui.studio.sharedFolderPollMs = 0;
    ui.studio.sharedFolderSaveMs = 0;
    auto menuItem = [&] {
        const auto menu = ui.main->getMenuForIndex (6, "Share");
        for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
            if (it.getItem().text == "Share in a Synced Folder...")
                return std::optional<bool> (it.getItem().isEnabled);
        return std::optional<bool>();
    };
    REQUIRE (menuItem() == std::optional<bool> (true));
    CHECK (ui.studio.syncStatus() == "Local only");

    const auto drive = ui.folder.path / "My Drive";
    fs::create_directories (drive);
    REQUIRE (ui.studio.session().renameProject ("Band"));
    ui.studio.edited();
    std::string error;
    REQUIRE (ui.studio.shareInFolder (drive, &error));
    ui.settle();
    const auto root = drive / "Band.weft";
    CHECK (ui.studio.projectRoot() == root);
    CHECK (ui.studio.inSharedFolder());
    CHECK (ui.studio.syncStatus() == "Shared folder");
    CHECK_FALSE (ui.studio.dirty());
    CHECK (menuItem() == std::optional<bool> (false));
    CHECK_FALSE (ui.studio.shareInFolder (drive, &error));
    const auto own = root / "Peers" / (ui.studio.deviceId() + ".automerge");
    REQUIRE (fs::exists (own));

    // An edit reaches this computer's file on the next check, with no Ctrl+S.
    const auto track = ui.m().trackOrder.front();
    REQUIRE (ui.studio.session().renameTrack (track, "Vox"));
    ui.studio.edited();
    CHECK (ui.studio.dirty());
    ui.studio.tick();
    CHECK_FALSE (ui.studio.dirty());
    {
        auto saved = doc::Document::load (project::readFile (own).value());
        REQUIRE (saved);
        commands::Session reader (std::move (*saved), "reader");
        CHECK (reader.model().track (track)->name == "Vox");
    }

    // Another computer adds a track; this one picks it up on its next check.
    project::ProjectFolder other (root, "ffffffffffffffff");
    auto opened = other.open();
    REQUIRE (opened.document);
    commands::Session theirs (std::move (*opened.document), "bandmate");
    REQUIRE (theirs.createTrack (model::TrackKind::audio, "Drums"));
    REQUIRE (other.save (theirs.document()));
    ui.studio.tick();
    ui.settle();
    bool drums = false;
    for (const auto& id : ui.m().trackOrder)
        drums = drums || ui.m().track (id)->name == "Drums";
    CHECK (drums);
    CHECK (ui.m().track (track)->name == "Vox");
}
