#pragma once

// F · Editors (spec 04 §7): the audio editor (F-EDA-1) and, until their milestones, the
// mixer and piano roll as labelled placeholders.

#include "Studio.h"
#include "Widgets.h"

namespace weft::ui
{
/** Shows the region's whole source file with the used part highlighted; its edges trim the region
    to the sample (D-UI-5). */
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
    juce::Rectangle<float> waveArea() const { return getLocalBounds().toFloat().withTrimmedTop (headerHeight).reduced (0, 4); }
    static constexpr int headerHeight = 26;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    std::int64_t fileFrames() const;
    /** A source sample snapped to the arrange grid (unless snap is off). */
    std::int64_t snapSource (const model::Region& r, double sourceSample, bool bypass) const;

    Studio& studio;
    Id shownRegion;
    Edge dragging = Edge::none;
    bool gestureOpen = false;
    bool needsFit = true;
};

class Placeholder final : public juce::Component
{
public:
    Placeholder (const juce::String& title, const juce::String& note) : text (note) { setTitle (title); }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (colour (Token::surface));
        g.setColour (colour (Token::textDim));
        g.setFont (juce::FontOptions (15.0f));
        g.drawText (getTitle() + ": " + text, getLocalBounds(), juce::Justification::centred);
    }

private:
    juce::String text;
};

class BottomArea final : public juce::Component, private juce::ChangeListener
{
public:
    explicit BottomArea (Studio& studio);
    ~BottomArea() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    AudioEditor& audioEditor() noexcept { return editor; }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    Studio& studio;
    AudioEditor editor;
    Placeholder mixer { "Mixer", "channel strips arrive with the mixer milestone." };
    Placeholder pianoRoll { "Piano Roll", "note editing arrives with the MIDI editing milestone." };
};
} // namespace weft::ui
