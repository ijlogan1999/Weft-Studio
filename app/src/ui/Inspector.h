#pragma once

// C · Inspector (spec 04 §4): the selected region's parameters above the selected
// track's, then two channel strips: the selected track's and its output's. Edits go straight
// to the session; each control change is one undo step. The strips are the mixer's own
// ChannelStrip, so the inspector and the mixer always show and edit the same data.

#include "Mixer.h"
#include "Studio.h"
#include "Widgets.h"

namespace weft::ui
{
/** The track's EQ at a glance (spec 04 §4.3, D-UI-16): the response curve of the first Weft EQ
    insert. Clicking opens that EQ, or adds one in the first free insert slot. */
class EqThumbnail final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit EqThumbnail (Studio& studio);
    void setTrack (const Id& trackId);
    const Id& trackId() const noexcept { return track; }
    /** The first Weft EQ insert on the track, or empty. */
    Id eqId() const;
    /** The curve in dB at `points` frequencies spaced evenly on a log scale, 20 Hz to 20 kHz. */
    std::vector<double> curve (int points) const;
    /** Opens the EQ, or adds one first (a click does this). */
    void open();
    /** Repaints when the EQ's parameters changed since the last paint (the inspector's timer). */
    void poll();
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    static constexpr double lowHz = 20.0, highHz = 20000.0, rangeDb = 18.0;

private:
    std::vector<double> parameterValues() const;
    Studio& studio;
    Id track;
    std::vector<double> shownValues;
};

class Inspector final : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    explicit Inspector (Studio& studio);
    ~Inspector() override;
    void resized() override;
    void refresh();

    // Channel section: the selected track's strip and its output's (Master or a bus).
    std::unique_ptr<ChannelStrip> trackStrip, outputStrip;
    EqThumbnail eq;
    /** The scrolling area (the sections can be taller than the panel). */
    juce::Viewport viewport;

    // Region section.
    juce::Label regionName;
    juce::ToggleButton regionMute { "Mute" }, regionLoop { "Loop" };
    juce::Slider regionGain, fadeIn, fadeOut;
    /** Playback delay for any region (L-RGN-6). */
    juce::Slider regionDelay;
    juce::ComboBox fadeInCurve, fadeOutCurve;
    // MIDI region quantize (F-QNT-1).
    juce::ComboBox regionQuantize;
    juce::Slider quantizeStrength, quantizeSwing;
    juce::TextButton applyQuantize { "Apply Quantize" };
    // MIDI region playback parameters (L-RPR-2).
    juce::Slider regionTranspose, regionVelocity;
    // Note section (piano roll focus with notes selected).
    juce::Label notePitch, noteVelocity, noteLength, notePosition, noteChannel;
    // Track section.
    juce::Label trackName;
    juce::ComboBox trackColour, monitor;

private:
    /** Holds the controls; its height is what the sections need, at least the panel's. */
    struct Body final : juce::Component
    {
        explicit Body (Inspector& i) : owner (i) {}
        void paint (juce::Graphics& g) override { owner.paintBody (g); }
        Inspector& owner;
    } body { *this };
    void paintBody (juce::Graphics&);
    /** Lays the sections out at `width` and returns the height they need. */
    int layout (int width, int visibleHeight);
    void refreshStrips();
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    const model::Region* region() const;
    std::vector<commands::NoteRef> notes() const;
    /** Sets a field on every selected note, as one undo step. */
    void editNotes (std::string_view name, const std::function<void (model::Note&)>& change);
    void setRegionQuantize();

    Studio& studio;
    bool refreshing = false;
    int regionTop = 0, trackTop = 0, noteTop = 0, channelTop = -1;
};
} // namespace weft::ui
