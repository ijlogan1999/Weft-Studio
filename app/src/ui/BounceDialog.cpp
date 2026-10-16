#include "BounceDialog.h"

#include "weft/time/Position.h"

namespace weft::ui
{
namespace
{
constexpr int labelX = 20, controlX = 130, rowHeight = 34;
const juce::StringArray rowLabels { "Range", "", "", "", "Tags", "", "Dither", "Folder", "File name" };

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

BounceDialog::BounceDialog (Studio& s) : studio (s)
{
    setTitle ("Bounce");
    const auto defaults = studio.defaultBounceSettings();
    const auto mp3Defaults = studio.defaultMp3Format();
    const auto& sigs = studio.model().signatures;

    start.setText (formatBarsBeats (defaults.range.from, sigs), false);
    end.setText (formatBarsBeats (defaults.range.to, sigs), false);
    describe (start, "Range start", "Where the bounce starts (bar.beat.division.tick)");
    describe (end, "Range end", "Where the bounce ends (bar.beat.division.tick)");
    tail.setToggleState (defaults.includeTail, juce::dontSendNotification);
    describe (tail, "Include tail", "Keep rendering past the end until effects fall silent (up to 10 s)");
    mode.addItemList ({ "Offline", "Real-time" }, 1);
    mode.setSelectedItemIndex (0, juce::dontSendNotification);
    describe (mode, "Mode", "Offline renders as fast as possible; real time plays the range while capturing it");

    wav.setToggleState (true, juce::dontSendNotification);
    describe (wav, "WAV", "Write a WAV file");
    bitDepth.addItemList ({ "16-bit", "24-bit", "32-bit float" }, 1);
    bitDepth.setSelectedItemIndex (1, juce::dontSendNotification);
    describe (bitDepth, "Bit depth");
    const auto rate = juce::String (studio.model().info.sampleRate / 1000.0, 1) + " kHz";
    wavRate.addItemList ({ "Project (" + rate + ")", "44.1 kHz", "48 kHz", "96 kHz" }, 1);
    wavRate.setSelectedItemIndex (0, juce::dontSendNotification);
    describe (wavRate, "WAV sample rate");

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

    dither.addItemList ({ "Auto", "Off", "On" }, 1);
    dither.setSelectedItemIndex (0, juce::dontSendNotification);
    describe (dither, "Dither", "Auto adds TPDF dither below 24 bits");

    folder.setText (utf8 (defaults.folder), false);
    name.setText (juce::String::fromUTF8 (defaults.name.c_str()), false);
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

    for (auto* t : { &wav, &mp3 })
        t->onClick = [this] { updateEnabled(); };
    bitDepth.onChange = [this] { updateEnabled(); };

    for (auto* c : std::initializer_list<juce::Component*> { &start, &end, &tail, &mode, &wav, &bitDepth, &wavRate, &mp3, &bitrate, &mp3Rate, &title,
                                                             &artist, &album, &year, &dither, &folder, &choose, &name, &warning, &bounceButton,
                                                             &cancel, &notice, &bar, &showInFolder, &addToProject, &close })
        addChildComponent (c);
    notice.setJustificationType (juce::Justification::centred);
    setStage (Stage::settings);
    updateEnabled();
    setSize (560, 410);
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
    s.realtime = mode.getSelectedItemIndex() == 1;
    if (wav.getToggleState())
    {
        static constexpr int depths[] = { 16, 24, 32 };
        static constexpr double rates[] = { 0, 44100, 48000, 96000 };
        s.wav = engine::WavFormat { depths[std::clamp (bitDepth.getSelectedItemIndex(), 0, 2)], rates[std::clamp (wavRate.getSelectedItemIndex(), 0, 3)] };
    }
    else
        s.wav.reset();
    if (mp3.getToggleState())
    {
        engine::Mp3Format f;
        const auto id = bitrate.getSelectedId();
        static constexpr int kbps[] = { 320, 256, 192, 128 };
        f.vbr = id >= 10;
        f.vbrQuality = f.vbr ? id - 10 : 2;
        f.bitrateKbps = f.vbr ? 320 : kbps[std::clamp (id - 1, 0, 3)];
        static constexpr double rates[] = { 0, 44100, 48000 };
        f.sampleRate = rates[std::clamp (mp3Rate.getSelectedItemIndex(), 0, 2)];
        f.title = title.getText().toStdString();
        f.artist = artist.getText().toStdString();
        f.album = album.getText().toStdString();
        f.year = year.getText().toStdString();
        s.mp3 = f;
    }
    static constexpr engine::DitherMode modes[] = { engine::DitherMode::automatic, engine::DitherMode::off, engine::DitherMode::on };
    s.dither = modes[std::clamp (dither.getSelectedItemIndex(), 0, 2)];
    s.folder = fromUtf8 (folder.getText());
    s.name = name.getText().trim().toStdString();
    if (s.name.empty())
        s.name = "Bounce";
    return s;
}

void BounceDialog::bounce()
{
    const auto s = settings();
    juce::StringArray existing;
    for (const auto& file : engine::bounceTargets (s))
        if (std::filesystem::exists (file))
            existing.add (utf8 (file.filename()));
    if (existing.isEmpty())
    {
        begin (s);
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
    auto s = settings();
    s.replace = choice == replaceAnswer;
    begin (s);
}

void BounceDialog::begin (engine::BounceSettings s)
{
    studio.onBounceDone = [safe = juce::Component::SafePointer<BounceDialog> (this)] {
        if (safe != nullptr)
            safe->finished();
    };
    if (! studio.startBounce (s))
    {
        studio.onBounceDone = nullptr;
        notice.setText (studio.status, juce::dontSendNotification);
        return;
    }
    progress = 0;
    notice.setText (studio.bounceNotice().isEmpty() ? juce::String ("Bouncing...") : studio.bounceNotice(), juce::dontSendNotification);
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
    addToProject.setEnabled (true);
    setStage (Stage::done);
}

void BounceDialog::setStage (Stage s)
{
    current = s;
    const bool settingsShown = s == Stage::settings;
    for (auto* c : std::initializer_list<juce::Component*> { &start, &end, &tail, &mode, &wav, &bitDepth, &wavRate, &mp3, &bitrate, &mp3Rate, &title,
                                                             &artist, &album, &year, &dither, &folder, &choose, &name, &warning, &bounceButton })
        c->setVisible (settingsShown);
    cancel.setVisible (s != Stage::done);
    notice.setVisible (s != Stage::settings);
    bar.setVisible (s == Stage::rendering);
    for (auto* c : std::initializer_list<juce::Component*> { &showInFolder, &addToProject, &close })
        c->setVisible (s == Stage::done);
    repaint();
}

void BounceDialog::updateEnabled()
{
    bitDepth.setEnabled (wav.getToggleState());
    wavRate.setEnabled (wav.getToggleState());
    for (auto* c : std::initializer_list<juce::Component*> { &bitrate, &mp3Rate, &title, &artist, &album, &year })
        c->setEnabled (mp3.getToggleState());
    bounceButton.setEnabled (wav.getToggleState() || mp3.getToggleState());
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
    g.setFont (juce::FontOptions (13.0f));
    for (int i = 0; i < rowLabels.size(); ++i)
        g.drawText (rowLabels[i], labelX, 16 + i * rowHeight, controlX - labelX - 8, 26, juce::Justification::centredLeft);
    g.drawText ("to", 252, 16, 28, 26, juce::Justification::centred);
}

void BounceDialog::resized()
{
    const int w = getWidth() - controlX - 20;
    auto row = [] (int i) { return 16 + i * rowHeight; };
    start.setBounds (controlX, row (0), 120, 26);
    end.setBounds (282, row (0), 120, 26);
    tail.setBounds (controlX, row (1), 130, 26);
    mode.setBounds (controlX + 140, row (1), 130, 26);
    wav.setBounds (labelX, row (2), 100, 26);
    bitDepth.setBounds (controlX, row (2), 130, 26);
    wavRate.setBounds (controlX + 140, row (2), 160, 26);
    mp3.setBounds (labelX, row (3), 100, 26);
    bitrate.setBounds (controlX, row (3), 130, 26);
    mp3Rate.setBounds (controlX + 140, row (3), 160, 26);
    title.setBounds (controlX, row (4), w / 2 - 5, 26);
    artist.setBounds (controlX + w / 2 + 5, row (4), w / 2 - 5, 26);
    album.setBounds (controlX, row (5), w / 2 - 5, 26);
    year.setBounds (controlX + w / 2 + 5, row (5), 80, 26);
    dither.setBounds (controlX, row (6), 130, 26);
    folder.setBounds (controlX, row (7), w - 95, 26);
    choose.setBounds (getWidth() - 105, row (7), 85, 26);
    name.setBounds (controlX, row (8), w, 26);
    warning.setBounds (labelX, row (9), getWidth() - 2 * labelX, 26);
    cancel.setBounds (getWidth() - 200, getHeight() - 46, 85, 28);
    bounceButton.setBounds (getWidth() - 105, getHeight() - 46, 85, 28);

    notice.setBounds (labelX, getHeight() / 2 - 60, getWidth() - 2 * labelX, 40);
    bar.setBounds (labelX, getHeight() / 2 - 10, getWidth() - 2 * labelX, 22);
    showInFolder.setBounds (labelX, getHeight() - 46, 120, 28);
    addToProject.setBounds (labelX + 130, getHeight() - 46, 120, 28);
    close.setBounds (getWidth() - 105, getHeight() - 46, 85, 28);
}

void BounceDialog::show (Studio& studio, juce::Component* parent)
{
    auto* dialog = new BounceDialog (studio);
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (dialog);
    options.dialogTitle = "Bounce";
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
