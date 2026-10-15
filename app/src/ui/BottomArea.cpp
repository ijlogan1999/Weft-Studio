#include "BottomArea.h"

#include <cmath>

namespace weft::ui
{
AudioEditor::AudioEditor (Studio& s) : studio (s)
{
    setTitle ("Audio Editor");
    studio.addChangeListener (this);
}

AudioEditor::~AudioEditor() { studio.removeChangeListener (this); }

const model::Region* AudioEditor::region() const
{
    const auto* r = studio.model().region (studio.editorRegion);
    return r != nullptr && ! r->isMidi ? r : nullptr;
}

std::int64_t AudioEditor::fileFrames() const
{
    const auto* r = region();
    if (r == nullptr)
        return 0;
    if (const auto* m = studio.model().mediaRef (r->audio.media); m != nullptr && m->lengthSamples > 0)
        return m->lengthSamples;
    return static_cast<std::int64_t> (studio.peaks (r->audio.media).size()) * Studio::peakFrames;
}

void AudioEditor::showWholeFile()
{
    const auto frames = std::max<std::int64_t> (fileFrames(), 1);
    samplesPerPixel = std::max (maxZoom, static_cast<double> (frames) / std::max (1, getWidth() - 20));
    originSample = -10.0 * samplesPerPixel;
}

void AudioEditor::zoomAround (double factor, double x)
{
    const auto anchor = xToSample (x);
    const auto maxSpp = std::max (1.0, static_cast<double> (fileFrames()) / std::max (1, getWidth() / 2));
    samplesPerPixel = std::clamp (samplesPerPixel / factor, maxZoom, maxSpp);
    originSample = anchor - x * samplesPerPixel;
    repaint();
}

void AudioEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (studio.editorRegion != shownRegion)
    {
        shownRegion = studio.editorRegion;
        needsFit = true;
    }
    if (needsFit && getWidth() > 0)
    {
        showWholeFile();
        needsFit = false;
    }
    repaint();
}

void AudioEditor::resized()
{
    if (shownRegion != studio.editorRegion)
        shownRegion = studio.editorRegion, needsFit = true;
    if (needsFit && getWidth() > 0)
    {
        showWholeFile();
        needsFit = false;
    }
}

void AudioEditor::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::lane));
    const auto* r = region();
    g.setColour (colour (Token::surface));
    g.fillRect (getLocalBounds().removeFromTop (headerHeight));
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.setColour (colour (Token::text));
    if (r == nullptr)
    {
        g.drawText ("Audio Editor", getLocalBounds().removeFromTop (headerHeight).reduced (10, 0), juce::Justification::centredLeft);
        g.setColour (colour (Token::textDim));
        g.setFont (juce::FontOptions (15.0f));
        g.drawText ("Double-click an audio region to edit it here.", getLocalBounds(), juce::Justification::centred);
        return;
    }
    const auto& m = studio.model();
    const auto* media = m.mediaRef (r->audio.media);
    g.drawText ("Audio Editor: " + juce::String (r->name) + (media != nullptr ? "  (" + juce::String (media->originalName) + ")" : juce::String()),
                getLocalBounds().removeFromTop (headerHeight).reduced (10, 0), juce::Justification::centredLeft);

    const auto area = waveArea();
    const auto used1 = static_cast<float> (sampleToX (static_cast<double> (r->audio.sourceOffset)));
    const auto used2 = static_cast<float> (sampleToX (static_cast<double> (r->audio.sourceOffset + r->length)));
    const auto regionCol = colourFor (m.regionColor (*r).palette, m.regionColor (*r).rgb);
    g.setColour (regionCol.withAlpha (0.35f));
    g.fillRect (juce::Rectangle<float> (used1, area.getY(), used2 - used1, area.getHeight()));

    const auto& pk = studio.peaks (r->audio.media);
    const auto frames = fileFrames();
    const auto mid = area.getCentreY();
    const auto half = area.getHeight() * 0.46f;
    for (int px = 0; px < getWidth(); ++px)
    {
        const auto s1 = xToSample (px), s2 = xToSample (px + 1);
        if (s2 < 0 || s1 >= static_cast<double> (frames))
            continue;
        const auto from = static_cast<std::int64_t> (std::max (0.0, s1)) / Studio::peakFrames;
        const auto to = static_cast<std::int64_t> (std::max (0.0, s2)) / Studio::peakFrames;
        float peak = 0;
        for (auto i = from; i <= to && i < static_cast<std::int64_t> (pk.size()); ++i)
            peak = std::max (peak, pk[static_cast<std::size_t> (i)]);
        const bool inside = static_cast<float> (px) >= used1 && static_cast<float> (px) < used2;
        g.setColour (inside ? colour (Token::text) : colour (Token::textDim).withAlpha (0.6f));
        g.drawVerticalLine (px, mid - peak * half, mid + peak * half);
    }
    // Trim handles.
    g.setColour (colour (Token::accent));
    g.fillRect (used1 - 1.5f, area.getY(), 3.0f, area.getHeight());
    g.fillRect (used2 - 1.5f, area.getY(), 3.0f, area.getHeight());
    g.setFont (juce::FontOptions (12.0f));
    g.setColour (colour (Token::textDim));
    g.drawText ("Start " + juce::String (r->audio.sourceOffset) + "  ·  Length " + juce::String (r->length) + " samples",
                getLocalBounds().removeFromTop (headerHeight).reduced (10, 0), juce::Justification::centredRight);
}

AudioEditor::Edge AudioEditor::edgeAt (float x) const
{
    const auto* r = region();
    if (r == nullptr)
        return Edge::none;
    const auto x1 = sampleToX (static_cast<double> (r->audio.sourceOffset));
    const auto x2 = sampleToX (static_cast<double> (r->audio.sourceOffset + r->length));
    if (std::abs (x - x1) <= 6)
        return Edge::start;
    if (std::abs (x - x2) <= 6)
        return Edge::end;
    return Edge::none;
}

std::int64_t AudioEditor::snapSource (const model::Region& r, double sourceSample, bool bypass) const
{
    const auto s = static_cast<std::int64_t> (std::llround (sourceSample));
    if (bypass || ! studio.snapEnabled || studio.snap.kind == arrange::Snap::Kind::off)
        return s;
    // Snap where that sample would play on the timeline, then map back.
    const auto& tempo = studio.model().tempo;
    const auto regionStartSample = tempo.tickToSample (static_cast<double> (r.start));
    const auto timelineSample = regionStartSample + static_cast<double> (s - r.audio.sourceOffset);
    const auto tick = static_cast<Ticks> (std::llround (tempo.sampleToTick (timelineSample)));
    const auto snappedSample = tempo.tickToSample (static_cast<double> (studio.snapped (tick)));
    return static_cast<std::int64_t> (std::llround (snappedSample - regionStartSample)) + r.audio.sourceOffset;
}

void AudioEditor::pointerDown (juce::Point<float> p)
{
    dragging = edgeAt (p.x);
    if (dragging != Edge::none)
    {
        studio.session().beginGesture ("Trim Region");
        gestureOpen = true;
    }
}

void AudioEditor::pointerDrag (juce::Point<float> p, bool bypassSnap)
{
    const auto* r = region();
    if (r == nullptr || dragging == Edge::none)
        return;
    const auto target = std::clamp<std::int64_t> (snapSource (*r, xToSample (p.x), bypassSnap), 0, fileFrames());
    if (dragging == Edge::start)
        studio.session().trimAudioStartSamples (r->id, target);
    else
        studio.session().trimRegionLength (r->id, std::max<std::int64_t> (target - r->audio.sourceOffset, model::minAudioRegionLength));
    studio.edited();
}

void AudioEditor::pointerUp()
{
    if (gestureOpen)
    {
        studio.session().endGesture();
        gestureOpen = false;
        studio.edited();
    }
    dragging = Edge::none;
}

void AudioEditor::mouseDown (const juce::MouseEvent& e) { pointerDown (e.position); }
void AudioEditor::mouseDrag (const juce::MouseEvent& e) { pointerDrag (e.position, e.mods.isAltDown()); }
void AudioEditor::mouseUp (const juce::MouseEvent&) { pointerUp(); }
void AudioEditor::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (edgeAt (e.position.x) != Edge::none ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}
void AudioEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown())
        zoomAround (std::pow (2.0, static_cast<double> (w.deltaY) * 2.0), e.position.x);
    else
    {
        originSample -= static_cast<double> (std::abs (w.deltaY) > 0.0f ? w.deltaY : w.deltaX) * 200.0 * samplesPerPixel;
        repaint();
    }
}

// Bottom area ---------------------------------------------------------------------------------

BottomArea::BottomArea (Studio& s) : studio (s), editor (s), roll (s)
{
    setTitle ("Editors");
    addChildComponent (editor);
    addChildComponent (mixer);
    addChildComponent (roll);
    studio.addChangeListener (this);
    changeListenerCallback (nullptr);
}

BottomArea::~BottomArea() { studio.removeChangeListener (this); }

void BottomArea::changeListenerCallback (juce::ChangeBroadcaster*)
{
    editor.setVisible (studio.layout.bottom == BottomTab::audioEditor);
    mixer.setVisible (studio.layout.bottom == BottomTab::mixer);
    roll.setVisible (studio.layout.bottom == BottomTab::pianoRoll);
}

void BottomArea::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::surface));
    g.setColour (colour (Token::border));
    g.drawHorizontalLine (0, 0.0f, static_cast<float> (getWidth()));
}

void BottomArea::resized()
{
    const auto r = getLocalBounds().withTrimmedTop (1);
    for (auto* c : std::initializer_list<juce::Component*> { &editor, &mixer, &roll })
        c->setBounds (r);
}
} // namespace weft::ui
