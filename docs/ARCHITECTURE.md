# Weft Studio: Architecture and Milestone Plan

Phase 2 starting document. It works from `/spec` only. It confirms or challenges the recommended stack, defines the module structure and threading model, and sets out the milestone plan. Each milestone closes with a report of what works, what's stubbed, and known issues.

**Status:** accepted. Milestone 0 is built; see §9 for the decisions made while building it.

---

## 1. Stack decision

| Area | Recommended in brief | Decision | Why |
|---|---|---|---|
| Language | C++20 | **Confirmed: C++20** | Real-time audio, VST3 SDK, and JUCE are all C++. |
| App framework (UI, audio I/O, MIDI, plugin hosting) | JUCE or Tracktion Engine | **JUCE 8, without Tracktion Engine** | See §1.1. |
| Audio drivers | WASAPI + ASIO | **Confirmed**, through JUCE's device layer | JUCE ships WASAPI (shared and exclusive) and ASIO backends. |
| Plugin format | VST3 | **Confirmed**, through JUCE's VST3 hosting (Steinberg VST3 SDK) | |
| CRDT | Automerge or Yjs style | **Automerge 2.x (Rust core), with its C API on the client and the native Rust crate in the relay** | See §1.2. |
| Relay server | WebSocket | **Rust: tokio + axum + automerge + SQLite/Postgres + S3-compatible or disk blob store** | One CRDT implementation shared by client and server. Small and fast. |
| MP3 encoding | (not specified) | **LAME 3.100 (LGPL), built from source by the `mp3lame-sys` crate and linked statically into `weft-ffi`** | MP3 patents expired in 2017. The DLL plan was for a closed-source app; Weft is AGPLv3, so static linking is fine (D-ARCH-31). |
| Hashing | BLAKE3 (from spec) | **Official BLAKE3 C implementation** (CC0 / Apache-2.0) | |
| Build | | **CMake (Ninja or Visual Studio), FetchContent with pinned tags from `third_party/cpp-deps.json`; Cargo for Rust parts** | See D-ARCH-1. |
| Tests | | **Catch2 v3** (C++), **cargo test** (relay), plus a Python/pytest harness for multi-client sync tests | |
| CI | | **GitHub Actions: `windows-latest` (full build, UI smoke tests) and `ubuntu-latest` (headless core, engine, relay)** | Keeps the build green on every push. |

### 1.1 JUCE vs. Tracktion Engine (and license flags)

**Licenses (checked October 2026; reconfirm before shipping):**
- **JUCE 8:** dual licensed, **AGPLv3** or a commercial JUCE license (free Starter tier below a revenue cap, then paid tiers). If Weft is closed source or sold, it needs a commercial JUCE license. If Weft is AGPLv3 open source, no fee applies, but the whole app must be AGPL. **This needs Ian's decision before release, not before development.**
- **Tracktion Engine:** dual licensed, **GPLv3** or a commercial license from Tracktion. It is built on JUCE, so using it means accepting *both* sets of terms (JUCE's plus Tracktion's).
- **Steinberg VST3 SDK:** **MIT** since VST 3.8 (October 2025). No longer a blocker.
- **Steinberg ASIO SDK:** now dual licensed, **GPLv3** or Steinberg's proprietary license agreement (free, requires signing). A closed-source Weft needs the proprietary agreement; a GPL/AGPL Weft can use GPLv3.
- **Automerge:** MIT. **LAME:** LGPL-2.0. **BLAKE3:** CC0/Apache-2.0. **Catch2:** BSL-1.0.

**Why not Tracktion Engine.** It would save time on the engine (playback graph, recording, plugin hosting, rendering), but:
1. Its *Edit* model is a JUCE `ValueTree` and acts as the source of truth. Weft's spec puts the source of truth in a CRDT document from day one (`03` §10, `05`). We would have to mirror every CRDT change into a ValueTree and back, which means two models drifting under concurrency.
2. It adds a second copyleft/commercial license on top of JUCE.
3. Many spec behaviors (take folders, per-user undo, read-time repair, cycle breaking, missing-plugin placeholders that preserve state byte-for-byte) need control over the model anyway.

**So:** JUCE for the platform layer (windowing, graphics, audio devices, MIDI, VST3 hosting, audio file I/O), with Weft's own document model, engine graph, and render pipeline. This costs more engine work up front but keeps one model.

**Challenge noted:** if Ian prefers fastest time-to-first-sound over architectural purity, Tracktion Engine is the alternative. I recommend against it for the reasons above.

### 1.2 CRDT library

| | Automerge 2.x | Yjs / yrs |
|---|---|---|
| Core | Rust, with C API (`automerge-c`) | JS (Yjs) / Rust port (yrs) with C FFI |
| Model fit | JSON-like maps, lists, text, counters; per-field LWW maps match `03` §10 directly | Similar types; maps are LWW |
| History / version at heads | First class: full change graph, view document at any heads, diff between heads | Snapshots need GC disabled; less direct |
| Sync protocol | Built-in sync protocol (bloom-filter based) | State-vector exchange |
| Server | Native Rust crate | Native Rust crate (yrs) |

**Decision: Automerge.** Version history with preview and restore (F-COL-15, `05` §9.3) is a core requirement, and Automerge's "view at heads" plus diff make it straightforward. Per-user undo (`05` §6) can be built from patches/diffs between heads.

**Risk:** document size and load time with large note counts. Mitigation: the spec target (`05` §3.5: 200k notes, ≤ 3 s load) gets a benchmark in Milestone 1; fallback is storing note batches as compact binary blobs per sequence with coarser merge granularity.

---

## 2. Module structure

```
weft/
├─ core/            C++ library, no JUCE GUI, no platform audio. Builds and tests on Linux and Windows.
│  ├─ doc/          CRDT wrapper (over crates/weft-crdt), typed accessors for the 03 schema, read-time repair, migrations
│  ├─ model/        Immutable projected model (Project, Track, Region...) rebuilt incrementally from doc patches
│  ├─ commands/     Edit commands → one CRDT transaction each; per-user undo/redo (05 §6)
│  ├─ time/         Tempo/signature maps, tick↔sample conversion, quantize/swing math
│  ├─ routing/      Routing graph, cycle detection and read-time breaker
│  └─ media/        BLAKE3 hashing, media index, import normalization (decode, resample)
├─ engine/          Real-time engine. Depends on core + juce_audio_basics/juce_audio_formats/juce_audio_processors
│  ├─ graph/        Compiled render graph snapshots, PDC, channel processing
│  ├─ transport/    Play/record/cycle/punch, metronome, count-in
│  ├─ disk/         Disk streaming readers, recording writers, lock-free FIFOs
│  ├─ plugins/      VST3 + built-in processor hosting, placeholders, scanning (out-of-process scanner exe)
│  ├─ dsp/          Built-in instrument (Weft Keys) and effects (EQ, compressor, delay, reverb, utility)
│  ├─ render/       Offline/real-time bounce, dither, WAV writer, LAME wrapper
│  └─ devices/      Device abstraction: JUCE devices (WASAPI/ASIO) and NullDevice for tests
├─ collab/          Client sync: WebSocket transport, sync protocol, presence, blob upload/download, auth
├─ app/             JUCE GUI application (Windows primary): main window, arrange, piano roll, mixer, panels
├─ relay/           Rust: relay server (member of the root Cargo workspace)
├─ crates/weft-crdt/ Rust: C API over Automerge (D-ARCH-2)
├─ crates/weft-codec/ Rust: C API over LAME, and the MP3 decoder tests use (D-ARCH-31)
├─ crates/weft-ffi/  Rust: the one static library linked into the client (D-ARCH-32)
├─ cli/             weft-cli headless driver
├─ tools/scanner/   Out-of-process plugin scanner
├─ tests/
│  ├─ unit/         Catch2 [U]
│  ├─ engine/       Catch2 [E] with NullDevice and fixture plugins
│  ├─ sync/         pytest [S]: spawns relay + headless clients (weft-cli)
│  ├─ ui/           [UI] Windows-only automation
│  └─ fixtures/     TestGain, TestLatency, TestSynth, TestCrashScan VST3 sources; signal files generated at build time
└─ docs/            ARCHITECTURE.md, MILESTONES.md, THIRD_PARTY.md, milestone reports
```

A headless **`weft-cli`** binary (core + engine + collab, NullDevice) drives engine and sync tests and lets the Linux container exercise almost everything except Windows drivers and the GUI.

---

## 3. Threading and real-time safety

Threads:
1. **Message (UI) thread:** owns the CRDT document, applies local commands and remote changes, rebuilds the projected model, compiles engine snapshots.
2. **Audio thread:** the driver callback. Reads only from the current immutable engine snapshot and lock-free queues.
3. **Disk threads (2):** read-ahead for playback streams; writing recorded audio.
4. **Network thread:** WebSocket I/O and blob transfers (HTTP), never touching the audio thread.
5. **Worker pool:** peak file generation, hashing, import decoding, offline bounce.

**Snapshot handoff (the central rule).**
- Every document change produces a new immutable `EngineSnapshot` (render graph, per-region playback descriptors, automation curves, tempo map) on the message thread. Unchanged parts are shared via reference counting, so a small edit is cheap.
- Publish: an atomic pointer swap. The audio thread loads the pointer once at the start of each callback.
- Retire: old snapshots go to a lock-free "retire queue". The message thread frees them after the audio thread has acknowledged a newer epoch (an atomic counter). **The audio thread never frees memory.**
- Plugin instances live outside snapshots (they're stateful) in a slot table. Adding/removing plugins is a two-phase commit: prepare on the message thread, then swap via snapshot, then release after the epoch passes.

**Audio-thread rules (enforced by AT-RT-1):** no locks, no allocation, no I/O, no system calls that may block, no exceptions. All buffers are preallocated to the max block size. Parameter changes and transport commands arrive through single-producer single-consumer FIFOs. Meters and playhead position go back through atomics/FIFOs.

**Debug enforcement:** in test builds, a thread-local "audio thread" flag plus overridden global `operator new/delete` and wrapped mutexes record violations, and AT-RT-1 asserts zero.

**Disk streaming:** each playing audio region has a reader with a lock-free ring buffer (≈ 2 s at 48 kHz) filled by disk threads ahead of the playhead. On a buffer underrun the audio thread outputs silence for that region and increments the disk-health counter (F-TRN-16). Media is project-rate WAV (D-DM-4), so there is no decode or resampling on the playback path.

**Recording:** the audio thread copies input into per-track lock-free FIFOs; the writer thread drains them to WAV files in `Recovery/`. On stop, files are finalized, latency-shifted by trimming the region offset (non-destructive), hashed, and moved into `Media/` (W-1).

---

## 4. Document flow

```
User gesture ──► Command ──► CRDT transaction (one change) ──► local doc
                                    │                              │
                                    ├──► recovery log (≤ 1 s)       ├──► patches ──► projected model ──► UI
                                    └──► collab outbox ──► relay    └──► engine snapshot compile ──► audio thread
Remote change ──► collab inbox ──► apply to doc ──► patches ──► (same path)
```

- **Commands** are the only way to edit. Each records the pre-change values of the fields it writes, which drives per-user undo (`05` §6: undo only fields that still hold my value).
- **Read-time repair** (`03` §10.4) runs in the projection step, so the projected model always satisfies invariants even when the raw document doesn't.
- **Local view state** (record-enable, solo, heights, zoom, metronome prefs) lives in `view.json` and never enters the document.

---

## 5. Collaboration implementation notes

- Relay protocol exactly as in `05` §3.3: CBOR envelopes, Automerge sync messages, presence frames.
- Auth: Argon2id passwords, JWT access tokens (15 min), refresh tokens (30 days).
- Blob store: content-addressed by BLAKE3, resumable 8 MB chunk uploads, HTTP Range downloads, membership-checked.
- Presence: ephemeral, never persisted, rate limits from `05` §5.
- **Dev and test:** the relay runs on localhost in CI with SQLite and a disk blob store. Hosted deployment is out of scope for the MVP build unless Ian wants it.

---

## 6. Platform caveat for this build environment

The build environment is a Linux container. Everything in `core/`, `engine/` (with NullDevice), `collab/`, `relay/`, and the fixture-plugin tests builds and runs here. **Windows-only parts can't run here:** WASAPI/ASIO devices, VST3 editor windows, Windows HiDPI behavior, and Windows UI automation. Those are written behind the `devices/` and plugin abstractions, compiled and tested on the `windows-latest` CI runner where possible, and the **[HW]** acceptance tests need a real Windows machine with an audio interface. Every milestone report will say which tests ran where.

---

## 7. Milestone plan

Collaboration is designed in from M1 (the document *is* the CRDT), and the network parts arrive as their own milestones once the single-user editing they sync exists. Each milestone ends with: all its acceptance tests passing in CI, a milestone report (works / stubbed / known issues), and a tag.

| # | Milestone | Main spec features | Acceptance tests |
|---|---|---|---|
| **M0** | Repo skeleton: CMake, JUCE, Automerge (via weft-crdt), Catch2, CI (Windows + Linux), NullDevice, fixture plugins, THIRD_PARTY.md | F-SYS-1 (license manifest) | AT-SYS-1 (build check part) |
| **M1** | Document core: CRDT wrapper, `03` schema, projection, read-time repair, commands, per-user undo, tempo/signature maps, local persistence + recovery log | F-COL-1, F-COL-16 (local), F-ARR-16, F-TMP-1, F-TMP-3, F-PRJ-1/3/5/7 (core) | AT-COL-1, AT-ARR-19/20, AT-TMP-1/4/5, AT-PRJ-1/2/4/9/11/12, CRDT load benchmark |
| **M2** | **Audio engine and transport:** snapshot graph, play/stop/cycle, metronome, count-in, tempo, position, CPU meter; JUCE device layer (WASAPI/ASIO) + NullDevice | F-TRN-1…11, 16, F-UI-7 (engine side) | AT-TRN-*, AT-RT-1/3, AT-TMP-2, AT-UI-5 |
| **M3** | **Audio and MIDI tracks, multitrack recording:** track types, disk streaming, recording writer, latency compensation, take folders, punch, MIDI recording, monitoring, Weft Keys | F-REC-*, F-TRK-1/2/4/6–12/17, F-PLG-9 | AT-REC-*, AT-TRK-* |
| **M4** | **Arrange view:** GUI shell (main window, tracks area, inspector, global tracks), region move/trim/split/join/fade/crossfade/gain/loop/mute, snap, markers, tools, import by drag and drop | F-ARR-*, F-EDA-1, F-UI-1/3/4/10, F-LIB-1/2 | AT-ARR-*, AT-EDA-1, AT-UI-1/2/7 |
| **M5** | **Piano roll:** editor, note editing, velocity, controller lanes, audition, quantize (destructive + region) | F-PRL-*, F-QNT-* | AT-PRL-*, AT-QNT-* |
| **M6** | **Mixer:** strips, volume/pan/mute/solo, meters, inserts, sends, buses, routing + cycle prevention, PDC, VST3 hosting, scanner, plugin manager, built-in effects, placeholders | F-MIX-*, F-PLG-1–5/10/15 | AT-MIX-*, AT-PLG-* |
| **M7** | **Automation:** lanes, drawing, curves, modes, live write, plugin params | F-AUT-*, F-PLG-14 | AT-AUT-* |
| **M8** | **Bounce/export:** offline/real-time render, WAV, MP3 (LAME), dither, tails, range | F-EXP-* | AT-EXP-* |
| **M9** | **Save/load UX:** Open/Save/Save As dialogs, recent projects, project settings, missing media/plugins dialogs, first-run audio setup | F-PRJ-*, F-UI-8/14 | AT-PRJ-*, AT-UI-8 |
| **M10** | **Relay and live sync:** Rust relay, auth, roles, rooms, sync protocol, offline merge, sign-in/share UI, sync status | F-COL-2–5, F-TRK-10 (local solo) | AT-COL-2–9, AT-TRK-9, AT-MIX-10, AT-TMP-7 |
| **M11** | **Presence and media sync:** presence UI, BLAKE3 blob upload/download, streaming playback of partial media, transfer UI | F-COL-6–10, F-COL-20 | AT-COL-10–18, AT-COL-31 |
| **M12** | **Comments, markers, chat, locks, version history** | F-COL-11–15, F-TRK-16 | AT-COL-19–30 |
| **M13** | Hardening: AT-RT-2 fuzzing, performance targets, Windows [HW] test pass, installer | all | AT-RT-2, [HW] set |

Ian's MVP order (1–8) is preserved as M2–M9. **One change, flagged:** save/load (Ian's #8) has its *core* in M1, because the CRDT document is the save format and every later milestone's tests need it; M9 adds the user-facing dialogs.

---

## 8. Open decisions for Ian

1. ~~License model for Weft~~ **Decided 2026-10-14 by Ian: open source.** Weft will be released under **AGPLv3** (required by JUCE's open-source license; ASIO SDK used under GPLv3, which may be combined with AGPLv3; VST3 SDK MIT; LAME LGPL as a DLL). A donation link is planned in the app and README.
2. ~~GitHub repository~~ **Decided 2026-10-14:** `ijlogan1999/weft-studio`.
3. **Hosting the relay:** localhost/self-hosted only for MVP (default), or a hosted deployment.

---

## 9. Build-time decisions

- **D-ARCH-1 (M0): FetchContent instead of vcpkg.** The C++ dependencies are few (JUCE, Catch2, Corrosion) and all build from source with CMake, so CMake's FetchContent with pinned tags is simpler than a vcpkg manifest and works the same on Windows, Linux and CI. `third_party/cpp-deps.json` is both the fetch list and the license manifest: `weft_fetch()` refuses any dependency not listed there (AT-SYS-1). vcpkg can be added later if a dependency needs it.
- **D-ARCH-2 (M0): own C API crate instead of automerge-c.** automerge-c's release build needs a nightly Rust toolchain (`build-std`). Weft instead links a small crate, `crates/weft-crdt`, that wraps the `automerge` Rust crate behind a C API (`weft_crdt.h`) and grows with each milestone. Benefits: client and relay share one Cargo workspace, so they always use the same Automerge version (`Cargo.lock`); the C API exposes exactly the operations the spec 03 schema needs. Corrosion builds the crate from CMake.
- **D-ARCH-3 (M0): JUCE 8.0.9, Ian confirmed JUCE (2026-10-14).** Ian asked whether Weft should write its own platform layer instead; he chose to keep JUCE. Every JUCE use stays behind Weft interfaces (`engine/devices`, plugin hosting, file I/O) so parts can be replaced later. JUCE 9 exists; moving to it is a later, separate change after rechecking its license.
- **D-ARCH-4 (M0): ASIO arrives in M2.** JUCE's ASIO backend needs Steinberg's ASIO SDK headers, which are not in JUCE. M2 adds them (GPLv3 option) with the JUCE device layer. M0 builds with WASAPI only.
- **D-ARCH-5 (M0): Fixture plugin `TestCrashScan` crashes with a null write** in its factory function rather than `abort()`, which would show a modal dialog in MSVC debug builds and hang CI.
- **D-ARCH-6 (M1): a gesture is one pending CRDT transaction.** `Session::beginGesture` opens it, every update (e.g. 200 mouse moves) writes into it, and `endGesture` commits one change (AT-ARR-20); `cancelGesture` rolls it back. Remote changes must not be applied while a gesture is open: M10 queues them and applies them after the gesture ends.
- **D-ARCH-7 (M1): how undo recognises "still mine" (05 §6, D-COL-6).** Each change keeps a record per written field: path, value before, value after. Undo restores a field only if it still holds the value this change wrote. For an object (a map or list written whole, such as a new region) "still holds" compares the CRDT op id of the object, so a collaborator's edit inside an object I created doesn't stop me undoing its creation. Undo and redo re-create deleted objects with new op ids; the session keeps an alias from the old op id to the new one so older undo records still recognise them. List entries (track order, plug-ins, sends) are recorded as insert/remove of an element id. Redo is the undo of the undo, under the same rule.
- **D-ARCH-8 (M1): loading skips change-hash verification.** Automerge can re-hash every change on load; that is about a third of the load time for a 200,000-note project and adds no protection the file's chunk checksum doesn't already give. Changes received over the network are still verified (M10).
- **D-ARCH-9 (M1): the recovery log is flushed, not fsynced, after each change.** A killed or crashed process loses nothing (AT-PRJ-11). A power cut can lose the last few changes in the log; the saved project file itself is written with fsync and an atomic rename (AT-PRJ-4). fsync per edit would make every fader gesture wait on the disk.
- **D-ARCH-10 (M1): split rebases the right-hand notes.** Notes that start in the right-hand window move into the new sequence (D-DM-3a) with positions relative to the split point, so the new region has `sequenceOffset = 0`; note ids are kept. Crossing notes stay whole in the left region (D-W3-3). Joining copies notes back with new ids, since ids only need to be unique within a sequence.
- **D-ARCH-11 (M1): orphaned regions show on a synthetic Recovered track until a client writes it.** The projection shows orphan regions on a read-only lane whose id is derived from the deleted track's id (D-DM-17). `Session::materializeRecoveredTracks` writes the real track and points the regions at it; two clients doing this at once write the same id, so one track results. M10 calls it when remote changes produce orphans.
- **D-ARCH-12 (M2, replaced by D-ARCH-17 in M3): media plays from memory until disk streaming lands (M3).** `MediaPool` decodes each project WAV fully into memory and snapshots share it. That is enough for the transport tests and short projects; M3 replaces it with the streaming readers in §3 without changing the snapshot format (segments point at a media source).
- **D-ARCH-13 (M2): count-in is click only, and it always clicks.** During a count-in the playhead runs from the count-in start to the record position with regions silent; playback and recording start together at the record position (AT-TRN-11/12). The count-in clicks sound even when the metronome is off for recording, because a silent count-in is useless; they use the metronome level.
- **D-ARCH-14 (M2): ASIO is built only when `WEFT_ASIO_SDK_DIR` is set.** JUCE's ASIO driver needs Steinberg's ASIO SDK headers, which Weft doesn't fetch or redistribute. Windows builds without it offer WASAPI shared (default, D-W0-2) and exclusive. Release builds point `WEFT_ASIO_SDK_DIR` at the SDK, used under its GPLv3 option.
- **D-ARCH-15 (M2): sample-rate mismatch uses cubic interpolation for now.** When the device can't run at the project rate (D-W0-1), segments read media with Catmull-Rom interpolation and recording is refused with a status message. A windowed-sinc resampler replaces the cubic one before release (M13); the snapshot only stores the step, so the swap is local.
- **D-ARCH-16 (M2): the engine counts in device samples.** The transport position, cycle and record points are device-rate samples, converted from ticks when a snapshot is compiled. A tempo edit during playback keeps the sample position, so the playhead's bar/beat moves with the new tempo. Since M3 the cycle also wraps while recording, and each pass becomes a take (W-1c). Muted regions neither play nor hide regions under them.
- **D-ARCH-17 (M3): disk streaming is a block cache per media file, not a ring buffer per region.** Each `MediaSource` splits its file into 32,768-frame blocks of planar float behind atomic pointers. One disk thread (`MediaStreamer`) loads the blocks that segments need for the next 2 s after the playhead, after a play or locate target, and after the cycle start while cycling, nearest first, and evicts the rest; an evicted block is freed only after the audio thread has finished a callback since. A missing block plays as silence and counts a disk underrun (F-TRN-16). Blocks instead of per-region rings because regions share files, loop, overlap and get jumped around by cycle and locate, and a block is valid for all of them. Files up to 30 s are read whole and never evicted, so short media and recordings never wait for the disk; offline render (`weft-cli render`) reads everything whole.
- **D-ARCH-18 (M3): the recording pipeline.** `Engine::record` builds a pass on the message thread: one lane per armed track, with a 10 s lock-free FIFO per audio lane and a preallocated event list per MIDI lane. The audio thread copies input into the FIFOs and stamps MIDI with its timeline sample; it never touches files. A writer thread drains the FIFOs into `Recovery/rec-*.wav` at the project bit depth with a `bext` chunk (description = track name, time reference = record start). On stop, `Engine::finishRecording (Session&)` closes the files, hashes each one, moves it to `Media/<hash>.wav`, and commits media, regions and take folders as one undo step "Record". Audio that doesn't fit the FIFO is dropped and reported. Recording audio needs a project folder; with none, record refuses with a message.
- **D-ARCH-19 (M3): the latency shift is a source offset.** The file keeps everything captured from the record start; the region starts at the record position with `sourceOffset` = input latency + output latency + the user's recording delay (F-REC-9). Nothing is cut from the file, so changing the offset later only moves the region's window. Autopunch moves its capture window by the same amount, so the punched region starts exactly 10 ms before punch-in with offset 0 (rounded up to the next tick, offset adjusted to stay sample-exact).
- **D-ARCH-20 (M3): autopunch with cycle on records the first pass only.** The transport keeps cycling, but capture stops at the first wrap, and the result is a plain punched region, not a take folder. MIDI recording honours the punch range too.
- **D-ARCH-21 (M3): fades don't hide what's under them.** A region's fade-in and fade-out zones leave the regions below it audible, playing the complementary curve (`1 - u` through the same curve), so stacked regions crossfade. This gives the 10 ms punch crossfades of D-W1-5 and is what the arrange view's crossfades (M4) build on.
- **D-ARCH-22 (M3): live MIDI.** `Engine::sendMidi` takes a message and the device frame it arrived at (or "next block"), through one single-producer queue, so the app funnels MIDI devices and the computer keyboard through one thread. Events arriving late play at the start of the block. The selected instrument track and every armed one receive it (D-W2-1), filtered by their MIDI channel. Live notes aren't tracked by the transport, so stopping doesn't cut a held key; releasing it does.
- **D-ARCH-23 (M4): explicit crossfades aren't ducked twice.** Inside a higher region's fade zone, a lower region that has its own fade over the same span (a crossfade the user made) plays through its own fade only; D-ARCH-21's complementary curve applies only to regions without one. An equal-power crossfade then peaks at +3 dB at its midpoint as specified (AT-ARR-14).
- **D-ARCH-24 (M4): the window is a library and is tested headless.** Everything in the window lives in `weft_app_lib`; the app adds only `Main.cpp`. `weft_ui_tests` links the library, runs one JUCE GUI session for the whole run, and drives components through their `pointerDown/Drag/Up` methods (or synthesized mouse events) without putting a window on screen. On Linux, ctest runs it under `xvfb-run`.
- **D-ARCH-25 (M6): the plug-in scanner reports through a file.** The app runs `weft-plugin-scanner --scan <file> --out <result>` for each new or changed VST3 file and waits up to 60 s. The scanner loads every audio class, processes one silent block, writes one tab-separated `PLUGIN` line per class and then `DONE`. A nonzero exit, a timeout or a result without `DONE` marks the file failed (a crash after a signal can look like exit code 0, so `DONE` is what counts). Known files are skipped unless their modification time changed, failed ones too unless the user asks to retry, and the list is kept in `plugins.json` in the user's settings folder.
- **D-ARCH-26 (M6): hosted plug-in modules stay loaded.** Once a VST3 module has been loaded in the app it isn't unloaded until exit, because unloading a module while one of its editors or threads is still alive crashes some plug-ins. The offline render runs the extra compensation latency at the end and drops it from the start, so a render is aligned with the timeline.
- **D-ARCH-27 (M6): headless UI tests keep windows off the desktop.** `ui::desktopWindows()` is false in `weft_ui_tests`. Plug-in windows and the Delete Track question are then created without a native window, and tests answer the question through `MainComponent::answer`, because a virtual X server without a window manager rejects some window properties.
- **D-ARCH-28 (M7): the fader taper.** Normalized volume and send levels map to dB as `6 + 60·log10(v)`, with `v = 0` as −inf (`minusInfinityDb`). +6 dB is at the top, 0 dB at v ≈ 0.794, −12 dB at 0.5 and −60 dB at about 0.13. Saved automation depends on it, so it doesn't change. The mixer fader uses the same mapping for its travel.
- **D-ARCH-29 (M7): how automation plays.** The snapshot compiler turns each lane the track's mode follows into an `AutomationPlan`: its points at device-rate sample positions (a segment is interpolated in time, so under a tempo change inside a segment the curve follows the clock), the static value, and a shared `AutomationSlot` keyed by track and target that outlives snapshots. Volume, pan and send levels take the lane's value at the end of each span and ramp to it across the span, so they move smoothly with no zipper. Mute reads its value at the start of the span. Insert parameters cut the span at every lane point inside it and set the parameter before each piece, so a change lands on its sample; hosted plug-ins get it in their parameter queue at offset 0 of that piece (D-W8-3). Instrument parameters update once per block. A touched control writes its value into the slot (an atomic); release clears it and Touch asks for the 200 ms glide, which the audio thread runs from the last applied value. A target with no lane gets a plan once touched, so overrides work before anything is written. The engine skips a parameter's static value while automation drives it and puts it back when that stops.
- **D-ARCH-30 (M7): live writing happens on the message thread.** `LiveAutomation` records what the user does from the UI timer (about every 30 ms) and from every control move, using the playhead the audio thread publishes, and thins as it goes (D-W8-2). When the transport stops it hands the pass to `Session::writeAutomation`: one undo step "Write Automation" that removes the points each written range covered, adds the new ones, and switches Write tracks to Latch. Recording on the audio thread instead would be more exact, but the UI only sees control values about 60 times a second anyway, so this keeps the audio thread simple.
- **D-ARCH-31 (M8): LAME comes from Cargo and links statically.** The `weft-codec` crate wraps LAME through `mp3lame-sys` (which builds LAME 3.100 from source with autotools on Linux and `cc` on Windows) and writes its own ID3v2.4 tag and the LAME/Info header. Static linking is allowed because Weft itself is AGPLv3 and its source, so anyone can relink. Keeping the C dependency inside a crate also keeps `third_party/cpp-deps.json` free of a project with no CMake build.
- **D-ARCH-32 (M8): one Rust static library.** Two Rust static libraries in one binary each carry the Rust runtime and clash at link time (MSVC refuses outright). `weft-crdt` and `weft-codec` are now plain Rust libraries, and the new `weft-ffi` crate bundles them into the one static library the client links. The license manifest follows `weft-ffi`.
- **D-ARCH-33 (M8): a bounce renders a slice per timer tick.** `BounceJob` renders about 40 ms worth of work per UI tick through `OfflineRenderer` (a private engine on a null device, as the rendered join already used), so the window stays responsive and Cancel works without a worker thread; hosted plug-ins are created on the message thread as everywhere else. A real-time bounce instead gives the live engine a preallocated `Engine::Capture`; the audio thread copies the master output into it (no allocation, no locks) and the job reads what it filled.
- **D-ARCH-34 (M9): project files beyond the document live in `core/project`.** `ProjectFiles` holds media lookup (`Media/` then `media-links.txt`), Save As, Locate…, the media index and the recent list, with no JUCE, so the [E] tests drive them directly. `Studio` owns one project at a time and swaps folder and session on New and Open; per-user view state is reset and reloaded with it.
- **D-ARCH-35 (M9): one MIDI producer.** `Engine::sendMidi` feeds a single-producer queue, but MIDI devices call from their own threads and the computer keyboard from the message thread. `MidiInputRouter` serializes them with a mutex (none of them is the audio thread) and drops disabled inputs. `JuceMidiInputs` in the platform layer opens the devices.
- **D-ARCH-36 (M9): a new device can change the block size.** `Engine::prepare` now rebuilds the snapshot when the maximum block size changes, not only the sample rate: Audio Setup can reopen the device with bigger buffers, and the old snapshot's buffers were sized for the old block.
- **D-ARCH-37 (M9): No-audio mode runs the engine on the UI timer.** Without an open device, `Studio` drives the engine through a private null device from the UI timer, in step with the system clock, so the transport and playhead keep working (W-0 step 7). Recording is refused.
- **D-ARCH-38 (M10): the relay speaks plain HTTP and WebSocket; TLS ends at a proxy.** `weft-relay` (axum, SQLite in WAL mode with full fsync, Argon2id, HS256 tokens) binds to loopback unless started with `--public`, and expects a TLS-terminating proxy in front of it for anything else (05 §11). Clients refuse `http://` and `ws://` except for localhost and loopback addresses.
- **D-ARCH-39 (M10): one protocol crate.** `crates/weft-proto` defines the frames of 05 §3.3 (1-byte type, CBOR envelope via ciborium, raw Automerge bytes for SYNC and CHANGE); the relay and the client transport both use it, so they can't drift apart.
- **D-ARCH-40 (M10): the client transport is Rust behind a C API; the document stays in C++.** `crates/weft-sync` runs the WebSocket (tungstenite, rustls with web PKI roots) on its own thread with reconnect backoff, keepalive and token refresh, plus blocking REST calls (ureq). It is linked into `weft-ffi` and exposed through `weft_sync.h` with codec values. `weft::collab::SyncClient` (no JUCE) drives it from the session's thread: the Automerge sync protocol runs on the C++ `Document` through new `weft-crdt` sync functions, so there is still exactly one copy of the document.
- **D-ARCH-41 (M10): nothing reads the document while a gesture is open.** Automerge commits an open transaction when asked for its heads or changes, which split a fader drag into a change nobody sent (and that the next change then depended on). `SyncClient::poll` holds every event while a gesture is open and handles them after it ends; found by AT-COL-6 under CPU load.
- **D-ARCH-42 (M10): online/offline switches have epochs.** Each switch bumps an epoch in the connection; a session from an older epoch ends and what it received is dropped, so going offline and straight back online always starts a fresh join and sync exchange.
- **D-ARCH-43 (M10): [S] tests are Catch2 tests against a relay process.** `tests/sync` starts `weft-relay` on a free localhost port (with a test clock for token expiry) and drives real `Session`s through `SyncClient`s; one-way delay is injected in the client transport. The relay's protocol-level behavior (roles, rate limits, kill and restart) is also covered by Rust integration tests in `relay/tests`.
- **D-ARCH-44 (M10): a room is unloaded only when nobody holds it.** The relay unloads an idle room (no clients) to free memory, but a connection that is joining holds the room before it is listed as a client. Unloading then left it in a room the next joiner couldn't see (the next one loaded a second copy from the log), so their changes stopped reaching each other while both were stored. A room is now unloaded only when the map holds the last reference; found by AT-COL-2's partitions.
- **D-ARCH-45 (M11): the relay keeps blobs as files.** `<data>/blobs/<hex>` holds each verified blob once; uploads in progress are `<data>/uploads/<id>.part`. SQLite records blobs, which owners store them (the D-COL-9 dedupe scope), which projects reference them, and open uploads with their offsets. Deleting a project drops its references and uploads; the files stay for the owner's other projects.
- **D-ARCH-46 (M11): transfers run on their own threads.** `weft::collab::Transfers` (no JUCE) has two upload and two download worker threads making blocking HTTP calls through `weft_http` in `crates/weft-sync`, which gained request headers and response headers for ranges. They refresh the access token on a 401 and back off 1, 2, 4… up to 30 s on network errors. Events queue under a lock and `poll()` hands them to the caller's thread; `MediaSync` turns the document into transfers there. Test hooks can stop an upload before a chunk or change downloaded bytes.
- **D-ARCH-47 (M11): a partial media source grows without locks on the audio thread.** `MediaSource::openPartial` reads a file that is still downloading; `extend()` (message thread) reopens the reader and loads each newly complete block into its slot with a release store, then publishes the playable frame count atomically. The audio thread only reads slots and that count, which also tells it whether a missing block is still on its way (silence) or a disk that can't keep up (an underrun).
- **D-ARCH-48 (M11): presence timing uses an injectable clock.** `SyncOptions::clock` (and `Studio::syncClock`) gives the Unix-ms clock for rates, idle, fades and extrapolation, so the [S] and [UI] tests can step through 2 minutes of idle or a 5 s fade without waiting. The app uses the system clock.
