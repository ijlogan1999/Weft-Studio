#pragma once

#include "weft/engine/devices/AudioDevice.h"
#include "weft/engine/graph/EngineSnapshot.h"
#include "weft/engine/media/MediaStreamer.h"
#include "weft/engine/record/RecordPass.h"
#include "weft/engine/record/RecordWriter.h"
#include "weft/engine/rt/SpscQueue.h"
#include "weft/engine/transport/Metronome.h"
#include "weft/model/ProjectModel.h"

#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace weft::commands
{
class Session;
}

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
    /** MIDI recording over an existing region: merge into it, or a new region on top (F-REC-7). */
    bool midiMergeLinear = false;
    bool midiMergeCycle = true;

    static TransportSettings fromDefaults (const model::MetronomeSettings& m);
};

struct RecordResult
{
    bool started = false; // transport started (playing or counting in)
    bool recording = false;
    std::string message; // status bar text, e.g. "No tracks armed"
};

/** What finishing a recording pass did. */
struct RecordingOutcome
{
    bool committed = false;        // regions were added (one undo step "Record")
    std::size_t files = 0;         // media files kept
    std::int64_t droppedFrames = 0; // input that didn't reach the disk in time
    std::string message;
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

    /** Makes the instrument for an instrument track that has none in the rack yet. The default
        creates Weft Keys for tracks with no instrument or the built-in one; hosted plug-ins arrive in M6. */
    using InstrumentFactory = std::function<std::shared_ptr<Instrument> (const model::Track&)>;
    void setInstrumentFactory (InstrumentFactory factory) { instrumentFactory = std::move (factory); }

    // Transport ------------------------------------------------------------------
    void play();                  // from the playhead
    void play (Ticks from);
    void stop();
    void togglePlay();
    /** Starts recording at the playhead (or `from`), with the count-in from the settings. */
    RecordResult record();
    RecordResult record (Ticks from);
    /** Ends the recording pass but keeps playing (R while recording, D-W1-2). */
    void stopRecording();
    void locate (Ticks to);

    void setSettings (const TransportSettings& settings);
    const TransportSettings& settings() const noexcept { return currentSettings; }
    /** Record-enabled tracks (local view state, D-DM-2). Recompiles the snapshot. */
    void setArmedTracks (std::set<Id> trackIds);
    /** Soloed tracks (local, F-TRK-10). Recompiles the snapshot. */
    void setSoloedTracks (std::set<Id> trackIds);
    /** The selected track: an instrument track receives live MIDI when selected (D-W2-1). */
    void setSelectedTrack (const Id& trackId);
    /** All of the local view state at once (record enables, solos, selection). */
    void setLocalView (const LocalMixState& view);
    const LocalMixState& localState() const noexcept { return local; }

    struct Meter
    {
        float output = 0; // post-fader peak, linear
        float input = 0;  // input peak of an armed track
    };
    /** Peaks since the last call for a track's channel (or the master, for an empty id). */
    Meter takeMeter (const Id& trackId);
    /** Seconds of input buffered between the audio thread and the disk writer, per armed track. */
    void setRecordCapacitySeconds (double seconds);
    /** The project folder: recordings go to Recovery/ while running and Media/ when done. */
    void setProjectFolder (std::filesystem::path folder) { projectFolder = std::move (folder); }
    /** The user's recording delay in samples, added to the driver latency (F-REC-9). */
    void setRecordDelay (int samples) noexcept { recordDelay = samples; }
    /** Writes recordings from a background thread (default). Off: finishRecording() writes everything. */
    void setRecordWriterThread (bool on) noexcept { writerThread = on; }
    /** Total latency a recording is shifted by. */
    std::int64_t recordLatency() const noexcept { return deviceLatency + recordDelay; }

    /** Live MIDI input (keyboard, MIDI devices). `deviceFrame` is the device frame it arrived at,
        or -1 for the next block. Call from one thread only. */
    bool sendMidi (std::uint8_t status, std::uint8_t data1, std::uint8_t data2, std::int64_t deviceFrame = -1) noexcept;
    /** Frames processed since the device started. */
    std::int64_t deviceFrames() const noexcept { return published.frames.load (std::memory_order_acquire); }

    TransportState state() const noexcept { return static_cast<TransportState> (published.state.load (std::memory_order_acquire)); }
    /** Playhead in device-rate samples (negative during a count-in before bar 1). */
    std::int64_t positionSamples() const noexcept { return published.position.load (std::memory_order_acquire); }
    double positionTicks() const noexcept;
    double sampleRate() const noexcept { return rate; }

    /** Once a pass has stopped: finishes its files, moves them into Media/ and commits the regions
        to the session. Nothing yet (still recording, or no pass): nullopt. Call from a timer. */
    std::optional<RecordingOutcome> finishRecording (commands::Session& session);
    bool recordingPending() const noexcept { return ! passes.empty(); }

    // Meters (F-TRN-16) -------------------------------------------------------------
    /** Audio-thread load as a fraction of the block period, smoothed. */
    double cpuLoad() const noexcept { return published.load.load (std::memory_order_relaxed); }
    /** Blocks whose processing overran their period, plus dropouts the device reported. */
    std::int64_t dropouts() const noexcept { return published.dropouts.load (std::memory_order_relaxed); }
    void addDeviceDropouts (std::int64_t count) noexcept { published.dropouts.fetch_add (count, std::memory_order_relaxed); }
    /** Reads of streamed media that found the block not loaded yet (played as silence). */
    std::int64_t diskUnderruns() const noexcept { return published.underruns.load (std::memory_order_relaxed); }

    // Disk streaming (D-ARCH-17) ----------------------------------------------------------
    /** Streams from a background thread while the device runs (default). Off: call pumpDisk(). */
    void setDiskThread (bool on);
    void pumpDisk() { streamer.pump(); }
    MediaStreamer& diskStreamer() noexcept { return streamer; }

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
        settings,
        stopRecording
    };
    struct Command
    {
        CommandType type = CommandType::stop;
        std::int64_t a = 0, b = 0; // play/locate/record: position (or fromPlayhead); record: b = count-in bars
        RecordPass* pass = nullptr;
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
    void renderSpan (const EngineSnapshot& snap, const float* const* inputs, std::int64_t from, int offset, int frames, bool timeline) noexcept;
    void mixSegments (const ChannelPlan& channel, std::int64_t from, int frames, float* left, float* right) noexcept;
    void renderInstrument (const ChannelPlan& channel, std::int64_t from, int offset, int frames, bool timeline, float* left, float* right) noexcept;
    bool monitoring (const ChannelPlan& channel) const noexcept;
    void renderClicks (const EngineSnapshot& snap, std::int64_t from, int offset, int frames, bool audible) noexcept;
    void flushAllNotes (const EngineSnapshot* snap) noexcept;
    void finishCapture() noexcept;
    void captureSpan (const float* const* inputs, std::int64_t from, int offset, int frames) noexcept;
    void captureWrap (std::int64_t at) noexcept;
    void takeLiveMidi (int numFrames) noexcept;

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
    LocalMixState local;
    InstrumentFactory instrumentFactory;
    double recordSeconds = 10;
    std::filesystem::path projectFolder;
    int recordDelay = 0;
    std::int64_t deviceLatency = 0;
    bool writerThread = true;
    struct Pass
    {
        std::unique_ptr<RecordPass> pass;
        std::unique_ptr<RecordWriter> writer;
    };
    std::vector<Pass> passes; // oldest first; the audio thread fills the newest
    std::atomic<bool> running { false };
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
        std::atomic<std::int64_t> frames { 0 };
        std::atomic<std::int64_t> underruns { 0 };
    } published;
    MediaStreamer streamer;
    bool diskThread = true;
    struct LiveMidiIn
    {
        std::int64_t frame = -1;
        std::uint8_t status = 0, data1 = 0, data2 = 0;
    };
    rt::SpscQueue<LiveMidiIn, 1024> midiIn;

    // Audio thread.
    TransportState transport = TransportState::stopped;
    std::int64_t position = 0, playStart = 0, recordStart = 0;
    std::int64_t frameCounter = 0;
    RecordPass* pass = nullptr; // the pass being recorded
    std::optional<LiveMidiIn> laterMidi; // popped, but for a later block
    static constexpr int maxLiveEvents = 512;
    std::array<MidiEvent, maxLiveEvents> liveEvents {}; // this block's live MIDI, offsets in the block
    int numLive = 0;
    bool returnToStart = true, clickOn = true, clickInPlay = false, clickInRecord = true;
    float clickGain = 1;
    double loadAverage = 0;
    std::vector<std::vector<float>> mix;
    std::vector<float*> mixPtrs;
    std::vector<MidiEvent> events;
    static constexpr int maxClicksPerSpan = 64;
    std::array<Metronome::Click, maxClicksPerSpan> clickScratch {};
    std::array<int, maxClicksPerSpan> clickOffsets {};
    std::array<bool, maxClicksPerSpan> clickAccents {};
    Metronome metronome;
};
} // namespace weft::engine
