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
