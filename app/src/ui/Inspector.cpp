#include "Inspector.h"

namespace weft::ui
{
namespace
{
const juce::StringArray curveNames { "Linear", "Equal power", "S-curve" };
const char* curveIds[] = { "linear", "equalPower", "sCurve" };

int curveIndex (const std::string& id)
{
    for (int i = 0; i < 3; ++i)
        if (id == curveIds[i])
            return i;
    return 1;
}
} // namespace

Inspector::Inspector (Studio& s) : studio (s)
{
    setTitle ("Inspector");
    regionName.setEditable (false, true, true);
    describe (regionName, "Region name", "Region name (double-click to rename)");
    regionName.onTextChange = [this] {
        if (const auto* r = region(); r != nullptr && regionName.getText().trim().isNotEmpty())
            studio.check (studio.session().setRegionName (r->id, regionName.getText().trim().toStdString()));
        else
            refresh();
    };
    describe (regionMute, Cmd::muteRegions);
    regionMute.onClick = [this] {
        if (const auto* r = region())
            studio.check (studio.session().setRegionMuted (r->id, regionMute.getToggleState()));
    };
    describe (regionLoop, Cmd::loopRegion);
    regionLoop.onClick = [this] {
        if (const auto* r = region())
        {
            const auto& m = studio.model();
            // Turning loop on repeats the region once; dragging the loop handle extends it.
            const auto end = regionLoop.getToggleState() ? std::optional<Ticks> (static_cast<Ticks> (m.regionEndTick (*r)) * 2 - r->start) : std::nullopt;
            studio.check (studio.session().setRegionLoop (r->id, end));
        }
    };
    auto setupSlider = [] (juce::Slider& sl, double lo, double hi, double step, const juce::String& suffix) {
        sl.setSliderStyle (juce::Slider::LinearHorizontal);
        sl.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 20);
        sl.setRange (lo, hi, step);
        sl.setTextValueSuffix (suffix);
    };
    setupSlider (regionGain, -30.0, 30.0, 0.1, " dB");
    regionGain.setDoubleClickReturnValue (true, 0.0);
    describe (regionGain, "Region gain", "Region gain in dB (double-click resets)");
    regionGain.onDragStart = [this] { studio.session().beginGesture ("Region Gain"); };
    regionGain.onDragEnd = [this] {
        studio.session().endGesture();
        studio.edited();
    };
    regionGain.onValueChange = [this] {
        if (const auto* r = region(); r != nullptr && ! refreshing)
            studio.check (studio.session().setRegionGain (r->id, regionGain.getValue()));
    };
    setupSlider (fadeIn, 0.0, 10000.0, 1.0, " ms");
    setupSlider (fadeOut, 0.0, 10000.0, 1.0, " ms");
    describe (fadeIn, "Fade in", "Fade-in length in milliseconds");
    describe (fadeOut, "Fade out", "Fade-out length in milliseconds");
    for (auto* f : { &fadeIn, &fadeOut })
    {
        f->setSkewFactorFromMidPoint (500.0);
        f->onValueChange = [this, f] {
            const auto* r = region();
            if (r == nullptr || refreshing || r->isMidi)
                return;
            const auto samples = static_cast<Samples> (f->getValue() / 1000.0 * studio.model().info.sampleRate);
            if (f == &fadeIn)
                studio.check (studio.session().setRegionFades (
                    r->id, model::Fade { std::min (samples, r->length - r->audio.fadeOut.length), r->audio.fadeIn.curve }, std::nullopt));
            else
                studio.check (studio.session().setRegionFades (
                    r->id, std::nullopt, model::Fade { std::min (samples, r->length - r->audio.fadeIn.length), r->audio.fadeOut.curve }));
        };
    }
    for (auto* c : { &fadeInCurve, &fadeOutCurve })
    {
        c->addItemList (curveNames, 1);
        c->onChange = [this, c] {
            const auto* r = region();
            if (r == nullptr || refreshing || r->isMidi)
                return;
            const std::string curve = curveIds[std::max (0, c->getSelectedItemIndex())];
            if (c == &fadeInCurve)
                studio.check (studio.session().setRegionFades (r->id, model::Fade { r->audio.fadeIn.length, curve }, std::nullopt));
            else
                studio.check (studio.session().setRegionFades (r->id, std::nullopt, model::Fade { r->audio.fadeOut.length, curve }));
        };
    }
    describe (fadeInCurve, "Fade-in curve");
    describe (fadeOutCurve, "Fade-out curve");

    trackName.setEditable (false, true, true);
    describe (trackName, "Track name", "Track name (double-click to rename)");
    trackName.onTextChange = [this] {
        const auto text = trackName.getText().trim();
        if (text.isEmpty() || studio.selectedTrack().empty())
            refresh();
        else
            studio.check (studio.session().renameTrack (studio.selectedTrack(), text.toStdString()));
    };
    for (int i = 0; i < paletteSize; ++i)
        trackColour.addItem ("Colour " + juce::String (i + 1), i + 1);
    describe (trackColour, "Track colour");
    trackColour.onChange = [this] {
        if (! refreshing && ! studio.selectedTrack().empty() && trackColour.getSelectedId() > 0)
            studio.check (studio.session().setTrackColor (studio.selectedTrack(), model::Color { trackColour.getSelectedId() - 1, {} }));
    };
    monitor.addItemList ({ "Auto", "On", "Off" }, 1);
    describe (monitor, "Input monitoring", "Input monitoring: Auto, On or Off");
    monitor.onChange = [this] {
        static const char* modes[] = { "auto", "on", "off" };
        if (! refreshing && ! studio.selectedTrack().empty() && monitor.getSelectedItemIndex() >= 0)
            studio.check (studio.session().setTrackMonitor (studio.selectedTrack(), modes[monitor.getSelectedItemIndex()]));
    };

    for (juce::Component* c : std::initializer_list<juce::Component*> { &regionName, &regionMute, &regionLoop, &regionGain, &fadeIn, &fadeOut,
                                                                          &fadeInCurve, &fadeOutCurve, &trackName, &trackColour, &monitor })
    {
        c->setWantsKeyboardFocus (dynamic_cast<juce::Label*> (c) != nullptr);
        addChildComponent (c);
    }
    studio.addChangeListener (this);
    refresh();
}

Inspector::~Inspector() { studio.removeChangeListener (this); }

const model::Region* Inspector::region() const
{
    const auto ids = studio.selection.ids();
    return ids.empty() ? nullptr : studio.model().region (ids.front());
}

void Inspector::refresh()
{
    const juce::ScopedValueSetter<bool> guard (refreshing, true);
    const auto* r = region();
    const bool audio = r != nullptr && ! r->isMidi;
    for (juce::Component* c : std::initializer_list<juce::Component*> { &regionName, &regionMute, &regionLoop })
        c->setVisible (r != nullptr);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &regionGain, &fadeIn, &fadeOut, &fadeInCurve, &fadeOutCurve })
        c->setVisible (audio);
    if (r != nullptr)
    {
        regionName.setText (juce::String (r->name), juce::dontSendNotification);
        regionMute.setToggleState (r->muted, juce::dontSendNotification);
        regionLoop.setToggleState (r->loop && r->loop->enabled, juce::dontSendNotification);
    }
    if (audio)
    {
        const auto rate = static_cast<double> (studio.model().info.sampleRate);
        regionGain.setValue (r->audio.gainDb, juce::dontSendNotification);
        fadeIn.setValue (static_cast<double> (r->audio.fadeIn.length) * 1000.0 / rate, juce::dontSendNotification);
        fadeOut.setValue (static_cast<double> (r->audio.fadeOut.length) * 1000.0 / rate, juce::dontSendNotification);
        fadeInCurve.setSelectedItemIndex (curveIndex (r->audio.fadeIn.curve), juce::dontSendNotification);
        fadeOutCurve.setSelectedItemIndex (curveIndex (r->audio.fadeOut.curve), juce::dontSendNotification);
    }
    const auto* t = studio.model().track (studio.selectedTrack());
    for (juce::Component* c : std::initializer_list<juce::Component*> { &trackName, &trackColour, &monitor })
        c->setVisible (t != nullptr);
    if (t != nullptr)
    {
        trackName.setText (juce::String (t->name), juce::dontSendNotification);
        trackColour.setSelectedId (t->color.rgb.empty() ? t->color.palette + 1 : 0, juce::dontSendNotification);
        monitor.setSelectedItemIndex (t->monitor == "on" ? 1 : t->monitor == "off" ? 2 : 0, juce::dontSendNotification);
        monitor.setVisible (commands::LocalView::canArm (*t));
    }
    resized();
    repaint();
}

void Inspector::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::surface));
    g.setColour (colour (Token::border));
    g.drawVerticalLine (getWidth() - 1, 0.0f, static_cast<float> (getHeight()));
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.setColour (colour (Token::textDim));
    if (regionName.isVisible())
        g.drawText ("REGION", 12, regionTop, getWidth() - 24, 18, juce::Justification::left);
    if (trackName.isVisible())
        g.drawText ("TRACK", 12, trackTop, getWidth() - 24, 18, juce::Justification::left);
    if (! regionName.isVisible() && ! trackName.isVisible())
        g.drawText ("Select a track or region.", getLocalBounds().reduced (12), juce::Justification::centredTop);
    g.setFont (juce::FontOptions (12.0f));
    auto label = [&] (juce::Component& c, const juce::String& text) {
        if (c.isVisible())
            g.drawText (text, 12, c.getY(), 70, c.getHeight(), juce::Justification::centredLeft);
    };
    label (regionGain, "Gain");
    label (fadeIn, "Fade in");
    label (fadeOut, "Fade out");
    label (trackColour, "Colour");
    label (monitor, "Monitor");
}

void Inspector::resized()
{
    auto r = getLocalBounds().reduced (8, 10);
    auto row = [&r] (int h = 26) {
        auto out = r.removeFromTop (h);
        r.removeFromTop (4);
        return out;
    };
    auto withLabel = [] (juce::Rectangle<int> b) { return b.withTrimmedLeft (76); };
    if (regionName.isVisible())
    {
        regionTop = r.getY();
        row (18);
        regionName.setBounds (row());
        auto toggles = row();
        regionMute.setBounds (toggles.removeFromLeft (toggles.getWidth() / 2));
        regionLoop.setBounds (toggles);
        if (regionGain.isVisible())
        {
            regionGain.setBounds (withLabel (row()));
            fadeIn.setBounds (withLabel (row()));
            fadeInCurve.setBounds (withLabel (row (24)));
            fadeOut.setBounds (withLabel (row()));
            fadeOutCurve.setBounds (withLabel (row (24)));
        }
        r.removeFromTop (14);
    }
    trackTop = r.getY();
    row (18);
    trackName.setBounds (row());
    trackColour.setBounds (withLabel (row (24)));
    monitor.setBounds (withLabel (row (24)));
}
} // namespace weft::ui
