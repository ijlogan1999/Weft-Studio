#pragma once

// G · Mixer (spec 04 §8, W-6, W-7): one channel strip per track in track order, then the
// buses, then Master pinned on the right. Strips edit the same data as the track headers.

#include "Studio.h"
#include "Widgets.h"

#include "weft/engine/graph/Mixer.h"

namespace weft::ui
{
/** Fader positions: the automation taper (D-ARCH-28), which puts 0 dB at about 79 % of the travel (F-MIX-2). */
namespace fader
{
inline constexpr double minDb = -90.0, maxDb = 6.0;
/** The fader's dB range with the taper as its proportion. */
juce::NormalisableRange<double> range();
/** Below minDb the fader is at the bottom: −inf (the model's −200). */
inline constexpr double silentDb = -200.0;
juce::String dbText (double db);
/** Parses "-6", "-6 dB", "-inf" (typed into the readout). */
std::optional<double> parseDb (const juce::String& text);
} // namespace fader

/** Knobs drawn to fill their bounds (the stock look leaves a 10 px margin, too much for a send
    knob); the drawing itself is the theme's, so knobs look the same everywhere. */
struct KnobLook final : juce::LookAndFeel_V4
{
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float start, float end, juce::Slider&) override;
};

/** A post-fader meter (F-MIX-5, D-UI-15): RMS as the solid bar, the peak above it in a lighter
    tone, a peak-hold line, and a clip light on top that clicking resets. Its scale is the fader's,
    so one set of dB marks between the two reads for both. */
class MeterView final : public juce::Component
{
public:
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void update (float peak, bool clip, double now) { update (peak, rmsLinear, clip, now); }
    void update (float peak, float rms, bool clip, double now);
    /** Where `db` sits in the bar area, 0 (bottom) to 1 (top): the fader's taper. */
    static float proportion (float db);
    /** The bar area: below the clip light, inset like the fader's travel. */
    juce::Rectangle<float> barArea() const;
    engine::MeterBallistics ballistics;
    float rmsDb = -144.0f;
    /** The highest peak since the last reset (the strip's peak number, D-W6-13). */
    float maxPeakDb = -144.0f;
    /** Clears the clip light and the peak number. */
    void resetPeak();
    std::function<void()> onResetClip;
    static constexpr int clipHeight = 8;
    static constexpr int clipGap = 4;

private:
    float rmsLinear = 0;
};

class ChannelStrip;

/** An insert slot: the left edge toggles bypass, the name opens the editor (filled) or the plug-in
    menu (empty); right-click offers Remove, Replace and Bypass; dragging reorders (W-6). */
class InsertSlot final : public juce::Component, public juce::SettableTooltipClient
{
public:
    InsertSlot (ChannelStrip& strip, std::size_t index, const model::PluginInstance* plugin, bool placeholder);
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    const std::size_t index;
    const Id pluginId; // empty: the free slot
    const juce::String label;
    const bool placeholder;
    static constexpr int bypassWidth = 14;

    ChannelStrip& strip() noexcept { return owner; }
    bool isBypassed() const noexcept { return bypassed; }
    /** The strip shows a bypass change without rebuilding its slots, so a swipe keeps its slot. */
    void showBypass (bool on);
    /** During a drag that started on a bypass dot (D-W6-10): the pointer is over `other`, which takes
        the state the first slot took. Tests call it directly. */
    void swipeBypassOver (InsertSlot* other);

private:
    ChannelStrip& owner;
    bool bypassed;
    bool dragging = false, removed = false, swipingBypass = false, bypassTarget = false;
};

/** A send slot: target bus, level knob, Pre/Post, mute (W-7). Empty slot: the bus menu. */
class SendSlot final : public juce::Component
{
public:
    SendSlot (ChannelStrip& strip, std::size_t index, const model::Send* send, const juce::String& busName, bool disabledEdge);
    ~SendSlot() override { level.setLookAndFeel (nullptr); }
    void resized() override;
    void paint (juce::Graphics&) override;
    const std::size_t index;
    const Id sendId;
    juce::SharedResourcePointer<KnobLook> knobs;
    juce::TextButton target, mode, mute { "M" };
    juce::Slider level;

private:
    ChannelStrip& owner;
    bool warning = false;
};

class ChannelStrip final : public juce::Component
{
public:
    /** trackId: a track or bus, or the master track. */
    ChannelStrip (Studio& studio, Id trackId);
    ~ChannelStrip() override;
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    /** Reads the track again (the mixer calls it after every change). */
    void refresh();
    void updateMeter (double now);
    /** Moves the fader, pan and mute with the automation the engine plays (the mixer's timer). */
    void followAutomation();

    const Id& trackId() const noexcept { return id; }
    bool isMaster() const;
    const model::Track* track() const;
    static constexpr int width = 104; // 13 grid units: room for an insert's name beside its bypass dot
    /** Clicking the strip selects its track (the mixer); the inspector's output strip doesn't. */
    bool selectsOnClick = true;
    /** The selected track's strip is drawn highlighted (the mixer; the inspector shows only it). */
    bool highlightSelection = true;
    /** Slot rows to lay out (the mixer gives every strip the same, so faders line up). */
    int insertRows = 1, sendRows = 1;
    int shownInsertRows() const;
    int shownSendRows() const;
    /** Height the strip needs to show its slot rows without squeezing the fader. */
    int preferredHeight() const;

    // Controls (tests drive them) -------------------------------------------------------
    juce::SharedResourcePointer<KnobLook> knobs;
    juce::Label name;
    juce::TextButton input;  // audio input, or the instrument slot of an instrument track
    juce::TextButton output;
    /** The Group and VCA slots (D-W6-15, D-W6-16), side by side under the output. Alt+click on the Group slot puts the
        track in the last group used; Shift+click on the VCA slot asks to show that VCA's strip (onShowVca). */
    juce::TextButton group, vca;
    std::function<void (const Id&)> onShowVca;
    juce::ComboBox automation;
    juce::Slider pan, volume;
    juce::Label readout;
    /** The highest peak since it was last clicked (D-W6-13); red over 0 dB. Clicking resets it and the clip light. */
    juce::Label peak;
    MeterView meter;
    SwipeButton mute { "M" }, solo { "S" }, arm { "R" }, monitor { "I" };
    std::vector<std::unique_ptr<InsertSlot>> inserts;
    std::vector<std::unique_ptr<SendSlot>> sends;

    // Menus: built as the mouse would show them; pick* does what choosing an item does. -------
    /** Empty slot: the plug-in choices (built-ins first, then VST3 by vendor). Filled: Open, Bypass, Remove, Replace,
        Insert Before. */
    juce::PopupMenu insertMenu (std::size_t slot) const;
    void pickInsert (std::size_t slot, int itemId);
    juce::PopupMenu instrumentMenu() const;
    void pickInstrument (int itemId);
    juce::PopupMenu inputMenu() const;
    void pickInput (int itemId);
    /** Existing buses (those that would loop disabled) and New Bus; a filled send adds Pre/Post, Mute, Remove. */
    juce::PopupMenu sendMenu (std::size_t slot) const;
    void pickSend (std::size_t slot, int itemId);
    juce::PopupMenu outputMenu() const;
    void pickOutput (int itemId);
    /** No Group, the groups (ticked: this one's), New Group, Group Settings... */
    juce::PopupMenu groupMenu() const;
    void pickGroup (int itemId);
    void clickGroup (juce::ModifierKeys mods);
    /** No VCA, the VCAs, New VCA. */
    juce::PopupMenu vcaMenu() const;
    void pickVca (int itemId);
    void clickVca (juce::ModifierKeys mods);
    enum SlotItem
    {
        slotNone = 1,
        slotNew,
        slotSettings,
        slotFirst = 10 // + index into groupsInOrder() or vcasInOrder()
    };
    /** Item ids shared by the menus. */
    enum Item
    {
        itemMaster = 1,
        itemNewBus,
        itemRemove,
        itemBypass,
        itemPre,
        itemMuteSend,
        itemOpen,
        itemBus = 100,             // + index into buses()
        itemPlugin = 1000,         // + index into pluginChoices
        itemInsertBefore = 100000  // + index into pluginChoices: a new plug-in in this slot, pushing the rest down (D-W6-10)
    };
    /** The buses a send or the output can go to, in track order. */
    std::vector<const model::Track*> buses() const;
    /** Moves an insert to another slot (drag). */
    void moveInsert (std::size_t from, std::size_t to);
    /** A slot dragged onto a slot of `target` (W-6 step 3, D-W6-9): reorders within a strip, moves to another
        channel, or with `copy` (Ctrl) leaves the original and makes a copy there. */
    void dropInsert (std::size_t from, ChannelStrip& target, std::size_t to, bool copy);
    /** Sets an insert's bypass (no change when it's already so). */
    void setBypass (std::size_t slot, bool on);
    void openEditor (const Id& pluginId);
    void showMenu (juce::PopupMenu menu, std::function<void (int)> picked);
    Studio& studio;

private:
    void rebuildSlots();
    std::map<juce::Component*, juce::String> tipsBeforeLock; // restored on unlock
    juce::String busName (const Id& busId) const;
    void setVolume (double db);
    juce::Rectangle<int> slotArea, scaleArea, buttonRow;
    /** Repaints the strip as the pointer enters or leaves the fader, for its value tag. */
    struct FaderHover final : juce::MouseListener
    {
        explicit FaderHover (ChannelStrip& s) : strip (s) {}
        void mouseEnter (const juce::MouseEvent&) override { strip.repaint(); }
        void mouseExit (const juce::MouseEvent&) override { strip.repaint(); }
        void mouseUp (const juce::MouseEvent&) override { strip.repaint(); }
        ChannelStrip& strip;
    } faderHover { *this };
    Id id;
    bool liveVolume = false, livePan = false; // the gesture in progress goes to live automation
    bool followed = false;
    std::string shownSlots; // what the slot components were built from
};

/** A VCA fader's strip (D-W6-16): its name (double-click to rename), a fader with its readout, and Mute.
    Right-click: Delete VCA. It scales the tracks assigned to it; it has no sound of its own. */
class VcaStrip final : public juce::Component
{
public:
    VcaStrip (Studio& studio, Id vcaId);
    ~VcaStrip() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void refresh();
    const Id& vcaId() const noexcept { return id; }
    juce::PopupMenu menu() const;
    void pick (int item);
    static constexpr int itemDelete = 1;

    juce::SharedResourcePointer<KnobLook> knobs;
    juce::Label name, readout, members;
    juce::Slider volume;
    juce::TextButton mute { "M" };

private:
    Studio& studio;
    Id id;
};

class MixerView final : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    explicit MixerView (Studio& studio);
    ~MixerView() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;

    /** The strips left to right: tracks, then buses; the master is separate and pinned. */
    const std::vector<std::unique_ptr<ChannelStrip>>& strips() const noexcept { return channelStrips; }
    ChannelStrip& masterStrip() noexcept { return *master; }
    ChannelStrip* stripFor (const Id& trackId);
    /** The VCA strips, after the channels (D-W6-16). */
    const std::vector<std::unique_ptr<VcaStrip>>& vcaStrips() const noexcept { return vcas; }
    void refresh();

    /** The view buttons and the strip filters above the strips (D-W6-14). Alt+click on a filter shows only that kind. */
    juce::TextButton singleView { "Single" }, tracksView { "Tracks" }, allView { "All" };
    juce::TextButton showAudio { "Audio" }, showInstrument { "Inst" }, showBus { "Bus" };
    void clickFilter (juce::TextButton& filter, juce::ModifierKeys mods);
    static constexpr int barHeight = 32;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void timerCallback() override;
    void layoutStrips();

    Studio& studio;
    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<ChannelStrip>> channelStrips;
    std::unique_ptr<ChannelStrip> master;
    std::vector<std::unique_ptr<VcaStrip>> vcas;
    std::vector<Id> shownOrder;
};
} // namespace weft::ui
