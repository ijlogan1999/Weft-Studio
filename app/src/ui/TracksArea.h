#pragma once

// D · Global tracks and E · Tracks area (spec 04 §5, §6): the ruler with the cycle strip,
// the marker lane, track headers and the timeline where regions are edited.

#include "Studio.h"
#include "Widgets.h"

#include "weft/arrange/RegionDrag.h"

namespace weft::ui
{
/** Ruler, cycle strip and the global tracks (marker, tempo and signature lanes), sharing the timeline's horizontal mapping. */
class Ruler final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit Ruler (Studio& studio);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    static constexpr int cycleStrip = 8, rulerHeight = 32, markerLane = 24, tempoLane = 40, signatureLane = 20;
    static constexpr int tempoTop = rulerHeight + markerLane, signatureTop = tempoTop + tempoLane;
    int preferredHeight() const { return rulerHeight + (studio.layout.globalTracks ? markerLane + tempoLane + signatureLane : 0); }

    /** Bar numbers (1-based) and their x, thinned so labels don't collide (AT-ARR-1). */
    static std::vector<std::pair<int, double>> barLabels (const model::ProjectModel& m, const arrange::TimelineView& view, double width);
    /** Marker under x in the marker lane. */
    const model::Marker* markerAt (double x) const;
    /** Comment pins in the marker lane (04 §9.3): every thread, at its position. */
    juce::Rectangle<float> pinBounds (const model::Comment& thread) const;
    const model::Comment* commentAt (juce::Point<float> p) const;

    /** Tempo lane (W-11): a step graph over a range that fits the map's tempos (frozen while a point is dragged). */
    std::pair<double, double> tempoRange() const;
    float tempoY (double bpm) const;
    double bpmAtY (float y) const;
    /** The tempo change drawn at x, if any. */
    const TempoPoint* tempoPointAt (double x) const;
    /** Signature lane: the change whose label is at x, and the bar whose start is nearest x (0-based). */
    const SignaturePoint* signatureAt (double x) const;
    int nearestBar (double x) const;
    /** Sets the time signature at a 0-based bar from text such as "3/4" (W-11 step 4). */
    bool setSignatureFromText (int bar, const juce::String& text);

    /** Pointer actions, also used by tests: click in the ruler moves the playhead; in the cycle strip drags the cycle;
        on a marker jumps to it (AT-ARR-21). With Autopunch on, dragging an end of the punch range in the strip moves it and
        Shift+drag sets a new one (W-1d). In the tempo lane Ctrl+click (or the pencil) adds a change, dragging a point
        changes its BPM (vertical) and position (horizontal), right-click deletes; right-click deletes a signature change. */
    void pointerDown (juce::Point<float> p, juce::ModifierKeys mods);
    void pointerDrag (juce::Point<float> p);
    void pointerUp();

private:
    Studio& studio;
    enum class Drag { none, cycle, playhead, marker, tempo, punchStart, punchEnd, punch } drag = Drag::none;
    Ticks dragFrom = 0;
    Id draggedMarker, draggedTempo;
    double dragBpm = 0;
    juce::Point<float> dragStart;
    std::pair<double, double> dragRange;
};

class TrackHeader final : public juce::Component, public juce::SettableTooltipClient
{
public:
    TrackHeader (Studio& studio, const Id& trackId, int number);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void refresh();

    const Id trackId;
    juce::Label name;
    juce::TextButton mute { "M" }, solo { "S" }, arm { "R" };
    /** The lock icon (04 §6.1): shown while the track is locked; click to unlock your own lock. */
    juce::ShapeButton lock { "Lock", {}, {}, {} };
    /** Right-click menu: Rename, Duplicate, Color, Move, Delete, Lock/Unlock, Break Lock (the Owner), Add Comment. */
    juce::PopupMenu menu() const;
    void pickMenu (int item);
    enum MenuItem
    {
        itemLock = 1,
        itemBreakLock,
        itemComment,
        itemRename,
        itemDuplicate,
        itemDuplicateWithRegions,
        itemMoveUp,
        itemMoveDown,
        itemDelete,
        itemColour = 100 // + 1 .. paletteSize
    };
    /** The header the pointer is over while this one is dragged to reorder; nullptr when not dragging. */
    const TrackHeader* dropTarget() const noexcept { return reorderOver; }
    /** Where a dragged header would land on this one: -1 above it, 1 below it, 0 not a drop target. */
    int dropMark = 0;
    // Automation view (W-8): the mode, and per lane the parameter selector and "+".
    juce::ComboBox mode;
    std::vector<std::unique_ptr<juce::ComboBox>> parameters;
    std::vector<std::unique_ptr<juce::TextButton>> addLane;

    /** Collaborators whose focus is this track (their badges, AT-COL-12). */
    std::vector<const collab::RemotePresence*> badges() const;

private:
    void rebuildAutomation();
    void lockClicked();
    Studio& studio;
    std::map<juce::Component*, juce::String> tipsBeforeLock;
    std::vector<std::string> shownChoices; // what the parameter selectors were built from
    int number;
    int resizeFrom = -1;
    bool reordering = false;
    TrackHeader* reorderOver = nullptr;
    TrackHeader* headerAt (juce::Point<int> inParent) const;
    void showDropTarget (TrackHeader* over);
};

/** The lanes and regions. Pointer handling goes through pointerDown/Drag/Up so tests can drive it. */
class Timeline final : public juce::Component, public juce::FileDragAndDropTarget, public juce::DragAndDropTarget
{
public:
    explicit Timeline (Studio& studio);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    /** A thread's pin on its track's lane, when it's pinned to a track that's shown (04 §9.3). */
    std::optional<juce::Rectangle<float>> pinBounds (const model::Comment& thread) const;
    const model::Comment* commentPinAt (juce::Point<float> p) const;
    /** Right-click on the timeline or a region: Add Comment (04 §9.3). */
    juce::PopupMenu contextMenu (juce::Point<float> p) const;
    void pickContext (int item, juce::Point<float> p);
    /** Where a collaborator's pointer is drawn (its lane's centre), when it's on a track (AT-COL-11). */
    std::optional<juce::Point<float>> collaboratorPointer (const std::string& clientId) const;

    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    bool isInterestedInDragSource (const SourceDetails&) override { return true; }
    void itemDropped (const SourceDetails& details) override;

    // Geometry ---------------------------------------------------------------------
    int scrollY = 0;
    int rowAt (float y) const;               // -1 above, rows() or more below the last track
    int rows() const;
    /** A track's whole row: its lane, then its automation lanes in automation view. */
    juce::Rectangle<float> rowBounds (int row) const;
    /** The track lane where its regions are (the top of its row). */
    juce::Rectangle<float> laneBounds (int row) const;
    /** An automation lane under a track (automation view). */
    juce::Rectangle<float> automationBounds (int row, std::size_t index) const;
    struct AutomationHit
    {
        Id trackId;
        model::AutomationTarget target;
        juce::Rectangle<float> box;
    };
    std::optional<AutomationHit> automationLaneAt (juce::Point<float> p) const;
    /** Where a point of a lane is drawn, and the value for a height in the lane. */
    juce::Point<float> pointPosition (const AutomationHit& lane, const model::AutoPoint& point) const;
    static double valueAtY (juce::Rectangle<float> box, float y);
    juce::Rectangle<float> regionBounds (const model::Region& r) const;
    const model::Region* regionAt (juce::Point<float> p) const;
    int contentHeight() const;

    enum class Zone
    {
        none,
        move,
        trimStart,
        trimEnd,
        fadeIn,
        fadeOut,
        loop,
        takes
    };
    Zone zoneAt (const model::Region& r, juce::Point<float> p) const;

    // Pointer (tests drive these directly) ------------------------------------------
    void pointerDown (juce::Point<float> p, juce::ModifierKeys mods, int clicks = 1);
    void pointerDrag (juce::Point<float> p, juce::ModifierKeys mods);
    void pointerUp (juce::Point<float> p, juce::ModifierKeys mods);
    /** Esc during a drag (W-3b step 6). */
    bool cancelDrag();
    void wheel (juce::Point<float> p, juce::ModifierKeys mods, float deltaY);
    /** Drop files at a point (AT-ARR-25, AT-LIB-1). */
    engine::ImportOutcome dropFiles (const std::vector<std::filesystem::path>& files, juce::Point<float> p);
    /** Makes the next take the active one (AT-REC-13: clicking the take strip). */
    void nextTake (const Id& regionId);

    std::function<void()> onScroll;

private:
    void paintCollaborators (juce::Graphics&);
    void paintAutomation (juce::Graphics&, const AutomationHit& lane);
    std::vector<model::AutoPoint> previewPoints (const AutomationHit& lane) const;
    void automationDown (const AutomationHit& hit, juce::Point<float> p, juce::ModifierKeys mods);
    void automationDrag (juce::Point<float> p, juce::ModifierKeys mods);
    void automationUp (juce::Point<float> p, juce::ModifierKeys mods);
    /** The lane point within reach of p, if any. */
    std::optional<model::AutoPoint> pointNear (const AutomationHit& lane, juce::Point<float> p) const;
    /** The move a point drag makes now: ticks (snapped at the grabbed point) and value. */
    std::pair<Ticks, double> pointDelta (juce::Point<float> p, juce::ModifierKeys mods) const;
    enum class AutoDrag { none, add, band, move, bend } autoDrag = AutoDrag::none;
    AutomationHit autoLane;
    model::AutoPoint grabbed;      // move: the point under the mouse; bend: the segment's first point
    double bendCurve = 0, bendSign = 1;
    bool toggleOnUp = false;
    juce::ModifierKeys dragMods;

    void paintGrid (juce::Graphics&, juce::Rectangle<float> area);
    void paintRegion (juce::Graphics&, const model::Region& r, juce::Rectangle<float> box, bool ghost);
    void startRegionDrag (const model::Region& r, Zone zone, juce::Point<float> p, juce::ModifierKeys mods);
    Ticks tickAt (float x) const { return static_cast<Ticks> (std::llround (studio.view.xToTick (x))); }

    Studio& studio;
    enum class Drag { none, move, trimStart, trimEnd, fadeIn, fadeOut, loop, rubberBand } drag = Drag::none;
    std::unique_ptr<arrange::RegionDrag> regionDrag;
    Id dragRegion;
    juce::Point<float> downAt, lastAt;
    bool gestureOpen = false;
};

class TracksArea final : public juce::Component, private juce::ChangeListener, private juce::ScrollBar::Listener, private juce::Timer
{
public:
    explicit TracksArea (Studio& studio);
    ~TracksArea() override;
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    Ruler& ruler() noexcept { return rulerComponent; }
    Timeline& timeline() noexcept { return lanes; }
    const std::vector<std::unique_ptr<TrackHeader>>& headers() const noexcept { return trackHeaders; }

    static constexpr int headerWidth = 208;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void scrollBarMoved (juce::ScrollBar*, double) override;
    void timerCallback() override;
    void rebuildHeaders();
    void layoutHeaders();

    Studio& studio;
    Ruler rulerComponent;
    Timeline lanes;
    bool hadCollaborators = false;
    juce::Component headerColumn;
    std::vector<std::unique_ptr<TrackHeader>> trackHeaders;
    std::vector<Id> shownTracks;
    juce::ScrollBar vertical { true }, horizontal { false };
};
} // namespace weft::ui
