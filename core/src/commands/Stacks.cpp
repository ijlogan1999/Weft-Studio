// Track stacks (D-W6-17): a main track (a bus) over member tracks. A folder stack's main scales and mutes its
// members like a VCA and carries no sound; a summing stack's main is the bus its members play into.

#include "weft/commands/Session.h"

#include <algorithm>

namespace weft::commands
{
using model::TrackKind;
namespace keys = model::keys;

namespace
{
Path trackPath (const Id& id) { return doc::path ({ keys::tracks, id }); }
EditResult wrote (bool success) { return success ? EditResult {} : EditResult::fail ("The document rejected the edit."); }
} // namespace

std::optional<Id> Session::createStack (const std::vector<Id>& trackIds, bool summing)
{
    // Members in track order; the master, stack mains and tracks already in a stack can't join.
    std::vector<Id> members;
    for (const auto& id : projection.trackOrder)
        if (std::find (trackIds.begin(), trackIds.end(), id) != trackIds.end())
            if (const auto* t = projection.track (id);
                t != nullptr && t->kind != TrackKind::master && ! t->isStackMain() && projection.stackMain (id) == nullptr)
                members.push_back (id);
    if (members.empty())
    {
        error = "Choose tracks that aren't in a stack.";
        return std::nullopt;
    }
    int count = 1;
    for (const auto& [id, t] : projection.tracks)
        count += t->isStackMain() ? 1 : 0;
    const auto& stored = projection.storedTrackOrder;
    const auto first = static_cast<std::size_t> (std::find (stored.begin(), stored.end(), members.front()) - stored.begin());
    const auto outputOfFirst = projection.track (members.front())->output.busId;

    beginGesture (summing ? "Create Summing Stack" : "Create Folder Stack");
    const auto main = createTrack (TrackKind::bus, "Stack " + std::to_string (count), 2, first);
    if (! main)
    {
        const auto why = error;
        cancelGesture();
        error = why;
        return std::nullopt;
    }
    // The members go right under the main track, in their order.
    for (std::size_t i = 0; i < members.size(); ++i)
    {
        const auto& shown = projection.storedTrackOrder;
        const auto at = static_cast<std::size_t> (std::find (shown.begin(), shown.end(), *main) - shown.begin()) + 1 + i;
        if (! moveTrack (members[i], at))
        {
            cancelGesture();
            return std::nullopt;
        }
    }
    const auto result = run ("Stack", [&] (TxnWriter& w) {
        bool good = w.put (trackPath (*main) / "stack", Value (summing ? "summing" : "folder")) && w.put (trackPath (*main) / "stackOpen", Value (true));
        for (const auto& id : members)
            good = good && w.put (trackPath (id) / "stackId", Value (*main));
        return wrote (good);
    });
    if (! result)
    {
        cancelGesture();
        error = result.error;
        return std::nullopt;
    }
    if (summing)
    {
        // The main plays where the first member did; every member plays into the main.
        if (! outputOfFirst.empty() && ! setTrackOutput (*main, outputOfFirst))
        {
            cancelGesture();
            return std::nullopt;
        }
        for (const auto& id : members)
            if (! setTrackOutput (id, *main))
            {
                cancelGesture();
                return std::nullopt;
            }
    }
    endGesture();
    return main;
}

EditResult Session::flattenStack (const Id& mainId)
{
    const auto* main = projection.track (mainId);
    if (main == nullptr || ! main->isStackMain())
        return reject ("That isn't a track stack.");
    const auto members = projection.stackMembers (mainId);
    const auto out = main->output;
    beginGesture ("Flatten Stack");
    // Members fed into a summing stack's main play where the main did.
    const auto result = run ("Flatten Stack", [&] (TxnWriter& w) {
        bool good = true;
        for (const auto& id : members)
        {
            good = good && w.put (trackPath (id) / "stackId", Value());
            if (projection.track (id)->output.busId == mainId)
                good = good && w.put (trackPath (id) / "output", out.toValue());
        }
        return wrote (good);
    });
    if (! result || ! deleteTrack (mainId))
    {
        const auto why = result ? error : result.error;
        cancelGesture();
        return EditResult::fail (why);
    }
    endGesture();
    return {};
}

EditResult Session::setStackOpen (const Id& mainId, bool open)
{
    const auto* main = projection.track (mainId);
    if (main == nullptr || ! main->isStackMain())
        return reject ("That isn't a track stack.");
    if (main->stackOpen == open)
        return reject (open ? "That stack is already open." : "That stack is already closed.");
    return run (open ? "Open Stack" : "Close Stack", [&] (TxnWriter& w) { return wrote (w.put (trackPath (mainId) / "stackOpen", Value (open))); });
}
} // namespace weft::commands
