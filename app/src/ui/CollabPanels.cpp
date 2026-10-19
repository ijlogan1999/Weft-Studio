#include "CollabPanels.h"

#include "PresenceUi.h"

namespace weft::ui
{
namespace
{
/** " · " between the parts of a label. */
juce::String sep() { return juce::String::fromUTF8 (" \xc2\xb7 "); }
} // namespace

namespace
{
constexpr float pad = 8.0f;

juce::Font bodyFont() { return font (Type::body); }
juce::Font smallFont() { return font (Type::small); }
juce::Font nameFont() { return font (Type::strong); }

void styleEditor (juce::TextEditor& e, const juce::String& hint, bool multiLine)
{
    e.setMultiLine (multiLine, true);
    e.setReturnKeyStartsNewLine (false);
    e.setTextToShowWhenEmpty (hint, colour (Token::textDim));
    e.setColour (juce::TextEditor::backgroundColourId, colour (Token::background));
    e.setColour (juce::TextEditor::textColourId, colour (Token::text));
    e.setColour (juce::TextEditor::outlineColourId, colour (Token::border));
    e.setFont (bodyFont());
}

void styleToggle (juce::TextButton& b)
{
    b.setClickingTogglesState (false);
    b.setWantsKeyboardFocus (false);
    b.setColour (juce::TextButton::buttonOnColourId, colour (Token::accent));
    b.setColour (juce::TextButton::textColourOnId, colour (Token::onAccent));
}

/** Text with mentions as names, as one paragraph wrapped to width. */
std::unique_ptr<juce::TextLayout> layoutText (const juce::String& text, float width, juce::Colour c)
{
    juce::AttributedString s;
    s.append (text, bodyFont(), c);
    s.setWordWrap (juce::AttributedString::byWord);
    auto layout = std::make_unique<juce::TextLayout>();
    layout->createLayout (s, std::max (20.0f, width));
    return layout;
}
} // namespace

juce::String commentPlace (const Studio& studio, const model::Comment& comment)
{
    const auto& m = studio.model();
    auto text = "Bar " + juce::String (m.signatures.barAt (comment.pos).bar + 1);
    const auto track = commands::commentTrack (m, comment);
    if (const auto* t = m.track (track))
        text << sep() << juce::String (t->name);
    return text;
}

juce::String shortTime (std::int64_t unixMs) { return juce::Time (unixMs).formatted ("%d %b %H:%M"); }

// Thread ------------------------------------------------------------------------------------------

ThreadView::ThreadView (Studio& s, Id threadId) : studio (s), fixed (std::move (threadId))
{
    setTitle ("Comment thread");
    styleEditor (box, "Reply...", true);
    box.onReturnKey = [this] { submit(); };
    describe (box, "Reply");
    describe (send, "Reply", "Send the reply (Enter)");
    describe (resolve, "Resolve", "Resolve or reopen the thread");
    describe (close, "Close", "Close the thread");
    send.onClick = [this] { submit(); };
    resolve.onClick = [this] {
        if (const auto* c = studio.session().model().comment (shown))
            studio.resolveComment (shown, ! c->resolved);
    };
    close.onClick = [this] {
        if (fixed.empty())
            studio.openThread.clear();
        if (onClose)
            onClose();
        studio.sendChangeMessage();
    };
    for (auto* c : std::initializer_list<juce::Component*> { &box, &send, &resolve, &close })
        addAndMakeVisible (c);
    studio.addChangeListener (this);
    refresh();
}

ThreadView::~ThreadView() { studio.removeChangeListener (this); }

std::vector<const model::Comment*> ThreadView::comments() const
{
    std::vector<const model::Comment*> out;
    const auto& m = studio.session().model();
    if (const auto* first = m.comment (shown))
    {
        out.push_back (first);
        for (const auto* r : commands::commentReplies (m, shown))
            out.push_back (r);
    }
    return out;
}

void ThreadView::refresh()
{
    shown = fixed.empty() ? studio.openThread : fixed;
    const auto list = comments();
    std::vector<std::string> now;
    for (const auto* c : list)
        now.push_back (c->id + "|" + c->body + "|" + (c->resolved ? "r" : "") + (c->deleted ? "d" : ""));
    if (now != builtFrom)
    {
        builtFrom = now;
        edits.clear();
        deletes.clear();
        for (const auto* c : list)
        {
            if (c->author != studio.session().user() || c->deleted)
                continue;
            auto e = std::make_unique<juce::TextButton> ("Edit");
            auto d = std::make_unique<juce::TextButton> ("Delete");
            describe (*e, "Edit", "Edit your comment");
            describe (*d, "Delete", "Delete your comment");
            e->setComponentID (c->id);
            d->setComponentID (c->id);
            const auto id = c->id;
            e->onClick = [this, id] { startEdit (id); };
            d->onClick = [this, id] { studio.deleteComment (id); };
            addAndMakeVisible (*e);
            addAndMakeVisible (*d);
            edits.push_back (std::move (e));
            deletes.push_back (std::move (d));
        }
    }
    const auto* first = list.empty() ? nullptr : list.front();
    resolve.setButtonText (first != nullptr && first->resolved ? "Reopen" : "Resolve");
    send.setButtonText (editing.empty() ? "Reply" : "Save");
    setVisible (first != nullptr);
    resized();
    repaint();
}

void ThreadView::startEdit (const Id& commentId)
{
    const auto* c = studio.session().model().comment (commentId);
    if (c == nullptr)
        return;
    editing = commentId;
    box.setText (studio.commentText (*c), false);
    send.setButtonText ("Save");
    if (box.isShowing())
        box.grabKeyboardFocus();
}

bool ThreadView::submit()
{
    const auto text = box.getText().trim();
    if (text.isEmpty() || shown.empty())
        return false;
    bool ok = false;
    if (! editing.empty())
        ok = studio.editComment (editing, text);
    else
        ok = studio.replyToComment (shown, text).has_value();
    if (ok)
    {
        box.clear();
        editing.clear();
        send.setButtonText ("Reply");
    }
    return ok;
}

void ThreadView::layoutRows (int width)
{
    rows.clear();
    float y = 28.0f; // the place and status line
    for (const auto* c : comments())
    {
        Row r;
        r.id = c->id;
        r.text = layoutText (studio.commentText (*c), static_cast<float> (width) - 2 * pad, c->deleted ? colour (Token::textDim) : colour (Token::text));
        const auto h = 18.0f + r.text->getHeight() + (c->author == studio.session().user() && ! c->deleted ? 22.0f : 0.0f) + 6.0f;
        r.area = { pad, y, static_cast<float> (width) - 2 * pad, h };
        y += h;
        rows.push_back (std::move (r));
    }
}

int ThreadView::preferredHeight (int width) const
{
    float y = 28.0f;
    for (const auto* c : comments())
        y += 18.0f + layoutText (studio.commentText (*c), static_cast<float> (width) - 2 * pad, colour (Token::text))->getHeight()
           + (c->author == studio.session().user() && ! c->deleted ? 22.0f : 0.0f) + 6.0f;
    return static_cast<int> (y) + 96;
}

void ThreadView::resized()
{
    layoutRows (getWidth());
    std::size_t own = 0;
    for (const auto& r : rows)
        if (own < edits.size() && edits[own]->getComponentID() == juce::String (r.id))
        {
            auto buttons = r.area.withTrimmedTop (r.area.getHeight() - 26.0f).withHeight (20.0f).toNearestInt();
            edits[own]->setBounds (buttons.removeFromLeft (48));
            buttons.removeFromLeft (4);
            deletes[own]->setBounds (buttons.removeFromLeft (56));
            ++own;
        }
    auto bottom = getLocalBounds().reduced (static_cast<int> (pad)).removeFromBottom (82);
    auto buttons = bottom.removeFromBottom (24);
    close.setBounds (buttons.removeFromRight (60));
    buttons.removeFromRight (4);
    resolve.setBounds (buttons.removeFromRight (74));
    send.setBounds (buttons.removeFromLeft (64));
    bottom.removeFromBottom (4);
    box.setBounds (bottom);
}

void ThreadView::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::surfaceRaised));
    const auto list = comments();
    if (list.empty())
        return;
    const auto* first = list.front();
    g.setColour (colour (Token::textDim));
    g.setFont (smallFont());
    g.drawText (commentPlace (studio, *first) + (first->resolved ? juce::String ("  ") + sep() + "  Resolved by " + studio.nameOf (first->resolvedBy) : juce::String()),
                juce::Rectangle<float> (pad, 6.0f, static_cast<float> (getWidth()) - 2 * pad, 16.0f), juce::Justification::centredLeft, true);
    for (const auto& r : rows)
    {
        const auto* c = studio.session().model().comment (r.id);
        if (c == nullptr)
            continue;
        g.setColour (userColour (studio, c->author));
        g.setFont (nameFont());
        g.drawText (studio.nameOf (c->author), r.area.withHeight (16.0f), juce::Justification::centredLeft, true);
        g.setColour (colour (Token::textDim));
        g.setFont (smallFont());
        g.drawText (shortTime (c->createdAt) + (c->editedAt ? " (edited)" : ""), r.area.withHeight (16.0f), juce::Justification::centredRight, true);
        r.text->draw (g, r.area.withTrimmedTop (18.0f).withHeight (r.text->getHeight()));
        g.setColour (colour (Token::border));
        g.drawHorizontalLine (static_cast<int> (r.area.getBottom()) - 2, r.area.getX(), r.area.getRight());
    }
}

// Comments tab -----------------------------------------------------------------------------------------

class CommentsPanel::List final : public juce::Component
{
public:
    explicit List (CommentsPanel& p) : panel (p) { setTitle ("Comment threads"); }
    static constexpr int rowHeight = 56;
    void paint (juce::Graphics& g) override
    {
        auto& studio = panel.studio;
        const auto& m = studio.session().model();
        const auto ids = panel.shownThreads();
        g.fillAll (colour (Token::surface));
        if (ids.empty())
        {
            g.setColour (colour (Token::textDim));
            g.setFont (bodyFont());
            g.drawText ("No comments here. Press Shift+C to add one at the playhead.", getLocalBounds().reduced (10).withHeight (40),
                        juce::Justification::centredLeft, true);
            return;
        }
        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            const auto* c = m.comment (ids[i]);
            if (c == nullptr)
                continue;
            auto row = juce::Rectangle<int> (0, static_cast<int> (i) * rowHeight, getWidth(), rowHeight);
            if (studio.openThread == c->id)
            {
                g.setColour (colour (Token::surfaceRaised));
                g.fillRect (row);
            }
            auto r = row.reduced (8, 4).toFloat();
            drawCommentPin (g, r.withSize (12.0f, 14.0f), userColour (studio, c->author), c->resolved);
            r.removeFromLeft (18.0f);
            const auto replies = commands::commentReplies (m, c->id).size();
            g.setColour (colour (Token::text));
            g.setFont (nameFont());
            g.drawText (studio.nameOf (c->author) + juce::String ("  ") + sep() + "  " + commentPlace (studio, *c), r.removeFromTop (16.0f), juce::Justification::centredLeft, true);
            g.setColour (c->resolved ? colour (Token::textDim) : colour (Token::text));
            g.setFont (bodyFont());
            g.drawText (studio.commentText (*c), r.removeFromTop (18.0f), juce::Justification::centredLeft, true);
            g.setColour (colour (Token::textDim));
            g.setFont (smallFont());
            g.drawText ((replies == 0 ? juce::String ("No replies") : juce::String (replies) + (replies == 1 ? " reply" : " replies"))
                            + (c->resolved ? juce::String ("  ") + sep() + "  Resolved" : ""),
                        r, juce::Justification::centredLeft, true);
            g.setColour (colour (Token::border));
            g.drawHorizontalLine (row.getBottom() - 1, 0.0f, static_cast<float> (getWidth()));
        }
    }
    void mouseUp (const juce::MouseEvent& e) override { panel.openRow (e.y / rowHeight); }

private:
    CommentsPanel& panel;
};

CommentsPanel::CommentsPanel (Studio& s) : thread (s), studio (s), list (std::make_unique<List> (*this))
{
    setTitle ("Comments");
    for (auto* b : { &open, &resolved, &mine, &mentions })
    {
        styleToggle (*b);
        addAndMakeVisible (*b);
    }
    describe (open, "Open", "Threads not resolved");
    describe (resolved, "Resolved", "Resolved threads");
    describe (mine, "Mine", "Threads you started or replied to");
    describe (mentions, "Mentions", "Threads that mention you");
    auto filter = [this] (commands::CommentFilter f) {
        // Clicking the filter that's on shows every thread.
        studio.commentFilter = studio.commentFilter == f ? commands::CommentFilter::all : f;
        studio.sendChangeMessage();
    };
    open.onClick = [filter] { filter (commands::CommentFilter::open); };
    resolved.onClick = [filter] { filter (commands::CommentFilter::resolved); };
    mine.onClick = [filter] { filter (commands::CommentFilter::mine); };
    mentions.onClick = [filter] { filter (commands::CommentFilter::mentions); };

    draftPlace.setColour (juce::Label::textColourId, colour (Token::textDim));
    draftPlace.setFont (smallFont());
    styleEditor (draft, "New comment (@name mentions someone)", true);
    draft.onReturnKey = [this] { postDraft(); };
    describe (draft, "New comment");
    describe (post, "Post", "Post the comment (Enter)");
    describe (cancel, "Cancel");
    post.onClick = [this] { postDraft(); };
    cancel.onClick = [this] {
        studio.draftComment.reset();
        draft.clear();
        studio.sendChangeMessage();
    };
    for (auto* c : std::initializer_list<juce::Component*> { &draftPlace, &draft, &post, &cancel })
        addChildComponent (c);
    viewport.setViewedComponent (list.get(), false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);
    addChildComponent (thread);
    studio.addChangeListener (this);
    refresh();
}

CommentsPanel::~CommentsPanel() { studio.removeChangeListener (this); }

std::vector<Id> CommentsPanel::shownThreads() const
{
    std::vector<Id> out;
    for (const auto* c : commands::commentThreads (studio.session().model(), studio.commentFilter, studio.session().user()))
        out.push_back (c->id);
    return out;
}

void CommentsPanel::openRow (int index)
{
    const auto ids = shownThreads();
    if (index < 0 || index >= static_cast<int> (ids.size()))
        return;
    const auto* c = studio.session().model().comment (ids[static_cast<std::size_t> (index)]);
    if (c == nullptr)
        return;
    studio.openThread = c->id;
    studio.locate (c->pos);
    studio.sendChangeMessage();
}

bool CommentsPanel::postDraft()
{
    if (! studio.draftComment)
        return false;
    const auto text = draft.getText().trim();
    if (text.isEmpty())
        return false;
    const auto anchor = *studio.draftComment;
    if (! studio.addComment (anchor, text))
        return false;
    draft.clear();
    return true;
}

void CommentsPanel::focusDraft()
{
    if (draft.isShowing())
        draft.grabKeyboardFocus();
}

void CommentsPanel::refresh()
{
    const auto f = studio.commentFilter;
    open.setToggleState (f == commands::CommentFilter::open, juce::dontSendNotification);
    resolved.setToggleState (f == commands::CommentFilter::resolved, juce::dontSendNotification);
    mine.setToggleState (f == commands::CommentFilter::mine, juce::dontSendNotification);
    mentions.setToggleState (f == commands::CommentFilter::mentions, juce::dontSendNotification);
    const bool drafting = studio.draftComment.has_value();
    for (auto* c : std::initializer_list<juce::Component*> { &draftPlace, &draft, &post, &cancel })
        c->setVisible (drafting);
    if (drafting)
    {
        model::Comment where;
        where.pos = studio.draftComment->pos;
        where.trackId = studio.draftComment->trackId;
        where.regionId = studio.draftComment->regionId;
        draftPlace.setText ("New comment at " + commentPlace (studio, where), juce::dontSendNotification);
    }
    thread.setVisible (studio.session().model().comment (studio.openThread) != nullptr);
    resized();
    list->repaint();
}

void CommentsPanel::resized()
{
    auto r = getLocalBounds().reduced (6);
    auto filters = r.removeFromTop (24);
    const auto w = filters.getWidth() / 4;
    for (auto* b : { &open, &resolved, &mine, &mentions })
        b->setBounds (filters.removeFromLeft (w).reduced (2, 0));
    r.removeFromTop (6);
    if (draft.isVisible())
    {
        draftPlace.setBounds (r.removeFromTop (18));
        draft.setBounds (r.removeFromTop (56));
        auto buttons = r.removeFromTop (26).withTrimmedTop (2);
        post.setBounds (buttons.removeFromLeft (64));
        cancel.setBounds (buttons.removeFromRight (64));
        r.removeFromTop (6);
    }
    if (thread.isVisible())
        thread.setBounds (r.removeFromBottom (std::min (std::max (180, thread.preferredHeight (r.getWidth())), r.getHeight() * 2 / 3)));
    viewport.setBounds (r);
    list->setSize (std::max (10, r.getWidth() - viewport.getScrollBarThickness()),
                   std::max (r.getHeight(), static_cast<int> (shownThreads().size()) * List::rowHeight));
}

void CommentsPanel::paint (juce::Graphics& g) { g.fillAll (colour (Token::surface)); }

// Chat tab --------------------------------------------------------------------------------------------

class ChatPanel::List final : public juce::Component
{
public:
    explicit List (ChatPanel& p) : panel (p) { setTitle ("Chat messages"); }

    struct Row
    {
        juce::Rectangle<float> area;
        std::unique_ptr<juce::TextLayout> text;
        UserId author;
        std::int64_t sentAt = 0;
        bool deleted = false;
        std::vector<std::pair<juce::Rectangle<float>, Ticks>> links;
    };
    std::vector<Row> rows;

    void rebuild (int width)
    {
        auto& studio = panel.studio;
        const auto& m = studio.session().model();
        rows.clear();
        float y = 6.0f;
        const float textX = 36.0f, textW = static_cast<float> (width) - textX - pad;
        for (const auto& msg : m.chat)
        {
            Row r;
            r.author = msg.author;
            r.sentAt = msg.sentAt;
            r.deleted = msg.deleted;
            const auto shown = msg.deleted ? std::string ("(message deleted)") : commands::mentionNames (msg.body, studio.memberNames());
            juce::AttributedString s;
            s.setWordWrap (juce::AttributedString::byWord);
            std::vector<std::pair<juce::Range<int>, Ticks>> spans;
            std::size_t at = 0;
            int chars = 0;
            for (const auto& link : msg.deleted ? std::vector<commands::TimeLink> {} : commands::timeLinks (shown, m))
            {
                const auto before = juce::String::fromUTF8 (shown.data() + at, static_cast<int> (link.start - at));
                s.append (before, bodyFont(), colour (Token::text));
                chars += before.length();
                const auto linkText = juce::String::fromUTF8 (shown.data() + link.start, static_cast<int> (link.length));
                s.append (linkText, font (Type::body).withStyle (juce::Font::underlined), colour (Token::accent));
                spans.push_back ({ { chars, chars + linkText.length() }, link.pos });
                chars += linkText.length();
                at = link.start + link.length;
            }
            s.append (juce::String::fromUTF8 (shown.data() + at, static_cast<int> (shown.size() - at)), bodyFont(),
                      msg.deleted ? colour (Token::textDim) : colour (Token::text));
            r.text = std::make_unique<juce::TextLayout>();
            r.text->createLayout (s, std::max (20.0f, textW));
            const auto textArea = juce::Rectangle<float> (textX, y + 18.0f, textW, r.text->getHeight());
            // Where each link landed: the runs holding its characters, line by line.
            for (const auto& [range, pos] : spans)
                for (int li = 0; li < r.text->getNumLines(); ++li)
                {
                    const auto& line = r.text->getLine (li);
                    for (const auto* run : line.runs)
                    {
                        if (run->glyphs.isEmpty() || run->stringRange.getIntersectionWith (range).isEmpty())
                            continue;
                        const auto& g0 = run->glyphs.getReference (0);
                        const auto& g1 = run->glyphs.getReference (run->glyphs.size() - 1);
                        const auto ys = line.getLineBoundsY();
                        r.links.push_back ({ juce::Rectangle<float> (textArea.getX() + line.lineOrigin.x + g0.anchor.x, textArea.getY() + ys.getStart(),
                                                                     g1.anchor.x + g1.width - g0.anchor.x, ys.getLength()),
                                             pos });
                    }
                }
            r.area = { pad, y, static_cast<float> (width) - 2 * pad, 18.0f + r.text->getHeight() + 8.0f };
            y = r.area.getBottom();
            rows.push_back (std::move (r));
        }
        setSize (width, std::max (static_cast<int> (y) + 6, panel.viewport.getHeight()));
    }

    void paint (juce::Graphics& g) override
    {
        auto& studio = panel.studio;
        g.fillAll (colour (Token::surface));
        if (rows.empty())
        {
            g.setColour (colour (Token::textDim));
            g.setFont (bodyFont());
            g.drawText ("No messages yet.", getLocalBounds().reduced (10).withHeight (30), juce::Justification::centredLeft);
        }
        for (const auto& r : rows)
        {
            const auto c = userColour (studio, r.author);
            drawBadge (g, juce::Rectangle<float> (r.area.getX(), r.area.getY() + 2.0f, 22.0f, 22.0f), c,
                       initials (studio.nameOf (r.author).toStdString()), false);
            auto head = juce::Rectangle<float> (36.0f, r.area.getY(), r.area.getRight() - 36.0f, 16.0f);
            g.setColour (c);
            g.setFont (nameFont());
            g.drawText (studio.nameOf (r.author), head, juce::Justification::centredLeft, true);
            g.setColour (colour (Token::textDim));
            g.setFont (smallFont());
            g.drawText (shortTime (r.sentAt), head, juce::Justification::centredRight, true);
            r.text->draw (g, juce::Rectangle<float> (36.0f, r.area.getY() + 18.0f, r.area.getRight() - 36.0f, r.text->getHeight()));
        }
    }

    void mouseUp (const juce::MouseEvent& e) override { panel.clickAt (e.position); }
    void mouseMove (const juce::MouseEvent& e) override
    {
        bool over = false;
        for (const auto& r : rows)
            for (const auto& [box, pos] : r.links)
                over = over || box.contains (e.position);
        setMouseCursor (over ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }

private:
    ChatPanel& panel;
};

bool ChatPanel::Input::keyPressed (const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::returnKey)
    {
        if (key.getModifiers().isShiftDown())
            insertTextAtCaret ("\n");
        else if (onSend)
            onSend();
        return true;
    }
    return juce::TextEditor::keyPressed (key);
}

ChatPanel::ChatPanel (Studio& s) : studio (s), list (std::make_unique<List> (*this))
{
    setTitle ("Chat");
    styleEditor (input, "Message (Enter sends, Shift+Enter for a new line)", true);
    describe (input, "Message");
    describe (send, "Send", "Send the message (Enter)");
    input.onSend = [this] { sendTyped(); };
    send.onClick = [this] { sendTyped(); };
    viewport.setViewedComponent (list.get(), false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);
    addAndMakeVisible (input);
    addAndMakeVisible (send);
    studio.addChangeListener (this);
    refresh();
}

ChatPanel::~ChatPanel() { studio.removeChangeListener (this); }

bool ChatPanel::sendTyped()
{
    const auto text = input.getText().trim();
    if (text.isEmpty())
        return false;
    if (text.toStdString().size() > model::maxMessageLength)
    {
        studio.setStatus ("Messages can be at most 4,000 characters.");
        return false;
    }
    if (! studio.sendChat (text))
        return false;
    input.clear();
    return true;
}

std::vector<std::pair<juce::Rectangle<float>, Ticks>> ChatPanel::links() const
{
    std::vector<std::pair<juce::Rectangle<float>, Ticks>> out;
    for (const auto& r : list->rows)
        out.insert (out.end(), r.links.begin(), r.links.end());
    return out;
}

bool ChatPanel::clickAt (juce::Point<float> p)
{
    for (const auto& [box, pos] : links())
        if (box.expanded (1.0f).contains (p))
        {
            studio.locate (pos);
            return true;
        }
    return false;
}

void ChatPanel::refresh()
{
    const auto count = studio.session().model().chat.size();
    const bool atBottom = viewport.getViewPositionY() + viewport.getHeight() >= list->getHeight() - 4;
    list->rebuild (std::max (40, viewport.getWidth() - viewport.getScrollBarThickness()));
    if (count != shownCount && atBottom)
        viewport.setViewPosition (0, std::max (0, list->getHeight() - viewport.getHeight()));
    shownCount = count;
    list->repaint();
}

void ChatPanel::resized()
{
    auto r = getLocalBounds().reduced (6);
    auto bottom = r.removeFromBottom (64);
    send.setBounds (bottom.removeFromRight (56).withSizeKeepingCentre (56, 26));
    bottom.removeFromRight (4);
    input.setBounds (bottom);
    r.removeFromBottom (6);
    viewport.setBounds (r);
    refresh();
}

void ChatPanel::paint (juce::Graphics& g) { g.fillAll (colour (Token::surface)); }

// History tab -----------------------------------------------------------------------------------------

class HistoryPanel::List final : public juce::Component
{
public:
    explicit List (HistoryPanel& p) : panel (p) { setTitle ("Versions"); }
    static constexpr int rowHeight = 40;
    void paint (juce::Graphics& g) override
    {
        auto& studio = panel.studio;
        const auto entries = panel.entries();
        g.fillAll (colour (Token::surface));
        if (entries.empty())
        {
            g.setColour (colour (Token::textDim));
            g.setFont (bodyFont());
            g.drawText ("No versions yet. Save one to come back to it later.", getLocalBounds().reduced (10).withHeight (30),
                        juce::Justification::centredLeft, true);
        }
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            const auto& e = entries[i];
            auto row = juce::Rectangle<int> (0, static_cast<int> (i) * rowHeight, getWidth(), rowHeight);
            if (! e.day && e.versionId == studio.previewedVersion())
            {
                g.setColour (colour (Token::accent).withAlpha (0.25f));
                g.fillRect (row);
            }
            auto r = row.reduced (10, 4).toFloat();
            if (e.day)
            {
                g.setColour (colour (Token::textDim));
                g.setFont (nameFont());
                g.drawText ((e.expanded ? "v  " : ">  ") + e.label + "  (" + juce::String (e.count) + ")", r, juce::Justification::centredLeft, true);
            }
            else if (const auto* v = studio.session().model().version (e.versionId))
            {
                if (v->autoVersion)
                    r.removeFromLeft (16.0f);
                g.setColour (colour (Token::text));
                g.setFont (v->autoVersion ? bodyFont() : nameFont());
                g.drawText (e.label, r.removeFromTop (17.0f), juce::Justification::centredLeft, true);
                g.setColour (colour (Token::textDim));
                g.setFont (smallFont());
                g.drawText (studio.nameOf (v->createdBy) + juce::String ("  ") + sep() + "  " + shortTime (v->createdAt), r, juce::Justification::centredLeft, true);
            }
            g.setColour (colour (Token::border));
            g.drawHorizontalLine (row.getBottom() - 1, 0.0f, static_cast<float> (getWidth()));
        }
    }
    void mouseUp (const juce::MouseEvent& e) override { panel.click (e.y / rowHeight); }

private:
    HistoryPanel& panel;
};

HistoryPanel::HistoryPanel (Studio& s) : studio (s), list (std::make_unique<List> (*this))
{
    setTitle ("History");
    styleEditor (name, "Version name", false);
    name.onReturnKey = [this] { saveTyped(); };
    describe (name, "Version name");
    describe (save, Cmd::saveVersion);
    save.onClick = [this] { saveTyped(); };
    viewport.setViewedComponent (list.get(), false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (name);
    addAndMakeVisible (save);
    addAndMakeVisible (viewport);
    studio.addChangeListener (this);
    refresh();
}

HistoryPanel::~HistoryPanel() { studio.removeChangeListener (this); }

std::vector<HistoryPanel::Entry> HistoryPanel::entries() const
{
    // Versions of the current project (not of the one being previewed).
    const auto& versions = studio.session().model().versions;
    std::vector<const model::VersionTag*> named, automatic;
    for (const auto& [id, v] : versions)
        (v->autoVersion ? automatic : named).push_back (v.get());
    auto newestFirst = [] (const model::VersionTag* a, const model::VersionTag* b) { return std::tie (a->createdAt, a->id) > std::tie (b->createdAt, b->id); };
    std::sort (named.begin(), named.end(), newestFirst);
    std::sort (automatic.begin(), automatic.end(), newestFirst);
    std::vector<Entry> out;
    for (const auto* v : named)
        out.push_back ({ false, juce::String (v->name), v->id, 0, false });
    for (std::size_t i = 0; i < automatic.size();)
    {
        const auto day = juce::Time (automatic[i]->createdAt).formatted ("%a %d %b %Y");
        std::size_t j = i;
        while (j < automatic.size() && juce::Time (automatic[j]->createdAt).formatted ("%a %d %b %Y") == day)
            ++j;
        const bool open = expandedDays.contains (day);
        out.push_back ({ true, juce::String ("Automatic") + sep() + day, {}, static_cast<int> (j - i), open });
        if (open)
            for (std::size_t k = i; k < j; ++k)
                out.push_back ({ false, juce::String (automatic[k]->name), automatic[k]->id, 0, false });
        i = j;
    }
    return out;
}

void HistoryPanel::click (int index)
{
    const auto all = entries();
    if (index < 0 || index >= static_cast<int> (all.size()))
        return;
    const auto& e = all[static_cast<std::size_t> (index)];
    if (e.day)
    {
        const auto day = e.label.fromFirstOccurrenceOf (juce::String ("Automatic") + sep(), false, false);
        if (! expandedDays.erase (day))
            expandedDays.insert (day);
        refresh();
        return;
    }
    if (studio.previewedVersion() == e.versionId)
        studio.returnToCurrent();
    else
        studio.previewVersion (e.versionId);
}

bool HistoryPanel::saveTyped()
{
    if (! studio.saveVersion (name.getText()))
        return false;
    name.clear();
    return true;
}

void HistoryPanel::refresh()
{
    save.setEnabled (studio.session().access() == commands::Session::Access::full || studio.previewing());
    resized();
    list->repaint();
}

void HistoryPanel::resized()
{
    auto r = getLocalBounds().reduced (6);
    auto top = r.removeFromTop (26);
    save.setBounds (top.removeFromRight (104));
    top.removeFromRight (4);
    name.setBounds (top);
    r.removeFromTop (6);
    viewport.setBounds (r);
    list->setSize (std::max (10, r.getWidth() - viewport.getScrollBarThickness()),
                   std::max (r.getHeight(), static_cast<int> (entries().size()) * List::rowHeight));
}

void HistoryPanel::paint (juce::Graphics& g) { g.fillAll (colour (Token::surface)); }

// Preview banner ------------------------------------------------------------------------------------------

PreviewBanner::PreviewBanner()
{
    setTitle ("Version preview");
    describe (restore, "Restore", "Make the project match this version (you can undo it)");
    describe (back, "Return to current", "Leave the preview");
    for (auto* c : std::initializer_list<juce::Component*> { &message, &restore, &back })
        addAndMakeVisible (c);
}

void PreviewBanner::paint (juce::Graphics& g) { g.fillAll (colour (Token::accent).withAlpha (0.3f)); }

void PreviewBanner::resized()
{
    auto r = getLocalBounds().reduced (10, 4);
    back.setBounds (r.removeFromRight (140));
    r.removeFromRight (6);
    restore.setBounds (r.removeFromRight (90));
    message.setBounds (r);
}
} // namespace weft::ui
