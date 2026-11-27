#include "BounceDialog.h"

#include "weft/time/Position.h"

namespace weft::ui
{
namespace
{
constexpr int labelX = 20, controlX = 130, rowHeight = 34;
const juce::StringArray rowLabels { "Range", "", "", "", "", "Tags", "", "Dither", "Channels", "Folder", "File name" };
constexpr int rowWav = 2, rowAiff = 3, rowMp3 = 4, rowTags = 5, rowDither = 7, rowChannels = 8, rowFolder = 9, rowName = 10, rowWarning = 11;

/** A track name as a file name: no characters a file system refuses. */
std::string fileNameFor (std::string name)
{
    for (auto& ch : name)
        if (std::string_view ("\\/:*?\"<>|").find (ch) != std::string_view::npos)
            ch = '_';
    return name.empty() ? std::string ("Track") : name;
}

juce::String utf8 (const std::filesystem::path& p)
{
    const auto s = p.u8string();
    return juce::String::fromUTF8 (reinterpret_cast<const char*> (s.c_str()), static_cast<int> (s.size()));
}

std::filesystem::path fromUtf8 (const juce::String& s)
{
    const auto bytes = s.toStdString();
    return std::filesystem::path (std::u8string (bytes.begin(), bytes.end()));
}
} // namespace

BounceDialog::BounceDialog (Studio& s, std::vector<Id> ts) : tracks (std::move (ts)), studio (s)
{
    setTitle (tracks.empty() ? "Bounce" : tracks.size() == 1 ? "Export Track" : "Export All Tracks");
    const auto defaults = studio.defaultBounceSettings();
    const auto mp3Defaults = studio.defaultMp3Format();
    const auto& sigs = studio.model().signatures;

    start.setText (formatBarsBeats (defaults.range.from, sigs), false);
    end.setText (formatBarsBeats (defaults.range.to, sigs), false);
    describe (start, "Range start", "Where the bounce starts (bar.beat.division.tick)");
    describe (end, "Range end", "Where the bounce ends (bar.beat.division.tick)");
    tail.setToggleState (defaults.includeTail, juce::dontSendNotification);
    describe (tail, "Include tail", "Keep rendering past the end until effects fall silent (up to 10 s)");
    mode.addItemList ({ "Automatic", "Offline", "Real-time" }, 1);
    mode.setSelectedItemIndex (0, juce::dontSendNotification);
    describe (mode, "Mode",
              "Automatic renders offline unless a plug-in can only run in real time; Offline always renders as fast as possible; "
              "Real-time plays the range while capturing it");
    secondPass.setToggleState (false, juce::dontSendNotification);
    describe (secondPass, "2nd cycle pass", "Render the range twice round and keep the second, so effect tails at the end carry into the start");
    if (! tracks.empty())
    {
        mode.setSelectedItemIndex (1, juce::dontSendNotification);
        mode.setEnabled (false); // track exports render offline
    }

    wav.setToggleState (true, juce::dontSendNotification);
    describe (wav, "WAV", "Write a WAV file");
    bitDepth.addItemList ({ "16-bit", "24-bit", "32-bit float" }, 1);
    bitDepth.setSelectedItemIndex (1, juce::dontSendNotification);
    describe (bitDepth, "Bit depth");
    const auto rate = juce::String (studio.model().info.sampleRate / 1000.0, 1) + " kHz";
    wavRate.addItemList ({ "Project (" + rate + ")", "44.1 kHz", "48 kHz", "96 kHz" }, 1);
    wavRate.setSelectedItemIndex (0, juce::dontSendNotification);
    describe (wavRate, "WAV sample rate");
    describe (aiff, "AIFF", "Also write an AIFF file, at the WAV bit depth and rate (24-bit at most)");
    describe (flac, "FLAC", "Also write a FLAC file, at the WAV bit depth and rate (24-bit at most)");

    mp3.setToggleState (false, juce::dontSendNotification);
    describe (mp3, "MP3", "Write an MP3 file");
    bitrate.addItemList ({ "320 kbps", "256 kbps", "192 kbps", "128 kbps" }, 1);
    for (int q = 0; q <= 9; ++q)
        bitrate.addItem ("VBR quality " + juce::String (q), 10 + q);
    bitrate.setSelectedItemIndex (0, juce::dontSendNotification);
    describe (bitrate, "MP3 bitrate", "Constant bitrate, or variable bitrate by quality (0 best, 9 smallest)");
    mp3Rate.addItemList ({ "Auto (44.1 or 48 kHz)", "44.1 kHz", "48 kHz" }, 1);
    mp3Rate.setSelectedItemIndex (0, juce::dontSendNotification);
    describe (mp3Rate, "MP3 sample rate");
    title.setText (juce::String::fromUTF8 (mp3Defaults.title.c_str()), false);
    artist.setText (juce::String::fromUTF8 (mp3Defaults.artist.c_str()), false);
    describe (title, "Title");
    describe (artist, "Artist");
    describe (album, "Album");
    describe (year, "Year");
    for (auto* e : { &title, &artist, &album, &year })
        e->setTextToShowWhenEmpty (e->getTitle(), colour (Token::textDim));
    tempoInfo.setToggleState (true, juce::dontSendNotification);
    describe (tempoInfo, "Include tempo", "Write the tempo at the range start into the MP3 tag");

    dither.addItemList ({ "Auto", "Off", "On" }, 1);
    dither.setSelectedItemIndex (0, juce::dontSendNotification);
    describe (dither, "Dither", "Auto adds TPDF dither below 24 bits");
    normalize.addItemList ({ "Off", "Overload Protection Only", "On" }, 1);
    normalize.setSelectedItemIndex (0, juce::dontSendNotification);
    describe (normalize, "Normalize", "Scale the bounce so its loudest peak is 0 dBFS: always, or only when it would clip");
    channels.addItemList ({ "Interleaved", "Split (L and R files)", "Mono" }, 1);
    channels.setSelectedItemIndex (0, juce::dontSendNotification);
    describe (channels, "Channels", "One stereo file; a mono file per side named .L and .R; or one mono file");

    folder.setText (utf8 (defaults.folder), false);
    name.setText (juce::String::fromUTF8 (defaults.name.c_str()), false);
    if (! tracks.empty())
    {
        // Each file is named after its track.
        const auto* t = studio.model().track (tracks.front());
        name.setText (tracks.size() == 1 && t != nullptr ? juce::String::fromUTF8 (fileNameFor (t->name).c_str()) : juce::String ("Named after each track"), false);
        name.setEnabled (tracks.size() == 1);
    }
    describe (folder, "Folder", "Where the files go");
    describe (name, "File name", "Without the extension");
    describe (choose, "Choose...", "Pick the folder");
    choose.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser> ("Bounce to", juce::File (folder.getText()));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe = juce::Component::SafePointer<BounceDialog> (this)] (const juce::FileChooser& fc) {
                                  if (safe != nullptr && fc.getResult() != juce::File())
                                      safe->folder.setText (fc.getResult().getFullPathName(), false);
                              });
    };
    describe (makeFolder, "New Folder...", "Make a folder inside this one and bounce there");
    makeFolder.onClick = [this] { newFolder(); };
    askFolderName = [safe = juce::Component::SafePointer<BounceDialog> (this)] (std::function<void (juce::String)> done) {
        if (safe == nullptr || ! desktopWindows())
            return;
        auto* w = new juce::AlertWindow ("New Folder", "Name the new folder.", juce::MessageBoxIconType::NoIcon, safe.getComponent());
        w->addTextEditor ("name", "Bounces");
        w->addButton ("Create", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([w, done] (int result) {
                                if (result == 1)
                                    done (w->getTextEditorContents ("name"));
                            }),
                            true);
    };

    // D-W9-1: media still downloading. Bounce becomes "Bounce Anyway"; Cancel waits.
    if (const auto missing = engine::regionsMissingMedia (studio.model(), studio.media()); missing > 0)
    {
        warning.setText (juce::String (missing) + (missing == 1 ? " region has" : " regions have") + " media not yet downloaded.",
                         juce::dontSendNotification);
        bounceButton.setButtonText ("Bounce Anyway");
    }
    warning.setColour (juce::Label::textColourId, colour (Token::warning));

    describe (bounceButton, "Bounce", "Render and write the files (Enter)");
    describe (cancel, "Cancel", "Close without bouncing (Esc)");
    bounceButton.addShortcut (juce::KeyPress (juce::KeyPress::returnKey));
    cancel.addShortcut (juce::KeyPress (juce::KeyPress::escapeKey));
    bounceButton.onClick = [this] { bounce(); };
    cancel.onClick = [this] {
        if (current == Stage::rendering)
            studio.cancelBounce();
        else
            closeDialog();
    };
    describe (showInFolder, "Show in Folder");
    describe (addToProject, "Add to Project", "Import the bounce as a new muted audio track");
    describe (close, "Close");
    showInFolder.onClick = [this] {
        if (studio.lastBounce && ! studio.lastBounce->files.empty())
            juce::File (utf8 (studio.lastBounce->files.front())).revealToUser();
    };
    addToProject.onClick = [this] {
        if (studio.lastBounce && ! studio.lastBounce->files.empty() && studio.addBounceToProject (studio.lastBounce->files.front()))
            addToProject.setEnabled (false);
    };
    close.onClick = [this] { closeDialog(); };

    for (auto* t : { &wav, &aiff, &flac, &mp3, &secondPass })
        t->onClick = [this] { updateEnabled(); };
    bitDepth.onChange = [this] { updateEnabled(); };
    mode.onChange = [this] { updateEnabled(); };

    for (auto* c : settingsControls())
        addChildComponent (c);
    for (auto* c : std::initializer_list<juce::Component*> { &cancel, &notice, &bar, &showInFolder, &addToProject, &close })
        addChildComponent (c);
    notice.setJustificationType (juce::Justification::centred);
    setStage (Stage::settings);
    updateEnabled();
    setSize (600, 16 + (rowWarning + 1) * rowHeight + 56);
}

BounceDialog::~BounceDialog()
{
    studio.onBounceDone = nullptr;
    if (current == Stage::rendering)
        studio.cancelBounce();
}

engine::BounceSettings BounceDialog::settings() const
{
    auto s = studio.defaultBounceSettings();
    const auto& sigs = studio.model().signatures;
    if (const auto from = parseBarsBeats (start.getText().toStdString(), sigs))
        s.range.from = *from;
    if (const auto to = parseBarsBeats (end.getText().toStdString(), sigs))
        s.range.to = *to;
    s.includeTail = tail.getToggleState();
    s.realtime = mode.getSelectedItemIndex() == 2;
    s.forceOffline = mode.getSelectedItemIndex() == 1;
    s.secondCyclePass = secondPass.getToggleState() && ! s.realtime;
    static constexpr int depths[] = { 16, 24, 32 };
    static constexpr double rates[] = { 0, 44100, 48000, 96000 };
    const engine::WavFormat pcm { depths[std::clamp (bitDepth.getSelectedItemIndex(), 0, 2)], rates[std::clamp (wavRate.getSelectedItemIndex(), 0, 3)] };
    s.wav = wav.getToggleState() ? std::optional (pcm) : std::nullopt;
    s.aiff = aiff.getToggleState() ? std::optional (pcm) : std::nullopt;
    s.flac = flac.getToggleState() ? std::optional (pcm) : std::nullopt;
    if (mp3.getToggleState())
    {
        engine::Mp3Format f;
        const auto id = bitrate.getSelectedId();
        static constexpr int kbps[] = { 320, 256, 192, 128 };
        f.vbr = id >= 10;
        f.vbrQuality = f.vbr ? id - 10 : 2;
        f.bitrateKbps = f.vbr ? 320 : kbps[std::clamp (id - 1, 0, 3)];
        static constexpr double mp3Rates[] = { 0, 44100, 48000 };
        f.sampleRate = mp3Rates[std::clamp (mp3Rate.getSelectedItemIndex(), 0, 2)];
        f.title = title.getText().toStdString();
        f.artist = artist.getText().toStdString();
        f.album = album.getText().toStdString();
        f.year = year.getText().toStdString();
        s.mp3 = f;
    }
    static constexpr engine::DitherMode modes[] = { engine::DitherMode::automatic, engine::DitherMode::off, engine::DitherMode::on };
    s.dither = modes[std::clamp (dither.getSelectedItemIndex(), 0, 2)];
    static constexpr engine::Normalize levels[] = { engine::Normalize::off, engine::Normalize::overloadOnly, engine::Normalize::on };
    s.normalize = levels[std::clamp (normalize.getSelectedItemIndex(), 0, 2)];
    static constexpr engine::FileChannels layouts[] = { engine::FileChannels::interleaved, engine::FileChannels::split, engine::FileChannels::mono };
    s.channels = layouts[std::clamp (channels.getSelectedItemIndex(), 0, 2)];
    s.includeTempo = tempoInfo.getToggleState();
    s.folder = fromUtf8 (folder.getText());
    s.name = name.getText().trim().toStdString();
    if (s.name.empty())
        s.name = "Bounce";
    return s;
}

std::vector<engine::BounceSettings> BounceDialog::allSettings() const
{
    const auto base = settings();
    if (tracks.empty())
        return { base };
    std::vector<engine::BounceSettings> out;
    for (const auto& id : tracks)
    {
        const auto* t = studio.model().track (id);
        if (t == nullptr)
            continue;
        auto s = base;
        s.soloTracks = { id };
        if (tracks.size() > 1)
            s.name = fileNameFor (t->name);
        out.push_back (std::move (s));
    }
    return out;
}

void BounceDialog::newFolder()
{
    if (! askFolderName)
        return;
    askFolderName ([safe = juce::Component::SafePointer<BounceDialog> (this)] (juce::String folderName) {
        folderName = folderName.trim();
        if (safe == nullptr || folderName.isEmpty())
            return;
        const auto made = fromUtf8 (safe->folder.getText()) / fromUtf8 (juce::String (fileNameFor (folderName.toStdString())));
        std::error_code ec;
        std::filesystem::create_directories (made, ec);
        if (ec)
        {
            safe->warning.setText ("Couldn't make the folder: " + juce::String (ec.message()), juce::dontSendNotification);
            return;
        }
        safe->folder.setText (utf8 (made), false);
    });
}

void BounceDialog::bounce()
{
    juce::StringArray existing;
    for (const auto& s : allSettings())
        for (const auto& file : engine::bounceTargets (s))
            if (std::filesystem::exists (file))
                existing.add (utf8 (file.filename()));
    if (existing.isEmpty())
    {
        begin (false);
        return;
    }
    asked = existing.joinIntoString (" and ") + (existing.size() == 1 ? " already exists." : " already exist.");
    if (! desktopWindows())
        return; // headless: answer() stands in for the buttons
    questionWindow = std::make_unique<juce::AlertWindow> ("Bounce", asked, juce::MessageBoxIconType::QuestionIcon, this);
    questionWindow->addButton ("Replace", replaceAnswer);
    questionWindow->addButton ("Keep Both", keepBothAnswer, juce::KeyPress (juce::KeyPress::returnKey));
    questionWindow->addButton ("Cancel", cancelAnswer, juce::KeyPress (juce::KeyPress::escapeKey));
    questionWindow->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<BounceDialog> (this)] (int result) {
                                         if (safe != nullptr)
                                             safe->answer (result);
                                     }),
                                     false);
}

void BounceDialog::answer (int choice)
{
    if (asked.isEmpty())
        return;
    asked.clear();
    if (questionWindow != nullptr)
    {
        questionWindow->exitModalState (0);
        questionWindow.reset();
    }
    if (choice == cancelAnswer)
        return;
    begin (choice == replaceAnswer);
}

void BounceDialog::begin (bool replace)
{
    auto all = allSettings();
    for (auto& s : all)
        s.replace = replace;
    studio.onBounceDone = [safe = juce::Component::SafePointer<BounceDialog> (this)] {
        if (safe != nullptr)
            safe->finished();
    };
    if (! studio.startBounces (std::move (all)))
    {
        studio.onBounceDone = nullptr;
        notice.setText (studio.status, juce::dontSendNotification);
        return;
    }
    progress = 0;
    notice.setText (studio.bounceNotice().isEmpty() ? juce::String ("Bouncing...") : studio.bounceNotice(), juce::dontSendNotification);
    notice.setColour (juce::Label::textColourId, colour (Token::text));
    setStage (Stage::rendering);
    startTimerHz (15);
}

void BounceDialog::timerCallback()
{
    if (studio.bouncing())
        progress = studio.bounceProgress();
}

void BounceDialog::finished()
{
    stopTimer();
    studio.onBounceDone = nullptr;
    const auto& r = studio.lastBounce;
    if (! r || ! r->ok)
    {
        // Cancelled or failed: back to the settings, with the reason.
        setStage (Stage::settings);
        warning.setText (r ? juce::String (r->cancelled ? "Bounce cancelled." : r->error) : juce::String(), juce::dontSendNotification);
        return;
    }
    progress = 1.0;
    notice.setText (juce::String (r->message), juce::dontSendNotification);
    // A clipped bounce reads as a warning, not as plain success (B6).
    notice.setColour (juce::Label::textColourId, colour (r->clipped ? Token::warning : Token::text));
    addToProject.setEnabled (true);
    setStage (Stage::done);
}

void BounceDialog::setStage (Stage s)
{
    current = s;
    const bool settingsShown = s == Stage::settings;
    for (auto* c : settingsControls())
        c->setVisible (settingsShown);
    cancel.setVisible (s != Stage::done);
    notice.setVisible (s != Stage::settings);
    bar.setVisible (s == Stage::rendering);
    for (auto* c : std::initializer_list<juce::Component*> { &showInFolder, &addToProject, &close })
        c->setVisible (s == Stage::done);
    repaint();
}

std::vector<juce::Component*> BounceDialog::settingsControls()
{
    return { &start, &end, &tail, &mode, &secondPass, &wav, &bitDepth, &wavRate, &aiff, &flac, &mp3, &bitrate, &mp3Rate, &title, &artist, &album,
             &year, &tempoInfo, &dither, &normalize, &channels, &folder, &choose, &makeFolder, &name, &warning, &bounceButton };
}

void BounceDialog::updateEnabled()
{
    const bool pcm = wav.getToggleState() || aiff.getToggleState() || flac.getToggleState();
    bitDepth.setEnabled (pcm);
    wavRate.setEnabled (pcm);
    for (auto* c : std::initializer_list<juce::Component*> { &bitrate, &mp3Rate, &title, &artist, &album, &year, &tempoInfo })
        c->setEnabled (mp3.getToggleState());
    // The second pass replaces the tail, and a real-time bounce plays the range once.
    const bool realtime = mode.getSelectedItemIndex() == 2;
    secondPass.setEnabled (! realtime);
    tail.setEnabled (! (secondPass.getToggleState() && ! realtime));
    bounceButton.setEnabled (pcm || mp3.getToggleState());
}

void BounceDialog::closeDialog()
{
    if (onClose)
        onClose();
}

void BounceDialog::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::surfaceRaised));
    if (current != Stage::settings)
        return;
    g.setColour (colour (Token::text));
    g.setFont (font (Type::body));
    for (int i = 0; i < rowLabels.size(); ++i)
        g.drawText (rowLabels[i], labelX, 16 + i * rowHeight, controlX - labelX - 8, 26, juce::Justification::centredLeft);
    g.drawText ("to", 252, 16, 28, 26, juce::Justification::centred);
    g.drawText ("Normalize", controlX + 140, 16 + rowDither * rowHeight, 80, 26, juce::Justification::centredLeft);
}

void BounceDialog::resized()
{
    const int w = getWidth() - controlX - 20;
    auto row = [] (int i) { return 16 + i * rowHeight; };
    start.setBounds (controlX, row (0), 120, 26);
    end.setBounds (282, row (0), 120, 26);
    tail.setBounds (controlX, row (1), 120, 26);
    mode.setBounds (controlX + 130, row (1), 120, 26);
    secondPass.setBounds (controlX + 260, row (1), 140, 26);
    wav.setBounds (labelX, row (rowWav), 100, 26);
    bitDepth.setBounds (controlX, row (rowWav), 130, 26);
    wavRate.setBounds (controlX + 140, row (rowWav), 160, 26);
    aiff.setBounds (labelX, row (rowAiff), 100, 26);
    flac.setBounds (controlX, row (rowAiff), 100, 26);
    mp3.setBounds (labelX, row (rowMp3), 100, 26);
    bitrate.setBounds (controlX, row (rowMp3), 130, 26);
    mp3Rate.setBounds (controlX + 140, row (rowMp3), 160, 26);
    title.setBounds (controlX, row (rowTags), w / 2 - 5, 26);
    artist.setBounds (controlX + w / 2 + 5, row (rowTags), w / 2 - 5, 26);
    album.setBounds (controlX, row (rowTags + 1), w / 2 - 5, 26);
    year.setBounds (controlX + w / 2 + 5, row (rowTags + 1), 70, 26);
    tempoInfo.setBounds (controlX + w / 2 + 80, row (rowTags + 1), w / 2 - 80, 26);
    dither.setBounds (controlX, row (rowDither), 130, 26);
    normalize.setBounds (controlX + 220, row (rowDither), w - 220, 26);
    channels.setBounds (controlX, row (rowChannels), 200, 26);
    folder.setBounds (controlX, row (rowFolder), w - 200, 26);
    choose.setBounds (getWidth() - 210, row (rowFolder), 85, 26);
    makeFolder.setBounds (getWidth() - 115, row (rowFolder), 95, 26);
    name.setBounds (controlX, row (rowName), w, 26);
    warning.setBounds (labelX, row (rowWarning), getWidth() - 2 * labelX, 26);
    bounceButton.setBounds (getWidth() - 200, getHeight() - 46, 85, 28); // primary first, as in every dialog
    cancel.setBounds (getWidth() - 105, getHeight() - 46, 85, 28);

    notice.setBounds (labelX, getHeight() / 2 - 60, getWidth() - 2 * labelX, 40);
    bar.setBounds (labelX, getHeight() / 2 - 10, getWidth() - 2 * labelX, 22);
    showInFolder.setBounds (labelX, getHeight() - 46, 120, 28);
    addToProject.setBounds (labelX + 130, getHeight() - 46, 120, 28);
    close.setBounds (getWidth() - 105, getHeight() - 46, 85, 28);
}

void BounceDialog::show (Studio& studio, juce::Component* parent, std::vector<Id> tracks)
{
    auto* dialog = new BounceDialog (studio, std::move (tracks));
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (dialog);
    options.dialogTitle = dialog->getTitle();
    options.dialogBackgroundColour = colour (Token::surfaceRaised);
    options.componentToCentreAround = parent;
    options.useNativeTitleBar = true;
    options.resizable = false;
    auto* window = options.launchAsync();
    dialog->onClose = [window] {
        if (window != nullptr)
            window->exitModalState (0);
    };
}
} // namespace weft::ui
