#pragma once

// Playhead position helpers for the transport (F-TRN-4, 5, 11): bars/beats
// display, time display, parsing typed positions, and bar stepping.

#include "weft/time/SignatureMap.h"
#include "weft/time/TempoMap.h"

#include <optional>
#include <string>
#include <string_view>

namespace weft
{
/** Display grid: one division is a 1/16 note (or one beat, if the beat is shorter). */
inline constexpr Ticks divisionTicks = ticksPerQuarter / 4;

struct BarsBeats
{
    int bar = 1;      // 1-based, as shown
    int beat = 1;     // 1-based
    int division = 1; // 1-based
    int tick = 0;     // 0-based ticks within the division
};

BarsBeats toBarsBeats (Ticks t, const SignatureMap& signatures) noexcept;
Ticks fromBarsBeats (const BarsBeats& position, const SignatureMap& signatures) noexcept;

/** "bar.beat.division.tick", e.g. "2.1.2.0" (AT-TRN-15). */
std::string formatBarsBeats (Ticks t, const SignatureMap& signatures);
/** "h:mm:ss.mmm", e.g. "0:00:02.125". Negative times (count-in) get a leading '-'. */
std::string formatTime (double seconds);

/** Parses "5 3", "5.3", "5 3 2 10" (bar, beat, division, tick; missing parts are 1/1/0). */
std::optional<Ticks> parseBarsBeats (std::string_view text, const SignatureMap& signatures);
/** Parses "30", "0:30", "1:02:03.5" (seconds, m:ss, h:mm:ss[.fraction]) into seconds. */
std::optional<double> parseTime (std::string_view text);

/** Period: the start of the next bar (F-TRN-5). */
Ticks nextBar (Ticks t, const SignatureMap& signatures) noexcept;
/** Comma: the start of the current bar, or of the previous bar when already on a bar start; never before 0. */
Ticks previousBar (Ticks t, const SignatureMap& signatures) noexcept;
} // namespace weft
