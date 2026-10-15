#pragma once

// Snap grid and timeline zoom math for the arrange view (F-ARR-1, F-ARR-2, F-ARR-21).
// Pure functions of the signature map, so the UI and the tests share them.

#include "weft/time/SignatureMap.h"
#include "weft/time/Time.h"

#include <string>
#include <vector>

namespace weft::arrange
{
/** The grid value chosen in the control bar's Snap dropdown. */
struct Snap
{
    enum class Kind
    {
        off,
        smart, // follows zoom (D-ARR-5)
        bar,
        beat,  // the meter's denominator note
        ticks  // a fixed division: 1/8 = 480, 1/16 = 240, 1/8T = 320 ...
    };
    Kind kind = Kind::smart;
    Ticks division = 240; // for Kind::ticks

    static Snap off() { return { Kind::off, 0 }; }
    static Snap smart() { return { Kind::smart, 0 }; }
    static Snap bar() { return { Kind::bar, 0 }; }
    static Snap beat() { return { Kind::beat, 0 }; }
    static Snap ticks (Ticks t) { return { Kind::ticks, t }; }
    friend bool operator== (const Snap&, const Snap&) = default;
};

/** The dropdown's entries in order, with their labels ("Bar", "Beat", "1/8", "1/16T", "Off"...). */
struct SnapChoice
{
    std::string label;
    Snap snap;
};
const std::vector<SnapChoice>& snapChoices();
std::string snapLabel (const Snap& snap);

/** Smart snap never draws grid lines closer than this (D-ARR-5). */
inline constexpr double smartGridMinPixels = 16.0;

/**
    The grid spacing in ticks at a position, or 0 for "snap off" and for the bar grid
    (bars aren't evenly spaced across meter changes, so the bar grid snaps to bar starts).
    pixelsPerTick is only used by smart snap.
*/
Ticks gridTicks (const Snap& snap, const SignatureMap& sigs, Ticks at, double pixelsPerTick);

/** True when the grid at `at` is whole bars. */
bool gridIsBars (const Snap& snap, const SignatureMap& sigs, Ticks at, double pixelsPerTick);

/**
    The nearest grid point to t (ties go later). Grid points restart at every bar line, so
    1/8T in 7/8 still lands on the bar line. bypass (Alt held) or snap off returns t.
*/
Ticks snapTick (Ticks t, const Snap& snap, const SignatureMap& sigs, double pixelsPerTick = 0.0, bool bypass = false);

/** One grid step from `from` in direction +1 or -1 (nudge, F-ARR-23). Snap off steps one tick. */
Ticks gridStep (Ticks from, int direction, const Snap& snap, const SignatureMap& sigs, double pixelsPerTick = 0.0);

/**
    The horizontal mapping of the tracks area: x = (tick - originTick) * pixelsPerTick.
    Zoom keeps the tick under the anchor pixel fixed (Ctrl+wheel at pointer, AT-ARR-1).
*/
struct TimelineView
{
    double originTick = 0.0;
    double pixelsPerTick = 40.0 / 3840.0; // one 4/4 bar = 40 px

    static constexpr double minPixelsPerTick = 1.0 / 3840.0 / 8.0; // 8 bars per pixel
    static constexpr double maxPixelsPerTick = 4.0;                 // a few samples per pixel at 120 BPM

    double tickToX (double tick) const noexcept { return (tick - originTick) * pixelsPerTick; }
    double xToTick (double x) const noexcept { return originTick + x / pixelsPerTick; }

    /** Multiplies the zoom by factor around the pixel anchorX. originTick never goes below 0. */
    void zoomAround (double factor, double anchorX) noexcept;
    /** Z (F-ARR-21): shows [start, end) using `fill` of the width, centred. */
    void fit (double startTick, double endTick, double width, double fill = 0.9) noexcept;
};
} // namespace weft::arrange
