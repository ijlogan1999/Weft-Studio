#pragma once

// Typed view of the project document (spec 03). Each struct converts to and
// from the document's Value tree; fromValue is lenient (missing or malformed
// fields fall back to defaults and out-of-range values are clamped), which is
// part of read-time repair (spec 03 §10.4 item 7).

#include "weft/core/Id.h"
#include "weft/doc/Value.h"
#include "weft/time/Time.h"

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
    std::string monitor = "auto"; // auto | on | off
    std::optional<LockInfo> locked;
    ChannelStrip strip;
    std::string automationMode = "read"; // off | read | touch | latch | write (F-AUT-5: read is default)

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
};

struct RegionQuantize
{
    Ticks grid = 240;
    double strength = 1.0; // 0..1
    double swing = 0.0;    // 0..1 (UI shows 0-100%)
};

struct MidiRegionData
{
    Id sequenceId;
    Ticks sequenceOffset = 0;
    std::optional<RegionQuantize> quantize;
    int transpose = 0;
    int velocityOffset = 0;
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
    std::string during = "record"; // record | play | both (W-1: on during record, off during play)
    int countInBars = 1;           // W-1: default 1 bar

    static MetronomeSettings fromValue (const Value& v);
    Value toValue() const;
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
};

bool isValidSampleRate (int rate) noexcept;
bool isValidBitDepth (int bits) noexcept;
} // namespace weft::model
