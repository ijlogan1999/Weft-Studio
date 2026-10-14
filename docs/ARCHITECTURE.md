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
| MP3 encoding | (not specified) | **LAME, LGPL, dynamically linked DLL** | MP3 patents expired in 2017. LGPL requires dynamic linking (or shipping object files) for a closed-source app. |
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
├─ crates/weft-crdt/ Rust: C API over Automerge, linked into the client (D-ARCH-2)
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
