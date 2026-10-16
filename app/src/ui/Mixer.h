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

/** Knobs drawn to fill their bounds (the stock look leaves a 10 px margin, too much for a send knob). */
struct KnobLook final : juce::LookAndFeel_V4
{
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float start, float end, juce::Slider&) override;
};

/** A post-fader peak meter with peak hold and a clip light; clicking the light resets it (F-MIX-5). */
class MeterView final : public juce::Component
{
public:
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void update (float peak, bool clip, double now);
    engine::MeterBallistics ballistics;
    std::function<void()> onResetClip;
    static constexpr int clipHeight = 8;
};

class ChannelStrip;

/** An insert slot: the left edge toggles bypass, the name opens the editor (filled) or the plug-in
    menu (empty); right-click offers Remove, Replace and Bypass; dragging reorders (W-6). */
class InsertSlot final : public juce::Component, public juce::SettableTooltipClient
{
public:
    InsertSlot (ChannelStrip& strip, std::size_t index, const model::PluginInstance* plugin, bool placeholder);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    const std::size_t index;
    const Id pluginId; // empty: the free slot
    const juce::String label;
    const bool bypassed, placeholder;
    static constexpr int bypassWidth = 14;

private:
    ChannelStrip& owner;
    bool dragging = false;
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
    static constexpr int width = 104;
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
    juce::ComboBox automation;
    juce::Slider pan, volume;
    juce::Label readout;
    MeterView meter;
    juce::TextButton mute { "M" }, solo { "S" }, arm { "R" }, monitor { "I" };
    std::vector<std::unique_ptr<InsertSlot>> inserts;
    std::vector<std::unique_ptr<SendSlot>> sends;

    // Menus: built as the mouse would show them; pick* does what choosing an item does. -------
    /** Empty slot: the plug-in choices (built-ins first, then VST3 by vendor). Filled: Remove, Replace, Bypass. */
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
        itemBus = 100,    // + index into buses()
        itemPlugin = 1000 // + index into pluginChoices
    };
    /** The buses a send or the output can go to, in track order. */
    std::vector<const model::Track*> buses() const;
    /** Moves an insert to another slot (drag). */
    void moveInsert (std::size_t from, std::size_t to);
    void openEditor (const Id& pluginId);
    void showMenu (juce::PopupMenu menu, std::function<void (int)> picked);
    Studio& studio;

private:
    void rebuildSlots();
    std::map<juce::Component*, juce::String> tipsBeforeLock; // restored on unlock
    juce::String busName (const Id& busId) const;
    void setVolume (double db);
    juce::Rectangle<int> slotArea;
    Id id;
    bool liveVolume = false, livePan = false; // the gesture in progress goes to live automation
    bool followed = false;
    std::string shownSlots; // what the slot components were built from
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
    void refresh();

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void timerCallback() override;
    void layoutStrips();

    Studio& studio;
    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<ChannelStrip>> channelStrips;
    std::unique_ptr<ChannelStrip> master;
    std::vector<Id> shownOrder;
};
} // namespace weft::ui
