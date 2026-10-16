// Studio: comments, chat, track locks and version history (M12, spec 05 §7, §9; 04 §9.3-9.6).

#include "Studio.h"

namespace weft::ui
{
namespace
{
std::int64_t nowMs() { return juce::Time::currentTimeMillis(); }
} // namespace

std::map<UserId, std::string> Studio::memberNames() const
{
    std::map<UserId, std::string> names;
    if (syncClient != nullptr)
        for (const auto& m : syncClient->members())
            names[m.userId] = m.displayName;
    if (! names.contains (sess->user()))
        names[sess->user()] = relayAccount.signedIn() ? relayAccount.displayName : (userDisplayName.empty() ? std::string ("Me") : userDisplayName);
    return names;
}

juce::String Studio::nameOf (const UserId& user) const
{
    const auto names = memberNames();
    auto it = names.find (user);
    return it == names.end() ? juce::String ("Someone") : juce::String (it->second);
}

// Comments ----------------------------------------------------------------------------------------

commands::CommentAnchor Studio::commentAnchorHere() const
{
    commands::CommentAnchor a;
    a.pos = playhead();
    if (! local.selectedTrack.empty() && model().tracks.contains (local.selectedTrack))
        a.trackId = local.selectedTrack;
    return a;
}

std::optional<Id> Studio::addComment (const commands::CommentAnchor& anchor, const juce::String& text)
{
    const auto id = sess->addComment (anchor, commands::mentionIds (text.trim().toStdString(), memberNames()));
    if (! id)
        setStatus (juce::String (sess->lastError()));
    else
    {
        openThread = *id;
        draftComment.reset();
    }
    edited();
    return id;
}

void Studio::startComment (const commands::CommentAnchor& anchor)
{
    draftComment = anchor;
    if (uiCommand)
        uiCommand (Cmd::addComment);
    sendChangeMessage();
}

void Studio::showThread (const Id& threadId, juce::Rectangle<int> screenArea)
{
    openThread = threadId;
    if (onShowThread)
        onShowThread (threadId, screenArea);
    sendChangeMessage();
}

std::optional<Id> Studio::replyToComment (const Id& commentId, const juce::String& text)
{
    const auto id = sess->replyToComment (commentId, commands::mentionIds (text.trim().toStdString(), memberNames()));
    if (! id)
        setStatus (juce::String (sess->lastError()));
    edited();
    return id;
}

bool Studio::editComment (const Id& commentId, const juce::String& text)
{
    return check (sess->editComment (commentId, commands::mentionIds (text.trim().toStdString(), memberNames())));
}

bool Studio::deleteComment (const Id& commentId) { return check (sess->deleteComment (commentId)); }

bool Studio::resolveComment (const Id& commentId, bool resolved) { return check (sess->setCommentResolved (commentId, resolved)); }

juce::String Studio::commentText (const model::Comment& comment) const
{
    return comment.deleted ? juce::String ("(deleted)") : juce::String (commands::mentionNames (comment.body, memberNames()));
}

bool Studio::commentsUnread() const { return commands::hasUnreadComments (sess->model(), sess->user(), commentsSeenAt); }

void Studio::markCommentsSeen()
{
    if (! commentsUnread())
        return;
    commentsSeenAt = std::max (commentsSeenAt, std::max (sess->now(), nowMs()));
    sendChangeMessage();
}

// Chat -------------------------------------------------------------------------------------------

bool Studio::sendChat (const juce::String& text)
{
    const auto body = text.trim().toStdString();
    if (body.empty())
        return false;
    const auto id = sess->sendChat (commands::mentionIds (body, memberNames()));
    if (! id)
        setStatus (juce::String (sess->lastError()));
    else
        chatReadId = *id; // your own message is read
    edited();
    return id.has_value();
}

bool Studio::deleteChat (const Id& messageId) { return check (sess->deleteChat (messageId)); }

std::size_t Studio::unreadChat() const { return commands::unreadChat (sess->model(), sess->user(), chatReadId); }

void Studio::markChatRead()
{
    const auto& chat = sess->model().chat;
    if (chat.empty() || chat.back().id == chatReadId)
        return;
    chatReadId = chat.back().id;
    sendChangeMessage();
}

// Locks --------------------------------------------------------------------------------------------

bool Studio::toggleLock (const Id& trackId)
{
    if (previewing())
        return false;
    const auto lock = sess->lockOf (trackId);
    const auto r = lock && lock->by == sess->user() ? sess->unlockTrack (trackId) : sess->lockTrack (trackId);
    if (r)
        setStatus (lock ? "Unlocked." : "Locked: collaborators can't edit this track until you unlock it.");
    return check (r);
}

bool Studio::breakLock (const Id& trackId)
{
    const auto who = sess->lockOf (trackId);
    const auto r = sess->breakLock (trackId);
    if (r && who)
        setStatus ("Broke " + nameOf (who->by) + "'s lock.");
    return check (r);
}

// Versions ---------------------------------------------------------------------------------------------

std::optional<Id> Studio::saveVersion (const juce::String& name)
{
    const auto id = sess->saveVersion (name.trim().toStdString());
    if (! id)
        setStatus (juce::String (sess->lastError()));
    else
        setStatus ("Saved version \"" + juce::String (sess->model().version (*id)->name) + "\".");
    edited();
    return id;
}

bool Studio::previewVersion (const Id& versionId)
{
    auto old = sess->versionModel (versionId);
    if (! old)
    {
        setStatus ("This version isn't available.");
        return false;
    }
    eng->stop();
    if (previewModel == nullptr)
    {
        accessBeforePreview = sess->access();
        accessReasonBeforePreview = sess->accessMessage();
    }
    const auto* v = sess->model().version (versionId);
    previewId = versionId;
    previewModel = std::make_unique<model::ProjectModel> (std::move (*old));
    // The version is read-only; comments and chat are live and stay open (D-COL-37).
    const auto access = accessBeforePreview == commands::Session::Access::none ? commands::Session::Access::none : commands::Session::Access::commentsOnly;
    sess->setAccess (access, "You're viewing version '" + v->name + "', which is read-only.");
    selection = {};
    piano.selection.clear();
    syncEngine();
    sendChangeMessage();
    return true;
}

void Studio::returnToCurrent()
{
    if (previewModel == nullptr)
        return;
    eng->stop();
    previewModel.reset();
    previewId.clear();
    sess->setAccess (accessBeforePreview, accessReasonBeforePreview);
    selection = {};
    syncEngine();
    sendChangeMessage();
}

bool Studio::restorePreviewed()
{
    if (previewModel == nullptr)
        return false;
    const auto id = previewId;
    returnToCurrent();
    const auto r = sess->restoreVersion (id);
    if (r)
        setStatus ("Restored '" + juce::String (sess->model().version (id)->name) + "'. Undo puts back what was there.");
    return check (r);
}

// Notices and session ends ------------------------------------------------------------------------------

void Studio::collabNotices()
{
    for (auto& n : sess->takeNotices())
    {
        setStatus (juce::String (n.text));
        if (onNotice)
            onNotice (n);
        notices.push_back (std::move (n));
    }
    // Mentions of this user that arrived while they were away (05 §9.1), once each.
    const auto mentions = commands::newMentions (sess->model(), sess->user(), mentionsSeenAt);
    if (! mentions.empty())
    {
        const auto* last = mentions.back();
        setStatus (nameOf (last->author) + " mentioned you: \"" + commentText (*last).substring (0, 80) + "\""
                   + (mentions.size() > 1 ? " (and " + juce::String (mentions.size() - 1) + " more)" : juce::String()));
        for (const auto* m : mentions)
        {
            mentionsShown.push_back (m->id);
            mentionsSeenAt = std::max (mentionsSeenAt, std::max (m->createdAt, m->editedAt.value_or (0)));
        }
    }
}

void Studio::endSessionVersion()
{
    // An automatic version when a session with changes ends (05 §9.3): on close or after 30
    // minutes idle. Changes the user chose not to save in a local project don't get one.
    if (previewing())
        returnToCurrent();
    if (! sess->hasLocalEdits() || ((unsaved || changesDiscarded) && ! shared()))
        return;
    const bool wasSaved = ! unsaved;
    if (sess->autoVersion())
    {
        if (syncClient != nullptr)
            syncClient->poll();
        if (wasSaved)
            save();
    }
}
} // namespace weft::ui
