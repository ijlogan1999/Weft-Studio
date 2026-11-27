#pragma once

// G · Editors (spec 04 §7): the mixer (F-MIX), the piano roll (F-PRL) and the audio editor (F-EDA-1).

#include "Mixer.h"
#include "PianoRoll.h"
#include "Studio.h"
#include "Widgets.h"

namespace weft::ui
{
/** Shows the region's whole source file with the used part highlighted; its edges trim the region
    to the sample (D-UI-5). With File on it is the Audio File Editor (D-UI-49): dragging selects part
    of the file and the Functions menu rewrites the file (every region playing it follows), one undo
    step each. Its own bar holds the zoom slider, Waveform Zoom, Snap, Play and the View menu (ruler
    and amplitude scales); a ruler sits under the bar and the amplitude scale at the left. */
class AudioEditor final : public juce::Component, private juce::ChangeListener
{
public:
    explicit AudioEditor (Studio& studio);
    ~AudioEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    // View: source samples per pixel and the first sample shown.
    double samplesPerPixel = 100.0;
    double originSample = 0.0;
    static constexpr double maxZoom = 1.0 / 16.0; // 16 px per sample
    double sampleToX (double s) const { return (s - originSample) / samplesPerPixel; }
    double xToSample (double x) const { return originSample + x * samplesPerPixel; }
    /** Fits the whole source file. */
    void showWholeFile();
    /** Zooms by factor (>1 zooms in) keeping the sample under x in place. */
    void zoomAround (double factor, double x);

    enum class Edge { none, start, end };
    Edge edgeAt (float x) const;
    /** Pointer actions (tests drive these). */
    void pointerDown (juce::Point<float> p);
    void pointerDrag (juce::Point<float> p, bool bypassSnap);
    void pointerUp();

    const model::Region* region() const;
    static constexpr int headerHeight = size::panelHeader;
    static constexpr int rulerHeight = 16;
    static constexpr int scaleWidth = 36;
    juce::Rectangle<float> waveArea() const
    {
        return getLocalBounds().toFloat().withTrimmedTop (static_cast<float> (headerHeight + rulerHeight)).withTrimmedLeft (scaleWidth).reduced (0, 4);
    }

    // The Audio File Editor (D-UI-49). The selection is in the file's frames.
    std::int64_t selectionStart() const noexcept { return std::min (selFrom, selTo); }
    std::int64_t selectionEnd() const noexcept { return std::max (selFrom, selTo); }
    void setSelection (std::int64_t from, std::int64_t to);
    void selectAll();
    /** A function from the menu on the selection (an empty selection is the whole file where that makes sense). */
    bool apply (engine::FileEdit edit, double gainDb = 0.0);
    bool copySelection();
    bool cutSelection();
    bool paste();
    /** Adjust Tempo Using Selection Length: the tempo at the region's start becomes the one that makes the selection
        a whole number of beats (the nearest). */
    bool adjustTempoToSelection();
    /** Plays the selection (or the region's part of the file); again stops. */
    void playSelection();
    enum class RulerScale { time, samples };
    enum class AmplitudeScale { percent, decibels };
    RulerScale rulerScale = RulerScale::time;
    AmplitudeScale amplitudeScale = AmplitudeScale::percent;
    /** The ruler's labels and the amplitude scale's, as drawn (tests read them). */
    juce::StringArray rulerLabels() const;
    juce::StringArray amplitudeLabels() const;
    /** Waveform Zoom: the waveform drawn this many times taller (1 or 4). */
    float waveZoom = 1.0f;

    juce::TextButton fileButton { "File" }, functions { "Functions" }, play { "Play" }, waveZoomButton { "Wave Zoom" }, snapButton { "Snap" },
        viewButton { "View" };
    juce::Slider zoom;
    /** The Functions menu's items, in order. */
    juce::PopupMenu functionsMenu();
    void pickFunction (int item);

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    std::int64_t fileFrames() const;
    /** A source sample snapped to the arrange grid (unless snap is off). */
    std::int64_t snapSource (const model::Region& r, double sourceSample, bool bypass) const;
    void syncControls();
    double rulerStep() const;

    Studio& studio;
    Id shownRegion;
    Edge dragging = Edge::none;
    bool gestureOpen = false;
    bool needsFit = true;
    bool selecting = false;
    std::int64_t selFrom = 0, selTo = 0;
};


class BottomArea final : public juce::Component, private juce::ChangeListener
{
public:
    explicit BottomArea (Studio& studio);
    ~BottomArea() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    AudioEditor& audioEditor() noexcept { return editor; }
    PianoRoll& pianoRoll() noexcept { return roll; }
    MixerView& mixerView() noexcept { return mixer; }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    Studio& studio;
    AudioEditor editor;
    MixerView mixer;
    PianoRoll roll;
};
} // namespace weft::ui
