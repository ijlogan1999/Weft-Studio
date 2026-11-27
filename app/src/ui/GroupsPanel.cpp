#include "GroupsPanel.h"

namespace weft::ui
{
namespace
{
constexpr int rowHeight = size::control;

struct LinkField
{
    juce::ToggleButton GroupsPanel::* button;
    bool model::Group::* flag;
};
const LinkField linkFields[] = {
    { &GroupsPanel::selection, &model::Group::linkSelection }, { &GroupsPanel::quantize, &model::Group::linkQuantize },
    { &GroupsPanel::automationMode, &model::Group::linkAutomationMode }, { &GroupsPanel::volume, &model::Group::linkVolume },
    { &GroupsPanel::mute, &model::Group::linkMute }, { &GroupsPanel::input, &model::Group::linkInput }, { &GroupsPanel::pan, &model::Group::linkPan },
    { &GroupsPanel::solo, &model::Group::linkSolo }, { &GroupsPanel::record, &model::Group::linkRecord }, { &GroupsPanel::color, &model::Group::linkColor },
    { &GroupsPanel::zoom, &model::Group::linkZoom }, { &GroupsPanel::hideLink, &model::Group::linkHide },
};
} // namespace

GroupsPanel::Row::Row (GroupsPanel& p, Id id) : panel (p), groupId (std::move (id))
{
    number.setJustificationType (juce::Justification::centred);
    number.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (number);
    name.setEditable (false, true);
    describe (name, "Group name", "Double-click to rename (Alt+click: select its tracks)");
    name.onTextChange = [this] {
        const auto text = name.getText().trim().toStdString();
        if (! text.empty())
            panel.change (groupId, [&] (model::Group& g) { g.name = text; });
        else
            panel.refresh();
    };
    name.addMouseListener (this, false);
    addAndMakeVisible (name);
    describe (on, "On", "Turns this group on or off");
    on.onClick = [this] { panel.change (groupId, [this] (model::Group& g) { g.enabled = on.getToggleState(); }); };
    addAndMakeVisible (on);
    describe (hide, "Hide", "Hides the group's tracks");
    hide.onClick = [this] { panel.change (groupId, [this] (model::Group& g) { g.hidden = hide.getToggleState(); }); };
    addAndMakeVisible (hide);
}

void GroupsPanel::Row::resized()
{
    auto r = getLocalBounds().reduced (space::half, 0);
    number.setBounds (r.removeFromLeft (28));
    on.setBounds (r.removeFromLeft (52));
    hide.setBounds (r.removeFromRight (44));
    name.setBounds (r);
}

void GroupsPanel::Row::paint (juce::Graphics& g)
{
    if (panel.chosen() == groupId)
    {
        g.setColour (colour (Token::hover));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), radius::r1);
    }
}

void GroupsPanel::Row::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isAltDown())
        panel.selectMembers (groupId);
    panel.choose (groupId);
}

GroupsPanel::GroupsPanel (Studio& s) : studio (s)
{
    describe (active, "Groups active", "Off: no group links its tracks (Shift+G)");
    active.onClick = [this] {
        studio.check (studio.session().setGroupsActive (active.getToggleState()));
        studio.edited();
    };
    addAndMakeVisible (active);
    add.onClick = [this] {
        if (const auto made = studio.session().createGroup())
            chosenGroup = *made;
        else
            studio.setStatus (juce::String (studio.session().lastError()));
        studio.edited();
    };
    addAndMakeVisible (add);
    remove.onClick = [this] {
        if (! chosenGroup.empty())
            studio.check (studio.session().deleteGroup (chosenGroup));
        studio.edited();
    };
    addAndMakeVisible (remove);

    linksTitle.setText ("Settings", juce::dontSendNotification);
    linksTitle.setFont (font (Type::strong));
    addAndMakeVisible (linksTitle);
    sendsTitle.setText ("Sends", juce::dontSendNotification);
    addAndMakeVisible (sendsTitle);
    for (const auto& f : linkFields)
    {
        auto& b = this->*f.button;
        const auto flag = f.flag;
        b.onClick = [this, &b, flag] { change (chosenGroup, [&b, flag] (model::Group& g) { g.*flag = b.getToggleState(); }); };
        addAndMakeVisible (b);
    }
    describe (quantize, "Quantize-Locked (Audio)", "Stored with the group; audio quantize isn't built yet");
    for (std::size_t i = 0; i < sends.size(); ++i)
    {
        auto& b = sends[i];
        b.setButtonText (juce::String (i + 1));
        describe (b, "Send " + juce::String (i + 1), "Links send " + juce::String (i + 1) + " across the group");
        b.onClick = [this, i, &b] {
            change (chosenGroup, [i, &b] (model::Group& g) {
                const int bit = 1 << static_cast<int> (i);
                g.linkSends = b.getToggleState() ? (g.linkSends | bit) : (g.linkSends & ~bit);
            });
        };
        addAndMakeVisible (b);
    }
    studio.addChangeListener (this);
    setSize (460, 520);
    refresh();
}

GroupsPanel::~GroupsPanel() { studio.removeChangeListener (this); }

std::vector<juce::ToggleButton*> GroupsPanel::linkButtons()
{
    std::vector<juce::ToggleButton*> out;
    for (const auto& f : linkFields)
        out.push_back (&(this->*f.button));
    return out;
}

void GroupsPanel::change (const Id& groupId, const std::function<void (model::Group&)>& edit)
{
    const auto& m = studio.model();
    const auto it = m.groups.find (groupId);
    if (it == m.groups.end())
        return;
    auto g = *it->second;
    edit (g);
    studio.check (studio.session().setGroup (g));
    studio.edited();
}

void GroupsPanel::choose (const Id& groupId)
{
    chosenGroup = groupId;
    refresh();
}

void GroupsPanel::selectMembers (const Id& groupId)
{
    const auto& m = studio.model();
    const auto members = m.groupMembers (groupId);
    if (members.empty())
        return;
    std::vector<Id> regions;
    for (const auto& id : members)
        for (const auto* r : m.regionsOnTrack (id))
            regions.push_back (r->id);
    studio.selectTrack (members.front());
    studio.selection.set (regions);
    studio.sendChangeMessage();
}

void GroupsPanel::refresh()
{
    const auto& m = studio.model();
    const auto groups = m.groupsInOrder();
    std::vector<Id> ids;
    for (const auto* g : groups)
        ids.push_back (g->id);
    if (ids != shownIds)
    {
        groupRows.clear();
        for (const auto& id : ids)
        {
            groupRows.push_back (std::make_unique<Row> (*this, id));
            addAndMakeVisible (*groupRows.back());
        }
        shownIds = ids;
    }
    if (! chosenGroup.empty() && ! m.groups.contains (chosenGroup))
        chosenGroup.clear();
    if (chosenGroup.empty() && ! ids.empty())
        chosenGroup = ids.front();
    for (auto& row : groupRows)
    {
        const auto& g = *m.groups.at (row->groupId);
        row->number.setText (juce::String (g.number), juce::dontSendNotification);
        if (! row->name.isBeingEdited())
            row->name.setText (g.name, juce::dontSendNotification);
        row->on.setToggleState (g.enabled, juce::dontSendNotification);
        row->hide.setToggleState (g.hidden, juce::dontSendNotification);
        row->repaint();
    }
    active.setToggleState (m.info.groupsActive, juce::dontSendNotification);
    const auto* g = chosenGroup.empty() ? nullptr : m.groups.at (chosenGroup).get();
    remove.setEnabled (g != nullptr);
    linksTitle.setText (g != nullptr ? "Settings: " + juce::String (g->name) : juce::String ("Settings"), juce::dontSendNotification);
    for (const auto& f : linkFields)
    {
        auto& b = this->*f.button;
        b.setEnabled (g != nullptr);
        b.setToggleState (g != nullptr && g->*f.flag, juce::dontSendNotification);
    }
    for (std::size_t i = 0; i < sends.size(); ++i)
    {
        sends[i].setEnabled (g != nullptr);
        sends[i].setToggleState (g != nullptr && (g->linkSends & (1 << static_cast<int> (i))) != 0, juce::dontSendNotification);
    }
    resized();
}

void GroupsPanel::paint (juce::Graphics& g) { g.fillAll (colour (Token::surface)); }

void GroupsPanel::resized()
{
    auto r = getLocalBounds().reduced (space::s1);
    auto top = r.removeFromTop (rowHeight);
    active.setBounds (top.removeFromLeft (140));
    remove.setBounds (top.removeFromRight (110));
    top.removeFromRight (space::half);
    add.setBounds (top.removeFromRight (100));
    r.removeFromTop (space::s1);
    auto list = r.removeFromLeft (r.getWidth() / 2);
    for (auto& row : groupRows)
        row->setBounds (list.removeFromTop (rowHeight));
    r.removeFromLeft (space::s1);
    linksTitle.setBounds (r.removeFromTop (rowHeight));
    for (auto* b : linkButtons())
        b->setBounds (r.removeFromTop (rowHeight));
    r.removeFromTop (space::half);
    sendsTitle.setBounds (r.removeFromTop (rowHeight));
    for (std::size_t row = 0; row < 2; ++row)
    {
        auto line = r.removeFromTop (rowHeight);
        for (std::size_t i = 0; i < 4; ++i)
            sends[row * 4 + i].setBounds (line.removeFromLeft (line.getWidth() / static_cast<int> (4 - i)));
    }
}
} // namespace weft::ui
