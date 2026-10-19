#pragma once

// G · Piano roll (spec 04 §7.1, F-PRL): keyboard, ruler, note grid and a controller lane for
// the MIDI regions of one track. Pointer handling goes through pointerDown/Drag/Up so tests
// can drive it; edits are previewed while dragging and committed as one undo step on release.

#include "Studio.h"
#include "Widgets.h"

namespace weft::ui
{
enum class NoteTool
{
    pointer,
    pencil,
    eraser,
    velocity,
    scissors
};

/** What the controller lane under the grid shows (F-PRL-12). */
struct LaneKind
{
    enum class Type
    {
        velocity,
        cc,
        pitchBend,
        pressure // channel aftertouch
    };
    Type type = Type::velocity;
    int number = 0; // CC number
    friend bool operator== (const LaneKind&, const LaneKind&) = default;
    int minValue() const noexcept { return type == Type::pitchBend ? -8192 : type == Type::velocity ? 1 : 0; }
    int maxValue() const noexcept { return type == Type::pitchBend ? 8191 : 127; }
};

/** Snap and quantize grid values (W-5): name and ticks; 0 = off. */
struct GridValue
{
    const char* name;
    Ticks ticks;
};
const std::vector<GridValue>& gridValues();

class PianoRoll final : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    explicit PianoRoll (Studio& studio);
    ~PianoRoll() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void visibilityChanged() override;

    // Toolbar.
    juce::ComboBox toolBox, snapBox, quantizeBox, laneBox;
    juce::Slider strength, swing;
    juce::TextButton quantizeButton { "Q" };
    juce::ToggleButton auditionToggle { "Audition" }, followToggle { "Follow" };
    NoteTool tool = NoteTool::pointer;
    LaneKind lane;

    // View.
    double pixelsPerTick = 0.05;
    double originTick = 0;
    int rowHeight = 10;
    int topPitch = 84; // the pitch in the top row
    bool laneShown = true;
    static constexpr int toolbarHeight = size::panelHeader, keyboardWidth = 56, rulerHeight = 24, laneHeight = 88;

    juce::Rectangle<int> keyboardArea() const;
    juce::Rectangle<int> rulerArea() const;
    juce::Rectangle<int> gridArea() const;
    juce::Rectangle<int> laneArea() const;
    double tickToX (double t) const { return gridArea().getX() + (t - originTick) * pixelsPerTick; }
    double xToTick (double x) const { return originTick + (x - gridArea().getX()) / pixelsPerTick; }
    float pitchToY (int pitch) const { return static_cast<float> (gridArea().getY() + (topPitch - pitch) * rowHeight); }
    int yToPitch (float y) const;
    int laneValueAt (float y) const;
    /** The velocity a pencil line in the velocity lane gives a note starting at x. */
    int lineValueAt (float x) const;
    float laneY (int value) const;
    /** Shows the shown regions whole and centres their notes. */
    void fitRegions();
    void setTool (NoteTool t);

    /** A note as drawn: where it plays (region quantize applied) and where it is stored. */
    struct Shown
    {
        commands::NoteRef ref;
        Ticks start = 0, stored = 0, length = 0;
        int pitch = 60, velocity = 100, channel = 1;
        bool muted = false;
        juce::Rectangle<float> bounds;
    };
    std::vector<Shown> shownNotes() const;
    std::optional<Shown> noteAt (juce::Point<float> p) const;
    juce::Rectangle<float> noteBounds (const Shown& n) const;

    /** Pointer actions (tests drive these). */
    void pointerDown (juce::Point<float> p, juce::ModifierKeys mods = {});
    void pointerDrag (juce::Point<float> p, juce::ModifierKeys mods = {});
    void pointerUp();
    /** Clicking a piano key: plays it and selects every note of that pitch (Shift adds). */
    void keyClicked (int pitch, bool add);

private:
    enum class Drag
    {
        none,
        move,
        copy,
        resizeLeft,
        resizeRight,
        rubber,
        velocity,
        laneVelocity,
        laneNew,
        laneMove,
        laneLine,
        ruler
    };
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void refreshToolbar();
    Ticks snapTick (double tick, bool floor = false) const;
    /** The shown region a click at this tick draws into: the one covering it, else the last one starting before it. */
    const model::Region* regionFor (Ticks tick) const;
    void gridDown (juce::Point<float> p, juce::ModifierKeys mods);
    void laneDown (juce::Point<float> p, juce::ModifierKeys mods);
    void drawNoteAt (juce::Point<float> p);
    void splitNoteAt (const Shown& n, juce::Point<float> p);
    struct LanePoint
    {
        Id regionId, eventId;
        Ticks tick = 0;
        int value = 0;
    };
    std::vector<LanePoint> lanePoints() const;
    std::optional<LanePoint> lanePointAt (juce::Point<float> p) const;
    void paintKeyboard (juce::Graphics&);
    void paintRuler (juce::Graphics&);
    void paintGrid (juce::Graphics&);
    void paintNotes (juce::Graphics&);
    void paintLane (juce::Graphics&);
    void paintCollaborators (juce::Graphics&);
    juce::Colour trackColour() const;
    Id shownTrack() const;

    Studio& studio;
    std::vector<Id> regions; // shown, by start
    std::string shownKey;    // which regions the view was fitted to
    bool needsFit = true;
    bool refreshing = false;

    Drag drag = Drag::none;
    juce::Point<float> downAt, dragAt;
    std::optional<Shown> anchor;
    Ticks deltaTicks = 0;
    int deltaPitch = 0;
    commands::NoteSpan span;  // resize preview
    int velocityDelta = 0;
    int laneValue = 0;
    Ticks laneTick = 0;
    std::optional<LanePoint> lanePoint;
    std::set<commands::NoteRef> rubberBase;
};
} // namespace weft::ui
