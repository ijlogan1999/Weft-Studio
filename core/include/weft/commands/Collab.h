#pragma once

// Read-side helpers for comments and chat (spec 04 §9.3, §9.4, 05 §9): thread lists and
// filters, mention notifications, chat time links and unread counts.

#include "weft/model/ProjectModel.h"

#include <map>
#include <string>
#include <vector>

namespace weft::commands
{
enum class CommentFilter
{
    all,
    open,     // threads not resolved
    resolved, // resolved threads
    mine,     // threads this user started or replied to
    mentions  // threads with a comment that mentions this user
};

/** Threads (first comments) passing the filter, sorted by timeline position (04 §9.3).
    A deleted first comment with no remaining replies is left out. */
std::vector<const model::Comment*> commentThreads (const model::ProjectModel& model, CommentFilter filter, const UserId& me);
/** A thread's replies, oldest first. */
std::vector<const model::Comment*> commentReplies (const model::ProjectModel& model, const Id& threadId);
/** The track a comment is pinned on, or empty: an anchor track that was deleted leaves the
    comment on the timeline at its position (03 §9.1). */
Id commentTrack (const model::ProjectModel& model, const model::Comment& comment);
/** Comments by others that mention `me`, written or edited after sinceMs (05 §9.1). */
std::vector<const model::Comment*> newMentions (const model::ProjectModel& model, const UserId& me, std::int64_t sinceMs);
/** Threads with comments by others newer than sinceMs (the unread dot on the Comments tab). */
bool hasUnreadComments (const model::ProjectModel& model, const UserId& me, std::int64_t sinceMs);

/** "@Ben" typed in a message becomes "@<Ben's user id>" for the members named; the reverse for display. */
std::string mentionIds (const std::string& typed, const std::map<UserId, std::string>& names);
std::string mentionNames (const std::string& stored, const std::map<UserId, std::string>& names);

/** A time reference in a chat message (D-UI-3): "bar N", "bar N beat M" or "m:ss". */
struct TimeLink
{
    std::size_t start = 0, length = 0; // bytes in the text
    Ticks pos = 0;
};
std::vector<TimeLink> timeLinks (const std::string& text, const model::ProjectModel& model);

/** Chat messages by others after the one with id lastReadId (all of them for an empty id). */
std::size_t unreadChat (const model::ProjectModel& model, const UserId& me, const Id& lastReadId);
} // namespace weft::commands
