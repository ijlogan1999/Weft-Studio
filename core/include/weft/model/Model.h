#pragma once

// Typed view of the project document (spec 03). Each struct converts to and
// from the document's Value tree; fromValue is lenient (missing or malformed
// fields fall back to defaults and out-of-range values are clamped), which is
// part of read-time repair (spec 03 §10.4 item 7).

#include "weft/core/Id.h"
#include "weft/doc/Value.h"
#include "weft/time/Time.h"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace weft::model
{
using doc::Value;

inline constexpr int schemaVersion = 1;
inline constexpr int maxInserts = 15;
inline constexpr int maxSends = 8;
inline constexpr Samples minAudioRegionLength = 64;
inline constexpr Ticks minMidiRegionLength = 1;
inline constexpr std::size_t maxMessageLength = 4000; // comment and chat bodies (03 §9.1, §9.2)

/** Palette index 0-23, or "#RRGGBB". */
struct Color
{
    int palette = 0;
    std::string rgb; // non-empty overrides palette

    static Color fromValue (const Value& v);
    Value toValue() const;
    friend bool operator== (const Color&, const Color&) = default;
};

enum class TrackKind
{
    audio,
    instrument,
    bus,
    master
};
std::string_view toString (TrackKind k);
std::optional<TrackKind> trackKindFromString (std::string_view s);

struct InputAssignment
{
    enum class Kind
    {
        none,
        audio,
        midi
    };
    Kind kind = Kind::none;
    int firstChannel = 0;      // audio
    std::string device = "all"; // midi
    int channel = 0;           // midi: 1-16, 0 = all

    static InputAssignment fromValue (const Value& v);
    Value toValue() const;
};

struct OutputAssignment
{
    Id busId; // empty = master

    bool toMaster() const noexcept { return busId.empty(); }
    static OutputAssignment fromValue (const Value& v);
    Value toValue() const;
};

struct PluginInstance
{
    Id id;
    std::string format = "builtin"; // builtin | vst3
    std::string pluginUid;
    std::string name, vendor, version;
    bool bypass = false;
    Value state;                         // bytes, or a blob hash string when over 64 KB
    std::map<std::string, double> params; // ParamId -> normalized 0..1
    UserId addedBy;                       // who inserted it (the Missing Plug-ins notice, F-COL-20)

    static PluginInstance fromValue (const Value& v);
    Value toValue() const;
};

struct Send
{
    Id id;
    Id targetBusId;
    double levelDb = 0.0;
    bool pre = false;
    bool muted = false;

    static Send fromValue (const Value& v);
    Value toValue() const;
};

struct ChannelStrip
{
    double volumeDb = 0.0; // -200 (-inf) .. +6
    double pan = 0.0;      // -1 .. +1
    std::optional<PluginInstance> instrument;
    std::vector<PluginInstance> inserts;
    std::vector<Send> sends;

    static ChannelStrip fromValue (const Value& v);
    Value toValue() const;
};

struct LockInfo
{
    UserId by;
    std::int64_t at = 0;
    std::string note;
};

/** Track playback parameters (P-TRI-12..19, D-W3-13). They change what an instrument track's
    MIDI plays, from its regions and from live input, without changing the notes. */
struct TrackPlayback
{
    static constexpr double maxDelayMs = 500, maxDelayTicks = 960;
    int transpose = 0;      // semitones, -96..+96
    int velocityOffset = 0; // -99..+99
    int keyLow = 0, keyHigh = 127;        // notes outside are not played
    int velocityLow = 1, velocityHigh = 127;
    double delay = 0;       // in milliseconds, or ticks when delayInTicks; negative plays early
    bool delayInTicks = false;
    bool noTranspose = false; // the regions' own transpose doesn't apply (drum kits)
    int outChannel = 0;       // 0: each event keeps its channel; 1-16: every event is sent on this one

    bool isDefault() const noexcept;
    /** The pitch and velocity played, or nullopt when the limits drop the note. */
    std::optional<std::pair<int, int>> play (int pitch, int velocity, int regionTranspose) const noexcept;
    static TrackPlayback fromValue (const Value& v);
    Value toValue() const;
    bool operator== (const TrackPlayback&) const = default;
};

struct Track
{
    Id id;
    TrackKind kind = TrackKind::audio;
    std::string name;
    Color color;
    int channels = 2;
    InputAssignment input;
    OutputAssignment output;
    bool mute = false;
    /** Track off (L-TRH-7): the channel isn't processed at all (no playback, plug-ins idle). Audio and instrument tracks. */
    bool off = false;
    /** Hidden (L-TRH-10): no row in the tracks area unless Show Hidden Tracks is on; still plays. */
    bool hidden = false;
    std::string monitor = "auto"; // auto | on | off
    std::optional<LockInfo> locked;
    ChannelStrip strip;
    std::string automationMode = "read"; // off | read | touch | latch | write (F-AUT-5: read is default)
    TrackPlayback playback; // instrument tracks

    static Track fromValue (const Id& id, const Value& v);
    Value toValue() const;
};

struct Fade
{
    Samples length = 0;
    std::string curve = "equalPower"; // linear | equalPower | sCurve
};

struct AudioRegionData
{
    std::string media; // blob hash
    Samples sourceOffset = 0;
    double gainDb = 0.0;
    Fade fadeIn, fadeOut;
    bool reversed = false; // plays a reversed copy of its audio (L-RGN-12); Reverse toggles it
};

struct RegionQuantize
{
    Ticks grid = 240;
    double strength = 1.0; // 0..1
    double swing = 0.0;    // 0..1 (UI shows 0-100%)
    /** Q-Range (D-W4-16): positive, only notes within this many ticks of their grid point move; negative,
        only notes further away than that; 0, every note. */
    Ticks range = 0;
    /** Q-Flam: notes landing together are spread this many ticks apart, upwards in pitch (negative: downwards). */
    Ticks flam = 0;
    Value toValue() const;
    bool operator== (const RegionQuantize&) const = default;
};

struct MidiRegionData
{
    Id sequenceId;
    Ticks sequenceOffset = 0;
    std::optional<RegionQuantize> quantize;
    int transpose = 0;
    int velocityOffset = 0;
    /** Dynamics (D-W4-16): each velocity's distance from 64 is scaled by this percent; 0 is Fixed (all 64), 100 off. */
    int dynamics = 100;
    /** Gate time: 0 off, else a percent of each note's length; gateFixed plays short notes, gateLegato ties them. */
    int gate = 0;
    static constexpr int gateFixed = -1, gateLegato = -2, maxGate = 200, maxDynamics = 300;
    static constexpr Ticks fixedGateTicks = 30;
    /** Clip Length: notes stop at the region's end (the default); off, they play their whole length. */
    bool clipLength = true;
};

struct RegionLoop
{
    bool enabled = false;
    Ticks endTicks = 0;
};

struct Region
{
    Id id;
    Id trackId;
    bool isMidi = false;
    Ticks start = 0;
    std::int64_t length = 0; // ticks for MIDI, samples for audio (D-TMP-1)
    std::string name;
    std::optional<Color> color;
    bool muted = false;
    /** Playback delay in milliseconds, -500..+500 (L-RGN-6, D-W3-5): the region plays this much later (or earlier). */
    double delayMs = 0.0;
    /** The delay in ticks instead (D-W4-16), -960..+960; whichever was set last applies. */
    bool delayInTicks = false;
    Ticks delayTicks = 0;
    static constexpr Ticks maxDelayTicks = 960;
    std::int64_t z = 0;
    std::optional<RegionLoop> loop;
    Id takeFolderId;
    AudioRegionData audio; // valid when !isMidi
    MidiRegionData midi;   // valid when isMidi
    UserId createdBy, modifiedBy;
    std::int64_t modifiedAt = 0;

    static Region fromValue (const Id& id, const Value& v);
    Value toValue() const;
};

struct TakeFolder
{
    Id id;
    Id trackId;
    Ticks start = 0, endTicks = 0;
    std::vector<Id> takeIds;
    Id activeTakeId;

    static TakeFolder fromValue (const Id& id, const Value& v);
    Value toValue() const;
};

struct Note
{
    Id id;
    Ticks pos = 0;
    Ticks length = 240;
    int pitch = 60;
    int velocity = 100;
    int releaseVelocity = 64;
    int channel = 1;
    bool muted = false;

    static Note fromValue (const Id& id, const Value& v);
    Value toValue() const;
    friend bool operator== (const Note&, const Note&) = default;
};

struct CcEvent
{
    Id id;
    Ticks pos = 0;
    std::string kind = "cc"; // cc | pitchBend | channelPressure | polyPressure | programChange
    int number = 0;
    int value = 0;
    int channel = 1;

    static CcEvent fromValue (const Id& id, const Value& v);
    Value toValue() const;
};

struct MidiSequence
{
    Id id;
    std::map<Id, Note> notes;
    std::map<Id, CcEvent> controllers;

    /** Notes ordered by position, then pitch, then id (D-DM-7: order is computed). */
    std::vector<const Note*> sortedNotes() const;

    static MidiSequence fromValue (const Id& id, const Value& v);
    Value toValue() const;
};

struct MediaRef
{
    std::string hash; // "b3:" + 64 hex
    std::string originalName;
    std::string format = "wav";
    int sampleRate = 48000;
    int channels = 1;
    Samples lengthSamples = 0;
    int bitDepth = 24;
    std::int64_t sizeBytes = 0;
    UserId addedBy;
    std::int64_t addedAt = 0;

    static MediaRef fromValue (const std::string& hash, const Value& v);
    Value toValue() const;
};

struct Marker
{
    Id id;
    Ticks pos = 0;
    std::string name;
    std::optional<Color> color;
    UserId createdBy;

    static Marker fromValue (const Id& id, const Value& v);
    Value toValue() const;
};

/** A comment pinned to the timeline (03 §9.1). Replies are one level deep. */
struct Comment
{
    Id id;
    Ticks pos = 0;
    std::optional<Ticks> endPos;
    Id trackId;  // optional anchor
    Id regionId; // optional anchor
    Id parentId; // reply to this comment
    UserId author;
    std::int64_t createdAt = 0;
    std::string body; // "@<UserId>" for mentions
    std::optional<std::int64_t> editedAt;
    bool resolved = false;
    UserId resolvedBy;
    bool deleted = false;

    /** The user ids mentioned as "@<UserId>" in the body. */
    std::vector<UserId> mentions() const;

    static Comment fromValue (const Id& id, const Value& v);
    Value toValue() const;
};

/** A chat message (03 §9.2). Chat is an append-only list. */
struct ChatMessage
{
    Id id;
    UserId author;
    std::int64_t sentAt = 0;
    std::string body;
    bool deleted = false;

    static ChatMessage fromValue (const Value& v);
    Value toValue() const;
};

/** A named or automatic checkpoint (03 §9.4): the change-graph heads of that state. */
struct VersionTag
{
    Id id;
    std::string name;
    std::vector<std::string> heads; // change hashes, hex
    UserId createdBy;
    std::int64_t createdAt = 0;
    bool autoVersion = false;

    static VersionTag fromValue (const Id& id, const Value& v);
    Value toValue() const;
};

struct AutomationTarget
{
    std::string kind = "volume"; // volume | pan | mute | sendLevel | pluginParam
    Id sendId;
    Id pluginId;
    std::string paramId;

    /** Identity of the target, for "one lane per (track, target)" (D-DM-11). */
    std::string key() const;
    /** The target a key() came from, if it parses. */
    static std::optional<AutomationTarget> fromKey (const std::string& key);
    static AutomationTarget fromValue (const Value& v);
    Value toValue() const;
};

struct AutoPoint
{
    Id id;
    Ticks pos = 0;
    double value = 0.0; // normalized
    double curve = 0.0; // -1..1
};

struct AutomationLane
{
    Id id;
    Id trackId;
    AutomationTarget target;
    std::map<Id, AutoPoint> points;
    std::vector<Id> mergedLaneIds; // derived: other stored lanes unioned into this one

    static AutomationLane fromValue (const Id& id, const Value& v);
    Value toValue() const;
};

struct TimeRange
{
    bool enabled = false;
    Ticks start = 0;
    Ticks end = 3840 * 4;

    static TimeRange fromValue (const Value& v, TimeRange fallback);
    Value toValue() const;
};

struct MetronomeSettings
{
    bool enabled = true;
    double levelDb = 0.0;
    std::string during = "both";   // record | play | both (D-W1-10: clicks whenever the metronome is on)
    int countInBars = 1;           // W-1: default 1 bar; see countInChoices

    static MetronomeSettings fromValue (const Value& v);
    Value toValue() const;
};

/** Count-in lengths in menu order (W-35, D-UI-42): none, 1 to 6 bars, then -1 and -2 for one or two beats. */
inline constexpr std::array<int, 9> countInChoices { 0, 1, 2, 3, 4, 5, 6, -1, -2 };
inline constexpr int minCountIn = -2, maxCountIn = 6;
/** The choice's index in countInChoices; 0 (none) for a value that isn't one. */
int countInIndex (int countIn) noexcept;
/** "None", "1 bar", "3 bars", "1 beat", "2 beats". */
std::string countInName (int countIn);

/** The project's key (F-TMP-10, D-DM-20): a tonic pitch class and major or minor. */
struct KeySignature
{
    int tonic = 0; // pitch class, 0 = C ... 11 = B
    bool minor = false;

    /** "C major", "F# minor", "Bb major": sharps or flats as the key is usually written. */
    std::string name() const;
    /** Sharps (positive) or flats (negative), -6..6, as a MIDI file's key signature event wants. */
    int fifths() const noexcept;
    /** Reads what a person types: "F# minor", "f#m", "Bb", "Eb major", "A min". */
    static std::optional<KeySignature> parse (const std::string& text);
    /** The key with that many sharps (positive) or flats (negative), as in a MIDI file. */
    static KeySignature fromFifths (int fifths, bool minor) noexcept;
    static KeySignature fromValue (const Value& v);
    Value toValue() const;
    bool operator== (const KeySignature&) const = default;
};

/** Scalar project fields (collections live in ProjectModel). */
struct ProjectInfo
{
    int schemaVersion = model::schemaVersion;
    Id id;
    std::string name;
    int sampleRate = 48000;
    int recordBitDepth = 24;
    std::int64_t createdAt = 0;
    UserId createdBy;
    Id masterTrackId;
    TimeRange cycle;
    TimeRange punch;
    std::optional<Ticks> endMarker;
    MetronomeSettings metronomeDefaults;
    KeySignature key;
    bool showHiddenTracks = false; // L-TRH-10: hidden tracks get rows again (to unhide them)
    double panLawDb = -3.0;        // W-35: a mono source's level at centre pan (isValidPanLaw)
    int midiInputFilter = 0;       // W-35: midiFilter bits for live MIDI the project ignores
};

/** Pan laws Project Settings offers (W-35, D-UI-42): 0, -3, -4.5 or -6 dB at centre. */
inline constexpr std::array<double, 4> panLaws { 0.0, -3.0, -4.5, -6.0 };
bool isValidPanLaw (double db) noexcept;
/** The law's index in panLaws, or -1. */
int panLawIndex (double db) noexcept;

/** Kinds of live MIDI input the project can ignore (W-35, D-UI-42): bits of ProjectInfo::midiInputFilter. */
namespace midiFilter
{
inline constexpr int notes = 1, programChange = 2, pitchBend = 4, controllers = 8, channelPressure = 16, polyPressure = 32, all = 63;
/** Whether a filter drops a live message. Note-offs (and velocity-0 note-ons) always pass, so no note hangs. */
bool drops (int filter, int status, int data2) noexcept;
} // namespace midiFilter

bool isValidSampleRate (int rate) noexcept;
bool isValidBitDepth (int bits) noexcept;
} // namespace weft::model
