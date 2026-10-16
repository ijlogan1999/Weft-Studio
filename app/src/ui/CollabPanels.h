#pragma once

// The side panel's Comments, Chat and History tabs (spec 04 §9.3-9.5), the comment thread
// shown there and in a pin's popover, and the version preview banner.

#include "Studio.h"
#include "Widgets.h"

#include <set>

namespace weft::ui
{
/** "Bar 9 · Drums": where a comment is pinned. */
juce::String commentPlace (const Studio& studio, const model::Comment& comment);
/** "16 Oct 14:32" in local time. */
juce::String shortTime (std::int64_t unixMs);

/** One thread: its comments, a reply box, Resolve/Reopen, and Edit/Delete on your own comments. */
class ThreadView final : public juce::Component, private juce::ChangeListener
{
public:
    /** threadId empty: shows studio.openThread (the Comments tab). */
    ThreadView (Studio& studio, Id threadId = {});
    ~ThreadView() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();
    const Id& thread() const noexcept { return shown; }
    /** The comments shown, first the thread's own. */
    std::vector<const model::Comment*> comments() const;
    /** Edit puts a comment's text in the box; Reply (or Save) sends what's typed. */
    void startEdit (const Id& commentId);
    bool submit();
    int preferredHeight (int width) const;

    juce::TextEditor box;
    juce::TextButton send { "Reply" }, resolve { "Resolve" }, close { "Close" };
    std::vector<std::unique_ptr<juce::TextButton>> edits, deletes; // one per own comment, in comments() order
    std::function<void()> onClose;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    struct Row
    {
        Id id;
        juce::Rectangle<float> area;
        std::unique_ptr<juce::TextLayout> text;
    };
    void layoutRows (int width);
    Studio& studio;
    const Id fixed;
    Id shown, editing;
    std::vector<Row> rows;
    std::vector<std::string> builtFrom;
};

/** The Comments tab: filters, a list of threads by position, the open thread, and the new-comment box. */
class CommentsPanel final : public juce::Component, private juce::ChangeListener
{
public:
    explicit CommentsPanel (Studio& studio);
    ~CommentsPanel() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();
    /** The threads the filter shows, in list order. */
    std::vector<Id> shownThreads() const;
    /** What clicking a row does: opens the thread and moves the playhead there. */
    void openRow (int index);
    /** Posts the new comment (Shift+C opened the box). */
    bool postDraft();
    void focusDraft();

    juce::TextButton open { "Open" }, resolved { "Resolved" }, mine { "Mine" }, mentions { "Mentions" };
    juce::Label draftPlace;
    juce::TextEditor draft;
    juce::TextButton post { "Post" }, cancel { "Cancel" };
    ThreadView thread;

private:
    class List;
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    Studio& studio;
    std::unique_ptr<List> list;
    juce::Viewport viewport;
};

/** The Chat tab: messages with time links (D-UI-3), and the box (Enter sends, Shift+Enter is a new line). */
class ChatPanel final : public juce::Component, private juce::ChangeListener
{
public:
    explicit ChatPanel (Studio& studio);
    ~ChatPanel() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();
    /** Sends what's typed; the box clears when it went. */
    bool sendTyped();
    /** The clickable time links, in the message list's coordinates, with where they go. */
    std::vector<std::pair<juce::Rectangle<float>, Ticks>> links() const;
    /** A click in the message list: a link moves the playhead. */
    bool clickAt (juce::Point<float> p);

    class Input final : public juce::TextEditor
    {
    public:
        bool keyPressed (const juce::KeyPress& key) override;
        std::function<void()> onSend;
    };
    Input input;
    juce::TextButton send { "Send" };
    juce::Viewport viewport;

private:
    class List;
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    Studio& studio;
    std::unique_ptr<List> list;
    std::size_t shownCount = static_cast<std::size_t> (-1);
};

/** The History tab: Save Version, named versions, and automatic ones collapsed by day (04 §9.5). */
class HistoryPanel final : public juce::Component, private juce::ChangeListener
{
public:
    explicit HistoryPanel (Studio& studio);
    ~HistoryPanel() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();
    struct Entry
    {
        bool day = false;    // a day of automatic versions (click to expand)
        juce::String label;  // the version's name, or the day
        Id versionId;        // a version
        int count = 0;       // a day: how many
        bool expanded = false;
    };
    /** The rows, top to bottom: named versions newest first, then automatic ones by day. */
    std::vector<Entry> entries() const;
    /** What clicking a row does: previews the version, or opens or closes a day. */
    void click (int index);
    bool saveTyped();

    juce::TextEditor name;
    juce::TextButton save { "Save Version" };

private:
    class List;
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    Studio& studio;
    std::set<juce::String> expandedDays;
    std::unique_ptr<List> list;
    juce::Viewport viewport;
};

/** Over the tracks area while previewing a version: "Viewing version 'X' – read-only." */
class PreviewBanner final : public juce::Component
{
public:
    PreviewBanner();
    void paint (juce::Graphics&) override;
    void resized() override;
    juce::Label message;
    juce::TextButton restore { "Restore" }, back { "Return to current" };
    static constexpr int height = 34;
};
} // namespace weft::ui
