#pragma once

// Groups window (D-W6-15, P-GRP-1 to P-GRP-19): every group with its On, name and H, and what the
// chosen group links. Opened from the Mix menu (Alt+Shift+G) or a Group slot; it floats.

#include "Studio.h"
#include "Widgets.h"

namespace weft::ui
{
class GroupsPanel final : public juce::Component, private juce::ChangeListener
{
public:
    explicit GroupsPanel (Studio& studio);
    ~GroupsPanel() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();

    /** One group: its number, On, name (double-click to rename) and H. Clicking picks it; Alt+click also
        selects its members (M-TRK-52). */
    struct Row final : juce::Component
    {
        Row (GroupsPanel& panel, Id groupId);
        void resized() override;
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        GroupsPanel& panel;
        const Id groupId;
        juce::Label number, name;
        juce::ToggleButton on { "On" }, hide { "H" };
    };
    const std::vector<std::unique_ptr<Row>>& rows() const noexcept { return groupRows; }

    juce::ToggleButton active { "Groups active" };
    juce::TextButton add { "New Group" }, remove { "Delete Group" };
    /** The chosen group's links, in the order P-GRP-6 to P-GRP-18 lists them. */
    juce::ToggleButton selection { "Editing (Selection)" }, quantize { "Quantize-Locked (Audio)" }, automationMode { "Automation Mode" },
        volume { "Volume" }, mute { "Mute" }, input { "Input" }, pan { "Pan" }, solo { "Solo" }, record { "Record" }, color { "Color" },
        zoom { "Track Zoom" }, hideLink { "Hide" };
    std::array<juce::ToggleButton, 8> sends;
    std::vector<juce::ToggleButton*> linkButtons();

    const Id& chosen() const noexcept { return chosenGroup; }
    void choose (const Id& groupId);
    /** Selects the group's first track and every region on its tracks. */
    void selectMembers (const Id& groupId);
    /** Applies a change to a copy of the group's settings. */
    void change (const Id& groupId, const std::function<void (model::Group&)>& edit);

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    Studio& studio;
    juce::Label linksTitle, sendsTitle;
    std::vector<std::unique_ptr<Row>> groupRows;
    std::vector<Id> shownIds;
    Id chosenGroup;
};
} // namespace weft::ui
