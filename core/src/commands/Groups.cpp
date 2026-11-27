// Groups (D-W6-15) and VCA faders (D-W6-16): the commands that make and change them. The linking
// itself is in each setter (volume, pan, mute, and so on), which asks linkedTracks.

#include "weft/commands/Session.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace weft::commands
{
using model::TrackKind;
namespace keys = model::keys;

namespace
{
Path trackPath (const Id& id) { return doc::path ({ keys::tracks, id }); }
Path groupPath (const Id& id) { return doc::path ({ keys::groups, id }); }
Path vcaPath (const Id& id) { return doc::path ({ keys::vcas, id }); }
EditResult wrote (bool success) { return success ? EditResult {} : EditResult::fail ("The document rejected the edit."); }

/** Older projects have no collection yet. */
bool ensureCollection (TxnWriter& w, const char* key)
{
    return w.doc().get (doc::path ({ key })) || w.put (doc::path ({ key }), Value::map());
}
} // namespace

std::vector<Id> Session::linkedTracks (const Id& trackId, bool model::Group::* link) const
{
    if (groupOverridden())
        return {};
    auto out = projection.linkedTracks (trackId, link);
    out.erase (std::remove_if (out.begin(), out.end(), [&] (const Id& id) { return lockedByOther (id); }), out.end());
    return out;
}

std::vector<Id> Session::linkedSendTracks (const Id& trackId, int slot) const
{
    if (groupOverridden())
        return {};
    auto out = projection.linkedSendTracks (trackId, slot);
    out.erase (std::remove_if (out.begin(), out.end(), [&] (const Id& id) { return lockedByOther (id); }), out.end());
    return out;
}

std::optional<Id> Session::createGroup (const std::vector<Id>& trackIds)
{
    std::set<int> taken;
    for (const auto& [id, g] : projection.groups)
        taken.insert (g->number);
    int number = 1;
    while (taken.contains (number))
        ++number;
    if (number > model::Group::maxNumber)
    {
        error = "A project has at most 32 groups.";
        return std::nullopt;
    }
    std::vector<Id> members;
    for (const auto& id : trackIds)
        if (const auto* t = projection.track (id); t != nullptr && t->kind != TrackKind::master && projection.tracks.contains (id))
            members.push_back (id);
    model::Group g;
    g.id = newId();
    g.number = number;
    g.name = "Group " + std::to_string (number);
    return runCreate ("Create Group", g.id, [&] (TxnWriter& w) {
        bool good = ensureCollection (w, keys::groups) && w.put (groupPath (g.id), g.toValue());
        for (const auto& id : members)
            good = good && w.put (trackPath (id) / "groupId", Value (g.id));
        return wrote (good);
    });
}

EditResult Session::setGroup (const model::Group& group)
{
    const auto it = projection.groups.find (group.id);
    if (it == projection.groups.end())
        return reject ("That group no longer exists.");
    if (group.name.empty())
        return reject ("A group needs a name.");
    auto g = group;
    g.number = it->second->number;
    const auto before = it->second->toValue();
    const auto now = g.toValue();
    return run ("Group Settings", [&] (TxnWriter& w) {
        // Field by field, so two people changing different settings both keep theirs.
        bool good = true;
        for (const auto* key : { "name", "enabled", "hidden" })
            if (! (now[key] == before[key]))
                good = good && w.put (groupPath (g.id) / key, now[key]);
        const auto links = now["links"];
        const auto old = before["links"];
        if (! w.doc().get (groupPath (g.id) / "links"))
            good = good && w.put (groupPath (g.id) / "links", links);
        else
            for (const auto& [key, value] : links.asMap())
                if (! (value == old[key]))
                    good = good && w.put (groupPath (g.id) / "links" / key, value);
        return wrote (good);
    });
}

EditResult Session::deleteGroup (const Id& groupId)
{
    if (! projection.groups.contains (groupId))
        return reject ("That group no longer exists.");
    const auto members = projection.groupMembers (groupId);
    return run ("Delete Group", [&] (TxnWriter& w) {
        bool good = true;
        for (const auto& id : members)
            good = good && w.put (trackPath (id) / "groupId", Value());
        return wrote (good && w.remove (groupPath (groupId)));
    });
}

EditResult Session::setTrackGroup (const Id& trackId, const Id& groupId)
{
    const auto* t = projection.track (trackId);
    if (t == nullptr || ! projection.tracks.contains (trackId))
        return reject ("That track no longer exists.");
    if (t->kind == TrackKind::master)
        return reject ("The master isn't in a group.");
    if (! groupId.empty() && ! projection.groups.contains (groupId))
        return reject ("That group no longer exists.");
    if (t->groupId == groupId)
        return reject (groupId.empty() ? "That track isn't in a group." : "That track is already in that group.");
    return run ("Set Group", [&] (TxnWriter& w) { return wrote (w.put (trackPath (trackId) / "groupId", groupId.empty() ? Value() : Value (groupId))); });
}

EditResult Session::setGroupsActive (bool active)
{
    if (projection.info.groupsActive == active)
        return reject (active ? "Groups are already on." : "Groups are already off.");
    return run (active ? "Enable Groups" : "Disable Groups", [&] (TxnWriter& w) { return wrote (w.put (doc::path ({ "groupsActive" }), Value (active))); });
}

std::optional<Id> Session::createVca (const std::string& name)
{
    model::Vca v;
    v.id = newId();
    v.name = name;
    for (int n = static_cast<int> (projection.vcas.size()) + 1; v.name.empty(); ++n)
    {
        const auto candidate = "VCA " + std::to_string (n);
        if (std::none_of (projection.vcas.begin(), projection.vcas.end(), [&] (const auto& e) { return e.second->name == candidate; }))
            v.name = candidate;
    }
    return runCreate ("Create VCA", v.id, [&] (TxnWriter& w) { return wrote (ensureCollection (w, keys::vcas) && w.put (vcaPath (v.id), v.toValue())); });
}

EditResult Session::renameVca (const Id& vcaId, const std::string& name)
{
    if (! projection.vcas.contains (vcaId))
        return reject ("That VCA no longer exists.");
    if (name.empty())
        return reject ("A VCA needs a name.");
    return run ("Rename VCA", [&] (TxnWriter& w) { return wrote (w.put (vcaPath (vcaId) / "name", Value (name))); });
}

EditResult Session::setVcaVolume (const Id& vcaId, double volumeDb)
{
    if (! projection.vcas.contains (vcaId))
        return reject ("That VCA no longer exists.");
    if (std::isnan (volumeDb) || volumeDb > 6.0 + 1e-9)
        return reject ("Volume is -inf to +6 dB.");
    return run ("VCA Volume", [&] (TxnWriter& w) { return wrote (w.put (vcaPath (vcaId) / "volumeDb", Value (std::clamp (volumeDb, minusInfinityDb, 6.0)))); });
}

EditResult Session::setVcaMute (const Id& vcaId, bool mute)
{
    if (! projection.vcas.contains (vcaId))
        return reject ("That VCA no longer exists.");
    return run (mute ? "Mute VCA" : "Unmute VCA", [&] (TxnWriter& w) { return wrote (w.put (vcaPath (vcaId) / "mute", Value (mute))); });
}

EditResult Session::deleteVca (const Id& vcaId)
{
    if (! projection.vcas.contains (vcaId))
        return reject ("That VCA no longer exists.");
    std::vector<Id> assigned;
    for (const auto& [id, t] : projection.tracks)
        if (t->vcaId == vcaId)
            assigned.push_back (id);
    return run ("Delete VCA", [&] (TxnWriter& w) {
        bool good = true;
        for (const auto& id : assigned)
            good = good && w.put (trackPath (id) / "vcaId", Value());
        return wrote (good && w.remove (vcaPath (vcaId)));
    });
}

EditResult Session::setTrackVca (const Id& trackId, const Id& vcaId)
{
    const auto* t = projection.track (trackId);
    if (t == nullptr || ! projection.tracks.contains (trackId))
        return reject ("That track no longer exists.");
    if (t->kind == TrackKind::master)
        return reject ("The master can't be on a VCA.");
    if (! vcaId.empty() && ! projection.vcas.contains (vcaId))
        return reject ("That VCA no longer exists.");
    if (t->vcaId == vcaId)
        return reject (vcaId.empty() ? "That track isn't on a VCA." : "That track is already on that VCA.");
    return run ("Assign VCA", [&] (TxnWriter& w) { return wrote (w.put (trackPath (trackId) / "vcaId", vcaId.empty() ? Value() : Value (vcaId))); });
}
} // namespace weft::commands
