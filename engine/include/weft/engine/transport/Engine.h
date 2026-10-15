#pragma once

#include "weft/engine/devices/AudioDevice.h"
#include "weft/engine/graph/EngineSnapshot.h"
#include "weft/engine/rt/SpscQueue.h"
#include "weft/engine/transport/Metronome.h"
#include "weft/model/ProjectModel.h"

#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace weft::engine
{
enum class TransportState : int
{
    stopped,
    countIn,
    playing,
    recording
};

enum class ClickDuring : int
{
    record,
    play,
    both
};

/** Local, per-user transport settings (D-DM-12, D-DM-13): never stored in the document. */
struct TransportSettings
{
    bool returnToStartOnStop = true;
    bool metronomeOn = true;
    double metronomeLevelDb = 0.0;
    ClickDuring metronomeDuring = ClickDuring::record;
    int countInBars = 1; // 0, 1 or 2

    static TransportSettings fromDefaults (const model::MetronomeSettings& m);
};

struct RecordResult
{
    bool started = false; // transport started (playing or counting in)
    bool recording = false;
    std::string message; // status bar text, e.g. "No tracks armed"
};

/** Audio captured by the last recording pass. Milestone 3 turns this into regions. */
struct RecordedTake
{
    Ticks startTick = 0;
    std::int64_t startSample = 0; // device rate
    media::AudioData audio;
    std::int64_t droppedFrames = 0; // frames that didn't fit the capture buffer
};

/**
    The real-time engine: plays engine snapshots through an AudioDevice and runs
    the transport (play, stop, record, cycle, count-in, metronome) and the load
    meter. Everything except the AudioCallback methods is called from the message
    thread; process() follows the audio-thread rules (no locks, allocation, I/O).
*/
class Engine final : public AudioCallback
{
public:
    Engine (MediaPool& media, InstrumentRack& instruments);
    ~Engine() override;

    /** Compiles and publishes a snapshot of the model. Call after every model change. */
    void setProject (const model::ProjectModel& model);
    /** Recompiles the last model (after media loads or instrument changes). */
    void refresh();

    // Transport ------------------------------------------------------------------
    void play();                  // from the playhead
    void play (Ticks from);
    void stop();
    void togglePlay();
    /** Starts recording at the playhead (or `from`), with the count-in from the settings. */
    RecordResult record();
    RecordResult record (Ticks from);
    void locate (Ticks to);

    void setSettings (const TransportSettings& settings);
    const TransportSettings& settings() const noexcept { return currentSettings; }
    /** Record-enabled tracks (local view state). */
    void setArmedTracks (std::set<Id> trackIds) { armed = std::move (trackIds); }
    /** Seconds of input to keep per recording pass (preallocated in prepare). */
    void setRecordCapacitySeconds (double seconds);

    TransportState state() const noexcept { return static_cast<TransportState> (published.state.load (std::memory_order_acquire)); }
    /** Playhead in device-rate samples (negative during a count-in before bar 1). */
    std::int64_t positionSamples() const noexcept { return published.position.load (std::memory_order_acquire); }
    double positionTicks() const noexcept;
    double sampleRate() const noexcept { return rate; }

    /** Takes the audio of the last finished recording pass, if any. */
    std::optional<RecordedTake> takeRecording();

    // Meters (F-TRN-16) -------------------------------------------------------------
    /** Audio-thread load as a fraction of the block period, smoothed. */
    double cpuLoad() const noexcept { return published.load.load (std::memory_order_relaxed); }
    /** Blocks whose processing overran their period, plus dropouts the device reported. */
    std::int64_t dropouts() const noexcept { return published.dropouts.load (std::memory_order_relaxed); }
    void addDeviceDropouts (std::int64_t count) noexcept { published.dropouts.fetch_add (count, std::memory_order_relaxed); }

    /** Frees snapshots the audio thread has moved past. Called by setProject; call it periodically too. */
    void collectGarbage();
    std::size_t pendingRetired() const noexcept { return retired.size(); }

    // AudioCallback --------------------------------------------------------------------
    void prepare (const DeviceConfig& config) override;
    void process (const float* const* inputs, float* const* outputs, int numFrames) noexcept override;
    void release() override;

private:
    enum class CommandType : std::uint8_t
    {
        play,
        stop,
        record,
        locate,
        settings
    };
    struct Command
    {
        CommandType type = CommandType::stop;
        std::int64_t a = 0, b = 0; // play/locate/record: position (or fromPlayhead); record: b = count-in bars
        bool capture = false;
        // settings
        bool returnToStart = true, clickOn = true, clickInPlay = false, clickInRecord = true;
        float clickGain = 1;
    };

    static constexpr std::int64_t fromPlayhead = INT64_MIN;
    void publish (std::unique_ptr<EngineSnapshot> snapshot);
    bool send (const Command& c);
    std::int64_t toSamples (Ticks t) const;

    // Audio thread.
    void apply (const Command& c, const EngineSnapshot* snap) noexcept;
    void renderTimeline (const EngineSnapshot& snap, std::int64_t from, int offset, int frames) noexcept;
    void renderInstruments (const EngineSnapshot& snap, std::int64_t from, int offset, int frames, bool timeline) noexcept;
    void renderClicks (const EngineSnapshot& snap, std::int64_t from, int offset, int frames, bool audible) noexcept;
    void flushAllNotes (const EngineSnapshot* snap) noexcept;
    void finishCapture() noexcept;
    void captureInput (const float* const* inputs, int offset, int frames) noexcept;

    MediaPool& mediaPool;
    InstrumentRack& rack;

    // Message thread.
    std::optional<model::ProjectModel> lastModel;
    std::unique_ptr<EngineSnapshot> live;
    struct Retired
    {
        std::unique_ptr<EngineSnapshot> snapshot;
    };
    std::vector<Retired> retired;
    std::uint64_t nextSerial = 1;
    TransportSettings currentSettings;
    std::set<Id> armed;
    double recordSeconds = 600;
    bool running = false;
    double rate = 48000;
    int maxBlock = 0;
    int numInputs = 0, numOutputs = 0;

    // Shared.
    std::atomic<const EngineSnapshot*> current { nullptr };
    std::atomic<std::uint64_t> ackedSerial { 0 };
    rt::SpscQueue<Command, 256> commands;
    struct Published
    {
        std::atomic<int> state { 0 };
        std::atomic<std::int64_t> position { 0 };
        std::atomic<double> load { 0 };
        std::atomic<std::int64_t> dropouts { 0 };
        std::atomic<std::int64_t> capturedFrames { 0 };
        std::atomic<std::int64_t> droppedFrames { 0 };
        std::atomic<std::int64_t> captureStart { 0 };
        std::atomic<bool> captureReady { false };
    } published;

    // Audio thread.
    TransportState transport = TransportState::stopped;
    std::int64_t position = 0, playStart = 0, recordStart = 0;
    bool capturing = false;
    bool returnToStart = true, clickOn = true, clickInPlay = false, clickInRecord = true;
    float clickGain = 1;
    double loadAverage = 0;
    std::vector<std::vector<float>> mix, scratch, capture;
    std::vector<float*> mixPtrs, scratchPtrs;
    std::vector<MidiEvent> events;
    static constexpr int maxClicksPerSpan = 64;
    std::array<Metronome::Click, maxClicksPerSpan> clickScratch {};
    std::array<int, maxClicksPerSpan> clickOffsets {};
    std::array<bool, maxClicksPerSpan> clickAccents {};
    Metronome metronome;
};
} // namespace weft::engine
