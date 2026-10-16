# 03 · Data Model

The Weft project document schema. This describes *what* is stored and its invariants, independent of the CRDT library chosen in Phase 2. Section 10 explains how the model maps onto a CRDT (maps, lists, registers) so that concurrent edits merge cleanly; `05-collaboration-spec.md` covers sync.

Notation: TypeScript-like types for readability only. `?` means optional. All IDs are globally unique strings (see §1.2).

---

## 1. Foundations

### 1.1 Time

| Type | Meaning |
|---|---|
| `Ticks` | int64. Musical time at **960 PPQN** from project start (tick 0 = bar 1 beat 1). |
| `Samples` | int64. Absolute time in samples at the project sample rate. |
| `Seconds` | float64. Used only in UI/conversion, never stored for positions. |
| `Db` | float32 decibels; `-Infinity` is stored as the sentinel `-200.0`. |
| `Normalized` | float32 in [0, 1], used for plugin parameters (VST3 convention). |

Conversion between ticks and samples always goes through the tempo map (§7). The project sample rate is fixed once the project has any recorded or imported audio (MVP).

### 1.2 Identifiers

- `Id` = a 128-bit random value rendered as 26-char Crockford base32 (ULID-style without the time prefix requirement). Generated on the creating client; collisions are treated as impossible.
- IDs never change, even when objects move between tracks.
- `UserId` comes from the relay account (stable per account). Offline-only projects use a locally generated `UserId` that is linked to an account if the project is later shared.

### 1.3 Colors

`Color` = index into Weft's 24-swatch palette (0–23) or `#RRGGBB`. Palette indices let the theme remap colors for light/dark mode.

---

## 2. Project

```ts
Project {
  schemaVersion: int              // starts at 1; migrations are forward-only
  id: Id
  name: string
  sampleRate: 44100 | 48000 | 88200 | 96000
  recordBitDepth: 16 | 24 | 32    // 32 = float
  createdAt: timestamp, createdBy: UserId

  tracks: Map<Id, Track>          // all tracks incl. buses and master
  trackOrder: List<Id>            // display order in tracks area/mixer (excludes master)
  masterTrackId: Id

  regions: Map<Id, Region>        // all regions, flat; region.trackId says where
  takeFolders: Map<Id, TakeFolder>
  midiSequences: Map<Id, MidiSequence>
  media: Map<BlobHash, MediaRef>

  tempoMap: TempoMap
  signatureMap: SignatureMap
  markers: Map<Id, Marker>
  cycle: { enabled: bool, start: Ticks, end: Ticks }
  punch: { enabled: bool, start: Ticks, end: Ticks }
  endMarker: Ticks?

  metronomeDefaults: MetronomeSettings   // per-user settings start from this (D-DM-12)
  automationLanes: Map<Id, AutomationLane>

  collab: CollabMeta              // §9
}
```

**Invariants**
- Exactly one track has `kind = master`, and `masterTrackId` points to it.
- Every `Id` in `trackOrder` exists in `tracks`; every non-master track appears in `trackOrder` exactly once (repaired on load, see §10.4).
- `cycle.start < cycle.end`, `punch.start < punch.end`.

**Decision D-DM-1:** regions are stored in one flat map with a `trackId` field rather than nested inside tracks. Moving a region between tracks is then a single register write, which merges cleanly under concurrency (a nested model would need delete + insert and could duplicate the region if two users moved it at once).

---

## 3. Track

```ts
Track {
  id: Id
  kind: 'audio' | 'instrument' | 'bus' | 'master'   // v2: 'folder', 'externalMidi'
  name: string
  color: Color
  channels: 1 | 2               // audio: per user choice; instrument/bus/master: 2
  input: InputAssignment?       // audio: hardware input channel(s); instrument: MIDI filter
  output: OutputAssignment      // { kind: 'master' } | { kind: 'bus', trackId: Id }
  mute: bool
  solo: bool                    // LOCAL ONLY (D-COL-7)
  recordEnabled: bool           // LOCAL ONLY, see below
  monitor: 'auto' | 'on' | 'off'
  heightPx: int                 // LOCAL ONLY
  locked: LockInfo?             // §9.3
  strip: ChannelStrip           // §5
  automationMode: 'off'|'read'|'touch'|'latch'|'write'
  visibleLaneIds: List<Id>      // LOCAL ONLY
}

InputAssignment =
  | { kind: 'audio', firstChannel: int }               // 0-based hw input; stereo uses firstChannel+1
  | { kind: 'midi', device: string | 'all', channel: 1..16 | 'all' }
```

**Local-only fields.** Fields marked LOCAL ONLY live in per-user view state (§8), not in the shared document: record-enable, solo, track height, which lanes are visible. **Decision D-DM-2:** arming is local, because one collaborator arming a track must not start recording on another's machine. Hardware input assignments *are* shared but each client maps them to its own hardware (see 05 §10).

**Invariants**
- `output` never forms a cycle (§5.3).
- `kind='master'` has `output = hardware` (implicit) and no sends.
- Bus and master tracks have no regions.

---

## 4. Regions, takes, and sources

### 4.1 Region (common)

```ts
Region {
  id: Id
  trackId: Id
  kind: 'audio' | 'midi'
  start: Ticks                  // timeline position of region start
  length: RegionLength          // see below
  name: string?                 // default derived from source
  color: Color?                 // null = inherit track color
  muted: bool
  z: int                        // stacking order on its track; higher plays on top
  loop: { enabled: bool, endTicks: Ticks }?    // loop repeats until endTicks
  takeFolderId: Id?             // set if this region is a take
  audio?: AudioRegionData
  midi?: MidiRegionData
  createdBy: UserId, modifiedBy: UserId, modifiedAt: timestamp
}
```

**Length.** MIDI regions store `length` in `Ticks`. Audio regions store `length` in `Samples`, because audio plays at fixed speed regardless of tempo in MVP. **Decision D-TMP-1:** in MVP an audio region is anchored at its start tick; its end position in ticks is derived from the tempo map. If the tempo changes, an audio region keeps its duration in seconds and its start stays on the same bar/beat. "Follow tempo" (stretching) is v2 (F-TMP-6).

### 4.2 Audio region data

```ts
AudioRegionData {
  media: BlobHash               // source file
  sourceOffset: Samples         // first sample of source used
  gainDb: Db                    // region gain, default 0
  fadeIn:  Fade                 // { length: Samples, curve: 'linear'|'equalPower'|'sCurve' }
  fadeOut: Fade
}
```

Invariants: `sourceOffset ≥ 0`; `sourceOffset + length ≤ media.lengthSamples`; `fadeIn.length + fadeOut.length ≤ length`.

### 4.3 MIDI region data

```ts
MidiRegionData {
  sequenceId: Id                // events live in a MidiSequence (shared by aliases in v2)
  sequenceOffset: Ticks         // event at this sequence tick plays at region.start
  quantize: { grid: Ticks, strength: 0..1, swing: 0..1 } | null
  transpose: int                // semitones, default 0 (v2 UI; field present from v1)
  velocityOffset: int           // default 0 (v2 UI)
}
```

A region plays events whose sequence position `p` satisfies `sequenceOffset ≤ p < sequenceOffset + length`. Notes that start inside and extend past the region end are cut at region end during playback (a note-off is sent at the region end). **Decision D-DM-3:** MIDI events live in a separate `MidiSequence` object so that loop repeats and v2 aliases can share data without copying notes.

**Decision D-DM-3a:** splitting a region copies the events in the right-hand window into a new `MidiSequence`, so the two halves are independent afterwards. (With a shared sequence, dragging a note across the split point in one half would make it appear in the other, which users would find surprising.)

### 4.4 Take folder

```ts
TakeFolder {
  id: Id
  trackId: Id
  start: Ticks, endTicks: Ticks
  takeIds: List<Id>             // ordered oldest → newest; regions with takeFolderId = this.id
  activeTakeId: Id
  expanded: bool                // LOCAL ONLY
  // v2: comp: List<{ takeId: Id, from: Ticks, to: Ticks }>
}
```

Only the active take plays. Non-active takes are regions with `takeFolderId` set; they are not drawn on the main lane.

### 4.5 Media reference

```ts
MediaRef {
  hash: BlobHash                // "b3:" + 64 hex chars (BLAKE3-256 of file bytes)
  originalName: string
  format: 'wav'|'aiff'|'flac'|'mp3'
  sampleRate: int, channels: int, lengthSamples: Samples, bitDepth: int
  sizeBytes: int64
  addedBy: UserId, addedAt: timestamp
}
```

Imported compressed files (MP3, FLAC) are decoded to WAV on import and the WAV is what's hashed and stored. Files at a different sample rate are converted to the project rate on import. **Decision D-DM-4:** normalizing all media to project-rate WAV keeps the audio engine simple (no realtime decoding or SRC) and makes blob hashes deterministic on every client.

---

## 5. Channel strip and plugin chain

### 5.1 Channel strip

```ts
ChannelStrip {
  volumeDb: Db                  // −200 (−inf) .. +6, default 0
  pan: float                    // −1 .. +1, default 0
  instrument: PluginInstance?   // instrument tracks only
  inserts: List<PluginInstance> // ordered; max 15 in MVP
  sends: List<Send>             // ordered; max 8
}

Send {
  id: Id
  targetBusId: Id               // a track with kind='bus'
  levelDb: Db                   // default 0
  mode: 'pre' | 'post'          // default 'post'
  muted: bool
}
```

### 5.2 Plugin instance

```ts
PluginInstance {
  id: Id
  format: 'builtin' | 'vst3'    // later: 'clap'
  pluginUid: string             // builtin: "weft.eq" etc.; vst3: 32-hex class ID
  name: string, vendor: string, version: string
  bypass: bool
  state: BlobHash | bytes       // opaque plugin state; > 64 KB stored as blob (§4.5 style)
  params: Map<ParamId, Normalized>   // last-known values for automation display; source of truth is `state`
  editorOpen: bool              // LOCAL ONLY
}
```

**Decision D-DM-5:** plugin state is opaque bytes, so concurrent edits to the *same plugin's* internal state cannot be merged; the CRDT treats `state` as a single last-writer-wins register. Concurrent edits to *different* plugins, or to automatable parameters recorded in `params`, merge normally. To reduce conflicts, the client writes `state` only when the user closes the plugin window or after 2 s without parameter changes, and writes individual `params` entries on every parameter change.

**Decision D-DM-20:** state over 64 KB is stored in the project folder as `Blobs/<hash>.bin`, named by the lowercase hex of its BLAKE3 hash, and `state` holds the `"b3:<hex>"` hash string; loading checks the hash. Blobs travel with the project like media.

**Decision D-DM-21:** a VST3 `pluginUid` is the 32-hex-digit class ID of the plug-in's audio processor class, as its factory reports it. A hosted plug-in's `ParamId` is its VST3 parameter ID written as a decimal string; built-ins use readable ids (`"threshold"`). When an undo removes a parameter from `params`, the plug-in goes back to what `state` says (or the parameter's default if there is no state).

### 5.3 Routing graph

Nodes: every track's channel. Edges: `output` assignments and sends. The graph must be a DAG. Any edit that would create a cycle is rejected locally. Because concurrent edits from two users could combine into a cycle (A→B by one user, B→A by another), the engine runs a **cycle breaker** on every document update: if a cycle exists, the edge belonging to the most recently modified send/output (by CRDT timestamp, ties by `Id`) is treated as disabled (silent) and flagged in the UI with "Disabled: feedback loop". **Decision D-DM-6.** The document is not mutated by the breaker; the user resolves it.

---

## 6. MIDI events

```ts
MidiSequence {
  id: Id
  notes: Map<Id, Note>          // map, not list: order is by position, computed
  controllers: Map<Id, CcEvent>
  // sysex: v2
}

Note {
  id: Id
  pos: Ticks                    // relative to sequence start
  length: Ticks                 // ≥ 1
  pitch: 0..127
  velocity: 1..127
  releaseVelocity: 0..127       // default 64
  channel: 1..16
  muted: bool
}

CcEvent {
  id: Id
  pos: Ticks
  kind: 'cc' | 'pitchBend' | 'channelPressure' | 'polyPressure' | 'programChange'
  number: 0..127                // CC number, or note for polyPressure; unused otherwise
  value: int                    // cc: 0..127; pitchBend: −8192..8191; pressure: 0..127
  channel: 1..16
}
```

Notes and CCs each have their own ID so that two users editing different notes never conflict, and two users editing the same note's different fields (one changes pitch, one velocity) both win. **Decision D-DM-7:** notes in a map keyed by ID, sorted at read time, instead of a CRDT list. Ordering by time is a property of the data, so list-position semantics would only add conflicts.

Overlapping notes of the same pitch and channel are allowed in the model. At playback, if a note-on arrives for a pitch already sounding on that channel, the engine sends a note-off first. **Decision D-DM-8.**

---

## 7. Tempo and signature maps

```ts
TempoMap {
  points: Map<Id, TempoPoint>
}
TempoPoint { id: Id, pos: Ticks, bpm: float64 /* 20..999 */, curve: 'step' /* v2: 'linear' */ }

SignatureMap {
  points: Map<Id, SignaturePoint>
}
SignaturePoint { id: Id, bar: int /* 0-based */, numerator: 1..32, denominator: 1|2|4|8|16|32 }
```

**Invariants and repair**
- There is always a point at `pos = 0` (tempo) and `bar = 0` (signature). A special fixed id `"initial"` is used for each so two users can't create two different initial points.
- Points are sorted by `pos`/`bar` at read time. If two points share a position (possible after concurrent inserts), the one with the greater `Id` wins and the other is ignored at read time (still stored; the UI shows a small conflict badge and lets the user delete one). **Decision D-DM-9.**

**Tick → sample conversion (step tempo).** For tempo segment *i* starting at tick `T_i` with tempo `bpm_i` and sample offset `S_i`:
```
samplesPerTick_i = sampleRate × 60 / (bpm_i × 960)
S_{i+1} = S_i + (T_{i+1} − T_i) × samplesPerTick_i
sample(t) = S_i + (t − T_i) × samplesPerTick_i     for T_i ≤ t < T_{i+1}
```
Results are computed in float64 and rounded to the nearest sample only at the point of use. **Decision D-DM-19:** adding a tempo or signature change where one already exists (same tick or bar) changes that point instead of adding a second one; the first tempo and signature (id `"initial"`) can be changed but not moved or deleted. The inverse (sample → tick) uses the same segments.

**Bar/beat ↔ ticks.** A bar of `n/d` lasts `n × (4/d) × 960` ticks. Signature changes are only at bar starts, so bar *b*'s start tick is the sum of preceding bar lengths. Signature points are stored by bar number, so changing a meter shifts later bar numbers. **Decision D-DM-10:** when a signature point is inserted or changed, existing regions keep their tick positions (they don't jump to stay on the same bar number). Users who want content to follow bar numbers re-place it. This is simplest and avoids moving every object in a CRDT transaction.

---

## 8. Markers, comments, automation, metronome, view state

### 8.1 Marker
```ts
Marker { id: Id, pos: Ticks, name: string, color: Color?, createdBy: UserId }
```

### 8.2 Automation lane
```ts
AutomationLane {
  id: Id
  trackId: Id
  target: AutomationTarget
  points: Map<Id, AutoPoint>
}
AutomationTarget =
  | { kind: 'volume' } | { kind: 'pan' } | { kind: 'mute' }
  | { kind: 'sendLevel', sendId: Id }
  | { kind: 'pluginParam', pluginId: Id, paramId: string }
AutoPoint { id: Id, pos: Ticks, value: Normalized, curve: float /* −1..1, applies to segment after this point */ }
```
- One lane per (track, target). If concurrent creation produces two lanes for the same target, the lane with the smaller `Id` is canonical and points from both are read as one set (union). **Decision D-DM-11.**
- Values are normalized: volume maps `0..1` to `−inf..+6 dB` with Weft's fader taper (defined in Phase 2 architecture and fixed thereafter, since saved automation depends on it); pan `0..1` → `−1..+1`; mute `<0.5` off, `≥0.5` on.
- Automation is anchored in ticks.
- **Decision D-DM-22:** a segment's shape is `t^(4^curve)` for `t` in 0..1 from its first point to the next: 0 is linear, +1 is `t^4` (starts slow), −1 is `t^0.25` (starts fast), and every curve in between is monotonic. The fader taper is `dB = 6 + 60·log10(v)` with `v = 0` as −inf (so 0 dB is at v ≈ 0.794 and −12 dB at v = 0.5); send levels use it too (D-ARCH-28). Two points at the same position play the one with the larger id from there on; the editors don't create such pairs.

### 8.3 Metronome
```ts
MetronomeSettings { enabled: bool, levelDb: Db, during: 'record'|'play'|'both', countInBars: 0|1|2 }
```
**Decision D-DM-12:** metronome and count-in settings are per user (local view state), because one person's click preference shouldn't change another's. The project stores only a default used when a user opens the project for the first time.

### 8.4 Per-user local view state
Stored outside the document at `%LOCALAPPDATA%/Weft/projects/<projectId>/view.json`: track heights, record-enable, solo, visible lanes, zoom, scroll, open editors, plugin windows, metronome, take folder expansion, playhead position. Never synced. **Decision D-DM-13.**

---

## 9. Collaboration metadata

```ts
CollabMeta {
  projectId: Id                 // same as Project.id
  relayUrl: string?             // null = never shared
  comments: Map<Id, Comment>
  chat: List<ChatMessage>       // append-only CRDT list
  versions: Map<Id, VersionTag> // named checkpoints (§9.4)
}
```

### 9.1 Comment
```ts
Comment {
  id: Id
  anchor: { pos: Ticks, endPos: Ticks?, trackId: Id?, regionId: Id? }
  parentId: Id?                 // reply to another comment; replies are one level deep
  author: UserId, createdAt: timestamp
  body: string                  // plain text, max 4000 chars; @mentions as "@<UserId>"
  editedAt: timestamp?
  resolved: bool, resolvedBy: UserId?
  deleted: bool                 // tombstone; body cleared
}
```
If a comment's anchor region or track is deleted, the comment stays at its `pos` on the timeline (anchor falls back to time only).

### 9.2 Chat message
```ts
ChatMessage { id: Id, author: UserId, sentAt: timestamp, body: string /* max 4000 */, deleted: bool }
```
Chat lives in the document so it is available offline and in history. **Decision D-DM-14.** Ordering: the UI shows messages in CRDT list order, which is the same on every client; `sentAt` is displayed but not used for sorting.

### 9.3 Lock info
```ts
LockInfo { by: UserId, at: timestamp, note: string? }
```
Locks are advisory but enforced by every compliant client (05 §7).

### 9.4 Version tag
```ts
VersionTag { id: Id, name: string, heads: List<ChangeHash>, createdBy: UserId, createdAt: timestamp, auto: bool }
```
`heads` identifies the exact document state (the CRDT's change-graph heads). Restoring is described in 05 §9.

---

## 10. Mapping to a CRDT

The Phase 2 architecture picks the library (Automerge 2.x via its C bindings, or Yjs via the `yrs` Rust port; to be decided in ARCHITECTURE.md). The mapping below is library-neutral:

| Model construct | CRDT type | Merge behavior |
|---|---|---|
| Object with fields (`Track`, `Region`, `Note`...) | Map | Each field is an independent last-writer-wins (LWW) register. Concurrent writes to *different* fields both apply. |
| `Map<Id, X>` collections | Map keyed by Id | Concurrent inserts of different IDs both apply. Delete vs. concurrent edit: delete wins (the object disappears), see §10.2. |
| `trackOrder`, `inserts`, `sends`, `takeIds` | List (sequence CRDT) | Concurrent inserts interleave deterministically; moves are delete + insert (see §10.3). |
| `chat` | List, append only | Concurrent appends both kept. |
| `name`, `body` strings | Register (MVP) | LWW. Collaborative character-level text editing for comments/names is not needed. **Decision D-DM-15.** |
| Numeric fields (`volumeDb`, `pan`...) | Register | LWW, not counters: a fader is set to a value, not incremented. |

### 10.1 Transactions
One user gesture = one CRDT change (transaction) containing all field writes for that gesture. A change is the unit of sync, undo, and attribution.

### 10.2 Delete vs. edit
If user A deletes a region while user B moves it, the region is deleted (B's move edits a field of an object no longer reachable). B sees the region disappear and a status toast "Region deleted by A". **Decision D-DM-16:** deletes win; version history makes recovery possible.

Exception: if A deletes a **track** while B adds a region to it, B's region would be orphaned (its `trackId` points to a missing track). Orphan repair on read: regions whose track is missing are shown on an auto-created read-only "Recovered" lane until the user moves or deletes them; the first client to notice creates a real track named "Recovered" in a change, using a deterministic ID derived from the deleted track's ID so two clients don't both create one. **Decision D-DM-17.**

### 10.3 List moves
Reordering `trackOrder` or `inserts` is delete + insert. If two users move the same item concurrently, the item could appear twice. Read-time repair: duplicate IDs in a list keep only the first occurrence; missing IDs are appended at the end. Phase 2 may use a library-native move op if available. **Decision D-DM-18.**

### 10.4 Read-time repair summary
All repairs are deterministic functions of the document state so every client shows the same result:
1. Duplicate list entries: keep first.
2. Track in `tracks` but not in `trackOrder`: append, sorted by Id.
3. Orphan regions: Recovered lane (D-DM-17).
4. Duplicate tempo/signature points at one position: greater Id wins (D-DM-9).
5. Routing cycles: disable newest edge (D-DM-6).
6. Duplicate automation lanes: union (D-DM-11).
7. Out-of-range values (e.g. fades longer than region after concurrent trim + fade edit): clamp at read time, and the next local edit writes the clamped value.

### 10.5 Schema versioning
`schemaVersion` is an integer. A client opening a document with a higher version than it supports refuses (D-W10-4). Migrations run once by the first client of the new version to open the document and are written as an ordinary change, so older clients connected to the same relay are told to update (relay checks `schemaVersion`, see 05 §3).

---

## 11. Example (abridged JSON view of a small project)

```json
{
  "schemaVersion": 1,
  "id": "01J9Z3W5Q7K8M2N4P6R8T0V2X4",
  "name": "Demo",
  "sampleRate": 48000,
  "recordBitDepth": 24,
  "masterTrackId": "MASTER00000000000000000000",
  "trackOrder": ["TRKA...", "TRKB...", "BUS1..."],
  "tracks": {
    "TRKA...": { "kind": "audio", "name": "Vox", "channels": 1,
                 "input": {"kind":"audio","firstChannel":0},
                 "output": {"kind":"master"},
                 "strip": { "volumeDb": -3.0, "pan": 0.0, "inserts": [], 
                            "sends": [{"id":"SND1...","targetBusId":"BUS1...","levelDb":-12.0,"mode":"post","muted":false}] } },
    "TRKB...": { "kind": "instrument", "name": "Keys",
                 "strip": { "instrument": {"format":"builtin","pluginUid":"weft.keys"} } },
    "BUS1...": { "kind": "bus", "name": "Reverb",
                 "strip": { "inserts": [{"format":"builtin","pluginUid":"weft.reverb"}] } }
  },
  "regions": {
    "RGN1...": { "trackId": "TRKA...", "kind": "audio", "start": 3840, "length": 480000, "z": 1,
                 "audio": { "media": "b3:9f2c...", "sourceOffset": 0, "gainDb": 0,
                            "fadeIn": {"length": 480, "curve": "equalPower"},
                            "fadeOut": {"length": 4800, "curve": "equalPower"} } },
    "RGN2...": { "trackId": "TRKB...", "kind": "midi", "start": 0, "length": 15360, "z": 1,
                 "midi": { "sequenceId": "SEQ1...", "sequenceOffset": 0, "quantize": null } }
  },
  "midiSequences": { "SEQ1...": { "notes": {
      "N1...": {"pos": 0, "length": 480, "pitch": 60, "velocity": 100, "channel": 1, "muted": false} } } },
  "tempoMap": { "points": { "initial": {"pos": 0, "bpm": 120.0, "curve": "step"} } },
  "signatureMap": { "points": { "initial": {"bar": 0, "numerator": 4, "denominator": 4} } }
}
```

---

## Decisions in this file

| ID | Decision |
|---|---|
| D-DM-1 | Regions stored flat with `trackId`. |
| D-DM-2 | Record-enable is local, not shared. |
| D-TMP-1 | Audio anchored at start tick, plays at fixed speed (no stretch) in MVP. |
| D-DM-3 / 3a | MIDI events in separate sequences; split copies into a new sequence. |
| D-DM-4 | All media normalized to project-rate WAV on import. |
| D-DM-5 | Plugin state is an LWW blob; params written individually. |
| D-DM-6 | Concurrent routing cycles broken at read time by disabling newest edge. |
| D-DM-7 | Notes in a map keyed by ID, not a list. |
| D-DM-8 | Same-pitch overlaps retrigger with a note-off first. |
| D-DM-9 | Duplicate tempo/signature points: greater Id wins. |
| D-DM-10 | Signature changes don't move content in ticks. |
| D-DM-11 | Duplicate automation lanes are unioned. |
| D-DM-12 | Metronome settings are per user. |
| D-DM-13 | View state is local, never synced. |
| D-DM-14 | Chat lives in the document. |
| D-DM-15 | Strings are LWW registers, not collaborative text. |
| D-DM-16 | Delete beats concurrent edit. |
| D-DM-17 | Orphaned regions go to a deterministic Recovered track. |
| D-DM-18 | List moves are delete+insert with dedupe repair. |
| D-DM-19 | Adding a tempo/signature change at an existing position edits that point. |
| D-DM-20 | Plugin state over 64 KB lives in `Blobs/<hash>.bin`; `state` holds the hash. |
| D-DM-21 | VST3 uid is the 32-hex class ID; hosted ParamIds are VST3 parameter IDs; removed params revert to state. |
| D-DM-22 | Segment curve shape and the fader taper formula. |
