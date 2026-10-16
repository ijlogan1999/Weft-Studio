#pragma once

// First-run Audio Setup and audio/MIDI preferences (W-0, F-UI-7, F-UI-8, F-UI-14). The
// device list comes from a DeviceProvider, so the [UI] tests run the dialog on the null device.

#include "Studio.h"
#include "Widgets.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace weft::ui
{
/** The audio device and MIDI inputs the user chose; kept in `audio-device.json` in the settings folder. */
struct AudioSettings
{
    std::string driver, output, input;
    double sampleRate = 0; // 0: the device's current rate
    int bufferSize = 0;    // 0: the device's default
    std::set<std::string> disabledMidiInputs;

    engine::DeviceConfig config() const;
    juce::String toString() const;
    static std::optional<AudioSettings> fromString (const juce::String& text);
    friend bool operator== (const AudioSettings&, const AudioSettings&) = default;
};

/** `<settings folder>/audio-device.json`. Its absence means first launch (AT-UI-8). */
std::filesystem::path audioSettingsFile();
std::optional<AudioSettings> loadAudioSettings();
bool saveAudioSettings (const AudioSettings& settings);

/** What the machine offers. The app uses JUCE's device layer; tests supply their own. */
class DeviceProvider
{
public:
    virtual ~DeviceProvider() = default;

    struct Capabilities
    {
        std::vector<double> sampleRates;
        std::vector<int> bufferSizes;
        double currentRate = 0;
        int defaultBufferSize = 0;
    };

    virtual std::vector<std::string> drivers() = 0;
    /** The name shown for a driver ("WASAPI (shared)"). */
    virtual juce::String driverLabel (const std::string& driver) { return juce::String::fromUTF8 (driver.c_str()); }
    virtual std::string defaultDriver() = 0;
    virtual std::vector<std::string> outputs (const std::string& driver) = 0;
    virtual std::vector<std::string> inputs (const std::string& driver) = 0;
    virtual std::string defaultOutput (const std::string& driver) = 0;
    virtual std::string defaultInput (const std::string& driver) = 0;
    virtual Capabilities capabilities (const std::string& driver, const std::string& output, const std::string& input) = 0;
    virtual std::unique_ptr<engine::AudioDevice> make (const AudioSettings& settings) = 0;
    virtual std::vector<std::string> midiInputs() = 0;
};

/** WASAPI shared/exclusive and ASIO (when installed) through JUCE. */
std::unique_ptr<DeviceProvider> juceDeviceProvider();

/** W-0 step 3: the default driver and devices at the device's current rate, with the buffer
    nearest 10 ms (480 samples at 48 kHz). */
AudioSettings defaultAudioSettings (DeviceProvider& provider);
/** The buffer size nearest 10 ms at a rate, from the sizes a device offers. */
int bufferNearestTenMs (const std::vector<int>& sizes, double rate, int fallback);

class AudioSetupDialog final : public juce::Component
{
public:
    /** firstRun: titled Audio Setup with Continue; otherwise the Audio & MIDI preferences with OK. */
    AudioSetupDialog (Studio& studio, DeviceProvider& provider, bool firstRun);
    ~AudioSetupDialog() override;
    void resized() override;
    void paint (juce::Graphics&) override;

    juce::ComboBox driver, output, input, sampleRate, bufferSize;
    juce::Label latency, help;
    juce::OwnedArray<juce::ToggleButton> midiInputs;
    juce::TextButton test { "Test" }, proceed { "Continue" }, cancel { "Cancel" };
    std::function<void()> onClose;
    /** Called after Continue / OK saved the settings. */
    std::function<void()> onSaved;
    /** Called when the dialog goes away without Continue / OK (its window was closed). */
    std::function<void()> onDismissed;

    AudioSettings settings() const;
    /** Opens the chosen device if it changed, then plays the test tone (W-0 step 5). */
    void playTest();
    /** Continue / OK: applies and saves the settings (W-0 step 6). */
    bool apply();

private:
    void fillDevices();
    void fillFormats();
    void showLatency();
    bool openChosen();

    Studio& studio;
    DeviceProvider& devices;
    std::vector<std::string> driverIds, outputIds, inputIds;
    std::optional<AudioSettings> opened; // what the studio's device was last opened with here
};
} // namespace weft::ui
