#pragma once

// D · Global tracks and E · Tracks area (spec 04 §5, §6): the ruler with the cycle strip,
// the marker lane, track headers and the timeline where regions are edited.

#include "Studio.h"
#include "Widgets.h"

#include "weft/arrange/RegionDrag.h"

namespace weft::ui
{
/** Ruler, cycle strip and marker lane, sharing the timeline's horizontal mapping. */
class Ruler final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit Ruler (Studio& studio);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    static constexpr int cycleStrip = 10, rulerHeight = 34, markerLane = 22;
    int preferredHeight() const { return rulerHeight + (studio.layout.globalTracks ? markerLane : 0); }

    /** Bar numbers (1-based) and their x, thinned so labels don't collide (AT-ARR-1). */
    static std::vector<std::pair<int, double>> barLabels (const model::ProjectModel& m, const arrange::TimelineView& view, double width);
    /** Marker under x in the marker lane. */
    const model::Marker* markerAt (double x) const;

    /** Pointer actions, also used by tests: click in the ruler moves the playhead; in the cycle strip drags the cycle;
        on a marker jumps to it (AT-ARR-21). */
    void pointerDown (juce::Point<float> p, juce::ModifierKeys mods);
    void pointerDrag (juce::Point<float> p);
    void pointerUp();

private:
    Studio& studio;
    enum class Drag { none, cycle, playhead, marker } drag = Drag::none;
    Ticks dragFrom = 0;
    Id draggedMarker;
};

class TrackHeader final : public juce::Component
{
public:
    TrackHeader (Studio& studio, const Id& trackId, int number);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void refresh();

    const Id trackId;
    juce::Label name;
    juce::TextButton mute { "M" }, solo { "S" }, arm { "R" };

private:
    Studio& studio;
    int number;
    int resizeFrom = -1;
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
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    bool isInterestedInDragSource (const SourceDetails&) override { return true; }
    void itemDropped (const SourceDetails& details) override;

    // Geometry ---------------------------------------------------------------------
    int scrollY = 0;
    int rowAt (float y) const;               // -1 above, rows() or more below the last track
    int rows() const;
    juce::Rectangle<float> laneBounds (int row) const;
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
    void paintGrid (juce::Graphics&, juce::Rectangle<float> area);
    void paintRegion (juce::Graphics&, const model::Region& r, juce::Rectangle<float> box, bool ghost);
    void startRegionDrag (const model::Region& r, Zone zone, juce::Point<float> p, juce::ModifierKeys mods);
    Ticks tickAt (float x) const { return static_cast<Ticks> (std::llround (studio.view.xToTick (x))); }

    Studio& studio;
    enum class Drag { none, move, trimStart, trimEnd, fadeIn, fadeOut, loop, rubberBand } drag = Drag::none;
    std::unique_ptr<arrange::RegionDrag> regionDrag;
    Id dragRegion;
    juce::Point<float> downAt, lastAt;
    float rowHeightAtDown = 0;
    bool gestureOpen = false;
};

class TracksArea final : public juce::Component, private juce::ChangeListener, private juce::ScrollBar::Listener, private juce::Timer
{
public:
    explicit TracksArea (Studio& studio);
    ~TracksArea() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    Ruler& ruler() noexcept { return rulerComponent; }
    Timeline& timeline() noexcept { return lanes; }
    const std::vector<std::unique_ptr<TrackHeader>>& headers() const noexcept { return trackHeaders; }

    static constexpr int headerWidth = 210;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void scrollBarMoved (juce::ScrollBar*, double) override;
    void timerCallback() override;
    void rebuildHeaders();
    void layoutHeaders();

    Studio& studio;
    Ruler rulerComponent;
    Timeline lanes;
    juce::Component headerColumn;
    std::vector<std::unique_ptr<TrackHeader>> trackHeaders;
    std::vector<Id> shownTracks;
    juce::ScrollBar vertical { true }, horizontal { false };
};
} // namespace weft::ui
