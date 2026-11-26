// AT-EDA-1 (audio editor) and AT-LIB-1 (file browser).

#include "UiFixture.h"

using namespace weft;
using namespace weft::ui;
using namespace weft::ui::test;

namespace
{
Id importSine (Ui& ui, Ticks at)
{
    const auto file = writeSine (ui.folder.path / "sine.wav");
    engine::DropTarget target;
    target.at = at;
    target.trackId = ui.m().trackOrder.front();
    const auto outcome = ui.studio.importFiles ({ file }, target);
    REQUIRE (outcome.regions.size() == 1);
    ui.settle();
    return outcome.regions.front();
}
} // namespace

TEST_CASE ("AT-EDA-1 audio editor shows the source and trims by single samples", "[UI]")
{
    Ui ui;
    const auto id = importSine (ui, bar);
    auto& s = ui.studio.session();
    // Use only the middle of the file so both edges have room to move.
    REQUIRE (s.trimAudioStartSamples (id, 12000));
    REQUIRE (s.trimRegionLength (id, 24000));
    ui.studio.edited();
    ui.settle();

    // Double-click opens the audio editor on that region.
    ui.timeline().pointerDown (ui.centreOf (id), {}, 2);
    ui.timeline().pointerUp (ui.centreOf (id), {});
    ui.settle();
    REQUIRE (ui.main->bottom().isVisible());
    auto& editor = ui.main->bottom().audioEditor();
    REQUIRE (editor.isVisible());
    REQUIRE (editor.region() != nullptr);
    CHECK (editor.region()->id == id);

    // The whole file fits, with the used part inside it.
    const auto width = static_cast<double> (editor.getWidth());
    CHECK (editor.sampleToX (0) >= 0.0);
    CHECK (editor.sampleToX (48000) <= width);
    CHECK (editor.sampleToX (12000) > editor.sampleToX (0));
    CHECK (editor.sampleToX (36000) < editor.sampleToX (48000));
    CHECK (editor.edgeAt (static_cast<float> (editor.sampleToX (12000))) == AudioEditor::Edge::start);
    CHECK (editor.edgeAt (static_cast<float> (editor.sampleToX (36000))) == AudioEditor::Edge::end);

    // Maximum zoom, snap off: one sample per step.
    ui.studio.snapEnabled = false;
    editor.samplesPerPixel = AudioEditor::maxZoom;
    editor.originSample = 12000.0 - 100.0 * AudioEditor::maxZoom;
    const auto startX = static_cast<float> (editor.sampleToX (12000));
    const auto y = editor.waveArea().getCentreY();
    const auto pxPerSample = static_cast<float> (1.0 / AudioEditor::maxZoom);
    editor.pointerDown ({ startX, y });
    editor.pointerDrag ({ startX + pxPerSample, y }, false);
    editor.pointerUp();
    ui.settle();
    CHECK (ui.region (id).audio.sourceOffset == 12001);
    CHECK (ui.region (id).length == 23999);

    editor.originSample = 36001.0 - 100.0 * AudioEditor::maxZoom;
    const auto endX = static_cast<float> (editor.sampleToX (36000));
    editor.pointerDown ({ endX, y });
    editor.pointerDrag ({ endX - pxPerSample, y }, false);
    editor.pointerUp();
    ui.settle();
    CHECK (ui.region (id).length == 23998);
    CHECK (ui.region (id).audio.sourceOffset == 12001);

    // The arrange view draws the new length.
    const auto box = ui.timeline().regionBounds (ui.region (id));
    const auto expectedEnd = ui.studio.view.tickToX (ui.m().regionEndTick (ui.region (id)));
    CHECK (box.getRight() == Catch::Approx (expectedEnd).margin (0.5));

    // Each drag is one undo step.
    ui.main->run (Cmd::undo);
    ui.settle();
    CHECK (ui.region (id).length == 23999);
}

TEST_CASE ("AT-LIB-1 file browser lists supported files, previews on click, drags into the arrange", "[UI]")
{
    Ui ui;
    const auto dir = ui.folder.path / "browse";
    std::filesystem::create_directories (dir / "Loops");
    const auto wav = writeSine (dir / "tone.wav", 0.5);
    { std::ofstream (dir / "notes.txt") << "hello"; }
    {
        // A minimal type 0 MIDI file (header + empty track).
        std::ofstream mid (dir / "beat.mid", std::ios::binary);
        const unsigned char bytes[] = { 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 1, 0xE0, 'M', 'T', 'r', 'k', 0, 0, 0, 4, 0, 0xFF, 0x2F, 0 };
        mid.write (reinterpret_cast<const char*> (bytes), sizeof (bytes));
    }

    ui.main->run (Cmd::toggleSidePanel);
    ui.settle();
    auto& browser = ui.main->side().browser();
    browser.setRoot (juce::File (juce::String (dir.string())));
    auto& list = browser.contents();
    for (int i = 0; i < 300 && (list.isStillLoading() || list.getNumFiles() < 3); ++i)
        juce::Thread::sleep (10);
    juce::StringArray names;
    for (int i = 0; i < list.getNumFiles(); ++i)
        names.add (list.getFile (i).getFileName());
    INFO (names.joinIntoString (", "));
    CHECK (names.contains ("Loops"));
    CHECK (names.contains ("tone.wav"));
    CHECK (names.contains ("beat.mid"));
    CHECK_FALSE (names.contains ("notes.txt"));

    // Clicking an audio file previews it.
    const juce::File tone (juce::String (wav.string()));
    browser.fileClicked (tone, mouse (browser.tree(), { 10.0f, 10.0f }));
    ui.renderAudio();
    CHECK (ui.studio.engine().previewing());

    // Dragging it from the browser onto the first track's lane imports it there (AT-ARR-25).
    list.dispatchPendingMessages(); // the tree builds its rows from the list's change message
    browser.tree().setSelectedFile (tone);
    REQUIRE (browser.tree().getSelectedFile() == tone);
    auto& tl = ui.timeline();
    const juce::Point<int> drop { static_cast<int> (ui.studio.view.tickToX (2.0 * bar)), static_cast<int> (tl.laneBounds (0).getCentreY()) };
    tl.itemDropped (juce::DragAndDropTarget::SourceDetails ("weft-files", &browser.tree(), drop));
    ui.settle();
    const auto regions = ui.m().regionsOnTrack (ui.m().trackOrder.front());
    REQUIRE (regions.size() == 1);
    CHECK (regions.front()->start == ui.studio.snapped (2 * bar));
    CHECK (regions.front()->length == 24000);
    CHECK (ui.main->side().media().rows().size() == 1);
}

TEST_CASE ("AT-LIB-3 file browser history, places, search and preview controls", "[UI][AT-LIB-3]")
{
    Ui ui;
    auto& device = static_cast<engine::NullDevice&> (ui.studio.device());
    device.setCaptureCapacity (4096);
    const auto dir = ui.folder.path / "browse";
    std::filesystem::create_directories (dir / "Loops");
    const auto wav = writeSine (dir / "tone.wav", 0.5);
    writeSine (dir / "Loops" / "drum.wav", 0.1);
    {
        std::ofstream mid (dir / "beat.mid", std::ios::binary);
        const unsigned char bytes[] = { 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 1, 0xE0, 'M', 'T', 'r', 'k', 0, 0, 0, 4, 0, 0xFF, 0x2F, 0 };
        mid.write (reinterpret_cast<const char*> (bytes), sizeof (bytes));
    }
    ui.main->run (Cmd::toggleSidePanel);
    ui.settle();
    auto& browser = ui.main->side().browser();
    auto& list = browser.contents();
    const auto names = [&] {
        for (int i = 0; i < 300 && list.isStillLoading(); ++i)
            juce::Thread::sleep (10);
        juce::StringArray out;
        for (int i = 0; i < list.getNumFiles(); ++i)
            out.add (list.getFile (i).getFileName());
        return out;
    };
    const juce::File root (juce::String (dir.string())), loops (juce::String ((dir / "Loops").string()));

    // Back and forward.
    browser.setRoot (root);
    browser.setRoot (loops);
    CHECK (list.getDirectory() == loops);
    REQUIRE (browser.canGoBack());
    CHECK (browser.backButton().isEnabled());
    CHECK_FALSE (browser.forwardButton().isEnabled());
    browser.backButton().onClick();
    ui.settle();
    CHECK (list.getDirectory() == root);
    REQUIRE (browser.canGoForward());
    browser.forwardButton().onClick();
    ui.settle();
    CHECK (list.getDirectory() == loops);
    browser.goBack();
    browser.setRoot (root.getParentDirectory()); // a new place drops the forward history
    CHECK_FALSE (browser.canGoForward());
    browser.goBack();
    CHECK (list.getDirectory() == root);

    // Places.
    browser.goHome();
    CHECK (list.getDirectory() == juce::File::getSpecialLocation (juce::File::userHomeDirectory));
    browser.goToProject();
    CHECK (list.getDirectory() == juce::File (juce::String (ui.studio.projectRoot().string())));
    browser.goBack();
    browser.goBack();
    CHECK (list.getDirectory() == root);

    // Search: every word, any case; folders stay.
    CHECK (names().contains ("beat.mid"));
    browser.searchField().setText ("TON", juce::dontSendNotification);
    browser.searchField().onTextChange(); // JUCE posts the change; deliver it now
    auto found = names();
    INFO (found.joinIntoString (", "));
    CHECK (found.contains ("tone.wav"));
    CHECK (found.contains ("Loops"));
    CHECK_FALSE (found.contains ("beat.mid"));
    browser.setSearch ("tone mid");
    CHECK_FALSE (names().contains ("tone.wav"));
    browser.setSearch ({});
    CHECK (browser.searchField().isEmpty());
    CHECK (names().contains ("beat.mid"));

    // Play previews the selected file at the preview volume; pressed again it stops.
    browser.volumeSlider().setValue (-12.0, juce::sendNotificationSync);
    list.dispatchPendingMessages();
    const juce::File tone (juce::String (wav.string()));
    browser.tree().setSelectedFile (tone);
    REQUIRE (browser.tree().getSelectedFile() == tone);
    browser.playButton().onClick();
    ui.settle();
    ui.renderAudio (2048);
    CHECK (ui.studio.engine().previewing());
    CHECK (browser.playButton().getButtonText() == "Stop");
    float peak = 0;
    for (auto v : device.capturedOutput (0))
        peak = std::max (peak, std::abs (v));
    CHECK (peak == Catch::Approx (0.5 * std::pow (10.0, -12.0 / 20.0)).epsilon (0.01));
    browser.playButton().onClick();
    ui.settle();
    ui.renderAudio (256);
    CHECK_FALSE (ui.studio.engine().previewing());
}

TEST_CASE ("AT-EDA-2 Snap Edits to Zero Crossings moves audio editor trims onto a zero crossing", "[UI][AT-EDA-2]")
{
    Ui ui;
    const auto id = importSine (ui, bar); // 440 Hz: a crossing about every 54.5 samples
    ui.timeline().pointerDown (ui.centreOf (id), {}, 2);
    ui.timeline().pointerUp (ui.centreOf (id), {});
    ui.settle();
    auto& editor = ui.main->bottom().audioEditor();
    REQUIRE (editor.region() != nullptr);
    REQUIRE (ui.studio.perform (Cmd::snapZeroCrossings));
    REQUIRE (ui.studio.snapZeroCrossings);

    const auto source = ui.studio.media().find (ui.region (id).audio.media);
    REQUIRE (source != nullptr);
    const auto value = [&] (std::int64_t f) {
        bool missing = false;
        return source->sample (0, f, missing) + source->sample (1, f, missing);
    };
    editor.samplesPerPixel = 1.0;
    editor.originSample = 0.0;
    const auto y = editor.waveArea().getCentreY();
    editor.pointerDown ({ static_cast<float> (editor.sampleToX (0)), y });
    editor.pointerDrag ({ static_cast<float> (editor.sampleToX (1000)), y }, false);
    editor.pointerUp();
    ui.settle();
    const auto offset = ui.region (id).audio.sourceOffset;
    CHECK (std::abs (offset - 1000) <= 30);
    const auto a = value (offset - 1), b = value (offset);
    CHECK ((b == 0.0f || (a < 0.0f) != (b < 0.0f)));

    // The helper on its own: no crossing within the radius keeps the frame.
    media::AudioData dc;
    dc.channels.assign (1, std::vector<float> (1000, 0.25f));
    const engine::MediaSource flat (dc);
    CHECK (engine::nearestZeroCrossing (flat, 500, 100) == 500);
    CHECK (engine::nearestZeroCrossing (flat, 50, 100) == 0); // the file start counts
}

TEST_CASE ("AT-ARR-39 Text, Zoom, Fade, Gain and Quantize tools; Next and Previous Tool", "[UI][AT-ARR-39]")
{
    Ui ui;
    const auto audio = importSine (ui, bar); // one second: bar 2 to bar 2 + 2 beats at 120 BPM
    auto& tl = ui.timeline();
    auto& s = ui.studio.session();
    const auto box = [&] { return tl.regionBounds (ui.region (audio)); };

    // Text: click a region, type a name.
    ui.studio.tool = Tool::text;
    tl.promptForText = [] (const juce::String& title, const juce::String& current, std::function<void (juce::String)> done) {
        CHECK (title == "Rename region");
        CHECK (current.isNotEmpty());
        done ("Lead take");
    };
    ui.click (box().getCentre());
    CHECK (ui.region (audio).name == "Lead take");

    // Fade: a drag in the left half sets the fade in, in the right half the fade out; one undo step each.
    ui.studio.tool = Tool::fade;
    {
        const auto b = box();
        tl.pointerDown (juce::Point<float> { b.getX() + b.getWidth() * 0.1f, b.getCentreY() }, {}, 1);
        tl.pointerDrag (juce::Point<float> { b.getX() + b.getWidth() * 0.25f, b.getCentreY() }, {});
        tl.pointerUp (juce::Point<float> { b.getX() + b.getWidth() * 0.25f, b.getCentreY() }, {});
        ui.settle();
    }
    CHECK (ui.region (audio).audio.fadeIn.length == Catch::Approx (0.25 * static_cast<double> (ui.region (audio).length)).margin (500));
    CHECK (ui.region (audio).audio.fadeOut.length == 0);
    CHECK (s.undoName() == "Fade");
    {
        const auto b = box();
        tl.pointerDown (juce::Point<float> { b.getRight() - 2.0f, b.getCentreY() }, {}, 1);
        tl.pointerDrag (juce::Point<float> { b.getRight() - b.getWidth() * 0.5f, b.getCentreY() }, {});
        tl.pointerUp (juce::Point<float> { b.getRight() - b.getWidth() * 0.5f, b.getCentreY() }, {});
        ui.settle();
    }
    CHECK (ui.region (audio).audio.fadeOut.length > 0);

    // Gain: up is louder, 0.25 dB a pixel, one undo step.
    ui.studio.tool = Tool::gain;
    {
        const auto c = box().getCentre();
        tl.pointerDown (c, {}, 1);
        tl.pointerDrag (c.translated (0.0f, -24.0f), {});
        tl.pointerUp (c.translated (0.0f, -24.0f), {});
        ui.settle();
    }
    CHECK (ui.region (audio).audio.gainDb == Catch::Approx (6.0));
    CHECK (s.undoName() == "Region Gain");
    REQUIRE (ui.studio.perform (Cmd::undo));
    CHECK (ui.region (audio).audio.gainDb == Catch::Approx (0.0));

    // Quantize: a click quantizes a MIDI region's notes to the piano roll's quantize grid.
    const auto keysTrack = ui.instrumentTracks (1).front();
    const auto midi = *s.createMidiRegion (keysTrack, 4 * bar, bar);
    const auto note = *s.drawNote (midi, 4 * bar + 250, 60, 120, 100);
    ui.studio.edited();
    ui.settle();
    ui.studio.tool = Tool::quantize;
    ui.studio.piano.quantizeGrid = 240;
    ui.studio.piano.strength = 100.0;
    ui.click (tl.regionBounds (ui.region (midi)).getCentre());
    CHECK (commands::noteTimelineTick (ui.region (midi), commands::findNote (ui.m(), note)->pos) == 4 * bar + 240);

    // Zoom: a box fits its range; a click zooms in; Alt+click out.
    ui.studio.tool = Tool::zoom;
    const auto from = ui.studio.view.tickToX (2.0 * bar), to = ui.studio.view.tickToX (3.0 * bar);
    tl.pointerDown (juce::Point<float> { static_cast<float> (from), 10.0f }, {}, 1);
    tl.pointerDrag (juce::Point<float> { static_cast<float> (to), 30.0f }, {});
    tl.pointerUp (juce::Point<float> { static_cast<float> (to), 30.0f }, {});
    ui.settle();
    CHECK (ui.studio.view.xToTick (0.0) == Catch::Approx (2.0 * bar).margin (2.0));
    CHECK (ui.studio.view.xToTick (static_cast<double> (tl.getWidth())) == Catch::Approx (3.0 * bar).margin (2.0));
    const auto ppt = ui.studio.view.pixelsPerTick;
    ui.click ({ 100.0f, 10.0f });
    CHECK (ui.studio.view.pixelsPerTick == Catch::Approx (2.0 * ppt));
    ui.click ({ 100.0f, 10.0f }, juce::ModifierKeys::altModifier);
    CHECK (ui.studio.view.pixelsPerTick == Catch::Approx (ppt));

    // Next / Previous Tool go through the Tool menu's order and wrap.
    ui.studio.tool = Tool::quantize;
    REQUIRE (ui.studio.perform (Cmd::nextTool));
    CHECK (ui.studio.tool == Tool::pointer);
    REQUIRE (ui.studio.perform (Cmd::previousTool));
    CHECK (ui.studio.tool == Tool::quantize);
    // The Tool menu lists every tool with its key.
    juce::StringArray names, keys;
    const auto menu = ui.main->toolMenu();
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
    {
        names.add (it.getItem().text);
        keys.add (it.getItem().shortcutKeyDescription);
    }
    CHECK (names == juce::StringArray { "Pointer", "Pencil", "Scissors", "Glue", "Eraser", "Mute", "Text", "Zoom", "Fade", "Gain", "Quantize" });
    CHECK (keys == juce::StringArray { "T", "P", "I", "J", "E", "M", "N", "Z", "A", "G", "Q" });
}
