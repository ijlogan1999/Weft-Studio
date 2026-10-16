#pragma once

// Live automation writing (W-8, F-AUT-5, F-AUT-6): mixer controls touched during playback
// override their lanes in the engine and, in Touch, Latch and Write, are recorded. When the
// transport stops the pass becomes one Session::writeAutomation call (one undo step).

#include "weft/commands/Session.h"
#include "weft/engine/transport/Engine.h"
#include "weft/model/Automation.h"

#include <optional>
#include <string>
#include <vector>

namespace weft::engine
{
class LiveAutomation
{
public:
    explicit LiveAutomation (Engine& e) : engine (e) {}

    /** Whether a control gesture on this target goes through automation instead of changing
        the static value: the mode follows a lane with points, or Touch/Latch/Write while
        the transport plays. Off always changes the static value. */
    bool isLive (const model::ProjectModel& model, const Id& trackId, const model::AutomationTarget& target) const;

    /** The control is held at `value` (normalized). Call on the first touch and on every move. */
    void touch (const model::ProjectModel& model, const Id& trackId, const model::AutomationTarget& target, double value);
    /** The control is let go: Read snaps back, Touch glides back over 200 ms, Latch and Write hold. */
    void release (const model::ProjectModel& model, const Id& trackId, const model::AutomationTarget& target);

    /** A finished write pass, ready for Session::writeAutomation. */
    struct Pass
    {
        std::vector<commands::Session::AutomationWrite> writes;
        std::vector<Id> toLatch; // tracks that were in Write (D-W8-1)
    };
    /** Call regularly (the UI timer) and right after the transport stops: records held values,
        starts Write passes when playback starts, and returns the pass once it stops. */
    std::optional<Pass> tick (const model::ProjectModel& model);

    /** Tracks being written now (presence: "writing automation on <track>"). */
    std::vector<Id> writingTracks() const;
    bool touching (const Id& trackId, const model::AutomationTarget& target) const;

private:
    struct Take
    {
        Id trackId;
        model::AutomationTarget target;
        std::string mode;
        automation::Recorder recorder;
        double value = 0;
        bool held = false;     // touched now, or Latch/Write holding the release value
        bool touched = false;  // the user's hand is on it
        Ticks from = 0, last = 0;
    };
    Take* find (const Id& trackId, const model::AutomationTarget& target);
    const Take* find (const Id& trackId, const model::AutomationTarget& target) const;
    bool playing() const noexcept;
    /** Records the take's value now, or at a tick (the play start, taken as time 0 for thinning). */
    void record (Take& take, double value, std::optional<double> atTick = std::nullopt);
    void close (Take& take);

    Engine& engine;
    bool wasPlaying = false;
    std::vector<Take> takes;            // being recorded (or touched while not writing)
    std::vector<commands::Session::AutomationWrite> done; // closed Touch segments
    std::vector<Id> writeTracks;        // tracks in Write when this pass started
};
} // namespace weft::engine
