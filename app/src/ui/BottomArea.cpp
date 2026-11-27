#include "BottomArea.h"
#include "Waveform.h"

#include <cmath>

namespace weft::ui
{
AudioEditor::AudioEditor (Studio& s) : studio (s)
{
    setTitle ("Audio Editor");
    describe (fileButton, "Audio File Editor", "Select part of the file and edit the file itself");
    fileButton.onClick = [this] {
        studio.audioFileMode = ! studio.audioFileMode;
        setSelection (0, 0);
        studio.sendChangeMessage();
    };
    describe (functions, "Functions", "Edit the file: cut, copy, paste, trim, silence, fades, normalize, gain, invert, reverse");
    functions.onClick = [this] {
        functionsMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (functions),
                                       [safe = juce::Component::SafePointer<AudioEditor> (this)] (int item) {
                                           if (safe != nullptr && item > 0)
                                               safe->pickFunction (item);
                                       });
    };
    describe (play, "Play", "Play the selection, or the region's part of the file");
    play.onClick = [this] { playSelection(); };
    describe (waveZoomButton, "Waveform Zoom", "Draw the waveform four times taller");
    waveZoomButton.onClick = [this] {
        waveZoom = waveZoom > 1.0f ? 1.0f : 4.0f;
        syncControls();
        repaint();
    };
    describe (snapButton, "Snap", "Snap edits to the grid (as in the tracks area)");
    snapButton.onClick = [this] {
        studio.snapEnabled = ! studio.snapEnabled;
        studio.sendChangeMessage();
    };
    describe (viewButton, "View", "Ruler and amplitude scales");
    viewButton.onClick = [this] {
        juce::PopupMenu menu;
        menu.addSectionHeader ("Ruler");
        menu.addItem (1, "Time", true, rulerScale == RulerScale::time);
        menu.addItem (2, "Samples", true, rulerScale == RulerScale::samples);
        menu.addSectionHeader ("Amplitude");
        menu.addItem (3, "Percent", true, amplitudeScale == AmplitudeScale::percent);
        menu.addItem (4, "Decibels", true, amplitudeScale == AmplitudeScale::decibels);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (viewButton), [safe = juce::Component::SafePointer<AudioEditor> (this)] (int item) {
            if (safe == nullptr || item <= 0)
                return;
            if (item <= 2)
                safe->rulerScale = item == 1 ? RulerScale::time : RulerScale::samples;
            else
                safe->amplitudeScale = item == 3 ? AmplitudeScale::percent : AmplitudeScale::decibels;
            safe->repaint();
        });
    };
    for (auto* b : { &fileButton, &waveZoomButton, &snapButton })
        b->setClickingTogglesState (false);
    for (auto* b : { &fileButton, &functions, &play, &waveZoomButton, &snapButton, &viewButton })
        addChildComponent (*b);
    zoom.setSliderStyle (juce::Slider::LinearHorizontal);
    zoom.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    zoom.setRange (0.0, 1.0);
    describe (zoom, "Zoom", "Horizontal zoom");
    zoom.onValueChange = [this] {
        // 0 is the whole file, 1 is 16 pixels a sample; the centre stays put.
        const auto most = std::max (1.0, static_cast<double> (fileFrames()) / std::max (1, getWidth() - scaleWidth - 20));
        const auto spp = std::exp (std::log (most) + (std::log (maxZoom) - std::log (most)) * zoom.getValue());
        zoomAround (samplesPerPixel / spp, waveArea().getCentreX());
    };
    addChildComponent (zoom);
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
    return studio.waveform (r->audio.media).frames();
}

void AudioEditor::showWholeFile()
{
    const auto frames = std::max<std::int64_t> (fileFrames(), 1);
    samplesPerPixel = std::max (maxZoom, static_cast<double> (frames) / std::max (1, getWidth() - scaleWidth - 20));
    originSample = -static_cast<double> (scaleWidth + 10) * samplesPerPixel; // the file starts right of the amplitude scale
}

void AudioEditor::zoomAround (double factor, double x)
{
    const auto anchor = xToSample (x);
    const auto maxSpp = std::max (1.0, static_cast<double> (fileFrames()) / std::max (1, getWidth() / 2));
    samplesPerPixel = std::clamp (samplesPerPixel / factor, maxZoom, maxSpp);
    originSample = anchor - x * samplesPerPixel;
    repaint();
}

void AudioEditor::syncControls()
{
    const bool shown = region() != nullptr;
    for (auto* c : std::initializer_list<juce::Component*> { &fileButton, &play, &waveZoomButton, &snapButton, &viewButton, &zoom })
        c->setVisible (shown);
    functions.setVisible (shown && studio.audioFileMode);
    fileButton.setToggleState (studio.audioFileMode, juce::dontSendNotification);
    waveZoomButton.setToggleState (waveZoom > 1.0f, juce::dontSendNotification);
    snapButton.setToggleState (studio.snapEnabled, juce::dontSendNotification);
    play.setButtonText (studio.engine().previewing() ? "Stop" : "Play");
    const auto most = std::max (1.0, static_cast<double> (fileFrames()) / std::max (1, getWidth() - scaleWidth - 20));
    const auto pos = (std::log (most) - std::log (std::clamp (samplesPerPixel, maxZoom, most))) / (std::log (most) - std::log (maxZoom));
    zoom.setValue (std::clamp (pos, 0.0, 1.0), juce::dontSendNotification);
}

void AudioEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (studio.editorRegion != shownRegion)
    {
        shownRegion = studio.editorRegion;
        needsFit = true;
        setSelection (0, 0);
    }
    if (const auto frames = fileFrames(); selectionEnd() > frames)
        setSelection (std::min (selectionStart(), frames), frames);
    syncControls();
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
    auto bar = getLocalBounds().removeFromTop (headerHeight).reduced (space::half);
    viewButton.setBounds (bar.removeFromRight (48));
    bar.removeFromRight (space::half);
    snapButton.setBounds (bar.removeFromRight (48));
    bar.removeFromRight (space::half);
    waveZoomButton.setBounds (bar.removeFromRight (80));
    bar.removeFromRight (space::half);
    zoom.setBounds (bar.removeFromRight (96));
    bar.removeFromRight (space::half);
    play.setBounds (bar.removeFromRight (48));
    bar.removeFromRight (space::half);
    functions.setBounds (bar.removeFromRight (80));
    bar.removeFromRight (space::half);
    fileButton.setBounds (bar.removeFromRight (40));
    syncControls();
}

void AudioEditor::setSelection (std::int64_t from, std::int64_t to)
{
    const auto frames = fileFrames();
    selFrom = std::clamp<std::int64_t> (from, 0, frames);
    selTo = std::clamp<std::int64_t> (to, 0, frames);
    repaint();
}

void AudioEditor::selectAll() { setSelection (0, fileFrames()); }

bool AudioEditor::apply (engine::FileEdit edit, double gainDb)
{
    const auto* r = region();
    if (r == nullptr)
        return false;
    const auto from = selectionStart(), to = selectionEnd();
    const auto hash = r->audio.media;
    if (! studio.editAudioFile (hash, edit, from, to, gainDb))
        return false;
    // The selection follows: Trim leaves the whole (new) file, Delete a point, Paste the pasted part.
    if (edit == engine::FileEdit::trim)
        selectAll();
    else if (edit == engine::FileEdit::remove)
        setSelection (from, from);
    else if (edit == engine::FileEdit::insert && studio.audioClipboard != nullptr)
        setSelection (from, from + studio.audioClipboard->numFrames());
    return true;
}

bool AudioEditor::copySelection()
{
    const auto* r = region();
    if (r == nullptr || selectionStart() == selectionEnd())
        return studio.check (commands::EditResult::fail ("Select part of the file first."));
    std::string error;
    auto clip = engine::copyMediaFrames (studio.model(), r->audio.media, selectionStart(), selectionEnd(), studio.media(), studio.mediaDir(), &error);
    if (! clip)
        return studio.check (commands::EditResult::fail (error));
    studio.audioClipboard = std::make_shared<const media::AudioData> (std::move (*clip));
    return true;
}

bool AudioEditor::cutSelection() { return copySelection() && apply (engine::FileEdit::remove); }

bool AudioEditor::paste() { return apply (engine::FileEdit::insert); }

bool AudioEditor::adjustTempoToSelection()
{
    const auto* r = region();
    if (r == nullptr || selectionStart() == selectionEnd())
        return studio.check (commands::EditResult::fail ("Select part of the file first."));
    const auto seconds = static_cast<double> (selectionEnd() - selectionStart()) / static_cast<double> (studio.model().info.sampleRate);
    const auto now = studio.model().tempo.bpmAt (r->start);
    const auto beats = std::max (1.0, std::round (seconds * now / 60.0));
    const auto bpm = beats * 60.0 / seconds;
    if (bpm < 20.0 || bpm > 999.0)
        return studio.check (commands::EditResult::fail ("That selection would need a tempo outside 20 to 999 BPM."));
    return studio.check (studio.session().setTempoAt (r->start, bpm));
}

void AudioEditor::playSelection()
{
    const auto* r = region();
    if (r == nullptr)
        return;
    if (studio.engine().previewing())
    {
        studio.engine().stopPreview();
        syncControls();
        return;
    }
    auto from = selectionStart(), to = selectionEnd();
    if (from == to)
        from = r->audio.sourceOffset, to = r->audio.sourceOffset + r->audio.sourceLength (r->length);
    std::string error;
    auto clip = engine::copyMediaFrames (studio.model(), r->audio.media, from, to, studio.media(), studio.mediaDir(), &error);
    if (! clip)
    {
        studio.setStatus (juce::String (error));
        return;
    }
    studio.engine().preview (std::make_shared<const media::AudioData> (std::move (*clip)));
    syncControls();
}

juce::PopupMenu AudioEditor::functionsMenu()
{
    const bool selected = selectionStart() != selectionEnd();
    juce::PopupMenu menu;
    menu.addItem (1, "Select All");
    menu.addItem (2, "Copy", selected);
    menu.addItem (3, "Cut", selected);
    menu.addItem (4, "Delete", selected);
    menu.addItem (5, "Paste", studio.audioClipboard != nullptr);
    menu.addItem (6, "Trim", selected);
    menu.addSeparator();
    menu.addItem (7, "Silence");
    menu.addItem (8, "Fade In");
    menu.addItem (9, "Fade Out");
    menu.addItem (10, "Normalize");
    menu.addItem (11, "Change Gain...");
    menu.addItem (12, "Invert");
    menu.addItem (13, "Reverse");
    menu.addItem (14, "Remove DC Offset");
    menu.addSeparator();
    menu.addItem (15, "Adjust Tempo Using Selection Length", selected);
    return menu;
}

void AudioEditor::pickFunction (int item)
{
    using E = engine::FileEdit;
    switch (item)
    {
        case 1: selectAll(); break;
        case 2: copySelection(); break;
        case 3: cutSelection(); break;
        case 4: apply (E::remove); break;
        case 5: paste(); break;
        case 6: apply (E::trim); break;
        case 7: apply (E::silence); break;
        case 8: apply (E::fadeIn); break;
        case 9: apply (E::fadeOut); break;
        case 10: apply (E::normalize); break;
        case 11:
            askForText ("Change Gain (dB)", "-3", [safe = juce::Component::SafePointer<AudioEditor> (this)] (juce::String text) {
                if (safe == nullptr)
                    return;
                const auto db = text.trim().getDoubleValue();
                if (text.trim().isEmpty() || std::abs (db) > 48.0)
                    safe->studio.setStatus ("Type a gain from -48 to +48 dB.");
                else
                    safe->apply (E::gain, db);
            });
            break;
        case 12: apply (E::invert); break;
        case 13: apply (E::reverse); break;
        case 14: apply (E::removeDc); break;
        case 15: adjustTempoToSelection(); break;
        default: break;
    }
}

double AudioEditor::rulerStep() const
{
    // About every 80 px, on a 1-2-5 step (in samples, or in seconds as samples).
    const auto rate = static_cast<double> (studio.model().info.sampleRate);
    const auto want = 80.0 * samplesPerPixel / (rulerScale == RulerScale::time ? rate : 1.0);
    double step = 1e-4;
    for (int i = 0; step < want && i < 60; ++i)
        step *= (i % 3 == 1) ? 2.5 : 2.0;
    return step * (rulerScale == RulerScale::time ? rate : 1.0);
}

juce::StringArray AudioEditor::rulerLabels() const
{
    juce::StringArray out;
    if (region() == nullptr || getWidth() <= scaleWidth)
        return out;
    const auto area = waveArea();
    const auto step = rulerStep();
    const auto rate = static_cast<double> (studio.model().info.sampleRate);
    for (auto at = std::ceil (std::max (0.0, xToSample (area.getX())) / step) * step; sampleToX (at) <= area.getRight(); at += step)
    {
        if (rulerScale == RulerScale::samples)
            out.add (juce::String (static_cast<std::int64_t> (std::llround (at))));
        else
        {
            const auto seconds = at / rate;
            out.add (seconds >= 60.0 ? juce::String (static_cast<int> (seconds / 60.0)) + ":" + juce::String (std::fmod (seconds, 60.0), 3).paddedLeft ('0', 6)
                                     : juce::String (seconds, step / rate < 0.01 ? 4 : 3));
        }
        if (out.size() > 200)
            break;
    }
    return out;
}

juce::StringArray AudioEditor::amplitudeLabels() const
{
    // The top line, the half line and the centre, for the waveform's height at this zoom.
    const auto top = 1.0f / waveZoom;
    if (amplitudeScale == AmplitudeScale::percent)
        return { juce::String (juce::roundToInt (top * 100.0f)) + "%", juce::String (juce::roundToInt (top * 50.0f)) + "%", "0" };
    const auto db = [] (float x) { return juce::String (juce::roundToInt (20.0f * std::log10 (x))) + " dB"; };
    return { db (top), db (top * 0.5f), "-inf" };
}

void AudioEditor::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::lane));
    const auto* r = region();
    const auto header = getLocalBounds().removeFromTop (headerHeight);
    g.setColour (colour (Token::surfaceRaised));
    g.fillRect (header);
    depth::highlight (g, header.toFloat(), 0.0f);
    g.setColour (colour (Token::lineSubtle));
    g.fillRect (header.withTop (header.getBottom() - 1));
    g.setFont (font (Type::strong));
    g.setColour (colour (Token::text));
    if (r == nullptr)
    {
        g.drawText ("Audio Editor", getLocalBounds().removeFromTop (headerHeight).reduced (10, 0), juce::Justification::centredLeft);
        g.setColour (colour (Token::textDim));
        g.setFont (font (Type::title));
        g.drawText ("Double-click an audio region to edit it here.", getLocalBounds(), juce::Justification::centred);
        return;
    }
    const auto& m = studio.model();
    const auto* media = m.mediaRef (r->audio.media);
    const auto titleArea = getLocalBounds().removeFromTop (headerHeight).withRight (fileButton.getX() - space::half).reduced (10, 0);
    g.drawText ((studio.audioFileMode ? "Audio File: " : "Audio Editor: ") + juce::String::fromUTF8 (r->name.c_str())
                    + (media != nullptr ? "  (" + juce::String::fromUTF8 (media->originalName.c_str()) + ")" : juce::String()),
                titleArea, juce::Justification::centredLeft, true);

    // The ruler under the bar and the amplitude scale at the left.
    {
        const auto ruler = getLocalBounds().withTrimmedTop (headerHeight).removeFromTop (rulerHeight);
        g.setColour (colour (Token::surface));
        g.fillRect (ruler);
        g.fillRect (getLocalBounds().withTrimmedTop (headerHeight + rulerHeight).removeFromLeft (scaleWidth));
        g.setColour (colour (Token::lineSubtle));
        g.fillRect (ruler.withTop (ruler.getBottom() - 1));
        g.setFont (font (Type::monoSmall));
        const auto step = rulerStep();
        const auto labels = rulerLabels();
        const auto wave = waveArea();
        auto at = std::ceil (std::max (0.0, xToSample (wave.getX())) / step) * step;
        for (const auto& label : labels)
        {
            const auto x = static_cast<float> (sampleToX (at));
            g.setColour (colour (Token::gridStrong));
            g.fillRect (x, static_cast<float> (ruler.getBottom() - 5), 1.0f, 5.0f);
            g.setColour (colour (Token::textDim));
            g.drawText (label, juce::Rectangle<float> (x + 2.0f, static_cast<float> (ruler.getY()), 80.0f, static_cast<float> (rulerHeight - 2)),
                        juce::Justification::centredLeft, false);
            at += step;
        }
        g.drawText ("Start " + juce::String (r->audio.sourceOffset) + "  ·  Length " + juce::String (r->audio.sourceLength (r->length)) + " samples",
                    ruler.reduced (6, 0), juce::Justification::centredRight);
        const auto amp = amplitudeLabels();
        const float ys[] = { wave.getY() + 4.0f, wave.getY() + 4.0f + (wave.getHeight() - 8.0f) * 0.25f, wave.getCentreY() };
        for (int i = 0; i < amp.size(); ++i)
            g.drawText (amp[i], juce::Rectangle<float> (0.0f, ys[i] - 6.0f, scaleWidth - 4.0f, 12.0f), juce::Justification::centredRight, false);
    }

    const auto area = waveArea();
    const auto used1 = static_cast<float> (sampleToX (static_cast<double> (r->audio.sourceOffset)));
    const auto used2 = static_cast<float> (sampleToX (static_cast<double> (r->audio.sourceOffset + r->audio.sourceLength (r->length))));
    const auto regionCol = colourFor (m.regionColor (*r).palette, m.regionColor (*r).rgb);
    // The part the region plays sits on its body colour; the rest of the file is dimmed.
    g.setColour (regionBody (regionCol));
    g.fillRect (juce::Rectangle<float> (used1, area.getY(), used2 - used1, area.getHeight()));
    g.setColour (colour (Token::gridWeak));
    g.fillRect (area.getX(), area.getCentreY(), area.getWidth(), 1.0f);

    const auto& wave = studio.waveform (r->audio.media);
    const auto content = regionCol.brighter (0.6f);
    WaveformStyle style;
    // The file editor shows the file as it is; the region view shows the region's gain. Waveform Zoom draws it taller.
    style.gain = (studio.audioFileMode ? 1.0f : static_cast<float> (std::pow (10.0, r->audio.gainDb / 20.0))) * waveZoom;
    const auto first = xToSample (area.getX());
    const auto draw = [&] (juce::Colour c, float fromX, float toX) {
        style.peak = c.withMultipliedAlpha (0.55f);
        style.rms = c;
        style.startX = fromX;
        style.endX = toX;
        drawWaveform (g, wave, area.reduced (0.0f, 4.0f), first, samplesPerPixel, style);
    };
    draw (colour (Token::textDim).withAlpha (0.5f), area.getX(), used1);
    draw (content, used1, used2);
    draw (colour (Token::textDim).withAlpha (0.5f), used2, area.getRight());
    if (studio.audioFileMode && selectionStart() != selectionEnd())
    {
        const auto x1 = static_cast<float> (sampleToX (static_cast<double> (selectionStart())));
        const auto x2 = static_cast<float> (sampleToX (static_cast<double> (selectionEnd())));
        g.setColour (colour (Token::accent).withAlpha (0.25f));
        g.fillRect (juce::Rectangle<float> (x1, area.getY(), x2 - x1, area.getHeight()));
    }
    // Trim handles: a 2 px line with a grab tab at the top.
    for (const auto& [x, tabX] : { std::pair { used1, used1 - 1.0f }, std::pair { used2, used2 - 7.0f } })
    {
        g.setColour (colour (Token::accent));
        g.fillRect (x - 1.0f, area.getY(), 2.0f, area.getHeight());
        const auto tab = juce::Rectangle<float> (tabX, area.getY(), 8.0f, 12.0f);
        g.fillRoundedRectangle (tab, radius::r1);
    }
}

AudioEditor::Edge AudioEditor::edgeAt (float x) const
{
    const auto* r = region();
    if (r == nullptr)
        return Edge::none;
    const auto x1 = sampleToX (static_cast<double> (r->audio.sourceOffset));
    const auto x2 = sampleToX (static_cast<double> (r->audio.sourceOffset + r->audio.sourceLength (r->length)));
    if (std::abs (x - x1) <= 6)
        return Edge::start;
    if (std::abs (x - x2) <= 6)
        return Edge::end;
    return Edge::none;
}

std::int64_t AudioEditor::snapSource (const model::Region& r, double sourceSample, bool bypass) const
{
    auto s = static_cast<std::int64_t> (std::llround (sourceSample));
    if (studio.snapZeroCrossings && ! bypass)
        if (const auto source = studio.media().find (r.audio.media))
            s = engine::nearestZeroCrossing (*source, s, static_cast<std::int64_t> (source->sampleRate() / 100.0)); // 10 ms
    if (bypass || studio.snapZeroCrossings || ! studio.snapEnabled || studio.snap.kind == arrange::Snap::Kind::off)
        return s;
    // Snap where that sample would play on the timeline, then map back.
    const auto& tempo = studio.model().tempo;
    const auto regionStartSample = tempo.tickToSample (static_cast<double> (r.start));
    const auto timelineSample = regionStartSample + static_cast<double> (s - r.audio.sourceOffset) / r.audio.sourcePerSample();
    const auto tick = static_cast<Ticks> (std::llround (tempo.sampleToTick (timelineSample)));
    const auto snappedSample = tempo.tickToSample (static_cast<double> (studio.snapped (tick)));
    return static_cast<std::int64_t> (std::llround ((snappedSample - regionStartSample) * r.audio.sourcePerSample())) + r.audio.sourceOffset;
}

void AudioEditor::pointerDown (juce::Point<float> p)
{
    if (studio.audioFileMode)
    {
        // The file editor: drag to select part of the file.
        const auto* r = region();
        if (r == nullptr)
            return;
        selecting = true;
        const auto at = snapSource (*r, xToSample (p.x), ! studio.snapZeroCrossings);
        setSelection (at, at);
        return;
    }
    dragging = edgeAt (p.x);
    if (dragging != Edge::none)
    {
        studio.session().beginGesture ("Trim Region");
        gestureOpen = true;
    }
}

void AudioEditor::pointerDrag (juce::Point<float> p, bool bypassSnap)
{
    if (selecting)
    {
        if (const auto* r = region())
            setSelection (selFrom, snapSource (*r, xToSample (p.x), bypassSnap || ! studio.snapZeroCrossings));
        return;
    }
    const auto* r = region();
    if (r == nullptr || dragging == Edge::none)
        return;
    const auto target = std::clamp<std::int64_t> (snapSource (*r, xToSample (p.x), bypassSnap), 0, fileFrames());
    if (dragging == Edge::start)
        studio.session().trimAudioStartSamples (r->id, target);
    else
        studio.session().trimRegionLength (r->id, std::max<std::int64_t> (r->audio.timelineLength (target - r->audio.sourceOffset), model::minAudioRegionLength));
    studio.edited();
}

void AudioEditor::pointerUp()
{
    if (selecting)
    {
        selecting = false;
        setSelection (selectionStart(), selectionEnd());
        return;
    }
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
    setMouseCursor (studio.audioFileMode ? juce::MouseCursor::IBeamCursor
                    : edgeAt (e.position.x) != Edge::none ? juce::MouseCursor::LeftRightResizeCursor
                                                          : juce::MouseCursor::NormalCursor);
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

BottomArea::BottomArea (Studio& s) : studio (s), editor (s), mixer (s), roll (s)
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
