#include "AudioSetup.h"

#include "ProjectDialogs.h"

#include "weft/engine/devices/JuceAudioDevice.h"
#include "weft/engine/midi/JuceMidiInputs.h"

#include <cmath>

namespace weft::ui
{
namespace
{
constexpr int labelX = 20, controlX = 150, rowHeight = 34;

juce::String text (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }

int indexOf (const std::vector<std::string>& list, const std::string& item)
{
    const auto it = std::find (list.begin(), list.end(), item);
    return it == list.end() ? -1 : static_cast<int> (it - list.begin());
}

class JuceProvider final : public DeviceProvider
{
public:
    std::vector<std::string> drivers() override
    {
        auto all = engine::JuceAudioDevice::driverTypes();
#if JUCE_WINDOWS
        // W-0 step 2: WASAPI shared and exclusive, and ASIO when a driver is installed.
        std::erase_if (all, [] (const std::string& d) { return d != "Windows Audio" && d != "Windows Audio (Exclusive Mode)" && d != "ASIO"; });
        std::erase_if (all, [] (const std::string& d) { return d == "ASIO" && engine::JuceAudioDevice::deviceNames (d).empty(); });
#endif
        return all;
    }
    juce::String driverLabel (const std::string& d) override
    {
        if (d == "Windows Audio")
            return "WASAPI (shared)";
        if (d == "Windows Audio (Exclusive Mode)")
            return "WASAPI (exclusive)";
        return text (d);
    }
    std::string defaultDriver() override { return engine::JuceAudioDevice::defaultDriverType(); }
    std::vector<std::string> outputs (const std::string& d) override { return engine::JuceAudioDevice::deviceNames (d, false); }
    std::vector<std::string> inputs (const std::string& d) override { return engine::JuceAudioDevice::deviceNames (d, true); }
    std::string defaultOutput (const std::string& d) override { return engine::JuceAudioDevice::defaultDeviceName (d, false); }
    std::string defaultInput (const std::string& d) override { return engine::JuceAudioDevice::defaultDeviceName (d, true); }
    Capabilities capabilities (const std::string& d, const std::string& out, const std::string& in) override
    {
        const auto c = engine::JuceAudioDevice::capabilities (d, out, in);
        return { c.sampleRates, c.bufferSizes, c.currentRate, c.defaultBufferSize };
    }
    std::unique_ptr<engine::AudioDevice> make (const AudioSettings& s) override
    {
        return std::make_unique<engine::JuceAudioDevice> (s.driver, s.output, s.input);
    }
    std::vector<std::string> midiInputs() override { return engine::JuceMidiInputs::deviceNames(); }
};
} // namespace

// Settings ---------------------------------------------------------------------------------

engine::DeviceConfig AudioSettings::config() const
{
    engine::DeviceConfig c;
    if (sampleRate > 0)
        c.sampleRate = sampleRate;
    if (bufferSize > 0)
        c.maxBlockSize = bufferSize;
    return c;
}

juce::String AudioSettings::toString() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("driver", text (driver));
    o->setProperty ("output", text (output));
    o->setProperty ("input", text (input));
    o->setProperty ("sampleRate", sampleRate);
    o->setProperty ("bufferSize", bufferSize);
    juce::Array<juce::var> off;
    for (const auto& m : disabledMidiInputs)
        off.add (text (m));
    o->setProperty ("disabledMidiInputs", off);
    return juce::JSON::toString (juce::var (o), false);
}

std::optional<AudioSettings> AudioSettings::fromString (const juce::String& json)
{
    const auto v = juce::JSON::parse (json);
    if (! v.isObject())
        return std::nullopt;
    AudioSettings s;
    s.driver = v.getProperty ("driver", "").toString().toStdString();
    s.output = v.getProperty ("output", "").toString().toStdString();
    s.input = v.getProperty ("input", "").toString().toStdString();
    s.sampleRate = static_cast<double> (v.getProperty ("sampleRate", 0.0));
    s.bufferSize = static_cast<int> (v.getProperty ("bufferSize", 0));
    if (const auto* off = v.getProperty ("disabledMidiInputs", juce::var()).getArray())
        for (const auto& m : *off)
            s.disabledMidiInputs.insert (m.toString().toStdString());
    return s;
}

std::filesystem::path audioSettingsFile() { return Studio::settingsFolder() / "audio-device.json"; }

std::optional<AudioSettings> loadAudioSettings()
{
    const auto file = juce::File (pathText (audioSettingsFile()));
    if (! file.existsAsFile())
        return std::nullopt;
    return AudioSettings::fromString (file.loadFileAsString());
}

bool saveAudioSettings (const AudioSettings& settings)
{
    const auto file = juce::File (pathText (audioSettingsFile()));
    file.getParentDirectory().createDirectory();
    return file.replaceWithText (settings.toString());
}

std::unique_ptr<DeviceProvider> juceDeviceProvider() { return std::make_unique<JuceProvider>(); }

int bufferNearestTenMs (const std::vector<int>& sizes, double rate, int fallback)
{
    if (sizes.empty() || rate <= 0)
        return fallback;
    const double want = rate / 100.0;
    return *std::min_element (sizes.begin(), sizes.end(), [want] (int a, int b) { return std::abs (a - want) < std::abs (b - want); });
}

AudioSettings defaultAudioSettings (DeviceProvider& provider)
{
    AudioSettings s;
    s.driver = provider.defaultDriver();
    s.output = provider.defaultOutput (s.driver);
    s.input = provider.defaultInput (s.driver);
    const auto caps = provider.capabilities (s.driver, s.output, s.input);
    s.sampleRate = caps.currentRate > 0 ? caps.currentRate : 48000.0;
    s.bufferSize = bufferNearestTenMs (caps.bufferSizes, s.sampleRate, caps.defaultBufferSize > 0 ? caps.defaultBufferSize : 480);
    return s;
}

// Dialog -----------------------------------------------------------------------------------

AudioSetupDialog::AudioSetupDialog (Studio& s, DeviceProvider& provider, bool firstRun) : studio (s), devices (provider)
{
    const auto initial = loadAudioSettings().value_or (defaultAudioSettings (devices));
    driverIds = devices.drivers();
    for (std::size_t i = 0; i < driverIds.size(); ++i)
        driver.addItem (devices.driverLabel (driverIds[i]), static_cast<int> (i) + 1);
    driver.setSelectedItemIndex (std::max (0, indexOf (driverIds, initial.driver)), juce::dontSendNotification);
    describe (driver, "Driver", "WASAPI shared works everywhere; ASIO gives the lowest latency");
    describe (output, "Output device");
    describe (input, "Input device");
    describe (sampleRate, "Sample rate");
    describe (bufferSize, "Buffer size", "Smaller buffers lower the latency but need more CPU");
    driver.onChange = [this] {
        fillDevices();
        fillFormats();
    };
    output.onChange = [this] { fillFormats(); };
    input.onChange = [this] { fillFormats(); };
    fillDevices();
    output.setSelectedItemIndex (std::max (0, indexOf (outputIds, initial.output)), juce::dontSendNotification);
    const auto in = indexOf (inputIds, initial.input);
    input.setSelectedId (in < 0 ? 1000 : in + 1, juce::dontSendNotification);
    fillFormats();
    if (initial.sampleRate > 0)
        sampleRate.setSelectedId (static_cast<int> (std::lround (initial.sampleRate)), juce::dontSendNotification);
    if (initial.bufferSize > 0)
        bufferSize.setSelectedId (initial.bufferSize, juce::dontSendNotification);

    help.setText ("For the lowest latency, use an ASIO driver if your interface has one.", juce::dontSendNotification);
    help.setColour (juce::Label::textColourId, colour (Token::textDim));
    latency.setColour (juce::Label::textColourId, colour (Token::textDim));

    for (const auto& name : devices.midiInputs())
    {
        auto* t = midiInputs.add (new juce::ToggleButton (text (name)));
        t->setToggleState (! initial.disabledMidiInputs.contains (name), juce::dontSendNotification);
        describe (*t, text (name), "Use this MIDI input");
        addAndMakeVisible (t);
    }

    describe (test, "Test", "Play a 1 kHz tone at -18 dBFS for one second on outputs 1 and 2");
    proceed.setButtonText (firstRun ? "Continue" : "OK");
    describe (proceed, proceed.getButtonText(), "Save these settings (Enter)");
    describe (cancel, "Cancel", "Close without changes (Esc)");
    proceed.addShortcut (juce::KeyPress (juce::KeyPress::returnKey));
    if (! firstRun)
        cancel.addShortcut (juce::KeyPress (juce::KeyPress::escapeKey));
    test.onClick = [this] { playTest(); };
    proceed.onClick = [this] {
        apply();
        if (onClose)
            onClose();
    };
    cancel.onClick = [this] {
        if (onClose)
            onClose();
    };
    cancel.setVisible (! firstRun);
    for (auto* c : std::initializer_list<juce::Component*> { &driver, &output, &input, &sampleRate, &bufferSize, &latency, &help, &test, &proceed })
        addAndMakeVisible (c);
    addChildComponent (cancel);
    showLatency();
    setSize (520, 330 + 26 * std::max (1, midiInputs.size()));
}

AudioSetupDialog::~AudioSetupDialog()
{
    if (onDismissed)
        onDismissed();
}

void AudioSetupDialog::fillDevices()
{
    const auto d = driverIds.empty() ? std::string() : driverIds[static_cast<std::size_t> (std::max (0, driver.getSelectedItemIndex()))];
    outputIds = devices.outputs (d);
    inputIds = devices.inputs (d);
    output.clear (juce::dontSendNotification);
    input.clear (juce::dontSendNotification);
    for (std::size_t i = 0; i < outputIds.size(); ++i)
        output.addItem (text (outputIds[i]), static_cast<int> (i) + 1);
    input.addItem ("None", 1000);
    for (std::size_t i = 0; i < inputIds.size(); ++i)
        input.addItem (text (inputIds[i]), static_cast<int> (i) + 1);
    output.setSelectedItemIndex (std::max (0, indexOf (outputIds, devices.defaultOutput (d))), juce::dontSendNotification);
    const auto in = indexOf (inputIds, devices.defaultInput (d));
    input.setSelectedId (in < 0 ? 1000 : in + 1, juce::dontSendNotification);
}

void AudioSetupDialog::fillFormats()
{
    const auto s = settings();
    const auto caps = devices.capabilities (s.driver, s.output, s.input);
    const auto oldRate = sampleRate.getSelectedId(), oldBuffer = bufferSize.getSelectedId();
    sampleRate.clear (juce::dontSendNotification);
    bufferSize.clear (juce::dontSendNotification);
    for (const auto r : caps.sampleRates)
        sampleRate.addItem (juce::String (r / 1000.0, std::lround (r) % 1000 == 0 ? 0 : 1) + " kHz", static_cast<int> (std::lround (r)));
    const auto rate = caps.currentRate > 0 ? caps.currentRate : 48000.0;
    sampleRate.setSelectedId (oldRate != 0 && sampleRate.indexOfItemId (oldRate) >= 0 ? oldRate : static_cast<int> (std::lround (rate)),
                              juce::dontSendNotification);
    for (const auto b : caps.bufferSizes)
        bufferSize.addItem (juce::String (b) + " samples (" + juce::String (1000.0 * b / rate, 1) + " ms)", b);
    const auto best = bufferNearestTenMs (caps.bufferSizes, rate, caps.defaultBufferSize);
    bufferSize.setSelectedId (oldBuffer != 0 && bufferSize.indexOfItemId (oldBuffer) >= 0 ? oldBuffer : best, juce::dontSendNotification);
}

AudioSettings AudioSetupDialog::settings() const
{
    AudioSettings s;
    const auto pick = [] (const std::vector<std::string>& ids, int index) {
        return index >= 0 && index < static_cast<int> (ids.size()) ? ids[static_cast<std::size_t> (index)] : std::string();
    };
    s.driver = pick (driverIds, driver.getSelectedItemIndex());
    s.output = pick (outputIds, output.getSelectedId() - 1);
    s.input = input.getSelectedId() == 1000 ? std::string() : pick (inputIds, input.getSelectedId() - 1);
    s.sampleRate = sampleRate.getSelectedId();
    s.bufferSize = bufferSize.getSelectedId();
    for (auto* t : midiInputs)
        if (! t->getToggleState())
            s.disabledMidiInputs.insert (t->getButtonText().toStdString());
    return s;
}

bool AudioSetupDialog::openChosen()
{
    auto s = settings();
    auto device = s;
    device.disabledMidiInputs.clear();
    studio.midiInputs().setDisabled (s.disabledMidiInputs);
    if (opened && *opened == device && studio.audioRunning())
        return true;
    const bool ok = studio.setAudioDevice (devices.make (s), s.config());
    opened = device;
    showLatency();
    return ok;
}

void AudioSetupDialog::playTest()
{
    if (openChosen())
        studio.playTestTone();
}

bool AudioSetupDialog::apply()
{
    onDismissed = nullptr;
    openChosen();
    const auto ok = saveAudioSettings (settings());
    if (onSaved)
        onSaved();
    return ok;
}

void AudioSetupDialog::showLatency()
{
    if (! studio.audioRunning())
    {
        latency.setText ("No audio device is open.", juce::dontSendNotification);
        return;
    }
    const auto c = studio.device().config();
    const auto ms = c.sampleRate > 0 ? 1000.0 * (c.inputLatency + c.outputLatency) / c.sampleRate : 0.0;
    latency.setText ("Round-trip latency: " + juce::String (ms, 1) + " ms (as reported by the driver)", juce::dontSendNotification);
}

void AudioSetupDialog::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::surfaceRaised));
    g.setColour (colour (Token::text));
    g.setFont (juce::FontOptions (13.0f));
    const juce::StringArray labels { "Driver", "Output", "Input", "Sample rate", "Buffer size", "", "", "MIDI inputs" };
    for (int i = 0; i < labels.size(); ++i)
        g.drawText (labels[i], labelX, 16 + i * rowHeight, controlX - labelX - 8, 26, juce::Justification::centredLeft);
    if (midiInputs.isEmpty())
    {
        g.setColour (colour (Token::textDim));
        g.drawText ("No MIDI inputs connected.", controlX, 16 + 7 * rowHeight, getWidth() - controlX - 20, 26, juce::Justification::centredLeft);
    }
}

void AudioSetupDialog::resized()
{
    const int w = getWidth() - controlX - 20;
    int y = 16;
    for (auto* c : { &driver, &output, &input, &sampleRate, &bufferSize })
    {
        c->setBounds (controlX, y, w, 26);
        y += rowHeight;
    }
    latency.setBounds (controlX, y, w, 22);
    help.setBounds (controlX, y + rowHeight, w, 22);
    y += 2 * rowHeight;
    for (int i = 0; i < midiInputs.size(); ++i)
        midiInputs[i]->setBounds (controlX, y + i * 26, w, 24);
    test.setBounds (labelX, getHeight() - 42, 84, 28);
    proceed.setBounds (getWidth() - 104, getHeight() - 42, 84, 28);
    cancel.setBounds (getWidth() - 200, getHeight() - 42, 84, 28);
}
} // namespace weft::ui
