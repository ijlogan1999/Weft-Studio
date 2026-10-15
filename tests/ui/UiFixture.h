#pragma once

// A headless main window over a fresh project, for the [UI] acceptance tests. On Linux
// they run under a virtual X server (xvfb-run).

#include "ui/MainComponent.h"
#include "ui/NewTrackDialog.h"

#include "weft/engine/devices/NullDevice.h"
#include "weft/media/WavFile.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <fstream>
#include <filesystem>
#include <random>

namespace weft::ui::test
{
struct TempFolder
{
    std::filesystem::path path;
    TempFolder()
    {
        std::random_device rd;
        path = std::filesystem::temp_directory_path() / ("weft-ui-" + std::to_string (rd()) + std::to_string (rd()));
        std::filesystem::create_directories (path);
    }
    ~TempFolder()
    {
        std::error_code ec;
        std::filesystem::remove_all (path, ec);
    }
    TempFolder (const TempFolder&) = delete;
    TempFolder& operator= (const TempFolder&) = delete;
};

inline constexpr Ticks bar = 4 * ticksPerQuarter;

/** A one-second stereo sine at 48 kHz. */
inline std::filesystem::path writeSine (const std::filesystem::path& file, double seconds = 1.0)
{
    media::AudioData a;
    a.sampleRate = 48000;
    const auto n = static_cast<std::size_t> (seconds * 48000);
    a.channels.assign (2, std::vector<float> (n));
    for (std::size_t i = 0; i < n; ++i)
        a.channels[0][i] = a.channels[1][i] = 0.5f * static_cast<float> (std::sin (2.0 * 3.141592653589793 * 440.0 * static_cast<double> (i) / 48000.0));
    REQUIRE (media::writeWav (file, a, media::SampleFormat::int24));
    return file;
}

struct Ui
{
    TempFolder folder;
    Studio studio { std::make_unique<engine::NullDevice>(), folder.path / "projects" };
    std::unique_ptr<MainComponent> main;

    Ui()
    {
        std::filesystem::create_directories (folder.path / "browse");
        Theme::get().setDark (true);
        main = std::make_unique<MainComponent> (studio, juce::File (juce::String ((folder.path / "browse").string())));
        main->setSize (1400, 860);
        main->setVisible (true);
        settle();
    }
    ~Ui()
    {
        main.reset();
        Theme::get().setDark (true);
    }

    /** Delivers pending change messages (the UI refreshes on them). */
    void settle()
    {
        for (int i = 0; i < 3; ++i)
        {
            studio.dispatchPendingMessages();
            Theme::get().dispatchPendingMessages();
        }
    }

    /** Runs the audio callback for a while (the NullDevice only runs when asked). */
    void renderAudio (std::int64_t frames = 2048) { static_cast<engine::NullDevice&> (studio.device()).render (frames); }

    Timeline& timeline() { return main->tracks().timeline(); }
    Ruler& ruler() { return main->tracks().ruler(); }
    const model::ProjectModel& m() { return studio.model(); }
    const model::Region& region (const Id& id) { return *m().region (id); }

    /** Instrument tracks below "Audio 1". */
    std::vector<Id> instrumentTracks (int n)
    {
        auto ids = studio.session().createTracks (model::TrackKind::instrument, n);
        studio.edited();
        settle();
        return ids;
    }

    /** Centre of a region on screen (timeline coordinates). */
    juce::Point<float> centreOf (const Id& regionId) { return timeline().regionBounds (region (regionId)).getCentre(); }

    void click (juce::Point<float> p, juce::ModifierKeys mods = {})
    {
        timeline().pointerDown (p, mods);
        timeline().pointerUp (p, mods);
        settle();
    }

    bool key (int code, juce::ModifierKeys mods = {}, juce::juce_wchar ch = 0)
    {
        const auto used = main->keyPressed (juce::KeyPress (code, mods, ch == 0 ? static_cast<juce::juce_wchar> (code) : ch));
        settle();
        return used;
    }
};

/** A mouse event as JUCE would deliver it to c. */
inline juce::MouseEvent mouse (juce::Component& c, juce::Point<float> pos, juce::ModifierKeys mods = {}, int clicks = 1,
                               std::optional<juce::Point<float>> downAt = {})
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, mods, juce::MouseInputSource::defaultPressure,
                             juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                             juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, &c, &c, now, downAt.value_or (pos), now,
                             clicks, downAt.has_value() && *downAt != pos);
}

inline void forEachComponent (juce::Component& root, const std::function<void (juce::Component&)>& fn)
{
    fn (root);
    for (auto* child : root.getChildren())
        forEachComponent (*child, fn);
}
} // namespace weft::ui::test
